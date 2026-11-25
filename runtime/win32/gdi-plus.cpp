#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <wincodec.h>
#include <gdiplus.h>
#include <wrl/client.h>
#include "wic-stream.h"

// The encoder quality path uses floating point, so the compiler references
// _fltused. This adapter links with /nodefaultlib, so no CRT supplies it.
// The probe children define the same symbol for the same reason.
extern "C" int _fltused = 0;

namespace {

using namespace Gdiplus;
using Microsoft::WRL::ComPtr;

struct StoredBitmap {
    UINT width, height, stride, length;
    PixelFormat format;
    LONG busy;
    BitmapData* lock;

    BYTE* Pixels() { return reinterpret_cast<BYTE*>(this + 1); }
};

struct BitmapOperation {
    StoredBitmap* bitmap;
    explicit BitmapOperation(StoredBitmap* image) : bitmap(image) {
        if (InterlockedCompareExchange(&image->busy, 1, 0)) bitmap = nullptr;
    }
    ~BitmapOperation() { if (bitmap) InterlockedExchange(&bitmap->busy, 0); }
};

struct ImagingFactory {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    ~ImagingFactory() {
        factory.Reset();
        if (SUCCEEDED(apartment)) CoUninitialize();
    }
    HRESULT Open() {
        if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) return apartment;
        return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
};

struct Codec {
    CLSID clsid;
    GUID format;
    const GUID* container;
    const GUID* preferredPixels;
    const WCHAR *name, *description, *extensions, *mime;
};

constexpr GUID EncoderId(DWORD id) {
    return {0x557cf400 + id, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
}

constexpr GUID ImageId(DWORD id) {
    return {id, 0x0728, 0x11d3, {0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
}

const Codec codecs[] = {
    {EncoderId(0), ImageId(0xb96b3cab), &GUID_ContainerFormatBmp, nullptr,
        L"BMP", L"BMP", L"*.BMP;*.DIB;*.RLE", L"image/bmp"},
    {EncoderId(1), ImageId(0xb96b3cae), &GUID_ContainerFormatJpeg, &GUID_WICPixelFormat24bppBGR,
        L"JPEG", L"JPEG", L"*.JPG;*.JPEG;*.JPE;*.JFIF", L"image/jpeg"},
    {EncoderId(2), ImageId(0xb96b3cb0), &GUID_ContainerFormatGif, &GUID_WICPixelFormat8bppIndexed,
        L"GIF", L"GIF", L"*.GIF", L"image/gif"},
    {EncoderId(5), ImageId(0xb96b3cb1), &GUID_ContainerFormatTiff, nullptr,
        L"TIFF", L"TIFF", L"*.TIF;*.TIFF", L"image/tiff"},
    {EncoderId(6), ImageId(0xb96b3caf), &GUID_ContainerFormatPng, nullptr,
        L"PNG", L"PNG", L"*.PNG", L"image/png"},
};

constexpr GUID jpegQuality{0x1d5be4b5, 0xfa4a, 0x452d, {0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb}};
constexpr UINT codecTableSize = sizeof(ImageCodecInfo) * ARRAYSIZE(codecs);
static_assert(sizeof(BitmapData) == 32 && sizeof(ImageCodecInfo) == 104);

Status FromHRESULT(HRESULT code) {
    if (SUCCEEDED(code)) return Ok;
    if (code == E_OUTOFMEMORY) return OutOfMemory;
    if (code == E_INVALIDARG || code == E_POINTER) return InvalidParameter;
    if (code == WINCODEC_ERR_UNKNOWNIMAGEFORMAT || code == WINCODEC_ERR_COMPONENTNOTFOUND) return UnknownImageFormat;
    return GenericError;
}

const GUID& PixelType(PixelFormat format) {
    return format == PixelFormat24bppRGB ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
}

Status NewBitmap(UINT width, UINT height, PixelFormat format, StoredBitmap** output) {
    *output = nullptr;
    if (!width || !height || width > INT_MAX || height > INT_MAX) return InvalidParameter;
    if (format != PixelFormat24bppRGB && format != PixelFormat32bppARGB) return NotImplemented;
    const ULONGLONG row = (static_cast<ULONGLONG>(width) * (format == PixelFormat24bppRGB ? 3 : 4) + 3) & ~3ULL;
    if (row > INT_MAX || row * height > UINT_MAX) return OutOfMemory;
    const auto stride = static_cast<UINT>(row);
    const UINT length = stride * height;
    auto bitmap = static_cast<StoredBitmap*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(StoredBitmap) + length));
    if (!bitmap) return OutOfMemory;
    bitmap->width = width;
    bitmap->height = height;
    bitmap->stride = stride;
    bitmap->length = length;
    bitmap->format = format;
    *output = bitmap;
    return Ok;
}

Status FindQuality(const Codec& codec, const EncoderParameters* parameters, float& quality) {
    quality = -1;
    if (!parameters || !parameters->Count) return Ok;
    if (codec.container != &GUID_ContainerFormatJpeg || parameters->Count != 1) return NotImplemented;
    const auto& param = parameters->Parameter[0];
    if (!InlineIsEqualGUID(param.Guid, jpegQuality) || param.Type != EncoderParameterValueTypeLong ||
            param.NumberOfValues != 1 || !param.Value) return InvalidParameter;
    const auto percent = *static_cast<const ULONG*>(param.Value);
    quality = percent > 100 ? 1.0f : static_cast<float>(percent) / 100.0f;
    return Ok;
}

HRESULT Encode(IWICImagingFactory* factory, StoredBitmap& bitmap, const Codec& codec, float quality, IStream* output) {
    ComPtr<IStream> scratch;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    ComPtr<IWICBitmap> source;
    HRESULT error = CreateStreamOnHGlobal(nullptr, TRUE, &scratch);
    if (SUCCEEDED(error)) error = factory->CreateEncoder(*codec.container, nullptr, &encoder);
    if (SUCCEEDED(error)) error = encoder->Initialize(scratch.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(error)) error = encoder->CreateNewFrame(&frame, &properties);
    if (SUCCEEDED(error) && quality >= 0) {
        if (!properties) return E_NOINTERFACE;
        WCHAR name[] = L"ImageQuality";
        PROPBAG2 property{};
        property.pstrName = name;
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = quality;
        error = properties->Write(1, &property, &value);
    }
    if (SUCCEEDED(error)) error = frame->Initialize(properties.Get());
    if (SUCCEEDED(error)) error = frame->SetSize(bitmap.width, bitmap.height);
    const GUID& current = PixelType(bitmap.format);
    GUID desired = codec.preferredPixels ? *codec.preferredPixels : current;
    if (SUCCEEDED(error)) error = frame->SetPixelFormat(&desired);
    if (FAILED(error)) return error;

    if (InlineIsEqualGUID(desired, current)) {
        error = frame->WritePixels(bitmap.height, bitmap.stride, bitmap.length, bitmap.Pixels());
    } else {
        error = factory->CreateBitmapFromMemory(bitmap.width, bitmap.height, current, bitmap.stride,
            bitmap.length, bitmap.Pixels(), &source);
        ComPtr<IWICFormatConverter> converter;
        ComPtr<IWICPalette> palette;
        if (SUCCEEDED(error)) error = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(error) && InlineIsEqualGUID(desired, GUID_WICPixelFormat8bppIndexed)) {
            error = factory->CreatePalette(&palette);
            if (SUCCEEDED(error)) error = palette->InitializeFromBitmap(source.Get(), 256,
                bitmap.format == PixelFormat32bppARGB);
            if (SUCCEEDED(error)) error = frame->SetPalette(palette.Get());
        }
        if (SUCCEEDED(error)) error = converter->Initialize(source.Get(), desired, WICBitmapDitherTypeNone,
            palette.Get(), 0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(error)) error = frame->WriteSource(converter.Get(), nullptr);
    }
    if (SUCCEEDED(error)) error = frame->Commit();
    if (SUCCEEDED(error)) error = encoder->Commit();
    return FAILED(error) ? error : CopyEncodedStream(scratch.Get(), output);
}

} // namespace

