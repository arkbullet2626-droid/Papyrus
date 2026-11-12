// Papyrus - audio capture and AAC encoding.
//
// Up to four tracks, each 48 kHz stereo AAC-LC at 160 kbps, each from ONE source only:
//   "Game"        the game process and its child processes      (WASAPI process loopback)
//   "Spotify"     Spotify, if it is running                      (WASAPI process loopback)
//   "Discord"     Discord, if it is running                      (WASAPI process loopback)
//   "Microphone"  the default recording device, if switched on
// Nothing else is recorded: browsers, notification sounds and Papyrus's own beeps never reach a clip.
// Spotify and Discord are picked up when they start and dropped when they exit, without restarting anything.
//
// One thread serves all tracks. Every encoded packet is stamped with the time its sound was captured, on the same
// QueryPerformanceCounter clock the video uses, so a saved clip can line the tracks up exactly.
// Steady state allocates nothing: the Media Foundation samples and buffers are created once per track and reused.
// Any failure here only loses that track - it never stops the video.
#pragma once
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <avrt.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

extern "C" HRESULT WINAPI ActivateAudioInterfaceAsync(LPCWSTR deviceInterfacePath, REFIID riid, PROPVARIANT* activationParams,
                                                       IActivateAudioInterfaceCompletionHandler* completionHandler,
                                                       IActivateAudioInterfaceAsyncOperation** activationOperation);

namespace aud {

enum { RATE = 48000, CH = 2, FRAME = 1024, MAX_TRACKS = 4, BLOCK_BYTES = FRAME * CH * 2 };
enum { SLOT_GAME = 0, SLOT_SPOTIFY = 1, SLOT_DISCORD = 2, SLOT_MIC = 3 };
static const char* const kLabels[MAX_TRACKS] = { "Game", "Spotify", "Discord", "Microphone" };
static const int64_t kFrameTime = (int64_t)FRAME * 10000000 / RATE;        // one AAC frame in 100 ns units

// Called on the audio thread for every encoded AAC frame. t = capture time of the frame's first sample (100 ns, QPC clock).
typedef void (*PacketFn)(int track, const uint8_t* data, uint32_t size, int64_t t);
typedef void (*LogFn)(const char* fmt, ...);

struct Config {
    DWORD    gamePid = 0;        // 0 = no game track
    bool     mic = false;
    PacketFn onPacket = nullptr;
    LogFn    log = nullptr;
    HANDLE   pauseEvent = nullptr;   // while set, captured sound is thrown away
    int64_t  qpcFreq = 1;
};

// Process-loopback activation parameters (audioclientactivationparams.h is not in every toolchain).
struct ProcessLoopbackParams { DWORD targetProcessId; int mode; };        // mode 0 = include process tree, 1 = exclude it
struct ActivationParams { int activationType; ProcessLoopbackParams loopback; };   // activationType 1 = process loopback

struct ActivateHandler : IActivateAudioInterfaceCompletionHandler, IAgileObject {
    LONG   refs = 1;
    HANDLE done = nullptr;
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (iid == __uuidof(IAgileObject)) *out = static_cast<IAgileObject*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() override { return (ULONG)InterlockedDecrement(&refs); }     // lives on the stack
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override { SetEvent(done); return S_OK; }
};

struct Track {
    bool                 active = false;
    bool                 failed = false;     // the source went away: close it at the next check
    HANDLE               proc = nullptr;     // the app this track follows (null for the microphone)
    char                 label[16] = "";
    IAudioClient*        client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    HANDLE               ev = nullptr;
    IMFTransform*        enc = nullptr;
    IMFSample*           in[4] = {};
    IMFMediaBuffer*      inBuf[4] = {};
    IMFSample*           out = nullptr;
    IMFMediaBuffer*      outBuf = nullptr;
    uint32_t             inIdx = 0;
    int16_t              blk[FRAME * CH];
    uint32_t             fill = 0;           // frames in blk
    int64_t              blkT = 0;           // capture time of blk's first frame
    int64_t              prevEnd = 0;        // capture time just after the last frame we have (0 = not anchored)
    int64_t              times[16];          // capture times of blocks the encoder has not answered yet
    uint32_t             tHead = 0, tTail = 0;
    uint64_t             blocks = 0, packets = 0;
};

static Config  g_cfg;
static Track   g_tracks[MAX_TRACKS];
static HANDLE  g_thread = nullptr, g_stopEv = nullptr, g_readyEv = nullptr;

static inline int64_t now100() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (int64_t)((double)t.QuadPart * 1e7 / (double)g_cfg.qpcFreq);
}

