#pragma once

#include <objidl.h>

// Factorio's save stream accepts Write and rejects the other data methods.
struct WriteOnlyStream final : IStream {
    BYTE* buffer;
    ULONG capacity;
    ULONG length = 0;
    ULONG references = 1;

    WriteOnlyStream(BYTE* output, ULONG size) : buffer(output), capacity(size) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!InlineIsEqualGUID(iid, __uuidof(IUnknown)) && !InlineIsEqualGUID(iid, __uuidof(ISequentialStream)) &&
                !InlineIsEqualGUID(iid, __uuidof(IStream))) return E_NOINTERFACE;
        *result = static_cast<IStream*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE Write(const void* data, ULONG size, ULONG* written) override {
        if (written) *written = 0;
        if (size > capacity - length) return STG_E_MEDIUMFULL;
        const auto* source = static_cast<const BYTE*>(data);
        for (ULONG i = 0; i < size; ++i) buffer[length + i] = source[i];
        length += size;
        if (written) *written = size;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Read(void*, ULONG, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER, DWORD, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG*, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
};
