// Papyrus engine - Milestone 1 (CLI proof)
// Windows Graphics Capture -> GPU BGRA->NV12 (ID3D11VideoProcessor) -> NVENC HEVC -> .mp4 file
//
// Rules this file follows (see docs/SPEC.md):
//   - a raw frame never leaves the GPU (no readback, no CPU colour conversion)
//   - no hooks, no injection: capture is the public Windows Graphics Capture API only
//   - nothing is heap-allocated per frame or per packet in steady state
//   - no network, no telemetry
//
// Build: see build.sh (cross-compiles with zig/clang to a single static x64 exe).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <avrt.h>
#include <psapi.h>
#include <pdh.h>
#include <hstring.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <shellapi.h>
#include <conio.h>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <clocale>

#include "wgc_abi.h"
#include "mp4.h"
#include "ring.h"
#include "control.h"
#include "../../third_party/nv-codec-headers/nvEncodeAPI.h"

#ifndef PAPYRUS_VER
#define PAPYRUS_VER "dev"
#endif
#define PAPYRUS_VERSION PAPYRUS_VER " (Milestone 2)"

// ----------------------------------------------------------------------------------------------
// Logging
// ----------------------------------------------------------------------------------------------
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logLock;

static void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_logLock);
    fputs(buf, stdout);
    fflush(stdout);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
    LeaveCriticalSection(&g_logLock);
}

static bool g_pauseAtExit = false;
static wchar_t g_errPath[MAX_PATH] = L"";    // worker mode: where a fatal error is left for the control window
static wchar_t g_statPath[MAX_PATH] = L"";
static void writeSmallFile(const wchar_t* path, const char* text) {
    if (!path[0]) return;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(f, text, (DWORD)strlen(text), &wr, nullptr);
    CloseHandle(f);
}
static void pauseIfAsked() {
    if (!g_pauseAtExit) return;
    printf("\nPress any key to close this window . . . ");
    fflush(stdout);
    _getwch();
}

[[noreturn]] static void fatal(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logf("\nERROR: %s\n", buf);
    writeSmallFile(g_errPath, buf);
    logf("Papyrus stopped. Please send the whole text above (or the .log file) back so it can be fixed.\n");
    if (g_log) fclose(g_log);
    pauseIfAsked();
    ExitProcess(1);
}

static void hrcheck(HRESULT hr, const char* what) {
    if (FAILED(hr)) fatal("%s failed (HRESULT 0x%08lX)", what, (unsigned long)hr);
}

// ----------------------------------------------------------------------------------------------
// Time
// ----------------------------------------------------------------------------------------------
static int64_t g_qpf = 1;
static inline int64_t qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static inline double qpcMs(int64_t ticks) { return (double)ticks * 1000.0 / (double)g_qpf; }

// ----------------------------------------------------------------------------------------------
// Settings
// ----------------------------------------------------------------------------------------------
struct Settings {
    int      seconds     = 60;
    int      fps         = 120;
    int      bitrateMbps = 100;
    int      maxrateMbps = 150;
    int      monitor     = -1;      // -1 = primary
    int      delay       = 5;
    int      preset      = 0;       // P3 / P4 / P5, 0 = choose automatically
    bool     bitrateSet  = false;   // false = pick from the quality ladder
    bool     secondsSet  = false;
    bool     replay      = false;   // instant-replay mode: keep the last N seconds in RAM, save on hotkey
    int      clip        = 30;      // replay length in seconds
    int      autosave    = 0;       // test helper: save one clip automatically after this many seconds
    UINT     hkMods      = MOD_CONTROL | MOD_SHIFT;
    UINT     hkVk        = VK_F9;
    wchar_t  hkText[48]  = L"Ctrl+Shift+F9";
    bool     cursor      = true;
    bool     bench       = false;   // synthetic 3840x2160 encode benchmark, no capture
    bool     raw         = false;   // write a raw .h265 stream instead of .mp4
    bool     discard     = false;   // encode but do not write the file (for long soak tests)
    bool     list        = false;
    bool     pause       = false;   // wait for a key before the window closes (used by the Start menu shortcuts)
    bool     openClips   = false;   // just open the clips folder in Explorer
    wchar_t  outDir[MAX_PATH] = L"";
    bool     record      = false;   // record straight to a file
    bool     child       = false;   // hidden worker started by the control window
    bool     tray        = false;   // control window: start hidden in the tray
    DWORD    parentPid   = 0;
    wchar_t  out[MAX_PATH] = L"";
};
static Settings g_cfg;

// ----------------------------------------------------------------------------------------------
// Shared counters (written by worker threads, read by the stats printer)
// ----------------------------------------------------------------------------------------------
static std::atomic<uint64_t> a_submitted{0}, a_completed{0};
static std::atomic<uint64_t> a_newFrames{0}, a_repeats{0}, a_srcSkipped{0};
static std::atomic<uint64_t> a_lateTicks{0}, a_droppedTicks{0};
static std::atomic<uint64_t> a_bytes{0}, a_keyframes{0};
static std::atomic<int64_t>  a_encTicksSum{0}, a_encTicksMax{0}, a_encTicksMaxAll{0}, a_encTicksTotal{0};
static std::atomic<int64_t>  a_lateMaxTicks{0};
static std::atomic<uint64_t> a_slotStalls{0}, a_ringStalls{0};
static std::atomic<bool>     g_stop{false};         // Ctrl+C
static std::atomic<bool>     g_captureDone{false};
static std::atomic<bool>     g_outputQuit{false};
static std::atomic<bool>     g_writerQuit{false};

// ----------------------------------------------------------------------------------------------
// Byte ring between the encoder-output thread and the low-priority disk writer.
// One slab, allocated once. Single producer, single consumer.
// ----------------------------------------------------------------------------------------------
struct ByteRing {
    uint8_t* buf = nullptr;
    uint64_t cap = 0;
    std::atomic<uint64_t> head{0};   // total bytes produced
    std::atomic<uint64_t> tail{0};   // total bytes consumed
    HANDLE evData = nullptr, evSpace = nullptr;

    void init(uint64_t bytes) {
        cap = bytes;
        buf = (uint8_t*)VirtualAlloc(nullptr, (SIZE_T)bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buf) fatal("Could not allocate the %llu MB write buffer", (unsigned long long)(bytes >> 20));
        evData = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        evSpace = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    void push(const uint8_t* p, uint64_t n) {
        if (n > cap) fatal("Encoded packet (%llu bytes) is larger than the write buffer", (unsigned long long)n);
        while (cap - (head.load() - tail.load()) < n) {
            a_ringStalls++;
            WaitForSingleObject(evSpace, 5);
        }
        uint64_t off = head.load() % cap;
        uint64_t first = (n < cap - off) ? n : cap - off;
        memcpy(buf + off, p, (size_t)first);
        if (first < n) memcpy(buf, p + first, (size_t)(n - first));
        head.fetch_add(n);
        SetEvent(evData);
    }
};
static ByteRing g_ring;
static Mp4Mux        g_mp4;
static HevcParamSets g_ps;

// Instant replay state
static ReplayRing       g_replay;
static CRITICAL_SECTION g_replayLock;
static uint64_t         g_slabBytes = 0;
static HANDLE           g_evSave = nullptr;
static std::atomic<bool>     g_saveQuit{false};
static std::atomic<uint64_t> a_clipsSaved{0}, a_clipsFailed{0};
static wchar_t          g_outDir[MAX_PATH] = L"";
static uint8_t*         g_scratch = nullptr;
enum { SCRATCH_BYTES = 1 << 20 };
static HANDLE   g_file = INVALID_HANDLE_VALUE;

static DWORD WINAPI writerThread(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        uint64_t avail = g_ring.head.load() - g_ring.tail.load();
        if (avail == 0) {
            if (g_writerQuit.load()) break;
            WaitForSingleObject(g_ring.evData, 100);
            continue;
        }
        uint64_t off = g_ring.tail.load() % g_ring.cap;
        uint64_t chunk = avail;
        if (chunk > g_ring.cap - off) chunk = g_ring.cap - off;
        if (chunk > (4u << 20)) chunk = 4u << 20;
        DWORD written = 0;
        if (!WriteFile(g_file, g_ring.buf + off, (DWORD)chunk, &written, nullptr) || written != chunk)
            fatal("Writing the output file failed (Windows error %lu). Is the disk full?", GetLastError());
        g_ring.tail.fetch_add(chunk);
        SetEvent(g_ring.evSpace);
    }
    return 0;
}

// ----------------------------------------------------------------------------------------------
// D3D11 + video processor (GPU colour conversion BGRA -> NV12)
// ----------------------------------------------------------------------------------------------
static ID3D11Device*            g_dev  = nullptr;
static ID3D11DeviceContext*     g_ctx  = nullptr;
static ID3D11VideoDevice*       g_vdev = nullptr;
static ID3D11VideoContext*      g_vctx = nullptr;
static ID3D11VideoProcessorEnumerator* g_vpEnum = nullptr;
static ID3D11VideoProcessor*    g_vp   = nullptr;
static uint32_t g_srcW = 0, g_srcH = 0;   // capture size
static uint32_t g_encW = 0, g_encH = 0;   // encode size (even)
static wchar_t  g_adapterName[128] = L"";
static bool     g_hdrOn = false;

enum { POOL = 4 };                                       // NV12 textures / bitstream buffers / events
static ID3D11Texture2D*                 g_nv12[POOL] = {};
static ID3D11VideoProcessorOutputView*  g_nv12View[POOL] = {};

struct InputViewCacheEntry { ID3D11Texture2D* tex; ID3D11VideoProcessorInputView* view; };
enum { VIEW_CACHE = 8 };
static InputViewCacheEntry g_viewCache[VIEW_CACHE] = {};
static int  g_viewCacheCount = 0;
static bool g_copyPath = false;                          // fallback: copy the frame into our own texture first
static ID3D11Texture2D* g_copyTex = nullptr;
static ID3D11VideoProcessorInputView* g_copyView = nullptr;

static HMONITOR g_monitor = nullptr;

struct MonEnum { int want; int index; HMONITOR found; bool list; };
static BOOL CALLBACK monEnumProc(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    MonEnum* e = (MonEnum*)lp;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(m, &mi);
    if (e->list)
        logf("  monitor %d: %ldx%ld at (%ld,%ld)%s  %ls\n", e->index,
             mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
             mi.rcMonitor.left, mi.rcMonitor.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? " [primary]" : "", mi.szDevice);
    if (e->index == e->want) e->found = m;
    e->index++;
    return TRUE;
}