static const WAVEFORMATEX kFormat = { WAVE_FORMAT_PCM, CH, RATE, RATE * CH * 2, CH * 2, 16, 0 };

// --- AAC encoder (Media Foundation transform) -----------------------------------------------------------------
static bool createEncoder(Track& t) {
    if (FAILED(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFTransform), (void**)&t.enc))) return false;
    IMFMediaType* it = nullptr;
    IMFMediaType* ot = nullptr;
    bool ok = SUCCEEDED(MFCreateMediaType(&it)) && SUCCEEDED(MFCreateMediaType(&ot));
    if (ok) {
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        it->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        it->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        it->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RATE);
        it->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, CH);
        it->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, CH * 2);
        it->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, RATE * CH * 2);
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        ot->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        ot->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        ot->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RATE);
        ot->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, CH);
        ot->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 20000);          // 160 kbps
        ot->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);                        // raw AAC frames, no ADTS header
        ot->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);   // AAC-LC
        ok = SUCCEEDED(t.enc->SetInputType(0, it, 0)) && SUCCEEDED(t.enc->SetOutputType(0, ot, 0));
    }
    if (it) it->Release();
    if (ot) ot->Release();
    if (!ok) return false;

    MFT_OUTPUT_STREAM_INFO osi{};
    t.enc->GetOutputStreamInfo(0, &osi);
    const DWORD outSize = osi.cbSize > 8192 ? osi.cbSize : 8192;
    for (int i = 0; i < 4 && ok; i++)
        ok = SUCCEEDED(MFCreateSample(&t.in[i])) && SUCCEEDED(MFCreateMemoryBuffer(BLOCK_BYTES, &t.inBuf[i])) && SUCCEEDED(t.in[i]->AddBuffer(t.inBuf[i]));
    ok = ok && SUCCEEDED(MFCreateSample(&t.out)) && SUCCEEDED(MFCreateMemoryBuffer(outSize, &t.outBuf)) && SUCCEEDED(t.out->AddBuffer(t.outBuf));
    if (!ok) return false;
    t.enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    t.enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

static void drainEncoder(Track& t, int id) {
    for (int guard = 0; guard < 8; guard++) {
        MFT_OUTPUT_DATA_BUFFER o{};
        o.pSample = t.out;
        DWORD status = 0;
        t.outBuf->SetCurrentLength(0);
        const HRESULT hr = t.enc->ProcessOutput(0, 1, &o, &status);
        if (o.pEvents) o.pEvents->Release();
        if (FAILED(hr)) return;                                          // MF_E_TRANSFORM_NEED_MORE_INPUT is the normal exit
        BYTE* p = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(t.outBuf->Lock(&p, nullptr, &len))) {
            if (len && t.tTail != t.tHead) {
                // The decoder plays a packet's sound one frame later than the block that went in with it.
                const int64_t when = t.times[t.tTail % 16] - kFrameTime;
                t.tTail++;
                t.packets++;
                g_cfg.onPacket(id, p, len, when);
            }
            t.outBuf->Unlock();
        }
    }
}

static void encodeBlock(Track& t, int id) {
    IMFMediaBuffer* b = t.inBuf[t.inIdx % 4];
    IMFSample* s = t.in[t.inIdx % 4];
    t.inIdx++;
    BYTE* p = nullptr;
    if (FAILED(b->Lock(&p, nullptr, nullptr))) return;
    memcpy(p, t.blk, BLOCK_BYTES);
    b->Unlock();
    b->SetCurrentLength(BLOCK_BYTES);
    s->SetSampleTime((LONGLONG)t.blocks * kFrameTime);
    s->SetSampleDuration(kFrameTime);
    t.blocks++;
    if (t.tHead - t.tTail >= 16) t.tTail++;                              // should never happen; keep the queue sane
    t.times[t.tHead++ % 16] = t.blkT;
    HRESULT hr = t.enc->ProcessInput(0, s, 0);
    if (hr == MF_E_NOTACCEPTING) { drainEncoder(t, id); hr = t.enc->ProcessInput(0, s, 0); }
    if (SUCCEEDED(hr)) drainEncoder(t, id);
}