using namespace Gdiplus;

extern "C" Status WINAPI XboxGdiplusStartup(ULONG_PTR* token, const GdiplusStartupInput* input,
        GdiplusStartupOutput*) {
    if (!token || !input) return InvalidParameter;
    *token = 0;
    if (input->GdiplusVersion != 1) return UnsupportedGdiplusVersion;
    if (input->SuppressBackgroundThread) return NotImplemented;
    ImagingFactory imaging;
    const Status status = FromHRESULT(imaging.Open());
    if (status != Ok) return status;
    *token = reinterpret_cast<ULONG_PTR>(HeapAlloc(GetProcessHeap(), 0, 1));
    return *token ? Ok : OutOfMemory;
}

extern "C" void WINAPI XboxGdiplusShutdown(ULONG_PTR token) {
    if (token) HeapFree(GetProcessHeap(), 0, reinterpret_cast<void*>(token));
}

extern "C" void* WINAPI XboxGdipAlloc(size_t length) {
    return HeapAlloc(GetProcessHeap(), 0, length);
}

extern "C" void WINAPI XboxGdipFree(void* memory) {
    if (memory) HeapFree(GetProcessHeap(), 0, memory);
}

extern "C" Status WINAPI XboxGdipCreateBitmapFromScan0(INT width, INT height, INT stride,
        PixelFormat format, BYTE* pixels, GpBitmap** output) {
    if (!output) return InvalidParameter;
    *output = nullptr;
    if (width <= 0 || height <= 0) return InvalidParameter;
    if (stride || pixels) return NotImplemented;
    StoredBitmap* bitmap = nullptr;
    const Status status = NewBitmap(width, height, format, &bitmap);
    *output = reinterpret_cast<GpBitmap*>(bitmap);
    return status;
}