static void createDevice() {
    IDXGIFactory1* factory = nullptr;
    hrcheck(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory), "CreateDXGIFactory1");
    IDXGIAdapter1* chosen = nullptr;
    DXGI_ADAPTER_DESC1 chosenDesc{};
    for (UINT a = 0; !chosen; a++) {
        IDXGIAdapter1* ad = nullptr;
        if (factory->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; o++) {
            IDXGIOutput* out = nullptr;
            if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            if (od.Monitor == g_monitor) {
                chosen = ad;
                chosen->AddRef();
                ad->GetDesc1(&chosenDesc);
                IDXGIOutput6* out6 = nullptr;
                if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), (void**)&out6))) {
                    DXGI_OUTPUT_DESC1 d1{};
                    if (SUCCEEDED(out6->GetDesc1(&d1)))
                        g_hdrOn = (d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                    out6->Release();
                }
            }
            out->Release();
            if (chosen) break;
        }
        ad->Release();
    }
    factory->Release();
    if (!chosen) fatal("Could not find the graphics card that drives the chosen monitor");
    wcsncpy(g_adapterName, chosenDesc.Description, 127);
    if (chosenDesc.VendorId != 0x10DE)
        fatal("The monitor is driven by \"%ls\", which is not an NVIDIA card. Milestone 1 supports NVIDIA (NVENC) only; "
              "AMD and Intel arrive in Milestone 7. If this PC has an NVIDIA card, plug the monitor into it.",
              chosenDesc.Description);

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   levels, 2, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
    hrcheck(hr, "D3D11CreateDevice");
    chosen->Release();

    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(g_ctx->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    hrcheck(g_dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&g_vdev), "Get ID3D11VideoDevice");
    hrcheck(g_ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&g_vctx), "Get ID3D11VideoContext");
}

static void createVideoProcessor() {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate.Numerator = (UINT)g_cfg.fps;  cd.InputFrameRate.Denominator = 1;
    cd.InputWidth = g_srcW;  cd.InputHeight = g_srcH;
    cd.OutputFrameRate = cd.InputFrameRate;
    cd.OutputWidth = g_encW; cd.OutputHeight = g_encH;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    hrcheck(g_vdev->CreateVideoProcessorEnumerator(&cd, &g_vpEnum), "CreateVideoProcessorEnumerator");
    hrcheck(g_vdev->CreateVideoProcessor(g_vpEnum, 0, &g_vp), "CreateVideoProcessor");

    g_vctx->VideoProcessorSetStreamFrameFormat(g_vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    g_vctx->VideoProcessorSetStreamOutputRate(g_vp, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, nullptr);
    g_vctx->VideoProcessorSetStreamAutoProcessingMode(g_vp, 0, FALSE);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};     // full-range RGB (desktop)
    in.Usage = 0; in.RGB_Range = 0;
    g_vctx->VideoProcessorSetStreamColorSpace(g_vp, 0, &in);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};    // BT.709, studio range (16-235) - matches the VUI we signal in the stream
    out.Usage = 0; out.YCbCr_Matrix = 1; out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    g_vctx->VideoProcessorSetOutputColorSpace(g_vp, &out);

    for (int i = 0; i < POOL; i++) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = g_encW; td.Height = g_encH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        hrcheck(g_dev->CreateTexture2D(&td, nullptr, &g_nv12[i]), "Create NV12 texture");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
        ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hrcheck(g_vdev->CreateVideoProcessorOutputView(g_nv12[i], g_vpEnum, &ov, &g_nv12View[i]), "Create NV12 output view");
    }
}

// Returns the (cached) video-processor input view for a captured frame texture.
static ID3D11VideoProcessorInputView* inputViewFor(ID3D11Texture2D* tex) {
    if (!g_copyPath) {
        for (int i = 0; i < g_viewCacheCount; i++)
            if (g_viewCache[i].tex == tex) return g_viewCache[i].view;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ID3D11VideoProcessorInputView* view = nullptr;
        HRESULT hr = g_vdev->CreateVideoProcessorInputView(tex, g_vpEnum, &iv, &view);
        if (SUCCEEDED(hr)) {
            if (g_viewCacheCount == VIEW_CACHE) {            // pool was recreated: start over
                for (int i = 0; i < VIEW_CACHE; i++) { g_viewCache[i].view->Release(); g_viewCache[i].tex->Release(); }
                g_viewCacheCount = 0;
            }
            tex->AddRef();
            g_viewCache[g_viewCacheCount++] = { tex, view };
            return view;
        }
        // The driver refused to read the capture texture directly: fall back to one GPU->GPU copy.
        D3D11_TEXTURE2D_DESC sd{};
        tex->GetDesc(&sd);
        logf("note: direct conversion of capture textures not accepted (0x%08lX, bind flags 0x%X) - using GPU copy path\n",
             (unsigned long)hr, sd.BindFlags);
        D3D11_TEXTURE2D_DESC td = sd;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0; td.MiscFlags = 0; td.MipLevels = 1; td.ArraySize = 1;
        hrcheck(g_dev->CreateTexture2D(&td, nullptr, &g_copyTex), "Create copy texture");
        hrcheck(g_vdev->CreateVideoProcessorInputView(g_copyTex, g_vpEnum, &iv, &g_copyView), "Create input view (copy path)");
        g_copyPath = true;
    }
    g_ctx->CopyResource(g_copyTex, tex);
    return g_copyView;
}

// ----------------------------------------------------------------------------------------------
// Windows Graphics Capture (raw WinRT ABI, see wgc_abi.h)
// ----------------------------------------------------------------------------------------------
typedef HRESULT (WINAPI *RoInitialize_t)(int);
typedef HRESULT (WINAPI *RoGetActivationFactory_t)(HSTRING, REFIID, void**);
typedef HRESULT (WINAPI *WindowsCreateStringReference_t)(PCWSTR, UINT32, HSTRING_HEADER*, HSTRING*);
typedef HRESULT (WINAPI *CreateDirect3D11DeviceFromDXGIDevice_t)(IDXGIDevice*, IInspectable**);

static wgc::IGraphicsCaptureItem*        g_item = nullptr;
static wgc::IDirect3D11CaptureFramePool* g_pool = nullptr;
static wgc::IGraphicsCaptureSession*     g_session = nullptr;
static bool g_borderOff = false, g_minIntervalSet = false;

static void* activationFactory(RoGetActivationFactory_t roGet, WindowsCreateStringReference_t mkStr,
                               const wchar_t* cls, REFIID iid) {
    HSTRING_HEADER hdr{};
    HSTRING hs = nullptr;
    hrcheck(mkStr(cls, (UINT32)wcslen(cls), &hdr, &hs), "WindowsCreateStringReference");
    void* p = nullptr;
    HRESULT hr = roGet(hs, iid, &p);
    if (FAILED(hr)) fatal("Windows Graphics Capture is not available (%ls, 0x%08lX). Windows 10 1903 or newer is required.",
                          cls, (unsigned long)hr);
    return p;
}

static void startCapture() {
    HMODULE combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!combase || !d3d11) fatal("Could not load combase.dll / d3d11.dll");
    auto roInit = (RoInitialize_t)(void*)GetProcAddress(combase, "RoInitialize");
    auto roGet  = (RoGetActivationFactory_t)(void*)GetProcAddress(combase, "RoGetActivationFactory");
    auto mkStr  = (WindowsCreateStringReference_t)(void*)GetProcAddress(combase, "WindowsCreateStringReference");
    auto mkDev  = (CreateDirect3D11DeviceFromDXGIDevice_t)(void*)GetProcAddress(d3d11, "CreateDirect3D11DeviceFromDXGIDevice");
    if (!roInit || !roGet || !mkStr || !mkDev) fatal("This Windows version is missing the WinRT capture entry points");
    HRESULT hr = roInit(1 /* multithreaded */);
    if (FAILED(hr) && hr != (HRESULT)0x80010106L /* RPC_E_CHANGED_MODE */) hrcheck(hr, "RoInitialize");

    // GraphicsCaptureItem for the monitor
    auto interop = (wgc::IGraphicsCaptureItemInterop*)activationFactory(roGet, mkStr,
                        L"Windows.Graphics.Capture.GraphicsCaptureItem", wgc::IID_IGraphicsCaptureItemInterop);
    hr = interop->CreateForMonitor(g_monitor, wgc::IID_IGraphicsCaptureItem, (void**)&g_item);
    if (FAILED(hr)) fatal("Could not start capturing this monitor (0x%08lX)", (unsigned long)hr);
    interop->Release();
    wgc::SizeInt32 size{};
    hrcheck(g_item->get_Size(&size), "GraphicsCaptureItem.Size");
    g_srcW = (uint32_t)size.Width;  g_srcH = (uint32_t)size.Height;
    g_encW = g_srcW & ~1u;          g_encH = g_srcH & ~1u;

    // Wrap our D3D11 device for WinRT
    IDXGIDevice* dxgiDev = nullptr;
    hrcheck(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev), "Get IDXGIDevice");
    IInspectable* insp = nullptr;
    hrcheck(mkDev(dxgiDev, &insp), "CreateDirect3D11DeviceFromDXGIDevice");
    dxgiDev->Release();
    IInspectable* rtDevice = nullptr;
    hrcheck(insp->QueryInterface(wgc::IID_IDirect3DDevice, (void**)&rtDevice), "Get IDirect3DDevice");
    insp->Release();

    // Free-threaded frame pool: we poll it from the capture thread, no dispatcher or callbacks needed
    auto statics = (wgc::IDirect3D11CaptureFramePoolStatics2*)activationFactory(roGet, mkStr,
                        L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", wgc::IID_IDirect3D11CaptureFramePoolStatics2);
    hrcheck(statics->CreateFreeThreaded(rtDevice, wgc::PixelFormat_B8G8R8A8UIntNormalized, 4, size, &g_pool),
            "Direct3D11CaptureFramePool.CreateFreeThreaded");
    statics->Release();
    rtDevice->Release();
    hrcheck(g_pool->CreateCaptureSession(g_item, &g_session), "CreateCaptureSession");

    // Optional features - each one is feature-detected by asking for its interface; missing = skip, never crash
    wgc::IGraphicsCaptureSession2* s2 = nullptr;
    if (SUCCEEDED(g_session->QueryInterface(wgc::IID_IGraphicsCaptureSession2, (void**)&s2))) {
        s2->put_IsCursorCaptureEnabled(g_cfg.cursor ? 1 : 0);
        s2->Release();
    }
    wgc::IGraphicsCaptureSession3* s3 = nullptr;
    if (SUCCEEDED(g_session->QueryInterface(wgc::IID_IGraphicsCaptureSession3, (void**)&s3))) {
        g_borderOff = SUCCEEDED(s3->put_IsBorderRequired(0));
        s3->Release();
    }
    wgc::IGraphicsCaptureSession5* s5 = nullptr;
    if (SUCCEEDED(g_session->QueryInterface(wgc::IID_IGraphicsCaptureSession5, (void**)&s5))) {
        // Half a frame interval: guarantees a fresh frame is ready for every encoder tick when the game is fast enough.
        wgc::TimeSpan ts{ 10000000LL / (g_cfg.fps * 2) };
        g_minIntervalSet = SUCCEEDED(s5->put_MinUpdateInterval(ts));
        s5->Release();
    }
    hrcheck(g_session->StartCapture(), "StartCapture");
}

static void closeFrame(wgc::IDirect3D11CaptureFrame* f) {
    wgc::IClosable* c = nullptr;
    if (SUCCEEDED(f->QueryInterface(wgc::IID_IClosable, (void**)&c))) { c->Close(); c->Release(); }
    f->Release();
}

// ----------------------------------------------------------------------------------------------
// NVENC
// ----------------------------------------------------------------------------------------------
static NV_ENCODE_API_FUNCTION_LIST g_nv{};
static void*   g_enc = nullptr;
static bool    g_async = false;
static NV_ENC_REGISTERED_PTR g_reg[POOL] = {};
static NV_ENC_INPUT_PTR      g_mapped[POOL] = {};
static NV_ENC_OUTPUT_PTR     g_bs[POOL] = {};
static HANDLE                g_ev[POOL] = {};
static HANDLE                g_evEos = nullptr;
static int64_t               g_submitQpc[POOL] = {};
static HANDLE                g_semWork = nullptr;    // one count per submitted frame
static HANDLE                g_evSlot = nullptr;     // signalled when a slot is released
static uint32_t              g_driverNvencVer = 0;