// Adds `frames` of sound captured at time t0 (null = silence).
static void feed(Track& t, int id, const int16_t* src, uint32_t frames, int64_t t0) {
    uint32_t done = 0;
    while (done < frames) {
        if (t.fill == 0) t.blkT = t0 + (int64_t)done * 10000000 / RATE;
        uint32_t n = FRAME - t.fill;
        if (n > frames - done) n = frames - done;
        if (src) memcpy(t.blk + t.fill * CH, src + done * CH, (size_t)n * CH * 2);
        else memset(t.blk + t.fill * CH, 0, (size_t)n * CH * 2);
        t.fill += n;
        done += n;
        if (t.fill == FRAME) { encodeBlock(t, id); t.fill = 0; }
    }
}

// --- WASAPI ----------------------------------------------------------------------------------------------------
static bool finishClient(Track& t, DWORD flags) {
    HRESULT hr = t.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, &kFormat, nullptr);
    if (FAILED(hr)) { if (g_cfg.log) g_cfg.log("  audio: %s: Initialize failed (0x%08lX)\n", t.label, (unsigned long)hr); return false; }
    t.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(t.client->SetEventHandle(t.ev))) return false;
    if (FAILED(t.client->GetService(__uuidof(IAudioCaptureClient), (void**)&t.cap))) return false;
    return createEncoder(t) && SUCCEEDED(t.client->Start());
}

static bool openProcessLoopback(Track& t, DWORD pid, bool includeTree) {
    ActivationParams params{ 1, { pid, includeTree ? 0 : 1 } };
    PROPVARIANT pv;
    memset(&pv, 0, sizeof pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof params;
    pv.blob.pBlobData = (BYTE*)&params;
    ActivateHandler handler;
    handler.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(L"VAD\\Process_Loopback", __uuidof(IAudioClient), &pv, &handler, &op);
    bool ok = false;
    if (SUCCEEDED(hr) && WaitForSingleObject(handler.done, 3000) == WAIT_OBJECT_0) {
        HRESULT actHr = E_FAIL;
        IUnknown* unk = nullptr;
        if (SUCCEEDED(op->GetActivateResult(&actHr, &unk)) && SUCCEEDED(actHr) && unk) {
            ok = SUCCEEDED(unk->QueryInterface(__uuidof(IAudioClient), (void**)&t.client));
            unk->Release();
        } else hr = actHr;
    }
    if (op) op->Release();
    CloseHandle(handler.done);
    if (!ok) { if (g_cfg.log) g_cfg.log("  audio: %s: process loopback not available (0x%08lX)\n", t.label, (unsigned long)hr); return false; }
    return finishClient(t, AUDCLNT_STREAMFLAGS_LOOPBACK);
}

static bool openDevice(Track& t, bool render) {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) &&
              SUCCEEDED(en->GetDefaultAudioEndpoint(render ? eRender : eCapture, eConsole, &dev)) &&
              SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&t.client));
    if (dev) dev->Release();
    if (en) en->Release();
    if (!ok) { if (g_cfg.log) g_cfg.log("  audio: %s: no %s device\n", t.label, render ? "output" : "recording"); return false; }
    return finishClient(t, (render ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0) | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);
}

static void closeTrack(Track& t) {
    if (t.client) t.client->Stop();
    if (t.cap) t.cap->Release();
    if (t.client) t.client->Release();
    if (t.enc) t.enc->Release();
    for (int i = 0; i < 4; i++) { if (t.in[i]) t.in[i]->Release(); if (t.inBuf[i]) t.inBuf[i]->Release(); }
    if (t.out) t.out->Release();
    if (t.outBuf) t.outBuf->Release();
    if (t.ev) CloseHandle(t.ev);
    if (t.proc) CloseHandle(t.proc);
    t = Track{};
}

static int activeCount() {
    int n = 0;
    for (int i = 0; i < MAX_TRACKS; i++) if (g_tracks[i].active) n++;
    return n;
}

