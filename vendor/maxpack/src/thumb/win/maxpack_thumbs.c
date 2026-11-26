// SPDX-License-Identifier: GPL-3.0-or-later
// maxpack-thumbs.dll: the Explorer's thumbnail provider of .p5z files. The Explorer creates its class in a
// process of its own (dllhost), gives it a stream on the file (IInitializeWithStream) and asks for a bitmap
// of a size (IThumbnailProvider). The icon comes from mp_p5z_icon_read, which reads only the header, the
// footer, the index and the icon, within its caps; it is decoded as a PNG with WIC and scaled so its longer
// side is the size asked, never larger than its own. Every failure is "no thumbnail" (E_FAIL), and the
// Explorer shows the normal icon.
// It writes nothing: the window registers it (gui/internal/explorer).
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <thumbcache.h>
#include <wincodec.h>

#include <stddef.h>
#include <stdlib.h>

#include "mp_p5z_icon.h"

// A side of the PNG, or of the thumbnail asked, over this is never decoded (a PS5 icon is 512).
#define PIXELS_MAX 4096u

// {266D6364-17C0-4B3E-BB33-7EEB90C91837}
static const CLSID CLSID_MaxPackThumbs = {0x266D6364, 0x17C0, 0x4B3E, {0xBB, 0x33, 0x7E, 0xEB, 0x90, 0xC9, 0x18, 0x37}};

// Live objects, and references to the class factory and locks on it: while any, the DLL stays loaded.
static LONG objects, locks;

typedef struct {
    IInitializeWithStream init;
    IThumbnailProvider thumb;
    LONG refs;
    IStream *stream;
} provider;

static provider *of_init(IInitializeWithStream *i) { return (provider *)((char *)i - offsetof(provider, init)); }
static provider *of_thumb(IThumbnailProvider *t) { return (provider *)((char *)t - offsetof(provider, thumb)); }

// ---- IUnknown, for both interfaces -------------------------------------------------------------

static HRESULT query(provider *p, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IInitializeWithStream))
        *ppv = &p->init;
    else if (IsEqualIID(riid, &IID_IThumbnailProvider))
        *ppv = &p->thumb;
    else
        return E_NOINTERFACE;
    InterlockedIncrement(&p->refs);
    return S_OK;
}

