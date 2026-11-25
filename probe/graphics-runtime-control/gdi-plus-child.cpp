#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <propidl.h>
#include <gdiplus.h>
#include <wrl/client.h>
#include "../../runtime/shared/child-process.h"
#include "write-only-stream.h"

using namespace Gdiplus;
using namespace Gdiplus::DllExports;
using Microsoft::WRL::ComPtr;

static UINT Failure(UINT stage, UINT code = 0) {
    return 0x73000000u | (stage << 16) | (code & 0xffffu);
}

struct ProbeImage {
    GpImage* value = nullptr;
    ~ProbeImage() { if (value) GdipDisposeImage(value); }
};

struct Startup {
    ULONG_PTR token = 0;
    ~Startup() { if (token) GdiplusShutdown(token); }
};

static bool Matches(const WCHAR* left, const WCHAR* right) {
    if (!left) return false;
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

static UINT RoundTrip(const ImageCodecInfo& codec, UINT stage) {
    constexpr UINT width = 7, height = 8;
    const bool png = Matches(codec.MimeType, L"image/png");
    const bool jpeg = Matches(codec.MimeType, L"image/jpeg");
    const UINT channels = png ? 4 : 3;
    const PixelFormat format = png ? PixelFormat32bppARGB : PixelFormat24bppRGB;
    const GpRect full(0, 0, width, height);
    const BYTE expected[]{32, 96, 160, static_cast<BYTE>(png ? 128 : 255)};

    ProbeImage source;
    GpBitmap* bitmap = nullptr;
    Status status = GdipCreateBitmapFromScan0(width, height, 0, format, nullptr, &bitmap);
    if (status != Ok) return Failure(stage, status);
    source.value = bitmap;
    BitmapData write{};
    status = GdipBitmapLockBits(bitmap, &full, ImageLockModeWrite, format, &write);
    if (status != Ok) return Failure(stage + 1, status);
    const UINT pitch = (width * channels + 3) & ~3u;
    if (write.Stride != static_cast<INT>(pitch) || !write.Scan0 || write.Width != width || write.Height != height) {
        GdipBitmapUnlockBits(bitmap, &write);
        return Failure(stage + 2);
    }
    for (UINT row = 0; row < height; ++row) {
        auto* pixels = static_cast<BYTE*>(write.Scan0) + row * pitch;
        for (UINT i = 0; i < pitch; ++i) pixels[i] = i < width * channels ? expected[i % channels] : 0xcd;
    }
    status = GdipBitmapUnlockBits(bitmap, &write);
    if (status != Ok) return Failure(stage + 3, status);

    ProbeImage clone;
    status = GdipCloneImage(source.value, &clone.value);
    if (status != Ok) return Failure(stage + 4, status);
    status = GdipBitmapLockBits(bitmap, &full, ImageLockModeWrite, format, &write);
    if (status != Ok) return Failure(stage + 5, status);
    for (UINT i = 0; i < pitch * height; ++i) static_cast<BYTE*>(write.Scan0)[i] = 0;
    status = GdipBitmapUnlockBits(bitmap, &write);
    if (status != Ok) return Failure(stage + 6, status);

    static BYTE encoded[32768];
    WriteOnlyStream output(encoded, sizeof(encoded));
    ULONG quality = 75;
    const EncoderParameters params{1, {{{0x1d5be4b5, 0xfa4a, 0x452d,
        {0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb}}, 1, EncoderParameterValueTypeLong, &quality}}};
    status = GdipSaveImageToStream(clone.value, &output, &codec.Clsid, jpeg ? &params : nullptr);
    if (status != Ok || !output.length) return Failure(stage + 7, status);

    ComPtr<IStream> input;
    HRESULT result = CreateStreamOnHGlobal(nullptr, TRUE, &input);
    ULONG bytes = 0;
    if (SUCCEEDED(result)) result = input->Write(encoded, output.length, &bytes);
    if (FAILED(result) || bytes != output.length) return Failure(stage + 8, result);
    const LARGE_INTEGER start{};
    result = input->Seek(start, STREAM_SEEK_SET, nullptr);
    if (FAILED(result)) return Failure(stage + 8, result);

    ProbeImage decoded;
    GpBitmap* image = nullptr;
    status = GdipCreateBitmapFromStream(input.Get(), &image);
    if (status != Ok) return Failure(stage + 9, status);
    decoded.value = image;
    input.Reset();
    UINT decodedWidth = 0, decodedHeight = 0;
    status = GdipGetImageWidth(image, &decodedWidth);
    if (status == Ok) status = GdipGetImageHeight(image, &decodedHeight);
    if (status != Ok || decodedWidth != width || decodedHeight != height) return Failure(stage + 10, status);

    BitmapData read{};
    status = GdipBitmapLockBits(image, &full, ImageLockModeRead, PixelFormat32bppARGB, &read);
    if (status != Ok) return Failure(stage + 11, status);
    const int tolerance = jpeg ? 8 : 0;
    UINT pixelResult = PROBE_EXPECTED_CHILD_EXIT;
    if (!read.Scan0 || read.Stride < static_cast<INT>(width * 4)) pixelResult = Failure(stage + 12);
    for (UINT row = 0; row < height && pixelResult == PROBE_EXPECTED_CHILD_EXIT; ++row) {
        const auto* pixels = static_cast<const BYTE*>(read.Scan0) + row * read.Stride;
        for (UINT i = 0; i < width * 4; ++i) {
            const int difference = pixels[i] - expected[i % 4];
            if (difference < -tolerance || difference > tolerance) { pixelResult = Failure(stage + 12); break; }
        }
    }
    status = GdipBitmapUnlockBits(image, &read);
    return status == Ok ? pixelResult : Failure(stage + 13, status);
}

static UINT VerifyImageApi() {
    Startup startup;
    const GdiplusStartupInput input;
    Status status = GdiplusStartup(&startup.token, &input, nullptr);
    if (status != Ok || !startup.token) return Failure(1, status);
    void* wrapper = GdipAlloc(24);
    if (!wrapper) return Failure(2);
    GdipFree(wrapper);

    constexpr const WCHAR* expected[]{L"image/png", L"image/jpeg", L"image/bmp", L"image/gif", L"image/tiff"};
    ImageCodecInfo codecs[ARRAYSIZE(expected)];
    UINT count = 0, size = 0;
    status = GdipGetImageEncodersSize(&count, &size);
    if (status != Ok || count != ARRAYSIZE(codecs) || size != sizeof(codecs)) return Failure(3, status);
    status = GdipGetImageEncoders(count, size, codecs);
    if (status != Ok) return Failure(4, status);
    for (UINT i = 0; i < ARRAYSIZE(expected); ++i) {
        const ImageCodecInfo* selected = nullptr;
        for (const auto& codec : codecs) if (Matches(codec.MimeType, expected[i])) selected = &codec;
        if (!selected) return Failure(5, i);
        const UINT result = RoundTrip(*selected, 16 * (i + 1));
        if (result != PROBE_EXPECTED_CHILD_EXIT) return result;
    }
    return PROBE_EXPECTED_CHILD_EXIT;
}

extern "C" void WINAPI ChildEntry() {
    ExitProcess(VerifyImageApi());
}