static void nvcheck(NVENCSTATUS st, const char* what) {
    if (st == NV_ENC_SUCCESS) return;
    const char* detail = (g_enc && g_nv.nvEncGetLastErrorString) ? g_nv.nvEncGetLastErrorString(g_enc) : "";
    fatal("NVENC: %s failed (status %d) %s", what, (int)st, detail ? detail : "");
}

static void initEncoder() {
    HMODULE dll = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) fatal("nvEncodeAPI64.dll not found. Install or update the NVIDIA graphics driver (version 570 or newer).");
    typedef NVENCSTATUS (NVENCAPI *GetMax_t)(uint32_t*);
    typedef NVENCSTATUS (NVENCAPI *Create_t)(NV_ENCODE_API_FUNCTION_LIST*);
    auto getMax = (GetMax_t)(void*)GetProcAddress(dll, "NvEncodeAPIGetMaxSupportedVersion");
    auto create = (Create_t)(void*)GetProcAddress(dll, "NvEncodeAPICreateInstance");
    if (!getMax || !create) fatal("The NVIDIA encoder library is too old. Update the NVIDIA driver (570 or newer).");
    getMax(&g_driverNvencVer);
    const uint32_t need = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (g_driverNvencVer < need)
        fatal("The NVIDIA driver supports NVENC API %u.%u but Papyrus needs %d.%d. Update the NVIDIA driver to 570 or newer.",
              g_driverNvencVer >> 4, g_driverNvencVer & 0xF, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
    g_nv.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    nvcheck(create(&g_nv), "NvEncodeAPICreateInstance");

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.device = g_dev;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.apiVersion = NVENCAPI_VERSION;
    NVENCSTATUS st = g_nv.nvEncOpenEncodeSessionEx(&open, &g_enc);
    if (st != NV_ENC_SUCCESS) {
        g_enc = nullptr;
        fatal("NVENC could not open an encode session (status %d). Another recorder may be using every NVENC session, "
              "or the driver needs updating.", (int)st);
    }

    auto cap = [](NV_ENC_CAPS c) {
        NV_ENC_CAPS_PARAM p{};
        p.version = NV_ENC_CAPS_PARAM_VER;
        p.capsToQuery = c;
        int v = 0;
        g_nv.nvEncGetEncodeCaps(g_enc, NV_ENC_CODEC_HEVC_GUID, &p, &v);
        return v;
    };
    int maxW = cap(NV_ENC_CAPS_WIDTH_MAX), maxH = cap(NV_ENC_CAPS_HEIGHT_MAX);
    if (maxW == 0) fatal("This NVIDIA card does not support HEVC encoding.");
    if ((int)g_encW > maxW || (int)g_encH > maxH)
        fatal("This card's HEVC encoder is limited to %dx%d, the screen is %ux%u.", maxW, maxH, g_encW, g_encH);
    g_async = cap(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT) != 0;

    const GUID preset = g_cfg.preset <= 3 ? NV_ENC_PRESET_P3_GUID : g_cfg.preset == 4 ? NV_ENC_PRESET_P4_GUID : NV_ENC_PRESET_P5_GUID;
    static NV_ENC_PRESET_CONFIG pc;      // large structs: keep them off the stack
    static NV_ENC_CONFIG cfg;
    memset(&pc, 0, sizeof pc);
    pc.version = NV_ENC_PRESET_CONFIG_VER;
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    nvcheck(g_nv.nvEncGetEncodePresetConfigEx(g_enc, NV_ENC_CODEC_HEVC_GUID, preset, NV_ENC_TUNING_INFO_HIGH_QUALITY, &pc),
            "GetEncodePresetConfigEx");
    cfg = pc.presetCfg;
    cfg.version = NV_ENC_CONFIG_VER;
    cfg.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    cfg.gopLength = (uint32_t)g_cfg.fps;               // 1-second GOP
    cfg.frameIntervalP = 1;                            // no B-frames in Milestone 1 (benchmarked in Milestone 5)
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    cfg.rcParams.averageBitRate = (uint32_t)g_cfg.bitrateMbps * 1000000u;
    cfg.rcParams.maxBitRate = (uint32_t)g_cfg.maxrateMbps * 1000000u;
    cfg.rcParams.vbvBufferSize = cfg.rcParams.maxBitRate;      // 1 second
    cfg.rcParams.vbvInitialDelay = cfg.rcParams.maxBitRate / 2;
    cfg.rcParams.enableLookahead = 0;
    cfg.rcParams.lookaheadDepth = 0;
    cfg.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    NV_ENC_CONFIG_HEVC& h = cfg.encodeCodecConfig.hevcConfig;
    h.idrPeriod = cfg.gopLength;                       // every GOP starts with an IDR: any second is a valid clip start
    h.repeatSPSPPS = 1;
    h.tier = NV_ENC_TIER_HEVC_HIGH;
    h.level = NV_ENC_LEVEL_AUTOSELECT;
    h.chromaFormatIDC = 1;
    h.hevcVUIParameters.videoSignalTypePresentFlag = 1;
    h.hevcVUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    h.hevcVUIParameters.videoFullRangeFlag = 0;
    h.hevcVUIParameters.colourDescriptionPresentFlag = 1;
    h.hevcVUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    h.hevcVUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    h.hevcVUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;

    NV_ENC_INITIALIZE_PARAMS ip{};
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
    ip.presetGUID = preset;
    ip.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    ip.encodeWidth = g_encW;   ip.encodeHeight = g_encH;
    ip.darWidth = g_encW;      ip.darHeight = g_encH;
    ip.maxEncodeWidth = g_encW; ip.maxEncodeHeight = g_encH;
    ip.frameRateNum = (uint32_t)g_cfg.fps; ip.frameRateDen = 1;
    ip.enablePTD = 1;
    ip.enableEncodeAsync = g_async ? 1 : 0;
    ip.encodeConfig = &cfg;
    st = g_nv.nvEncInitializeEncoder(g_enc, &ip);
    if (st != NV_ENC_SUCCESS) {
        const char* detail = g_nv.nvEncGetLastErrorString ? g_nv.nvEncGetLastErrorString(g_enc) : "";
        fatal("NVENC refused %ux%u at %d fps HEVC (status %d) %s", g_encW, g_encH, g_cfg.fps, (int)st, detail ? detail : "");
    }

    for (int i = 0; i < POOL; i++) {
        NV_ENC_REGISTER_RESOURCE rr{};
        rr.version = NV_ENC_REGISTER_RESOURCE_VER;
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        rr.width = g_encW; rr.height = g_encH; rr.pitch = 0;
        rr.resourceToRegister = g_nv12[i];
        rr.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        rr.bufferUsage = NV_ENC_INPUT_IMAGE;
        nvcheck(g_nv.nvEncRegisterResource(g_enc, &rr), "RegisterResource");
        g_reg[i] = rr.registeredResource;

        NV_ENC_CREATE_BITSTREAM_BUFFER cb{};
        cb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        nvcheck(g_nv.nvEncCreateBitstreamBuffer(g_enc, &cb), "CreateBitstreamBuffer");
        g_bs[i] = cb.bitstreamBuffer;

        if (g_async) {
            g_ev[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            NV_ENC_EVENT_PARAMS ep{};
            ep.version = NV_ENC_EVENT_PARAMS_VER;
            ep.completionEvent = g_ev[i];
            nvcheck(g_nv.nvEncRegisterAsyncEvent(g_enc, &ep), "RegisterAsyncEvent");
        }
    }
    if (g_async) {
        g_evEos = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        NV_ENC_EVENT_PARAMS ep{};
        ep.version = NV_ENC_EVENT_PARAMS_VER;
        ep.completionEvent = g_evEos;
        nvcheck(g_nv.nvEncRegisterAsyncEvent(g_enc, &ep), "RegisterAsyncEvent");
    }
    g_semWork = CreateSemaphoreW(nullptr, 0, 0x7fffffff, nullptr);
    g_evSlot = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

// Encoder output thread: waits for each frame's completion event, copies the packet out, frees the slot.
static DWORD WINAPI outputThread(void*) {
    DWORD taskIdx = 0;
    AvSetMmThreadCharacteristicsW(L"Capture", &taskIdx);
    for (;;) {
        WaitForSingleObject(g_semWork, INFINITE);
        if (a_completed.load() == a_submitted.load()) {
            if (g_outputQuit.load()) break;
            continue;
        }
        const int idx = (int)(a_completed.load() % POOL);
        if (g_async && WaitForSingleObject(g_ev[idx], 10000) != WAIT_OBJECT_0)
            fatal("NVENC did not finish a frame within 10 seconds");
        NV_ENC_LOCK_BITSTREAM lb{};
        lb.version = NV_ENC_LOCK_BITSTREAM_VER;
        lb.outputBitstream = g_bs[idx];
        lb.doNotWait = 0;
        nvcheck(g_nv.nvEncLockBitstream(g_enc, &lb), "LockBitstream");
        const int64_t dt = qpc() - g_submitQpc[idx];
        a_encTicksSum.fetch_add(dt);
        a_encTicksTotal.fetch_add(dt);
        if (dt > a_encTicksMax.load()) a_encTicksMax.store(dt);
        if (dt > a_encTicksMaxAll.load()) a_encTicksMaxAll.store(dt);
        const uint8_t* pkt = (const uint8_t*)lb.bitstreamBufferPtr;
        const bool isKey = lb.pictureType == NV_ENC_PIC_TYPE_IDR;
        if (g_cfg.replay) {
            EnterCriticalSection(&g_replayLock);
            g_replay.beginPacket();
            hevcToLengthPrefixed(pkt, lb.bitstreamSizeInBytes, g_ps, [](const uint8_t* d, uint32_t n) { g_replay.write(d, n); });
            g_replay.endPacket(isKey, (int64_t)lb.outputTimeStamp);
            LeaveCriticalSection(&g_replayLock);
        } else if (!g_cfg.discard) {
            if (g_cfg.raw) g_ring.push(pkt, lb.bitstreamSizeInBytes);
            else {
                const uint32_t sz = hevcToLengthPrefixed(pkt, lb.bitstreamSizeInBytes, g_ps, [](const uint8_t* d, uint32_t n) { g_ring.push(d, n); });
                g_mp4.addIndexed(sz, isKey);
            }
        }
        a_bytes.fetch_add(lb.bitstreamSizeInBytes);
        if (lb.pictureType == NV_ENC_PIC_TYPE_IDR) a_keyframes++;
        nvcheck(g_nv.nvEncUnlockBitstream(g_enc, g_bs[idx]), "UnlockBitstream");
        nvcheck(g_nv.nvEncUnmapInputResource(g_enc, g_mapped[idx]), "UnmapInputResource");
        a_completed++;
        SetEvent(g_evSlot);
    }
    return 0;
}

// ----------------------------------------------------------------------------------------------
// Capture + pacing thread: one encoder submission per tick of a QPC clock, exactly fps per second.
// ----------------------------------------------------------------------------------------------
static HANDLE g_timer = nullptr;

static void waitUntil(int64_t due) {
    for (;;) {
        const int64_t rem = due - qpc();
        if (rem <= 0) return;
        const int64_t rem100ns = rem * 10000000 / g_qpf;
        if (rem100ns < 500) return;                       // within 50 microseconds: close enough, do not spin
        LARGE_INTEGER li;
        li.QuadPart = -rem100ns;
        SetWaitableTimer(g_timer, &li, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(g_timer, 1000);
    }
}

static DWORD WINAPI captureThread(void*) {
    DWORD taskIdx = 0;
    AvSetMmThreadCharacteristicsW(L"Capture", &taskIdx);

    // Wait for the very first frame so the clock starts on real content
    wgc::IDirect3D11CaptureFrame* held = nullptr;
    ID3D11VideoProcessorInputView* heldView = nullptr;
    const int64_t waitStart = qpc();
    while (!held && !g_stop.load()) {
        g_pool->TryGetNextFrame(&held);
        if (held) break;
        if (qpcMs(qpc() - waitStart) > 2000.0)
            fatal("Windows Graphics Capture delivered no frame for 2 seconds. (The DXGI Desktop Duplication fallback "
                  "is planned for a later milestone - please report this.)");
        Sleep(1);
    }
    auto bindHeld = [&]() {
        IInspectable* surf = nullptr;
        hrcheck(held->get_Surface(&surf), "CaptureFrame.Surface");
        wgc::IDirect3DDxgiInterfaceAccess* acc = nullptr;
        hrcheck(surf->QueryInterface(wgc::IID_IDirect3DDxgiInterfaceAccess, (void**)&acc), "Get IDirect3DDxgiInterfaceAccess");
        ID3D11Texture2D* tex = nullptr;
        hrcheck(acc->GetInterface(__uuidof(ID3D11Texture2D), (void**)&tex), "Get frame texture");
        heldView = inputViewFor(tex);
        tex->Release(); acc->Release(); surf->Release();
    };
    if (held) bindHeld();

    const uint64_t total = (g_cfg.replay && !g_cfg.secondsSet) ? ~0ull : (uint64_t)g_cfg.seconds * (uint64_t)g_cfg.fps;
    const int64_t  t0 = qpc();
    const int64_t  halfInterval = g_qpf / (g_cfg.fps * 2);
    bool first = true;
    wgc::IDirect3D11CaptureFrame* pending[2] = {};
    int nPending = 0;

    for (uint64_t k = 0; k < total && !g_stop.load(); k++) {
        const int64_t due = t0 + (int64_t)(k * (uint64_t)g_qpf / (uint64_t)g_cfg.fps);
        waitUntil(due);
        const int64_t late = qpc() - due;
        if (late > a_lateMaxTicks.load()) a_lateMaxTicks.store(late);
        if (late > halfInterval) a_lateTicks++;
        if (late > g_qpf / 2) {                            // more than 0.5 s behind: give up on those ticks
            const uint64_t skip = (uint64_t)late * (uint64_t)g_cfg.fps / (uint64_t)g_qpf;
            a_droppedTicks.fetch_add(skip);
            k += skip;
            if (k >= total) break;
        }

        // Collect frames that arrived since the last tick into a tiny queue (at most 2 waiting).
        // Frames often arrive in uneven bursts (two in one tick, none in the next). Showing the older one now
        // and the newer one next tick keeps every unique frame and avoids a repeat. If the game is simply
        // faster than the recording, the oldest waiting frame is the one that gets skipped.
        for (;;) {
            wgc::IDirect3D11CaptureFrame* f = nullptr;
            g_pool->TryGetNextFrame(&f);
            if (!f) break;
            if (nPending == 2) { closeFrame(pending[0]); pending[0] = pending[1]; nPending = 1; a_srcSkipped++; }
            pending[nPending++] = f;
        }
        if (nPending) {
            closeFrame(held);
            held = pending[0];
            pending[0] = pending[1];
            nPending--;
            bindHeld();
            a_newFrames++;
        } else if (first) {
            a_newFrames++;                                 // the frame we waited for above
        } else {
            a_repeats++;                                   // nothing changed on screen: repeat the last picture
        }
        first = false;

        // Wait for a free slot (only happens if the encoder falls behind)
        while (a_submitted.load() - a_completed.load() >= POOL) {
            a_slotStalls++;
            WaitForSingleObject(g_evSlot, 2);
        }
        const int idx = (int)(a_submitted.load() % POOL);

        // GPU colour conversion BGRA -> NV12 straight into the texture NVENC reads
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = heldView;
        HRESULT hr = g_vctx->VideoProcessorBlt(g_vp, g_nv12View[idx], 0, 1, &stream);
        if (FAILED(hr)) fatal("GPU colour conversion failed (HRESULT 0x%08lX)", (unsigned long)hr);

        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = g_reg[idx];
        nvcheck(g_nv.nvEncMapInputResource(g_enc, &map), "MapInputResource");
        g_mapped[idx] = map.mappedResource;

        static NV_ENC_PIC_PARAMS pp;
        memset(&pp, 0, sizeof pp);
        pp.version = NV_ENC_PIC_PARAMS_VER;
        pp.inputWidth = g_encW;  pp.inputHeight = g_encH;  pp.inputPitch = g_encW;
        pp.inputBuffer = map.mappedResource;
        pp.bufferFmt = map.mappedBufferFmt;
        pp.outputBitstream = g_bs[idx];
        pp.completionEvent = g_async ? g_ev[idx] : nullptr;
        pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pp.inputTimeStamp = k;
        pp.inputDuration = 1;
        g_submitQpc[idx] = qpc();
        NVENCSTATUS st = g_nv.nvEncEncodePicture(g_enc, &pp);
        if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) nvcheck(st, "EncodePicture");
        a_submitted++;
        ReleaseSemaphore(g_semWork, 1, nullptr);
    }
    if (held) closeFrame(held);
    for (int i = 0; i < nPending; i++) closeFrame(pending[i]);
    g_captureDone.store(true);
    return 0;
}

// ----------------------------------------------------------------------------------------------
// Diagnostics: CPU, RAM, NVENC load (NVML), per-process GPU engine load (same counters Task Manager uses)
// ----------------------------------------------------------------------------------------------
struct MemCountersEx2 {
    PROCESS_MEMORY_COUNTERS_EX ex;
    ULONG64 PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};
static double privateWorkingSetMb(bool* exact) {
    MemCountersEx2 m{};
    m.ex.cb = sizeof m;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m, sizeof m)) {
        *exact = true;
        return (double)m.PrivateWorkingSetSize / 1048576.0;
    }
    PROCESS_MEMORY_COUNTERS_EX e{};
    e.cb = sizeof e;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&e, sizeof e);
    *exact = false;
    return (double)e.WorkingSetSize / 1048576.0;
}

