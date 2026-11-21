# Testing one Steam client function

Valve’s Windows x64 client ships `V_binarytohex` in `vstdlib_s64.dll`. It converts bytes to lowercase hexadecimal and terminates the string with a NUL byte. Valve documents the operation in the Source SDK. This small, deterministic operation let us compare original behavior with an independently written implementation without needing a Steam account or a running client. [Pinned client archives](../valve-release.json) · [Valve declaration](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/public/tier1/strtools.h#L602-L614) · [Valve implementation](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/tier1/strtools.cpp#L1645-L1658)

## Findings

- REA identified the PE32+ x64 export. We verified the extracted DLL SHA-256 as `d1b604121cc8aea3ad49e60a9ed861e8afdee873d4ac3ba509bbec7e5b9f18ab` and the export RVA as `0x198b0` before each test run.
- Unicorn 2.1.4 emulated the original machine code on 268 valid input cases. Its complete output, including the NUL byte, matched Python’s `bytes.hex()` reference. The test bounded the instruction count and permitted instruction execution only in the selected function, verifying that each call returned. Every tested case had enough output space. The invalid-capacity assertion path requires other code and was outside this test.
- A separate C implementation built on Linux matched the reference on the same 268 valid cases. It also passed checks for its own buffer and null-pointer guards. Clang compiled the C source into an x64 Windows COFF object. Device execution remains untested.

This experiment establishes behavior for the tested inputs to one public utility function. It does not measure the feasibility of translating Steam’s service operations, Chromium integration, or whole-client startup.

## Reproduction

Use the Windows `bins_win64` archive pinned in the [release inventory](../valve-release.json). Install Python packages `unicorn==2.1.4` and `pefile` in your test environment. Run these commands in this directory with Clang and a host C compiler available.

```sh
./run-emulation.sh /path/to/the/pinned/bins_win64.zip
CC=cc CLANG=clang ./run-spec-check.sh
```

The emulator checks the DLL hash before using the build-specific code range. It reads the PE offline and uses no native Windows loader. The repository includes the experiment and the independent C function. [REA CLI](https://github.com/morluto/rea/blob/main/docs/getting-started/cli.md)