// Opens one slot. pid = the app to follow (process loopback of it and its child processes); 0 = the microphone.
static bool openSlot(int slot, DWORD pid) {
    Track& t = g_tracks[slot];
    t = Track{};
    strncpy(t.label, kLabels[slot], sizeof t.label - 1);
    const bool ok = pid ? openProcessLoopback(t, pid, true) : openDevice(t, false);
    if (!ok) { closeTrack(t); return false; }
    if (pid) t.proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    t.active = true;
    if (g_cfg.log) g_cfg.log("  audio: %s track started\n", kLabels[slot]);
    return true;
}

// The first-started process of an app: the one named `exe` whose parent is not also named `exe`.
// Following that one with its child processes covers the whole app.
static DWORD findAppRoot(const wchar_t* const* names, int nNames) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    enum { MAXP = 64 };
    DWORD pids[MAXP], parents[MAXP];
    int n = 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            for (int k = 0; k < nNames; k++)
                if (!_wcsicmp(pe.szExeFile, names[k]) && n < MAXP) { pids[n] = pe.th32ProcessID; parents[n] = pe.th32ParentProcessID; n++; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    for (int i = 0; i < n; i++) {
        bool parentIsSameApp = false;
        for (int j = 0; j < n; j++) if (pids[j] == parents[i]) parentIsSameApp = true;
        if (!parentIsSameApp) return pids[i];
    }
    return 0;
}

// Starts tracks for apps that have appeared and closes tracks whose app has gone.
static void scanApps() {
    static const wchar_t* const spotify[] = { L"Spotify.exe" };
    static const wchar_t* const discord[] = { L"Discord.exe", L"DiscordPTB.exe", L"DiscordCanary.exe" };
    for (int i = 0; i < MAX_TRACKS; i++) {
        Track& t = g_tracks[i];
        if (t.active && (t.failed || (t.proc && WaitForSingleObject(t.proc, 0) == WAIT_OBJECT_0))) {
            if (g_cfg.log) g_cfg.log("  audio: %s track ended\n", kLabels[i]);
            closeTrack(t);
        }
    }
    if (!g_tracks[SLOT_SPOTIFY].active) { const DWORD pid = findAppRoot(spotify, 1); if (pid) openSlot(SLOT_SPOTIFY, pid); }
    if (!g_tracks[SLOT_DISCORD].active) { const DWORD pid = findAppRoot(discord, 3); if (pid) openSlot(SLOT_DISCORD, pid); }
}

