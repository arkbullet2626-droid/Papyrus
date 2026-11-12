// Papyrus - minimal hand-written ABI for the Windows Graphics Capture WinRT interfaces.
// We call WinRT through its raw COM ABI so the engine needs no C++/WinRT, no .NET and no extra runtime.
// Interface IDs and method order follow the Windows SDK (windows.graphics.capture.idl).
#pragma once
#include <windows.h>
#include <inspectable.h>
#include <dxgi.h>

namespace wgc {

struct SizeInt32 { INT32 Width; INT32 Height; };
struct TimeSpan  { INT64 Duration; };          // 100 ns units
struct EventToken { INT64 value; };
typedef unsigned char boolean_t;

constexpr INT32 PixelFormat_B8G8R8A8UIntNormalized = 87;

#define PAPYRUS_GUID(name, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    static const GUID name = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }

PAPYRUS_GUID(IID_IGraphicsCaptureItemInterop,        0x3628e81b, 0x3cac, 0x4c60, 0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56);
PAPYRUS_GUID(IID_IGraphicsCaptureItem,               0x79c3f95b, 0x31f7, 0x4ec2, 0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60);
PAPYRUS_GUID(IID_IDirect3D11CaptureFramePoolStatics2,0x589b103f, 0x6bbc, 0x5df5, 0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5);
PAPYRUS_GUID(IID_IDirect3D11CaptureFramePool,        0x24eb6d22, 0x1975, 0x422e, 0x82, 0xe7, 0x78, 0x0d, 0xbd, 0x8d, 0xdf, 0x24);
PAPYRUS_GUID(IID_IDirect3D11CaptureFrame,            0xfa50c623, 0x38da, 0x4b32, 0xac, 0xf3, 0xfa, 0x97, 0x34, 0xad, 0x80, 0x0e);
PAPYRUS_GUID(IID_IGraphicsCaptureSession,            0x814e42a9, 0xf70f, 0x4ad7, 0x93, 0x9b, 0xfd, 0xdc, 0xc6, 0xeb, 0x88, 0x0d);
PAPYRUS_GUID(IID_IGraphicsCaptureSession2,           0x2c39ae40, 0x7d2e, 0x5044, 0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e);
PAPYRUS_GUID(IID_IGraphicsCaptureSession3,           0xf2cdd966, 0x22ae, 0x5ea1, 0x95, 0x96, 0x3a, 0x28, 0x93, 0x44, 0xc3, 0xbe);
PAPYRUS_GUID(IID_IGraphicsCaptureSession5,           0x67c0ea62, 0x1f85, 0x5061, 0x92, 0x5a, 0x23, 0x9b, 0xe0, 0xac, 0x09, 0xcb);
PAPYRUS_GUID(IID_IDirect3DDevice,                    0xa37624ab, 0x8d5f, 0x4650, 0x9d, 0x3e, 0x9e, 0xae, 0x3d, 0x9b, 0xc6, 0x70);
PAPYRUS_GUID(IID_IDirect3DDxgiInterfaceAccess,       0xa9b3d012, 0x3df2, 0x4ee3, 0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1);
PAPYRUS_GUID(IID_IClosable,                          0x30d5a829, 0x7fa4, 0x4026, 0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e);

struct IGraphicsCaptureItemInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID iid, void** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID iid, void** result) = 0;
};

struct IGraphicsCaptureItem : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Closed(void* handler, EventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Closed(EventToken token) = 0;
};

struct IGraphicsCaptureSession : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0;
};
struct IGraphicsCaptureSession2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(boolean_t* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(boolean_t value) = 0;
};
struct IGraphicsCaptureSession3 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(boolean_t* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(boolean_t value) = 0;
};
struct IGraphicsCaptureSession5 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MinUpdateInterval(TimeSpan* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MinUpdateInterval(TimeSpan value) = 0;
};

struct IDirect3D11CaptureFrame : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Surface(IInspectable** value) = 0;      // IDirect3DSurface
    virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(TimeSpan* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32* value) = 0;
};

struct IDirect3D11CaptureFramePool : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Recreate(IInspectable* device, INT32 pixelFormat, INT32 numberOfBuffers, SizeInt32 size) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(IDirect3D11CaptureFrame** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(void* handler, EventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(EventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(IGraphicsCaptureItem* item, IGraphicsCaptureSession** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(void** value) = 0;
};

struct IDirect3D11CaptureFramePoolStatics2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(IInspectable* device, INT32 pixelFormat, INT32 numberOfBuffers,
                                                         SizeInt32 size, IDirect3D11CaptureFramePool** result) = 0;
};

struct IDirect3DDxgiInterfaceAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** object) = 0;
};

struct IClosable : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

} // namespace wgc