static ULONG release(provider *p) {
    LONG n = InterlockedDecrement(&p->refs);
    if (n == 0) {
        if (p->stream) IStream_Release(p->stream);
        free(p);
        InterlockedDecrement(&objects);
    }
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE init_query(IInitializeWithStream *This, REFIID riid, void **ppv) {
    return query(of_init(This), riid, ppv);
}
static ULONG STDMETHODCALLTYPE init_addref(IInitializeWithStream *This) {
    return (ULONG)InterlockedIncrement(&of_init(This)->refs);
}
static ULONG STDMETHODCALLTYPE init_release(IInitializeWithStream *This) { return release(of_init(This)); }

static HRESULT STDMETHODCALLTYPE thumb_query(IThumbnailProvider *This, REFIID riid, void **ppv) {
    return query(of_thumb(This), riid, ppv);
}
static ULONG STDMETHODCALLTYPE thumb_addref(IThumbnailProvider *This) {
    return (ULONG)InterlockedIncrement(&of_thumb(This)->refs);
}
static ULONG STDMETHODCALLTYPE thumb_release(IThumbnailProvider *This) { return release(of_thumb(This)); }

// ---- IInitializeWithStream: the stream is kept until the object goes ----------------------------

static HRESULT STDMETHODCALLTYPE init_initialize(IInitializeWithStream *This, IStream *stream, DWORD mode) {
    provider *p = of_init(This);
    (void)mode;
    if (!stream) return E_INVALIDARG;
    if (p->stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    IStream_AddRef(stream);
    p->stream = stream;
    return S_OK;
}

// ---- IThumbnailProvider ------------------------------------------------------------------------

// The reader's source: n bytes at off of the stream, all of them or an error.
static int stream_read_at(void *ctx, void *buf, size_t n, uint64_t off) {
    IStream *s = ctx;
    LARGE_INTEGER at;
    if (off > (uint64_t)INT64_MAX) return -1;
    at.QuadPart = (LONGLONG)off;
    if (FAILED(IStream_Seek(s, at, STREAM_SEEK_SET, NULL))) return -1;
    uint8_t *p = buf;
    while (n) {
        ULONG k = n > (1u << 30) ? (1u << 30) : (ULONG)n, got = 0;
        if (FAILED(IStream_Read(s, p, k, &got)) || got == 0 || got > k) return -1;
        p += got;
        n -= got;
    }
    return 0;
}

// The PNG as a top-down 32-bit DIB of straight (not premultiplied) BGRA, the alpha the Explorer takes with
// WTSAT_ARGB, whose longer side is cx, or the PNG's own when that is smaller: it is never enlarged.
static HRESULT to_bitmap(const uint8_t *png, size_t len, UINT cx, HBITMAP *out) {
    IWICImagingFactory *f = NULL;
    IWICStream *ws = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *pbgra = NULL, *bgra = NULL;
    IWICBitmapScaler *scaler = NULL;
    HBITMAP bmp = NULL;
    void *bits = NULL;
    UINT w = 0, h = 0, ow = cx, oh = cx;
    *out = NULL;
    HRESULT hr = len > MAXDWORD ? E_FAIL
                                : CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                                   &IID_IWICImagingFactory, (void **)&f);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(f, &ws);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromMemory(ws, (BYTE *)png, (DWORD)len);
    // PNG only: no other codec of the system is offered the bytes
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateDecoder(f, &GUID_ContainerFormatPng, NULL, &dec);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_Initialize(dec, (IStream *)ws, WICDecodeMetadataCacheOnDemand);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(dec, 0, &frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameDecode_GetSize(frame, &w, &h);
    if (SUCCEEDED(hr) && (!w || !h || w > PIXELS_MAX || h > PIXELS_MAX)) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        UINT longer = w >= h ? w : h;
        if (cx > longer) ow = oh = cx = longer;   // the Explorer takes a smaller bitmap
        if (w >= h) {
            oh = (UINT)(((UINT64)h * cx + w / 2) / w);
            if (!oh) oh = 1;
        } else {
            ow = (UINT)(((UINT64)w * cx + h / 2) / h);
            if (!ow) ow = 1;
        }
    }
    // premultiplied before scaling, so transparent pixels do not bleed their colour into the edges
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(f, &pbgra);
    if (SUCCEEDED(hr))
        hr = IWICFormatConverter_Initialize(pbgra, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppPBGRA,
                                            WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateBitmapScaler(f, &scaler);
    if (SUCCEEDED(hr))
        hr = IWICBitmapScaler_Initialize(scaler, (IWICBitmapSource *)pbgra, ow, oh, WICBitmapInterpolationModeFant);
    // then straight BGRA, as the Explorer wants it with WTSAT_ARGB
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(f, &bgra);
    if (SUCCEEDED(hr))
        hr = IWICFormatConverter_Initialize(bgra, (IWICBitmapSource *)scaler, &GUID_WICPixelFormat32bppBGRA,
                                            WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = (LONG)ow;
        bi.bmiHeader.biHeight = -(LONG)oh;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!bmp || !bits) hr = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_CopyPixels(bgra, NULL, ow * 4, ow * 4 * oh, bits);
    if (bgra) IWICFormatConverter_Release(bgra);
    if (scaler) IWICBitmapScaler_Release(scaler);
    if (pbgra) IWICFormatConverter_Release(pbgra);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (ws) IWICStream_Release(ws);
    if (f) IWICImagingFactory_Release(f);
    if (FAILED(hr)) {
        if (bmp) DeleteObject(bmp);
        return E_FAIL;
    }
    *out = bmp;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE thumb_get(IThumbnailProvider *This, UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *alpha) {
    provider *p = of_thumb(This);
    if (!phbmp || !alpha) return E_POINTER;
    *phbmp = NULL;
    *alpha = WTSAT_UNKNOWN;
    if (!p->stream) return E_UNEXPECTED;
    if (!cx || cx > PIXELS_MAX) return E_INVALIDARG;
    STATSTG st;
    if (FAILED(IStream_Stat(p->stream, &st, STATFLAG_NONAME))) return E_FAIL;
    mp_icon_src src = {stream_read_at, st.cbSize.QuadPart, p->stream};
    uint8_t *png = NULL;
    size_t len = 0;
    if (mp_p5z_icon_read(&src, &png, &len, NULL, 0) != 0) return E_FAIL;
    HBITMAP bmp = NULL;
    HRESULT hr = to_bitmap(png, len, cx, &bmp);
    free(png);
    if (FAILED(hr)) return E_FAIL;
    *phbmp = bmp;
    *alpha = WTSAT_ARGB;
    return S_OK;
}

static IInitializeWithStreamVtbl init_vtbl = {init_query, init_addref, init_release, init_initialize};
static IThumbnailProviderVtbl thumb_vtbl = {thumb_query, thumb_addref, thumb_release, thumb_get};

// ---- the class factory: one, static -------------------------------------------------------------

static HRESULT STDMETHODCALLTYPE factory_query(IClassFactory *This, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_IClassFactory)) return E_NOINTERFACE;
    *ppv = This;
    IClassFactory_AddRef(This);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE factory_addref(IClassFactory *This) {
    (void)This;
    InterlockedIncrement(&locks);
    return 2;
}
static ULONG STDMETHODCALLTYPE factory_release(IClassFactory *This) {
    (void)This;
    InterlockedDecrement(&locks);
    return 1;
}

static HRESULT STDMETHODCALLTYPE factory_create(IClassFactory *This, IUnknown *outer, REFIID riid, void **ppv) {
    (void)This;
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    provider *p = calloc(1, sizeof *p);
    if (!p) return E_OUTOFMEMORY;
    p->init.lpVtbl = &init_vtbl;
    p->thumb.lpVtbl = &thumb_vtbl;
    p->refs = 1;
    InterlockedIncrement(&objects);
    HRESULT hr = query(p, riid, ppv);
    release(p);
    return hr;
}

static HRESULT STDMETHODCALLTYPE factory_lock(IClassFactory *This, BOOL lock) {
    (void)This;
    if (lock)
        InterlockedIncrement(&locks);
    else
        InterlockedDecrement(&locks);
    return S_OK;
}

static IClassFactoryVtbl factory_vtbl = {factory_query, factory_addref, factory_release, factory_create, factory_lock};
static IClassFactory factory = {&factory_vtbl};

// ---- what the DLL exports ----------------------------------------------------------------------

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!IsEqualCLSID(rclsid, &CLSID_MaxPackThumbs)) return CLASS_E_CLASSNOTAVAILABLE;
    return factory_query(&factory, riid, ppv);
}

STDAPI DllCanUnloadNow(void) { return objects == 0 && locks == 0 ? S_OK : S_FALSE; }