static void pump(Track& t, int id, bool paused) {
    for (;;) {
        UINT32 next = 0;
        if (FAILED(t.cap->GetNextPacketSize(&next))) { t.failed = true; return; }
        if (!next) break;
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 pos = 0, qpcPos = 0;
        if (FAILED(t.cap->GetBuffer(&data, &frames, &flags, &pos, &qpcPos))) break;
        const int64_t now = now100();
        const int64_t dur = (int64_t)frames * 10000000 / RATE;
        int64_t bt = (int64_t)qpcPos;
        const bool trusted = bt > 0 && bt <= now + 10000000 && bt >= now - 10000000;     // the device gave a usable timestamp
        if (!trusted) bt = now - dur;
        if (paused) { t.fill = 0; t.prevEnd = 0; }
        else {
            if (t.prevEnd) {
                const int64_t gap = bt - t.prevEnd;
                const int64_t fillFrom = trusted ? 300000 : 2000000;                     // 30 ms, or 200 ms when we are only guessing
                if (gap > fillFrom && gap < 50000000) feed(t, id, nullptr, (uint32_t)(gap * RATE / 10000000), t.prevEnd);   // the device was idle: fill with silence
                else if (gap >= 50000000) t.fill = 0;
                else if (!trusted || gap > -300000) bt = t.prevEnd;                      // normal jitter: keep the stream continuous
            }
            feed(t, id, (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : (const int16_t*)data, frames, bt);
            t.prevEnd = bt + dur;
        }
        t.cap->ReleaseBuffer(frames);
    }
    // Loopback devices deliver nothing while nothing is playing: keep the track's timeline going with silence.
    if (!paused) {
        const int64_t now = now100();
        if (!t.prevEnd) t.prevEnd = now;
        else if (now - t.prevEnd > 1000000) {
            const uint32_t frames = (uint32_t)((now - 500000 - t.prevEnd) * RATE / 10000000);
            feed(t, id, nullptr, frames, t.prevEnd);
            t.prevEnd += (int64_t)frames * 10000000 / RATE;
        }
    }
}

static DWORD WINAPI threadProc(void*) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Audio", &task);
    if (mf) {
        if (g_cfg.gamePid) openSlot(SLOT_GAME, g_cfg.gamePid);
        scanApps();
        if (g_cfg.mic) openSlot(SLOT_MIC, 0);
    }
    SetEvent(g_readyEv);

    ULONGLONG lastScan = GetTickCount64();
    for (;;) {
        HANDLE waits[MAX_TRACKS + 1];
        DWORD n = 0;
        waits[n++] = g_stopEv;
        for (int i = 0; i < MAX_TRACKS; i++) if (g_tracks[i].active) waits[n++] = g_tracks[i].ev;
        const DWORD r = WaitForMultipleObjects(n, waits, FALSE, 20);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
        const bool paused = g_cfg.pauseEvent && WaitForSingleObject(g_cfg.pauseEvent, 0) == WAIT_OBJECT_0;
        for (int i = 0; i < MAX_TRACKS; i++) if (g_tracks[i].active && !g_tracks[i].failed) pump(g_tracks[i], i, paused);
        if (mf && GetTickCount64() - lastScan > 3000) { lastScan = GetTickCount64(); scanApps(); }
    }
    for (int i = 0; i < MAX_TRACKS; i++) closeTrack(g_tracks[i]);
    if (mm) AvRevertMmThreadCharacteristics(mm);
    if (mf) MFShutdown();
    if (com) CoUninitialize();
    return 0;
}

// Starts audio capture. Returns the number of tracks running right now; more can appear later (see scanApps).
static int start(const Config& cfg) {
    g_cfg = cfg;
    g_stopEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_readyEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, threadProc, nullptr, 0, nullptr);
    if (!g_thread) return 0;
    WaitForSingleObject(g_readyEv, 8000);
    return activeCount();
}

static void stop() {
    if (!g_thread) return;
    SetEvent(g_stopEv);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}

// --- Volume change for the clip editor ---------------------------------------------------------------------------
// Decodes AAC frames, scales the samples, encodes them again. Runs once per edited track; allowed to allocate.
DEFINE_GUID(kAacDecoderClsid, 0x32d186a7, 0x218f, 0x4c75, 0x88, 0x76, 0xdd, 0x77, 0x27, 0x3a, 0x89, 0x99);   // Microsoft AAC decoder

static std::vector<std::vector<uint8_t>>* g_regainOut = nullptr;
static void regainCollect(int, const uint8_t* data, uint32_t size, int64_t) { g_regainOut->emplace_back(data, data + size); }

