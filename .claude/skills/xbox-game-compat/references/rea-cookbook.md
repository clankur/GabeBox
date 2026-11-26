# REA for Steam game images

[REA](https://github.com/morluto/rea) ("reverse engineer anything") is the tool this
repository uses to read a shipped Windows game binary: its imports, its delay imports,
its managed boundaries, and — with a decompiler provider — its call sites. Its output
carries an explicit `limitations` list describing what the evidence does *not*
establish, which is exactly the distinction the compatibility notes have to preserve.

Contents: setup · which command answers which question · keeping output small ·
limits and fallbacks · reporting from REA output.

## Setup

```sh
rea --version || npm install --global rea-agents
rea doctor
rea providers
```

REA needs a recent Node (6.10.1 requires Node 22.23.4+, 24.22.1+, or 26.12.1+) and
refuses to run on anything older with a clear message. Installing it globally gives the
plain `rea` command used throughout this skill and the repository's `--rea` flags.

Registering REA as an MCP server is optional — `rea setup --client claude_code`, then
restart the client — and the CLI is usually the better fit here, because audits run
inside subagents and shell pipelines. A sandboxed agent may be refused permission to
write the client configuration; fall back to the CLI rather than fighting it.

**Providers.** Static PE and managed inspection is built in. Deep analysis —
`function`, `decompile`, `decompile-all`, `xrefs`, `inspect-native-api`,
`search --kind procedures` — binds to Ghidra, Hopper, IDA or Kuna, and without one of
them every such call fails with `code: provider_unavailable` and lists those four
candidates. `rea setup --yes --install-hopper` offers Hopper on macOS; a local Ghidra
install is the free route. Until a provider exists, use LLVM objdump for disassembly
(`xcrun -f llvm-objdump` on macOS) — the repository's scanners already accept an
`--objdump` path, and the published findings pair REA's import slot RVAs with LLVM
disassembly of the call sites.

## Which command answers which question

| Question about the game | Command |
| --- | --- |
| What does this image import, and from which DLL? | `rea inspect-pe-imports <pe>` |
| Does it have delay imports the loader will resolve later? | same command — read `normalized_result.delay_imports` |
| Does a candidate compatibility DLL export everything this image needs? | `rea inspect-native-exports <dll>` and compare with the import list |
| Is this a managed assembly, and what does it call natively? | `rea inspect-managed-native-boundaries <pe>` |
| What managed members and string literals exist? | `rea inspect-managed-members <pe>` |
| Do its P/Invoke declarations match the native image? | `rea verify-managed-native-boundaries` |
| Are there TLS callbacks or load-config behavior before `main`? | `rea inspect-pe-load-config <pe>` |
| Is the image signed, and what does staging have to preserve? | `rea inspect-pe-certificates <pe>` |
| Where does this string or symbol appear? | `rea search <pe> <pattern>` (`--kind procedures` needs a provider) |
| What does this function do with the return value? | `rea function <pe> <rva-or-name>` (provider) |
| Who calls this address? | `rea xrefs <pe> <address>` (provider) |
| What is recoverable from this shipped artifact at all? | `rea audit-release-exposure <path>` |
| How does a packaged Electron or JS game work? | `rea analyze-javascript-application <dir>` |

`rea guidance` searches REA's own installed workflow notes, and `rea --llms` prints a
machine-readable manifest of a command's contract when a flag is unclear.

The repository's scanner wraps the import half of this for a whole install or a whole
library, which is the right entry point for an audit:

```sh
python3 -m tools.steam_compatibility scan --app-id 1145360 --build-id 10929685 \
  --install-dir "$install" --binary x64/EngineWin64s.dll --rea "$(command -v rea)" > audit.json
```

Pass `--rea` so delay imports are included, `--objdump` for images above REA's file-size
limit, `--runtime-dll` once per candidate adapter to compare required imports against
its exports, and `--xbox-import-report` to intersect the audit with what the console
actually resolved. For a single image, `python3 -m build_tools.prepare_steam_core
--inspect-pe` reports its system imports and builds the matching control.

## Keeping output small

REA emits one evidence record per call and it is verbose — a bare import list of a game
engine can be tens of thousands of tokens. Shape it at the source instead of reading it
all:

```sh
rea inspect-pe-imports "$engine" --token-count                     # cost before paying it
rea inspect-pe-imports "$engine" --format csv \
    --filter-output normalized_result.imports --columns name       # DLL names only
rea inspect-pe-imports "$engine" \
    --filter-output 'normalized_result.coverage,normalized_result.imports[0,8].name'
rea search "$engine" D3D11 --token-limit 2000 --token-offset 2000  # page a large result
```

`--format csv|tsv` with `--columns` is the compact form for inventories, `--filter-output`
takes dotted key paths and `[a,b]` row selections, and `--token-limit`/`--token-offset`
page anything still too large. Redirect full records to a file and read what you need:
`rea inspect-pe-imports "$engine" --format json > audit/engine-imports.json`.

Prefer running these inside a subagent whose report is the finding — "`ImmGetIMEFileNameA`
at import slot RVA `0x01f82238`, two call sites, both branch away when it returns zero"
— rather than the record.

## Limits and fallbacks

- **File size.** REA declines images past its limit (`max_file_bytes` appears in the
  echoed parameters). Send those to LLVM objdump; note that the LLVM reader records a
  null delay-import table when it cannot enumerate one, which is not the same as an
  image having no delay imports.
- **Static only.** These commands read the file on disk. They do not establish that an
  import is bound, reachable, or that the console's export behaves. That is what the
  signed controls and the observed launch are for.
- **Addresses.** With Ghidra, a space-qualified address (`ram:0x...`) avoids a
  file-offset fallback; `address-to-file-offset` and `inspect-native-load-image` map
  between address spaces when an RVA from one tool has to be used in another.
- **Signed images.** Audits of a signed source should carry the full-file SHA-256
  forward into the recipe (`--signed-source-sha256`), so staging verifies the original
  including its certificate table before adapting a copy.

## Reporting from REA output

A compatibility note earns trust by saying precisely what was observed. Carry REA's own
hedges through:

> REA 6.10.1 found build `26246811`'s `ImmGetIMEFileNameA` import slot at RVA
> `0x01f82238`. LLVM disassembly confirmed two direct calls at `0x01acf2e9` and
> `0x01acf58e`; both branch away before reading the filename when the call returns zero.
> The Series X import report recorded the ANSI export missing. Static evidence only —
> the adapter still needs a console run.

Always record the REA version, the image SHA-256 that REA reported, and the AppID and
build the image came from. An RVA without a build is unusable on the next pass.
