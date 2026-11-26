#include <windows.h>
#include <initguid.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "../../runtime/shared/child-process.h"
#include "../../runtime/win32/wic-stream.h"
#include "write-only-stream.h"

using Microsoft::WRL::ComPtr;

static UINT Failure(UINT stage, HRESULT error) {
    return 0x71000000u | (stage << 16) | (static_cast<UINT>(error) & 0xffffu);
}

static UINT VerifyCodec(IWICImagingFactory* factory, bool png) {
    const UINT stage = png ? 3 : 11;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    HRESULT error = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (SUCCEEDED(error)) error = factory->CreateEncoder(png ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg,
        nullptr, &encoder);
    if (SUCCEEDED(error)) error = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(error)) return Failure(stage, error);

    constexpr UINT rows = 8;
    const UINT columns = png ? 8 : 7;
    const UINT channels = png ? 4 : 3;
    const GUID requested = png ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    GUID format = requested;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> settings;
    error = encoder->CreateNewFrame(&frame, &settings);
    if (SUCCEEDED(error) && !png) {
        if (!settings) return Failure(stage + 1, E_NOINTERFACE);
        wchar_t name[] = L"ImageQuality";
        PROPBAG2 property{};
        property.pstrName = name;
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = 0.75f;
        error = settings->Write(1, &property, &value);
    }
    if (SUCCEEDED(error)) error = frame->Initialize(settings.Get());
    if (SUCCEEDED(error)) error = frame->SetSize(columns, rows);
    if (SUCCEEDED(error)) error = frame->SetPixelFormat(&format);
    if (FAILED(error) || !InlineIsEqualGUID(format, requested))
        return Failure(stage + 1, FAILED(error) ? error : E_FAIL);

    const BYTE color[]{32, 96, 160, 128};
    const UINT rowBytes = columns * channels;
    const UINT stride = (rowBytes + 3) & ~3u;
    BYTE pixels[rows * 8 * 4];
    for (UINT row = 0; row < rows; ++row) {
        BYTE* line = pixels + row * stride;
        for (UINT i = 0; i < stride; ++i) line[i] = i < rowBytes ? color[i % channels] : 0xcd;
    }
    error = frame->WritePixels(rows, stride, stride * rows, pixels);
    if (SUCCEEDED(error)) error = frame->Commit();
    if (SUCCEEDED(error)) error = encoder->Commit();
    if (FAILED(error)) return Failure(stage + 2, error);

    static BYTE outputBytes[32768];
    WriteOnlyStream output(outputBytes, sizeof(outputBytes));
    error = CopyEncodedStream(stream.Get(), &output);
    if (FAILED(error) || !output.length) return Failure(stage + 3, FAILED(error) ? error : STG_E_WRITEFAULT);

    ComPtr<IWICStream> input;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> decoded;
    ComPtr<IWICFormatConverter> converter;
    error = factory->CreateStream(&input);
    if (SUCCEEDED(error)) error = input->InitializeFromMemory(outputBytes, output.length);
    if (SUCCEEDED(error)) error = factory->CreateDecoderFromStream(input.Get(), nullptr,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(error)) error = decoder->GetFrame(0, &decoded);
    if (FAILED(error)) return Failure(stage + 4, error);

    UINT width = 0, height = 0;
    error = decoded->GetSize(&width, &height);
    if (FAILED(error) || width != columns || height != rows)
        return Failure(stage + 5, FAILED(error) ? error : E_FAIL);
    error = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(error)) error = converter->Initialize(decoded.Get(), requested, WICBitmapDitherTypeNone,
        nullptr, 0, WICBitmapPaletteTypeCustom);
    for (UINT i = 0; i < stride * rows; ++i) pixels[i] = 0xec;
    if (SUCCEEDED(error)) error = converter->CopyPixels(nullptr, stride, stride * rows, pixels);
    if (FAILED(error)) return Failure(stage + 6, error);
    const int tolerance = png ? 0 : 8;
    for (UINT row = 0; row < rows; ++row) {
        const BYTE* line = pixels + row * stride;
        for (UINT i = 0; i < rowBytes; ++i) {
            const int difference = line[i] - color[i % channels];
            if (difference < -tolerance || difference > tolerance) return Failure(stage + 7, E_FAIL);
        }
    }
    return PROBE_EXPECTED_CHILD_EXIT;
}

static UINT VerifyCodecs() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return Failure(1, initialized);
    UINT result;
    {
        ComPtr<IWICImagingFactory> factory;
        const HRESULT error = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        result = FAILED(error) ? Failure(2, error) : VerifyCodec(factory.Get(), true);
        if (result == PROBE_EXPECTED_CHILD_EXIT) result = VerifyCodec(factory.Get(), false);
    }
    CoUninitialize();
    return result;
}

extern "C" void WINAPI ChildEntry() {
    ExitProcess(VerifyCodecs());
}