static int64_t processCpu100ns() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return (int64_t)(((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
           (int64_t)(((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

// NVML (ships with the NVIDIA driver) - whole-card encoder load
typedef int (*nvmlInit_t)();
typedef int (*nvmlHandle_t)(unsigned, void**);
typedef int (*nvmlEncUtil_t)(void*, unsigned*, unsigned*);
static nvmlEncUtil_t g_nvmlEncUtil = nullptr;
static void* g_nvmlDev = nullptr;
static void initNvml() {
    HMODULE m = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return;
    auto init = (nvmlInit_t)(void*)GetProcAddress(m, "nvmlInit_v2");
    auto handle = (nvmlHandle_t)(void*)GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2");
    auto enc = (nvmlEncUtil_t)(void*)GetProcAddress(m, "nvmlDeviceGetEncoderUtilization");
    if (!init || !handle || !enc) return;
    if (init() != 0) return;
    if (handle(0, &g_nvmlDev) != 0) return;
    g_nvmlEncUtil = enc;
}
static int nvencUtil() {
    if (!g_nvmlEncUtil) return -1;
    unsigned util = 0, period = 0;
    return g_nvmlEncUtil(g_nvmlDev, &util, &period) == 0 ? (int)util : -1;
}

// "GPU Engine" performance counters, filtered to our own process
static PDH_HQUERY   g_pdhQuery = nullptr;
static PDH_HCOUNTER g_pdhCounter = nullptr;
static uint8_t*     g_pdhBuf = nullptr;
static const DWORD  g_pdhBufSize = 1u << 20;
static wchar_t      g_pidPrefix[32];
static void initGpuCounters() {
    swprintf(g_pidPrefix, 32, L"pid_%lu_", GetCurrentProcessId());
    if (PdhOpenQueryW(nullptr, 0, &g_pdhQuery) != ERROR_SUCCESS) { g_pdhQuery = nullptr; return; }
    if (PdhAddEnglishCounterW(g_pdhQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_pdhCounter) != ERROR_SUCCESS) {
        PdhCloseQuery(g_pdhQuery); g_pdhQuery = nullptr; return;
    }
    g_pdhBuf = (uint8_t*)VirtualAlloc(nullptr, g_pdhBufSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    PdhCollectQueryData(g_pdhQuery);
}
static bool gpuEngineUsage(double* threeD, double* videoEncode, double* other) {
    *threeD = *videoEncode = *other = 0;
    if (!g_pdhQuery || !g_pdhBuf) return false;
    if (PdhCollectQueryData(g_pdhQuery) != ERROR_SUCCESS) return false;
    DWORD size = g_pdhBufSize, count = 0;
    auto items = (PDH_FMT_COUNTERVALUE_ITEM_W*)g_pdhBuf;
    if (PdhGetFormattedCounterArrayW(g_pdhCounter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return false;
    for (DWORD i = 0; i < count; i++) {
        if (!items[i].szName || !wcsstr(items[i].szName, g_pidPrefix)) continue;
        const double v = items[i].FmtValue.doubleValue;
        if (wcsstr(items[i].szName, L"engtype_3D")) *threeD += v;
        else if (wcsstr(items[i].szName, L"engtype_VideoEncode")) *videoEncode += v;
        else *other += v;
    }
    return true;
}

// ----------------------------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------------------------
// Releases everything initEncoder() created, so the NVENC session is freed immediately.
static void destroyEncoder() {
    for (int i = 0; i < POOL; i++) {
        g_nv.nvEncUnregisterResource(g_enc, g_reg[i]);
        g_nv.nvEncDestroyBitstreamBuffer(g_enc, g_bs[i]);
        if (g_async) {
            NV_ENC_EVENT_PARAMS ep{};
            ep.version = NV_ENC_EVENT_PARAMS_VER;
            ep.completionEvent = g_ev[i];
            g_nv.nvEncUnregisterAsyncEvent(g_enc, &ep);
            CloseHandle(g_ev[i]);
            g_ev[i] = nullptr;
        }
    }
    if (g_async) {
        NV_ENC_EVENT_PARAMS ep{};
        ep.version = NV_ENC_EVENT_PARAMS_VER;
        ep.completionEvent = g_evEos;
        g_nv.nvEncUnregisterAsyncEvent(g_enc, &ep);
        CloseHandle(g_evEos);
        g_evEos = nullptr;
    }
    g_nv.nvEncDestroyEncoder(g_enc);
    g_enc = nullptr;
    CloseHandle(g_semWork);
    CloseHandle(g_evSlot);
}

// ----------------------------------------------------------------------------------------------
// Synthetic 4K benchmark: generated 3840x2160 pictures -> GPU colour conversion -> NVENC, as fast as it will go.
// Measures how long the conversion + encode really takes per frame at each preset, with no capture involved,
// so it works on a monitor of any resolution.
// ----------------------------------------------------------------------------------------------
static void runBench4k() {
    enum { NSRC = 4, FRAMES = 720, WARMUP = 120 };
    const uint32_t W = g_encW, H = g_encH;

    // Moving test pictures (gradient + checkerboard + fine noise), uploaded once before any timing starts
    ID3D11VideoProcessorInputView* srcView[NSRC] = {};
    uint32_t* px = (uint32_t*)VirtualAlloc(nullptr, (SIZE_T)W * H * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!px) fatal("Could not allocate the benchmark picture");
    for (uint32_t y = 0; y < H; y++) {
        uint32_t* row = px + (size_t)y * W;
        for (uint32_t x = 0; x < W; x++) {
            const uint32_t n = (((x * 73856093u) ^ (y * 19349663u)) >> 13) & 31;
            const uint32_t r = (x * 200 / W + n) & 255;
            const uint32_t g = (y * 200 / H + n) & 255;
            const uint32_t b = ((((x >> 6) + (y >> 6)) & 1) ? 170u : 40u) + n;
            row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    ID3D11Texture2D* first = nullptr;
    for (int j = 0; j < NSRC; j++) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = px; init.SysMemPitch = W * 4;
        ID3D11Texture2D* tex = nullptr;
        hrcheck(g_dev->CreateTexture2D(&td, j == 0 ? &init : nullptr, &tex), "Create benchmark texture");
        if (j == 0) { first = tex; first->AddRef(); }
        else {
            // the same picture scrolled sideways by 12 px per step, copied GPU-to-GPU (keeps system RAM out of it)
            const UINT shift = (UINT)j * 12;
            D3D11_BOX a{ shift, 0, 0, W, H, 1 }, b{ 0, 0, 0, shift, H, 1 };
            g_ctx->CopySubresourceRegion(tex, 0, 0, 0, 0, first, 0, &a);
            g_ctx->CopySubresourceRegion(tex, 0, W - shift, 0, 0, first, 0, &b);
        }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        hrcheck(g_vdev->CreateVideoProcessorInputView(tex, g_vpEnum, &iv, &srcView[j]), "Create benchmark input view");
        tex->Release();
    }
    first->Release();
    VirtualFree(px, 0, MEM_RELEASE);
    g_ctx->Flush();

    const double budgetMs = 1000.0 / g_cfg.fps;
    logf("  Graphics card : %ls\n", g_adapterName);
    logf("  Test          : %d synthetic %ux%u frames per preset (first %d not timed), HEVC 8-bit, VBR %d/%d Mbps, no B-frames\n",
         FRAMES, W, H, WARMUP, g_cfg.bitrateMbps, g_cfg.maxrateMbps);
    logf("  Target        : %d fps = %.2f ms per frame; budget is 70%% of that = %.2f ms\n\n", g_cfg.fps, budgetMs, budgetMs * 0.7);
    logf("  preset | ms per frame | max fps | %% of frame time | Mbps  | RAM MB | result\n");

    double ms[6] = {};
    bool ok[6] = {};
    static NV_ENC_PIC_PARAMS pp;
    for (int preset = 3; preset <= 5 && !g_stop.load(); preset++) {
        g_cfg.preset = preset;
        initEncoder();
        uint64_t submitted = 0, completed = 0, bytes = 0;
        int64_t t0 = 0;
        auto complete = [&]() {
            const int idx = (int)(completed % POOL);
            if (g_async && WaitForSingleObject(g_ev[idx], 10000) != WAIT_OBJECT_0) fatal("NVENC did not finish a frame within 10 seconds");
            NV_ENC_LOCK_BITSTREAM lb{};
            lb.version = NV_ENC_LOCK_BITSTREAM_VER;
            lb.outputBitstream = g_bs[idx];
            nvcheck(g_nv.nvEncLockBitstream(g_enc, &lb), "LockBitstream");
            if (completed >= WARMUP) bytes += lb.bitstreamSizeInBytes;
            nvcheck(g_nv.nvEncUnlockBitstream(g_enc, g_bs[idx]), "UnlockBitstream");
            nvcheck(g_nv.nvEncUnmapInputResource(g_enc, g_mapped[idx]), "UnmapInputResource");
            completed++;
            if (completed == WARMUP) t0 = qpc();
        };
        for (uint64_t k = 0; k < FRAMES; k++) {
            while (submitted - completed >= POOL) complete();
            const int idx = (int)(submitted % POOL);
            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE;
            stream.pInputSurface = srcView[k % NSRC];
            hrcheck(g_vctx->VideoProcessorBlt(g_vp, g_nv12View[idx], 0, 1, &stream), "GPU colour conversion");
            NV_ENC_MAP_INPUT_RESOURCE map{};
            map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
            map.registeredResource = g_reg[idx];
            nvcheck(g_nv.nvEncMapInputResource(g_enc, &map), "MapInputResource");
            g_mapped[idx] = map.mappedResource;
            memset(&pp, 0, sizeof pp);
            pp.version = NV_ENC_PIC_PARAMS_VER;
            pp.inputWidth = W; pp.inputHeight = H; pp.inputPitch = W;
            pp.inputBuffer = map.mappedResource;
            pp.bufferFmt = map.mappedBufferFmt;
            pp.outputBitstream = g_bs[idx];
            pp.completionEvent = g_async ? g_ev[idx] : nullptr;
            pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            pp.inputTimeStamp = k;
            pp.inputDuration = 1;
            nvcheck(g_nv.nvEncEncodePicture(g_enc, &pp), "EncodePicture");
            submitted++;
        }
        while (completed < submitted) complete();
        const int64_t t1 = qpc();
        memset(&pp, 0, sizeof pp);
        pp.version = NV_ENC_PIC_PARAMS_VER;
        pp.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        pp.completionEvent = g_async ? g_evEos : nullptr;
        g_nv.nvEncEncodePicture(g_enc, &pp);
        if (g_async) WaitForSingleObject(g_evEos, 1000);
        bool exact = true;
        const double ram = privateWorkingSetMb(&exact);
        destroyEncoder();

        const int timed = FRAMES - WARMUP;
        ms[preset] = qpcMs(t1 - t0) / timed;
        ok[preset] = ms[preset] < budgetMs * 0.7;
        logf("    P%d   | %12.2f | %7.1f | %14.0f%% | %5.1f | %6.1f | %s\n", preset, ms[preset], 1000.0 / ms[preset],
             ms[preset] / budgetMs * 100.0, (double)bytes * 8.0 / 1e6 / ((double)timed / g_cfg.fps), ram,
             ok[preset] ? "PASS" : ms[preset] < budgetMs ? "keeps up, but under the 30% safety margin" : "FAIL - cannot reach the target fps");
        Sleep(300);
    }

    int pick = 0;
    for (int p = 5; p >= 3; p--) if (ok[p]) { pick = p; break; }
    logf("\n================ Papyrus 4K benchmark result ================\n");
    if (pick) logf("  Best preset inside the budget for %ux%u @ %d fps: P%d (%.2f ms per frame)\n", W, H, g_cfg.fps, pick, ms[pick]);
    else if (ms[3] > 0 && ms[3] < budgetMs)
        logf("  No preset fits the 70%% budget. P3 keeps up at %.2f ms per frame (%.0f%% of the frame time) with little margin.\n",
             ms[3], ms[3] / budgetMs * 100.0);
    else logf("  No preset reaches %d fps at %ux%u on this card.\n", g_cfg.fps, W, H);
    logf("  Note: these are generated pictures with the encoder working flat out. A running game shares the GPU,\n"
         "        so real recordings will be somewhat slower than this.\n");
    logf("=============================================================\n");
}

// Quality ladder (docs/SPEC.md section 8) and preset choice, used when the user did not set them by hand.
static void applyAutoTier() {
    const uint64_t px = (uint64_t)g_encW * g_encH;
    const bool hi = g_cfg.fps > 60;
    if (!g_cfg.bitrateSet) {
        if (px > 2560ull * 1440 * 11 / 10)      { g_cfg.bitrateMbps = hi ? 100 : 60; g_cfg.maxrateMbps = hi ? 150 : 90; }
        else if (px > 1920ull * 1080 * 11 / 10) { g_cfg.bitrateMbps = hi ? 50 : 35;  g_cfg.maxrateMbps = hi ? 75 : 50; }
        else                                    { g_cfg.bitrateMbps = hi ? 30 : 20;  g_cfg.maxrateMbps = hi ? 45 : 30; }
    }
    // Measured on an RTX 5070: above 1440p120 only P3 leaves the 30 % safety margin (see the 4K benchmark).
    if (g_cfg.preset == 0) g_cfg.preset = (px * (uint64_t)g_cfg.fps > 2560ull * 1440 * 120 * 12 / 10) ? 3 : 4;
}

// "alt+f10", "ctrl+shift+f9", "ctrl+alt+s" ...
static bool parseHotkey(const wchar_t* text) {
    wchar_t buf[48];
    wcsncpy(buf, text, 47); buf[47] = 0;
    UINT mods = 0, vk = 0;
    wchar_t pretty[48] = L"";
    wchar_t* ctx = nullptr;
    for (wchar_t* tok = wcstok(buf, L"+", &ctx); tok; tok = wcstok(nullptr, L"+", &ctx)) {
        if (!_wcsicmp(tok, L"ctrl") || !_wcsicmp(tok, L"control")) { mods |= MOD_CONTROL; wcscat(pretty, L"Ctrl+"); }
        else if (!_wcsicmp(tok, L"alt")) { mods |= MOD_ALT; wcscat(pretty, L"Alt+"); }
        else if (!_wcsicmp(tok, L"shift")) { mods |= MOD_SHIFT; wcscat(pretty, L"Shift+"); }
        else if (!_wcsicmp(tok, L"win")) { mods |= MOD_WIN; wcscat(pretty, L"Win+"); }
        else if ((tok[0] == L'f' || tok[0] == L'F') && tok[1] && _wtoi(tok + 1) >= 1 && _wtoi(tok + 1) <= 24) {
            vk = VK_F1 + (UINT)_wtoi(tok + 1) - 1;
            wchar_t k[8]; swprintf(k, 8, L"F%d", _wtoi(tok + 1)); wcscat(pretty, k);
        }
        else if (tok[1] == 0 && ((tok[0] >= L'a' && tok[0] <= L'z') || (tok[0] >= L'A' && tok[0] <= L'Z') || (tok[0] >= L'0' && tok[0] <= L'9'))) {
            vk = (UINT)towupper(tok[0]);
            wchar_t k[2] = { (wchar_t)vk, 0 }; wcscat(pretty, k);
        }
        else return false;
    }
    if (!vk) return false;
    g_cfg.hkMods = mods; g_cfg.hkVk = vk;
    wcscpy(g_cfg.hkText, pretty);
    return true;
}

// Writes the last g_cfg.clip seconds from the replay ring to a faststart MP4. No re-encode: packets are copied as they are.
static void saveClip() {
    const int64_t t0 = qpc();
    uint64_t off = 0, end = 0;
    bool have = false;
    EnterCriticalSection(&g_replayLock);
    uint64_t startPkt = 0;
    if (g_ps.complete() && g_replay.clipStart((int64_t)g_cfg.clip * g_cfg.fps, &startPkt)) {
        g_mp4.reset();
        const uint64_t endPkt = g_replay.pktHead;
        for (uint64_t i = startPkt; i < endPkt; i++) g_mp4.addIndexed(g_replay.at(i).size, g_replay.at(i).key != 0);
        off = g_replay.at(startPkt).off;
        end = g_replay.at(endPkt - 1).off + g_replay.at(endPkt - 1).size;
        have = g_mp4.nSamples > 0;
    }
    LeaveCriticalSection(&g_replayLock);
    if (!have) { logf("  [save] nothing in the buffer yet - try again in a second\n"); Beep(300, 150); a_clipsFailed++; return; }

    uint32_t moovLen = g_mp4.moovLength(g_ps);
    const uint64_t dataStart = (uint64_t)Mp4Mux::FTYP_LEN + moovLen + Mp4Mux::MDAT_HDR_LEN;
    moovLen = g_mp4.buildMoov(g_ps, dataStart);

    SYSTEMTIME now; GetLocalTime(&now);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\Papyrus-clip-%04d%02d%02d-%02d%02d%02d.mp4", g_outDir,
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { logf("  [save] could not create %ls (Windows error %lu)\n", path, GetLastError()); Beep(300, 150); a_clipsFailed++; return; }

    bool ok = true;
    DWORD wr = 0;
    uint8_t hdr[64];
    const DWORD ftypLen = Mp4Mux::writeFtyp(hdr);
    ok = ok && WriteFile(f, hdr, ftypLen, &wr, nullptr) && wr == ftypLen;
    ok = ok && WriteFile(f, g_mp4.moov, moovLen, &wr, nullptr) && wr == moovLen;
    const DWORD mdatLen = Mp4Mux::writeMdatHeader(hdr, g_mp4.dataBytes);
    ok = ok && WriteFile(f, hdr, mdatLen, &wr, nullptr) && wr == mdatLen;
    bool evicted = false;
    while (ok && off < end) {
        const uint32_t chunk = (uint32_t)((end - off < SCRATCH_BYTES) ? end - off : SCRATCH_BYTES);
        EnterCriticalSection(&g_replayLock);                 // held only for one 1 MB copy, so the encoder never waits long
        const bool got = g_replay.read(off, g_scratch, chunk);
        LeaveCriticalSection(&g_replayLock);
        if (!got) { evicted = true; ok = false; break; }
        ok = WriteFile(f, g_scratch, chunk, &wr, nullptr) && wr == chunk;
        off += chunk;
    }
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(path);
        logf("  [save] FAILED: %s\n", evicted ? "the disk was too slow and the start of the clip was overwritten" : "could not write the file (disk full?)");
        Beep(300, 150);
        a_clipsFailed++;
        return;
    }
    const double secs = (double)g_mp4.nSamples / g_cfg.fps;
    logf("  [save] Clip saved (%02d:%02d, %ux%u @ %d fps, %.1f MB) in %.0f ms -> %ls\n", (int)secs / 60, (int)(secs + 0.5) % 60,
         g_encW, g_encH, g_cfg.fps, (double)(dataStart + g_mp4.dataBytes) / 1048576.0, qpcMs(qpc() - t0), path);
    a_clipsSaved++;
    Beep(1200, 80);
}

static DWORD WINAPI saveThread(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        WaitForSingleObject(g_evSave, INFINITE);
        if (g_saveQuit.load()) break;
        saveClip();
    }
    return 0;
}

static BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        if (type == CTRL_CLOSE_EVENT) Sleep(4000);        // window closed: give the engine time to shut down cleanly
        return TRUE;
    }
    return FALSE;
}

static void usage() {
    printf("Papyrus engine " PAPYRUS_VERSION "\n\n"
           "papyrus-engine.exe [options]\n"
           " Modes\n"
           "  --replay       instant replay: keep the last seconds in RAM, save an .mp4 when the hotkey is pressed\n"
           "  (no mode)      open the Papyrus control window (tray icon and switches)\n"
           "  --tray         same, but start hidden in the tray\n"
           "  --record       record straight to an .mp4 for --seconds\n"
           "  --bench4k      synthetic 3840x2160 encoder benchmark at presets P3, P4, P5 (no capture, no file)\n"
           "  --list         list monitors and exit\n"
           " Replay options\n"
           "  --clip N       replay length in seconds: 15, 30, 60, 120 or 300 (default 30)\n"
           "  --hotkey K     save hotkey, e.g. ctrl+shift+f9 (default) or alt+f10\n"
           "  --autosave N   test helper: save one clip automatically after N seconds\n"
           " Common options\n"
           "  --seconds N    how long to run (record: default 60; replay: default until Ctrl+C)\n"
           "  --fps N        frames per second (default 120)\n"
           "  --bitrate N    target bitrate in Mbps (default: from the quality ladder for the resolution)\n"
           "  --maxrate N    maximum bitrate in Mbps\n"
           "  --preset N     NVENC preset 3, 4 or 5 (default: P3 above 1440p120, otherwise P4)\n"
           "  --monitor N    which monitor (see --list; default = primary)\n"
           "  --delay N      countdown before recording starts, seconds (record mode, default 5)\n"
           "  --no-cursor    do not record the mouse cursor\n"
           "  --discard      record mode: encode but do not write a file (long soak tests)\n"
           "  --raw          record mode: write a raw .h265 stream instead of an .mp4\n"
           "  --out FILE     record mode: output file\n"
           "  --outdir DIR   folder for clips and recordings (default: Videos\\Papyrus)\n"
           "  --open-clips   open the clips folder and exit\n"
           "  --pause        wait for a key before closing the window\n");
}

int wmain(int argc, wchar_t** argv) {
    InitializeCriticalSection(&g_logLock);
    setlocale(LC_ALL, ".UTF8");
    SetConsoleOutputCP(CP_UTF8);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = f.QuadPart;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Papyrus is a windowed program. Modes that print results (benchmark, self-test, record) get a console here;
    // the control window and the hidden worker never open one.
    bool wantConsole = false, isChild = false;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--child")) isChild = true;
        else if (wcscmp(argv[i], L"--tray") && wcscmp(argv[i], L"--open-clips")) wantConsole = true;
    }
    if (wantConsole && !isChild && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen("CONIN$", "r", stdin);
        SetConsoleTitleW(L"Papyrus");
    }

    for (int i = 1; i < argc; i++) {
        auto is = [&](const wchar_t* s) { return wcscmp(argv[i], s) == 0; };
        auto num = [&](int lo, int hi) {
            if (i + 1 >= argc) { usage(); ExitProcess(2); }
            int v = _wtoi(argv[++i]);
            if (v < lo || v > hi) { printf("Value out of range for %ls (allowed %d..%d)\n", argv[i - 1], lo, hi); ExitProcess(2); }
            return v;
        };
        if (is(L"--seconds")) { g_cfg.seconds = num(1, 24 * 3600); g_cfg.secondsSet = true; }
        else if (is(L"--fps")) g_cfg.fps = num(1, 240);
        else if (is(L"--bitrate")) { g_cfg.bitrateMbps = num(1, 400); g_cfg.bitrateSet = true; if (g_cfg.maxrateMbps < g_cfg.bitrateMbps * 3 / 2) g_cfg.maxrateMbps = g_cfg.bitrateMbps * 3 / 2; }
        else if (is(L"--maxrate")) g_cfg.maxrateMbps = num(1, 600);
        else if (is(L"--replay")) g_cfg.replay = true;
        else if (is(L"--record")) g_cfg.record = true;
        else if (is(L"--child")) g_cfg.child = true;
        else if (is(L"--tray")) g_cfg.tray = true;
        else if (is(L"--parent")) g_cfg.parentPid = (DWORD)num(1, 0x7fffffff);
        else if (is(L"--clip")) { g_cfg.clip = num(5, 300); }
        else if (is(L"--autosave")) g_cfg.autosave = num(1, 24 * 3600);
        else if (is(L"--hotkey") && i + 1 < argc) {
            if (!parseHotkey(argv[++i])) { printf("Could not understand the hotkey \"%ls\". Examples: alt+f10, ctrl+shift+f9\n", argv[i]); return 2; }
        }
        else if (is(L"--monitor")) g_cfg.monitor = num(0, 15);
        else if (is(L"--delay")) g_cfg.delay = num(0, 600);
        else if (is(L"--preset")) g_cfg.preset = num(3, 5);
        else if (is(L"--no-cursor")) g_cfg.cursor = false;
        else if (is(L"--discard")) g_cfg.discard = true;
        else if (is(L"--raw")) g_cfg.raw = true;
        else if (is(L"--bench4k")) g_cfg.bench = true;
        else if (is(L"--list")) g_cfg.list = true;
        else if (is(L"--pause")) g_cfg.pause = true;
        else if (is(L"--open-clips")) g_cfg.openClips = true;
        else if (is(L"--outdir") && i + 1 < argc) wcsncpy(g_cfg.outDir, argv[++i], MAX_PATH - 1);
        else if (is(L"--out") && i + 1 < argc) wcsncpy(g_cfg.out, argv[++i], MAX_PATH - 1);
        else { usage(); return is(L"--help") || is(L"-h") || is(L"/?") ? 0 : 2; }
    }
    if (g_cfg.maxrateMbps < g_cfg.bitrateMbps) g_cfg.maxrateMbps = g_cfg.bitrateMbps;
    if (g_cfg.replay) { g_cfg.delay = 0; g_cfg.discard = false; g_cfg.raw = false; }

    g_pauseAtExit = g_cfg.pause;

    // Clips and recordings go to Videos\Papyrus, logs to %LOCALAPPDATA%\Papyrus\logs (both writable without admin rights)
    wchar_t outDir[MAX_PATH] = L"", logDir[MAX_PATH] = L"", dataDir[MAX_PATH] = L"";
    PWSTR known = nullptr;
    if (g_cfg.outDir[0]) wcscpy(outDir, g_cfg.outDir);
    else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &known))) swprintf(outDir, MAX_PATH, L"%ls\\Papyrus", known);
    if (known) { CoTaskMemFree(known); known = nullptr; }
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &known))) {
        swprintf(logDir, MAX_PATH, L"%ls\\Papyrus", known);
        wcscpy(dataDir, logDir);
        CreateDirectoryW(logDir, nullptr);
        wcscat(logDir, L"\\logs");
    }
    if (known) CoTaskMemFree(known);
    if (!outDir[0] || !logDir[0]) {                       // last resort: next to the exe
        wchar_t exeDir[MAX_PATH];
        GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
        if (wchar_t* slash = wcsrchr(exeDir, L'\\')) *slash = 0;
        if (!outDir[0]) swprintf(outDir, MAX_PATH, L"%ls\\output", exeDir);
        if (!logDir[0]) wcscpy(logDir, outDir);
    }
    CreateDirectoryW(outDir, nullptr);
    CreateDirectoryW(logDir, nullptr);
    wcscpy(g_outDir, outDir);
    if (!dataDir[0]) wcscpy(dataDir, logDir);

    // No mode given: this is the control window (tray icon + switches). It starts the worker on demand.
    if (!g_cfg.replay && !g_cfg.record && !g_cfg.bench && !g_cfg.list && !g_cfg.openClips) {
        wchar_t ver[64];
        swprintf(ver, 64, L"version %hs", PAPYRUS_VER);
        return ctl::run(g_cfg.tray, outDir, dataDir, ver, g_cfg.hkText);
    }
    if (g_cfg.child) {
        swprintf(g_errPath, MAX_PATH, L"%ls\\last-error.txt", dataDir);
        swprintf(g_statPath, MAX_PATH, L"%ls\\status.txt", dataDir);
        g_pauseAtExit = false;
    }
    if (g_cfg.openClips) {
        ShellExecuteW(nullptr, L"open", outDir, nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }
    wchar_t logPath[MAX_PATH];
    SYSTEMTIME now; GetLocalTime(&now);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04d%02d%02d-%02d%02d%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    if (!g_cfg.out[0]) swprintf(g_cfg.out, MAX_PATH, L"%ls\\papyrus-%ls.%ls", outDir, stamp, g_cfg.raw ? L"h265" : L"mp4");
    swprintf(logPath, MAX_PATH, L"%ls\\papyrus-%ls.log", logDir, stamp);
    if (!g_cfg.list) g_log = _wfopen(logPath, L"w");

    logf("Papyrus engine " PAPYRUS_VERSION "\n");

    MonEnum me{ g_cfg.monitor, 0, nullptr, g_cfg.list };
    if (g_cfg.list) logf("Monitors:\n");
    EnumDisplayMonitors(nullptr, nullptr, monEnumProc, (LPARAM)&me);
    if (g_cfg.list) { pauseIfAsked(); return 0; }
    g_monitor = g_cfg.monitor < 0 ? MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY) : me.found;
    if (!g_monitor) fatal("Monitor %d does not exist. Run with --list to see the monitors.", g_cfg.monitor);

    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    if (g_cfg.bench) {
        createDevice();
        g_srcW = g_encW = 3840;
        g_srcH = g_encH = 2160;
        applyAutoTier();
        createVideoProcessor();
        runBench4k();
        if (g_log) { logf("  Log: %ls\n", logPath); fclose(g_log); }
        pauseIfAsked();
        return 0;
    }

    const int64_t tStartup = qpc();
    createDevice();
    startCapture();
    applyAutoTier();
    createVideoProcessor();
    initEncoder();
    const double startupMs = qpcMs(qpc() - tStartup);

    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const bool hiRes = g_timer != nullptr;
    if (!g_timer) { timeBeginPeriod(1); g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr); }

    if (g_cfg.replay) {
        // One slab for the whole buffer, allocated once: (clip + 2 s spare) at the target bitrate plus 15 % headroom.
        g_slabBytes = (uint64_t)g_cfg.bitrateMbps * 1000000ull / 8 * (uint64_t)(g_cfg.clip + 2) * 115 / 100;
        uint8_t* slab = (uint8_t*)VirtualAlloc(nullptr, (SIZE_T)g_slabBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!slab) fatal("Could not allocate the %llu MB replay buffer. Choose a shorter --clip.", (unsigned long long)(g_slabBytes >> 20));
        const uint64_t maxPkts = (uint64_t)(g_cfg.clip + 6) * (uint64_t)g_cfg.fps;
        if (!g_replay.init(slab, g_slabBytes, maxPkts, (int64_t)(g_cfg.clip + 2) * g_cfg.fps)) fatal("Could not allocate the replay index");
        if (!g_mp4.init(maxPkts, g_encW, g_encH, (uint32_t)g_cfg.fps)) fatal("Could not allocate the MP4 index tables");
        g_scratch = (uint8_t*)VirtualAlloc(nullptr, SCRATCH_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        InitializeCriticalSectionAndSpinCount(&g_replayLock, 2000);
        g_evSave = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    } else if (!g_cfg.discard) {
        g_file = CreateFileW(g_cfg.out, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (g_file == INVALID_HANDLE_VALUE) fatal("Could not create the output file %ls (Windows error %lu)", g_cfg.out, GetLastError());
        FILE_IO_PRIORITY_HINT_INFO hint{ IoPriorityHintLow };
        SetFileInformationByHandle(g_file, FileIoPriorityHintInfo, &hint, sizeof hint);
        g_ring.init(16ull << 20);
        if (!g_cfg.raw) {
            if (!g_mp4.init((uint64_t)g_cfg.seconds * (uint64_t)g_cfg.fps, g_encW, g_encH, (uint32_t)g_cfg.fps))
                fatal("Could not allocate the MP4 index tables");
            uint8_t hdr[64];
            DWORD hdrLen = Mp4Mux::writeFtyp(hdr), wr = 0;
            hdrLen += Mp4Mux::writeMdatHeader(hdr + hdrLen, 0);
            if (!WriteFile(g_file, hdr, hdrLen, &wr, nullptr) || wr != hdrLen) fatal("Could not write the MP4 header");
        }
    }
    const double slabMb = (double)g_slabBytes / 1048576.0;
    const double ramBudget = g_cfg.replay ? 100.0 + slabMb : 150.0;
    initNvml();

    logf("  Graphics card : %ls\n", g_adapterName);
    logf("  NVENC API     : driver %u.%u, app %d.%d, %s mode\n", g_driverNvencVer >> 4, g_driverNvencVer & 0xF,
         NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION, g_async ? "async" : "sync");
    logf("  Capture       : Windows Graphics Capture, monitor %ux%u, border %s, min-interval %s, cursor %s\n",
         g_srcW, g_srcH, g_borderOff ? "off" : "on (not supported here)", g_minIntervalSet ? "set" : "not available",
         g_cfg.cursor ? "on" : "off");
    logf("  Encode        : HEVC 8-bit %ux%u @ %d fps, preset P%d, VBR %d/%d Mbps, 1 s GOP, no B-frames\n",
         g_encW, g_encH, g_cfg.fps, g_cfg.preset, g_cfg.bitrateMbps, g_cfg.maxrateMbps);
    logf("  Timer         : %s\n", hiRes ? "high-resolution" : "1 ms fallback");
    logf("  Startup       : %.0f ms to ready\n", startupMs);
    if (g_cfg.replay) {
        logf("  Replay buffer : last %d s kept in RAM, %.0f MB slab (fills up over the first %d s)\n", g_cfg.clip, slabMb, g_cfg.clip + 2);
        logf("  RAM budget    : 100 MB + %.0f MB buffer = %.0f MB\n", slabMb, ramBudget);
        logf("  Clips go to   : %ls\n", g_outDir);
    }
    else if (g_cfg.discard) logf("  Output        : (discarded - soak test)\n");
    else logf("  Output        : %ls\n", g_cfg.out);
    if (g_hdrOn) logf("  NOTE: HDR is switched on for this monitor. This version records SDR only, so colours may look\n"
                      "        washed out or too bright in the file. HDR recording arrives in Milestone 5.\n");

    bool hotkeyOk = false;
    if (g_cfg.replay) {
        hotkeyOk = RegisterHotKey(nullptr, 1, g_cfg.hkMods | MOD_NOREPEAT, g_cfg.hkVk) != 0;
        if (g_cfg.child) {
            char st[400];
            if (hotkeyOk) snprintf(st, sizeof st, "Recording %ux%u at %d fps. Press %ls to save the last %d seconds (high beep = saved).",
                                   g_encW, g_encH, g_cfg.fps, g_cfg.hkText, g_cfg.clip);
            else snprintf(st, sizeof st, "Recording %ux%u at %d fps, but another program already uses %ls. Use the \"Save clip now\" button instead.",
                          g_encW, g_encH, g_cfg.fps, g_cfg.hkText);
            writeSmallFile(g_statPath, st);
        }
        if (!hotkeyOk)
            logf("\n  WARNING: Windows would not give Papyrus the hotkey %ls - another program already owns it.\n"
                 "           Close that program, or start Papyrus with a different hotkey (see script 1b).\n", g_cfg.hkText);
        logf("\nReplay buffer is running. Press %ls in any game to save the last %d seconds.\n", g_cfg.hkText, g_cfg.clip);
        logf("High beep = clip saved, low beep = save failed. Press Ctrl+C in this window to stop.\n");
        if (g_cfg.autosave) logf("(test mode: one clip will be saved automatically after %d s)\n", g_cfg.autosave);
    } else {
        for (int s = g_cfg.delay; s > 0 && !g_stop.load(); s--) {
            logf("\r  Recording starts in %d s - switch to your game now...   ", s);
            Sleep(1000);
        }
        logf("\n\nRecording %d s. Press Ctrl+C to stop early.\n", g_cfg.seconds);
    }
    logf("  time |  src fps | enc fps | repeat | skip | late | drop | latency ms avg/max |   Mbps |  CPU %% | RAM MB | NVENC %% | app GPU: 3D / enc / other%s\n",
         g_cfg.replay ? " | buffer" : "");

    HANDLE hSave = g_cfg.replay ? CreateThread(nullptr, 0, saveThread, nullptr, 0, nullptr) : nullptr;
    HANDLE hWriter = (g_cfg.discard || g_cfg.replay) ? nullptr : CreateThread(nullptr, 0, writerThread, nullptr, 0, nullptr);
    HANDLE hOutput = CreateThread(nullptr, 0, outputThread, nullptr, 0, nullptr);
    initGpuCounters();
    HANDLE hCapture = CreateThread(nullptr, 0, captureThread, nullptr, 0, nullptr);
    Beep(880, 120);

    // Stats loop (this thread does nothing else)
    const int ncpu = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const int64_t runStart = qpc();
    int64_t lastQpc = runStart, lastCpu = processCpu100ns();
    const int64_t cpuStart = lastCpu;
    uint64_t lSkip = 0, lNew = 0, lRep = 0, lDone = 0, lLate = 0, lDrop = 0, lBytes = 0;
    double peakRam = 0, sumNvenc = 0, sum3d = 0, sumVe = 0, peak3d = 0;
    int nNvenc = 0, nGpu = 0, sec = 0;
    bool ramExact = true;
    const int interval = g_cfg.replay ? 10 : 1;          // seconds between table rows
    bool autosaved = false;
    // Worker mode: the control window asks us to stop or save through named events, and we stop if it disappears.
    HANDLE hCtlStop = nullptr, hCtlSave = nullptr, hParent = nullptr;
    if (g_cfg.child) {
        hCtlStop = OpenEventW(SYNCHRONIZE, FALSE, ctl::kStopEvent);
        hCtlSave = OpenEventW(SYNCHRONIZE, FALSE, ctl::kSaveEvent);
        if (g_cfg.parentPid) hParent = OpenProcess(SYNCHRONIZE, FALSE, g_cfg.parentPid);
    }
    // Waits `ms`, handling the save hotkey meanwhile. Returns true when the capture thread has finished.
    auto pump = [&](DWORD ms) {
        const int64_t deadline = qpc() + (int64_t)ms * g_qpf / 1000;
        for (;;) {
            if (g_cfg.autosave && !autosaved && qpcMs(qpc() - runStart) >= g_cfg.autosave * 1000.0) { autosaved = true; SetEvent(g_evSave); }
            const int64_t rem = deadline - qpc();
            if (rem <= 0) return false;
            DWORD wait = (DWORD)(rem * 1000 / g_qpf) + 1;
            if (wait > 250) wait = 250;
            HANDLE hs[4];
            DWORD n = 0;
            hs[n++] = hCapture;
            const DWORD iStop = hCtlStop ? n : 99;   if (hCtlStop) hs[n++] = hCtlStop;
            const DWORD iSave = hCtlSave ? n : 99;   if (hCtlSave) hs[n++] = hCtlSave;
            const DWORD iParent = hParent ? n : 99;  if (hParent) hs[n++] = hParent;
            const DWORD r = MsgWaitForMultipleObjects(n, hs, FALSE, wait, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0) return true;
            if (r == WAIT_OBJECT_0 + iStop) { g_stop.store(true); hCtlStop = nullptr; }
            else if (r == WAIT_OBJECT_0 + iParent) { g_stop.store(true); hParent = nullptr; }
            else if (r == WAIT_OBJECT_0 + iSave) { if (g_evSave) SetEvent(g_evSave); }
            else if (r == WAIT_OBJECT_0 + n) {
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
                    if (m.message == WM_HOTKEY && g_evSave) SetEvent(g_evSave);
            }
        }
    };
    while (!g_captureDone.load()) {
        if (pump((DWORD)interval * 1000)) break;
        sec += interval;
        const int64_t t = qpc(), cpu = processCpu100ns();
        const double wall = (double)(t - lastQpc) / (double)g_qpf;
        const double cpuPct = (double)(cpu - lastCpu) / 1e7 / wall / ncpu * 100.0;
        lastQpc = t; lastCpu = cpu;
        const uint64_t cNew = a_newFrames.load(), cRep = a_repeats.load(), cDone = a_completed.load(),
                       cLate = a_lateTicks.load(), cDrop = a_droppedTicks.load(), cBytes = a_bytes.load();
        const int64_t encSum = a_encTicksSum.exchange(0), encMax = a_encTicksMax.exchange(0);
        const uint64_t dDone = cDone - lDone;
        const double ram = privateWorkingSetMb(&ramExact);
        if (ram > peakRam) peakRam = ram;
        const int nv = nvencUtil();
        if (nv >= 0) { sumNvenc += nv; nNvenc++; }
        double g3 = 0, gve = 0, goth = 0;
        const bool gpuOk = gpuEngineUsage(&g3, &gve, &goth);
        if (gpuOk && sec > interval) { sum3d += g3; sumVe += gve; nGpu++; if (g3 > peak3d) peak3d = g3; }
        char gpuTxt[64];
        if (gpuOk) snprintf(gpuTxt, sizeof gpuTxt, "%4.1f / %4.1f / %4.1f", g3, gve, goth);
        else snprintf(gpuTxt, sizeof gpuTxt, "n/a");
        char nvTxt[16];
        if (nv >= 0) snprintf(nvTxt, sizeof nvTxt, "%3d", nv); else snprintf(nvTxt, sizeof nvTxt, "n/a");
        const uint64_t cSkip = a_srcSkipped.load();
        char bufTxt[48] = "";
        if (g_cfg.replay) {
            EnterCriticalSection(&g_replayLock);
            const uint64_t pk = g_replay.count(), by = g_replay.bytes();
            LeaveCriticalSection(&g_replayLock);
            snprintf(bufTxt, sizeof bufTxt, " | %.0f s, %.0f MB", (double)pk / g_cfg.fps, (double)by / 1048576.0);
        }
        logf(" %4ds | %8.1f | %7.1f | %6llu | %4llu | %4llu | %4llu | %8.2f / %6.2f | %6.1f | %6.2f | %6.1f | %7s | %s%s\n",
             sec, (double)(cNew - lNew) / wall, (double)dDone / wall,
             (unsigned long long)(cRep - lRep), (unsigned long long)(cSkip - lSkip), (unsigned long long)(cLate - lLate), (unsigned long long)(cDrop - lDrop),
             dDone ? qpcMs(encSum) / (double)dDone : 0.0, qpcMs(encMax),
             (double)(cBytes - lBytes) * 8.0 / 1e6 / wall, cpuPct, ram, nvTxt, gpuTxt, bufTxt);
        lSkip = cSkip; lNew = cNew; lRep = cRep; lDone = cDone; lLate = cLate; lDrop = cDrop; lBytes = cBytes;
    }
    WaitForSingleObject(hCapture, INFINITE);
    const double runSeconds = (double)(qpc() - runStart) / (double)g_qpf;
    const double cpuAvg = (double)(processCpu100ns() - cpuStart) / 1e7 / runSeconds / ncpu * 100.0;

    // Drain the encoder, then flush it
    while (a_completed.load() < a_submitted.load()) Sleep(1);
    g_outputQuit.store(true);
    ReleaseSemaphore(g_semWork, 1, nullptr);
    WaitForSingleObject(hOutput, 5000);
    static NV_ENC_PIC_PARAMS eos;
    memset(&eos, 0, sizeof eos);
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    eos.completionEvent = g_async ? g_evEos : nullptr;
    g_nv.nvEncEncodePicture(g_enc, &eos);
    if (g_async) WaitForSingleObject(g_evEos, 1000);
    if (hWriter) {
        g_writerQuit.store(true);
        SetEvent(g_ring.evData);
        WaitForSingleObject(hWriter, 30000);
        if (!g_cfg.raw) {
            // Finish the MP4: append the index, then fill in the size of the video data box
            const uint64_t dataStart = (uint64_t)Mp4Mux::FTYP_LEN + Mp4Mux::MDAT_HDR_LEN;
            const uint32_t moovLen = g_mp4.buildMoov(g_ps, dataStart);
            DWORD wr = 0;
            LARGE_INTEGER pos;
            if (!moovLen) logf("warning: no frames were written, the .mp4 will not play\n");
            else {
                pos.QuadPart = (LONGLONG)(dataStart + g_mp4.dataBytes);
                SetFilePointerEx(g_file, pos, nullptr, FILE_BEGIN);
                if (!WriteFile(g_file, g_mp4.moov, moovLen, &wr, nullptr) || wr != moovLen) fatal("Could not write the MP4 index");
                uint8_t mh[16];
                Mp4Mux::writeMdatHeader(mh, g_mp4.dataBytes);
                pos.QuadPart = Mp4Mux::FTYP_LEN;
                SetFilePointerEx(g_file, pos, nullptr, FILE_BEGIN);
                WriteFile(g_file, mh, sizeof mh, &wr, nullptr);
            }
            if (g_mp4.overflow) logf("warning: %llu frames did not fit the MP4 index\n", (unsigned long long)g_mp4.overflow);
        }
        CloseHandle(g_file);
    }
    if (hSave) {
        g_saveQuit.store(true);
        SetEvent(g_evSave);
        WaitForSingleObject(hSave, 10000);
        UnregisterHotKey(nullptr, 1);
    }
    Beep(660, 120);

    destroyEncoder();

    // Summary against the budgets
    const bool openEnded = g_cfg.replay && !g_cfg.secondsSet;
    const uint64_t expected = (uint64_t)g_cfg.seconds * (uint64_t)g_cfg.fps;
    const uint64_t encoded = a_completed.load();
    const uint64_t dropped = a_droppedTicks.load();
    const bool stoppedEarly = g_stop.load();
    const double encAvgAll = encoded ? qpcMs(a_encTicksTotal.load()) / (double)encoded : 0.0;
    const double budgetMs = 1000.0 / g_cfg.fps;
    const double mbps = encoded ? (double)a_bytes.load() * 8.0 / 1e6 / ((double)encoded / g_cfg.fps) : 0;
    auto verdict = [](bool ok) { return ok ? "PASS" : "FAIL"; };

    logf("\n================ Papyrus %s result ================\n", g_cfg.replay ? "replay" : "recording");
    if (openEnded) logf("  Frames encoded        : %llu over %.0f s\n", (unsigned long long)encoded, runSeconds);
    else logf("  Frames encoded        : %llu of %llu expected%s   %s\n", (unsigned long long)encoded, (unsigned long long)expected,
              stoppedEarly ? " (stopped early)" : "", stoppedEarly ? "n/a" : verdict(encoded == expected));
    if (g_cfg.replay) logf("  Clips saved           : %llu   failed: %llu   %s\n", (unsigned long long)a_clipsSaved.load(),
                           (unsigned long long)a_clipsFailed.load(), verdict(a_clipsFailed.load() == 0));
    if (g_cfg.replay && !hotkeyOk) logf("  Hotkey                : %ls could NOT be registered   FAIL\n", g_cfg.hkText);
    logf("  Dropped ticks         : %llu   %s\n", (unsigned long long)dropped, verdict(dropped == 0));
    logf("  Late ticks (>half)    : %llu   (worst %.2f ms late)\n", (unsigned long long)a_lateTicks.load(), qpcMs(a_lateMaxTicks.load()));
    logf("  New source frames     : %llu   repeated (screen unchanged): %llu   source frames skipped (game faster than %d fps): %llu\n",
         (unsigned long long)a_newFrames.load(), (unsigned long long)a_repeats.load(), g_cfg.fps, (unsigned long long)a_srcSkipped.load());
    logf("  Keyframes (IDR)       : %llu   (expect about %d)\n", (unsigned long long)a_keyframes.load(),
         (int)((encoded + (uint64_t)g_cfg.fps - 1) / (uint64_t)g_cfg.fps));
    logf("  Capture-to-packet lag : avg %.2f ms, worst %.2f ms   (includes waiting behind the game for GPU time; information only)\n",
         encAvgAll, qpcMs(a_encTicksMaxAll.load()));
    if (nNvenc) {
        const double busy = sumNvenc / nNvenc;
        logf("  Encoder busy          : %.0f %% of the time = about %.2f ms per frame   budget <= 70 %% (%.2f ms)   %s\n",
             busy, busy / 100.0 * budgetMs, budgetMs * 0.7, verdict(busy <= 70.0));
    } else logf("  Encoder busy          : not available (NVML not found)\n");
    logf("  Encoder stalls        : %llu slot waits, %llu disk-buffer waits   %s\n", (unsigned long long)a_slotStalls.load(),
         (unsigned long long)a_ringStalls.load(), verdict(a_slotStalls.load() == 0 && a_ringStalls.load() == 0));
    logf("  Average bitrate       : %.1f Mbps   (%.1f MB)\n", mbps, (double)a_bytes.load() / 1048576.0);
    logf("  CPU average           : %.2f %% of total CPU (%d logical cores)   budget <= 2.00   %s\n", cpuAvg, ncpu, verdict(cpuAvg <= 2.0));
    logf("  RAM peak (%s) : %.1f MB   budget <= %.0f%s   %s\n", ramExact ? "private working set" : "working set       ",
         peakRam, ramBudget, g_cfg.replay ? " (100 + buffer)" : "", verdict(peakRam <= ramBudget));
    if (g_cfg.replay) logf("  Engine RAM without the buffer : about %.1f MB\n", peakRam - (double)(g_replay.byteHead < g_slabBytes ? g_replay.byteHead : g_slabBytes) / 1048576.0);
    if (nGpu) logf("  App GPU 3D usage      : avg %.2f %%, peak %.2f %%   budget <= 3.00 avg   %s\n", sum3d / nGpu, peak3d, verdict(sum3d / nGpu <= 3.0));
    else logf("  App GPU 3D usage      : not available here - read it from Task Manager (see the test list)\n");
    if (nGpu) logf("  App video-encode load : avg %.1f %% (this is the NVENC block doing the work - expected to be high)\n", sumVe / nGpu);
    logf("  Capture path          : Windows Graphics Capture%s\n", g_copyPath ? " + GPU copy fallback" : ", zero-copy");
    if (!g_cfg.discard && !g_cfg.replay) logf("  File                  : %ls\n", g_cfg.out);
    logf("  Log                   : %ls\n", logPath);
    logf("============================================================\n");
    if (g_log) fclose(g_log);
    pauseIfAsked();
    return 0;
}
