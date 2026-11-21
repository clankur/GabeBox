# Run on a disposable Windows VM without a Steam desktop process.
param(
    [Parameter(Mandatory)]
    [string]$WorkDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ((Test-Path $WorkDirectory) -or (Get-Process -Name steam, steamwebhelper -ErrorAction SilentlyContinue)) {
    throw 'Use a clean Windows VM and a new work directory.'
}
$work = New-Item -ItemType Directory -Path $WorkDirectory
$pin = Get-Content (Join-Path $PSScriptRoot 'valve-release.json') -Raw | ConvertFrom-Json
$spec = $pin.archives | Where-Object { $_.members -contains 'steamclient64.dll' }
$url = [uri]::new([uri]$pin.manifest_url, $spec.file)
$download = Join-Path $work.FullName 'bins_win64.zip'
Invoke-WebRequest -Uri $url -OutFile $download -TimeoutSec 60
if ((Get-Item $download).Length -ne $spec.size -or (Get-FileHash $download).Hash -ne $spec.sha256) {
    throw 'The downloaded archive does not match the pinned release.'
}
$archive = [IO.Compression.ZipFile]::OpenRead($download)
try {
    foreach ($file in 'steamclient64.dll', 'tier0_s64.dll', 'vstdlib_s64.dll') {
        [IO.Compression.ZipFileExtensions]::ExtractToFile($archive.GetEntry($file),
            (Join-Path $work.FullName $file))
    }
} finally {
    $archive.Dispose()
}
$core = Join-Path $work.FullName 'steamclient64.dll'
$signature = Get-AuthenticodeSignature -LiteralPath $core
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(?:^|,\s*)CN=Valve (?:Corp\.|Corporation)(?:,|$)') {
    throw 'The pinned client core did not have a trusted Valve signature.'
}

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class SteamCoreControl {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);

    [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    [DllImport("kernel32.dll")]
    private static extern bool FreeLibrary(IntPtr module);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate IntPtr CreateInterface([MarshalAs(UnmanagedType.LPStr)] string version, out int code);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int CreatePipe();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.I1)]
    private delegate bool ReleasePipe(int pipe);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int CreateGlobalUser(ref int pipe);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void ReleaseUser(int pipe, int user);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.I1)]
    private delegate bool LoggedOn(int user, int pipe);

    private static T GetExport<T>(IntPtr module, string name) where T : Delegate {
        var address = GetProcAddress(module, name);
        if (address == IntPtr.Zero) throw new Win32Exception();
        return Marshal.GetDelegateForFunctionPointer<T>(address);
    }

    public static object Check(string path) {
        // Include the adjacent pinned dependencies and the standard loader directories.
        var module = LoadLibraryEx(path, IntPtr.Zero, 0x1100);
        if (module == IntPtr.Zero) throw new Win32Exception();
        try {
            var createInterface = GetExport<CreateInterface>(module, "CreateInterface");
            var client = createInterface("SteamClient023", out int code);
            int? pipe = null;
            bool? released = null;
            object bootstrap = null;
            if (client != IntPtr.Zero && code == 0) {
                var create = GetExport<CreatePipe>(module, "Steam_CreateSteamPipe");
                var release = GetExport<ReleasePipe>(module, "Steam_BReleaseSteamPipe");
                pipe = create();
                if (pipe > 0) released = release(pipe.Value);
                if (pipe == 0) {
                    var createUser = GetExport<CreateGlobalUser>(module, "Steam_CreateGlobalUser");
                    var releaseUser = GetExport<ReleaseUser>(module, "Steam_ReleaseUser");
                    var loggedOn = GetExport<LoggedOn>(module, "Steam_BLoggedOn");
                    int bootstrapPipe = 0;
                    int user = createUser(ref bootstrapPipe);
                    bool? signedIn = null;
                    if (user > 0 && bootstrapPipe > 0) {
                        signedIn = loggedOn(user, bootstrapPipe);
                        releaseUser(bootstrapPipe, user);
                    }
                    bool? closed = bootstrapPipe > 0 ? release(bootstrapPipe) : null;
                    bootstrap = new { user, pipe = bootstrapPipe, signedIn, released = closed };
                }
            }
            return new { interfacePresent = client != IntPtr.Zero, factoryCode = code, pipe, released, bootstrap };
        } finally {
            FreeLibrary(module);
        }
    }
}
'@

[Environment]::CurrentDirectory = $work.FullName
$result = [SteamCoreControl]::Check($core)
$evidence = [ordered]@{
    windowsBuild = (Get-CimInstance Win32_OperatingSystem).BuildNumber
    valveManifest = $pin.version
    coreSha256 = (Get-FileHash -LiteralPath $core).Hash.ToLowerInvariant()
    interfaceVersion = 'SteamClient023'
    desktopProcessBefore = $false
    outcome = $result
}
$evidence | ConvertTo-Json -Depth 5 | Tee-Object -FilePath (Join-Path $work.FullName 'evidence.json')