extern "C" Status WINAPI XboxGdipCreateBitmapFromStream(IStream* stream, GpBitmap** output) {
    if (!output) return InvalidParameter;
    *output = nullptr;
    if (!stream) return InvalidParameter;
    ImagingFactory imaging;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    HRESULT error = imaging.Open();
    if (SUCCEEDED(error)) error = imaging.factory->CreateDecoderFromStream(stream, nullptr,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(error)) error = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(error)) error = imaging.factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(error)) error = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    UINT width = 0, height = 0;
    if (SUCCEEDED(error)) error = converter->GetSize(&width, &height);
    if (FAILED(error)) return FromHRESULT(error);

    StoredBitmap* bitmap = nullptr;
    const Status status = NewBitmap(width, height, PixelFormat32bppARGB, &bitmap);
    if (status != Ok) return status;
    error = converter->CopyPixels(nullptr, bitmap->stride, bitmap->length, bitmap->Pixels());
    if (FAILED(error)) {
        XboxGdipFree(bitmap);
        return FromHRESULT(error);
    }
    *output = reinterpret_cast<GpBitmap*>(bitmap);
    return Ok;
}

extern "C" Status WINAPI XboxGdipCloneImage(GpImage* image, GpImage** output) {
    if (!output) return InvalidParameter;
    *output = nullptr;
    if (!image) return InvalidParameter;
    auto bitmap = reinterpret_cast<StoredBitmap*>(image);
    const BitmapOperation operation(bitmap);
    if (!operation.bitmap) return ObjectBusy;
    StoredBitmap* copy = nullptr;
    const Status status = NewBitmap(bitmap->width, bitmap->height, bitmap->format, &copy);
    if (status != Ok) return status;
    for (UINT i = 0; i < bitmap->length; ++i) copy->Pixels()[i] = bitmap->Pixels()[i];
    *output = reinterpret_cast<GpImage*>(copy);
    return Ok;
}

extern "C" Status WINAPI XboxGdipDisposeImage(GpImage* image) {
    if (!image) return InvalidParameter;
    auto bitmap = reinterpret_cast<StoredBitmap*>(image);
    if (InterlockedCompareExchange(&bitmap->busy, 1, 0)) return ObjectBusy;
    XboxGdipFree(bitmap);
    return Ok;
}

