# Valve Steam Windows bootstrapper: static portability evidence

## Scope and provenance

This is a static inspection of the current Windows bootstrap installer downloaded from Valve's official Steam CDN at `https://cdn.akamai.steamstatic.com/client/installer/SteamSetup.exe`. The executable was kept in `/tmp` and deleted after analysis; the repository contains evidence JSON and hashes, not Valve binaries. The work did not install or run Steam, contact a Steam account, or assess exploits. Reproduce with `./audit-steamsetup.sh`; set `REA_BIN` if REA is not on `PATH`.

Observed file size was 2,380,800 bytes; SHA-256 was `7d3654531c32d941b8cae81c4137fc542172bfa9635f169cb392f245a0a12bcb`.

## REA findings

- `inspect-pe-imports` identified PE32 / x86 (machine `0x014c`), 32-bit, not x64 or ARM64. Coverage was complete for the import table: 7 DLLs, 166 named imports, no delay imports.
- Imported DLLs: `KERNEL32.dll` (66), `USER32.dll` (65), `GDI32.dll` (8), `SHELL32.dll` (6), `ADVAPI32.dll` (13), `COMCTL32.dll` (4), `ole32.dll` (4). Examples include `CreateProcessW`, `CreateFileW`, `MoveFileW`, registry operations (`RegCreateKeyExW`, `RegSetValueExW`), and desktop UI/window APIs (`CreateWindowExW`, `DialogBoxParamW`). These are static imports of the installer, not the installed Steam client's complete dependency list.
- `inspect-pe-certificates` found one PKCS#7 certificate table and structurally decoded a certificate whose subject is `Valve Corp.` (certificate validity dates 2021-10-07 to 2024-10-09). REA explicitly does not validate Authenticode signatures, certificate chains, timestamps, or digest coverage, so this is not a signature-validity conclusion. The official Valve CDN URL is the provenance basis for this sample.
- `inspect-artifact` produced a complete root-only inventory with one PE file. It did not unpack the installer payload, so this experiment does **not** report the installed `steam.exe` architecture/imports or prove the installer's whole file payload is represented by this root PE.

## Portability interpretation

The observed artifact is a 32-bit desktop Windows PE installer that uses Win32 desktop and registry APIs. The [client audit](../README.md) examines five components shipped separately from the bootstrapper.

## Evidence files

- `steamsetup.sha256` — SHA-256 of the exact CDN download.
- `steamsetup-imports.json` — complete static PE import evidence.
- `steamsetup-certificates.json` — certificate table evidence and its verification limitations.
- `steamsetup-artifact.json` — root-only artifact inventory.
- `audit-steamsetup.sh` — reproducible downloader and REA invocation; the binary remains temporary.