static bool regain(const std::vector<std::vector<uint8_t>>& in, int gainPercent, std::vector<std::vector<uint8_t>>& out, uint32_t* extraDelay) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { if (com) CoUninitialize(); return false; }
    bool ok = false;
    std::vector<int16_t> pcm;
    IMFTransform* dec = nullptr;
    IMFMediaType* it = nullptr;
    IMFMediaType* ot = nullptr;
    if (SUCCEEDED(CoCreateInstance(kAacDecoderClsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFTransform), (void**)&dec)) &&
        SUCCEEDED(MFCreateMediaType(&it)) && SUCCEEDED(MFCreateMediaType(&ot))) {
        // HEAACWAVEINFO tail (payload type raw, AAC-LC) followed by the AudioSpecificConfig for AAC-LC 48 kHz stereo
        static const uint8_t user[14] = { 0, 0, 0x29, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x90 };
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        it->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        it->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RATE);
        it->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, CH);
        it->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        it->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        it->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        it->SetBlob(MF_MT_USER_DATA, user, sizeof user);
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        ot->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        ot->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        ot->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RATE);
        ot->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, CH);
        ot->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, CH * 2);
        ot->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, RATE * CH * 2);
        if (SUCCEEDED(dec->SetInputType(0, it, 0)) && SUCCEEDED(dec->SetOutputType(0, ot, 0))) {
            MFT_OUTPUT_STREAM_INFO osi{};
            dec->GetOutputStreamInfo(0, &osi);
            const bool decAllocates = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            const DWORD outSize = osi.cbSize > 65536 ? osi.cbSize : 65536;
            dec->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            dec->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            auto drain = [&]() {
                for (int guard = 0; guard < 16; guard++) {
                    MFT_OUTPUT_DATA_BUFFER o{};
                    IMFSample* s = nullptr;
                    IMFMediaBuffer* b = nullptr;
                    if (!decAllocates) {
                        if (FAILED(MFCreateSample(&s)) || FAILED(MFCreateMemoryBuffer(outSize, &b))) { if (s) s->Release(); return; }
                        s->AddBuffer(b);
                        o.pSample = s;
                    }
                    DWORD st = 0;
                    const HRESULT hr = dec->ProcessOutput(0, 1, &o, &st);
                    if (o.pEvents) o.pEvents->Release();
                    if (SUCCEEDED(hr) && o.pSample) {
                        IMFMediaBuffer* all = nullptr;
                        if (SUCCEEDED(o.pSample->ConvertToContiguousBuffer(&all))) {
                            BYTE* p = nullptr;
                            DWORD len = 0;
                            if (SUCCEEDED(all->Lock(&p, nullptr, &len))) {
                                const size_t n = len / 2;
                                const size_t at = pcm.size();
                                pcm.resize(at + n);
                                memcpy(pcm.data() + at, p, n * 2);
                                all->Unlock();
                            }
                            all->Release();
                        }
                    }
                    if (decAllocates && o.pSample) o.pSample->Release();
                    if (b) b->Release();
                    if (s) s->Release();
                    if (FAILED(hr)) return;
                }
            };
            LONGLONG ts = 0;
            for (const auto& pkt : in) {
                IMFSample* s = nullptr;
                IMFMediaBuffer* b = nullptr;
                if (FAILED(MFCreateSample(&s)) || FAILED(MFCreateMemoryBuffer((DWORD)pkt.size(), &b))) { if (s) s->Release(); break; }
                BYTE* p = nullptr;
                if (SUCCEEDED(b->Lock(&p, nullptr, nullptr))) { memcpy(p, pkt.data(), pkt.size()); b->Unlock(); }
                b->SetCurrentLength((DWORD)pkt.size());
                s->AddBuffer(b);
                s->SetSampleTime(ts);
                s->SetSampleDuration(kFrameTime);
                ts += kFrameTime;
                HRESULT hr = dec->ProcessInput(0, s, 0);
                if (hr == MF_E_NOTACCEPTING) { drain(); hr = dec->ProcessInput(0, s, 0); }
                b->Release();
                s->Release();
                drain();
            }
            dec->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            drain();
        }
    }
    if (it) it->Release();
    if (ot) ot->Release();
    if (dec) dec->Release();

    const size_t expected = in.size() * FRAME * CH;
    if (!pcm.empty() && pcm.size() <= expected) {
        const size_t missingFrames = (expected - pcm.size()) / CH;             // some decoders drop their own run-in
        for (int16_t& v : pcm) {
            long x = (long)v * gainPercent / 100;
            v = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        }
        Track enc;
        const PacketFn savedFn = g_cfg.onPacket;
        g_cfg.onPacket = regainCollect;
        g_regainOut = &out;
        if (createEncoder(enc)) {
            const uint32_t frames = (uint32_t)(pcm.size() / CH);
            feed(enc, 0, pcm.data(), frames, 0);
            if (enc.fill) feed(enc, 0, nullptr, FRAME - enc.fill, 0);                // pad the last block with silence
            feed(enc, 0, nullptr, FRAME, 0);                                         // one more block pushes the tail out
            enc.enc->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            drainEncoder(enc, 0);
            ok = !out.empty();
            *extraDelay = missingFrames < FRAME ? (uint32_t)(FRAME - missingFrames) : 0;   // the encoder adds one frame of delay
        }
        closeTrack(enc);
        g_cfg.onPacket = savedFn;
        g_regainOut = nullptr;
    }
    MFShutdown();
    if (com) CoUninitialize();
    return ok;
}

} // namespace aud
