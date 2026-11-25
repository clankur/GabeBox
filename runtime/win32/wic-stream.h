#pragma once

#include <objidl.h>

// Encoders can seek while committing. The caller only needs to accept the final bytes.
static HRESULT CopyEncodedStream(IStream* source, IStream* output) {
    const LARGE_INTEGER beginning{};
    HRESULT error = source->Seek(beginning, STREAM_SEEK_SET, nullptr);
    if (FAILED(error)) return error;
    BYTE block[512];
    for (;;) {
        ULONG read = 0;
        error = source->Read(block, sizeof(block), &read);
        if (FAILED(error)) return error;
        if (!read) return S_OK;
        if (read > sizeof(block)) return STG_E_READFAULT;
        ULONG written = 0;
        error = output->Write(block, read, &written);
        if (FAILED(error)) return error;
        if (written != read) return STG_E_WRITEFAULT;
    }
}