extern "C" Status WINAPI XboxGdipGetImageWidth(GpImage* image, UINT* width) {
    if (!image || !width) return InvalidParameter;
    *width = reinterpret_cast<const StoredBitmap*>(image)->width;
    return Ok;
}

extern "C" Status WINAPI XboxGdipGetImageHeight(GpImage* image, UINT* height) {
    if (!image || !height) return InvalidParameter;
    *height = reinterpret_cast<const StoredBitmap*>(image)->height;
    return Ok;
}

extern "C" Status WINAPI XboxGdipBitmapLockBits(GpBitmap* image, const GpRect* rect, UINT flags,
        PixelFormat format, BitmapData* data) {
    if (!image || !data || !flags) return InvalidParameter;
    auto bitmap = reinterpret_cast<StoredBitmap*>(image);
    if (format != bitmap->format || (flags & ~(ImageLockModeRead | ImageLockModeWrite))) return NotImplemented;
    if (rect && (rect->X || rect->Y || rect->Width != static_cast<INT>(bitmap->width) ||
            rect->Height != static_cast<INT>(bitmap->height))) return NotImplemented;
    if (InterlockedCompareExchange(&bitmap->busy, 1, 0)) return ObjectBusy;
    data->Width = bitmap->width;
    data->Height = bitmap->height;
    data->Stride = static_cast<INT>(bitmap->stride);
    data->PixelFormat = bitmap->format;
    data->Scan0 = bitmap->Pixels();
    data->Reserved = reinterpret_cast<UINT_PTR>(bitmap);
    bitmap->lock = data;
    return Ok;
}

extern "C" Status WINAPI XboxGdipBitmapUnlockBits(GpBitmap* image, BitmapData* data) {
    if (!image || !data) return InvalidParameter;
    auto bitmap = reinterpret_cast<StoredBitmap*>(image);
    if (bitmap->lock != data || data->Reserved != reinterpret_cast<UINT_PTR>(bitmap)) return InvalidParameter;
    bitmap->lock = nullptr;
    data->Reserved = 0;
    InterlockedExchange(&bitmap->busy, 0);
    return Ok;
}

extern "C" Status WINAPI XboxGdipGetImageEncodersSize(UINT* count, UINT* size) {
    if (!count || !size) return InvalidParameter;
    *count = ARRAYSIZE(codecs);
    *size = codecTableSize;
    return Ok;
}

extern "C" Status WINAPI XboxGdipGetImageEncoders(UINT count, UINT size, ImageCodecInfo* output) {
    if (!output || count != ARRAYSIZE(codecs) || size != codecTableSize) return GenericError;
    for (UINT i = 0; i < count; ++i) {
        const auto& codec = codecs[i];
        output[i] = {codec.clsid, codec.format, const_cast<WCHAR*>(codec.name), nullptr,
            const_cast<WCHAR*>(codec.description), const_cast<WCHAR*>(codec.extensions),
            const_cast<WCHAR*>(codec.mime), ImageCodecFlagsEncoder | ImageCodecFlagsSupportBitmap |
            ImageCodecFlagsBuiltin, 1, 0, 0, nullptr, nullptr};
    }
    return Ok;
}

extern "C" Status WINAPI XboxGdipSaveImageToStream(GpImage* image, IStream* stream, const CLSID* clsid,
        const EncoderParameters* parameters) {
    if (!image || !stream || !clsid) return InvalidParameter;
    const Codec* codec = nullptr;
    for (const auto& item : codecs) {
        if (InlineIsEqualGUID(item.clsid, *clsid)) { codec = &item; break; }
    }
    if (!codec) return UnknownImageFormat;
    float quality = -1;
    const auto status = FindQuality(*codec, parameters, quality);
    if (status != Ok) return status;
    auto bitmap = reinterpret_cast<StoredBitmap*>(image);
    const BitmapOperation operation(bitmap);
    if (!operation.bitmap) return ObjectBusy;
    ImagingFactory imaging;
    const HRESULT error = imaging.Open();
    return FromHRESULT(FAILED(error) ? error : Encode(imaging.factory.Get(), *bitmap, *codec, quality, stream));
}
