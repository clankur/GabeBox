# An isolated signed application with no network capability, accounts, or runtime broker.
param([Parameter(Mandatory=$true)] [string] $Control)
$ErrorActionPreference = 'Stop'
$identity = 'XboxPortabilityProbe.SteamPrimaryControl'
$cert = Import-Certificate -FilePath "$Control/SteamPrimaryControl.cer" -CertStoreLocation Cert:\LocalMachine\TrustedPeople
try {
    if (!(Get-AppxPackage -Name Microsoft.VCLibs.140.00 | Where-Object Architecture -eq 'X64')) {
        $kits = "${env:ProgramFiles(x86)}\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\AppX\Retail\x64"
        $framework = Get-ChildItem $kits -Filter 'Microsoft.VCLibs.x64.14.00.appx' -File | Select-Object -First 1
        if (!$framework) { throw 'The Windows runner has no redistributable x64 UWP VCLibs framework.' }
        Add-AppxPackage -Path $framework.FullName
    }
    Add-AppxPackage -Path "$Control/SteamPrimaryControl.appx"
    $family = (Get-AppxPackage -Name $identity).PackageFamilyName
    if (!$family) { throw 'Isolated control was not registered.' }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("2E941141-7F97-4756-BA1D-9DECDE894A3D"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IActivationManager {
  [PreserveSig] int ActivateApplication([MarshalAs(UnmanagedType.LPWStr)] string app,
    [MarshalAs(UnmanagedType.LPWStr)] string args, uint flags, out uint pid);
}
public static class PrimaryActivation {
  public static uint Launch(string id) {
    var cls = Type.GetTypeFromCLSID(new Guid("45BA127D-10A8-46EA-8AB7-56EA9078943C"));
    var manager = (IActivationManager)Activator.CreateInstance(cls);
    uint pid;
    Marshal.ThrowExceptionForHR(manager.ActivateApplication(id, null, 0, out pid));
    return pid;
  }
}
'@
    $processId = [PrimaryActivation]::Launch("$family!App")
    $state = "$env:LOCALAPPDATA/Packages/$family/LocalState"
    foreach ($i in 1..30) {
        if ((Test-Path "$state/big-picture-primary-proof.txt") -and
            (Test-Path "$state/big-picture-primary-startup.txt") -and
            (Select-String -Quiet -Path "$state/big-picture-primary-startup.txt" -SimpleMatch 'stage=account-document-verified hresult=0x00000000')) { break }
        Start-Sleep -Seconds 1
    }
    if (Test-Path "$state/big-picture-primary-startup.txt") {
        Get-Content "$state/big-picture-primary-startup.txt"
        if (!(Select-String -Quiet -Path "$state/big-picture-primary-startup.txt" -SimpleMatch 'stage=account-document-verified hresult=0x00000000')) {
            throw 'The native XAML host did not verify the committed account document.'
        }
    }
    $proof = Get-Content "$state/big-picture-primary-proof.txt"
    if ($proof -notcontains 'account_script=ready') { throw 'Local JavaScript did not prove WinRT access.' }
    Write-Host "Verified registered native activation and the packaged account-free WebView. PID $processId"
} finally {
    Get-AppxPackage -Name $identity | Remove-AppxPackage
    Remove-Item -LiteralPath "Cert:\LocalMachine\TrustedPeople\$($cert.Thumbprint)"
}
