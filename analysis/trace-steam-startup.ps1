# Run on a disposable Windows VM as an administrator.
param(
    [Parameter(Mandatory)]
    [string]$WorkDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ((Test-Path $WorkDirectory) -or (Get-Process -Name steam, steamwebhelper, steamservice -ErrorAction SilentlyContinue)) {
    throw 'Use a clean Windows VM and a new work directory.'
}

$work = New-Item -ItemType Directory -Path $WorkDirectory
$evidence = New-Item -ItemType Directory -Path (Join-Path $work.FullName 'evidence')
$installation = Join-Path $work.FullName 'client'
$installer = Join-Path $work.FullName 'SteamSetup.exe'
$steam = Join-Path $installation 'steam.exe'
$installerUrl = 'https://cdn.akamai.steamstatic.com/client/installer/SteamSetup.exe'
$traceProvider = 'Microsoft-Windows-Kernel-Process'

function Save-Json($Name, $Value) {
    ConvertTo-Json -Depth 10 -InputObject $Value | Set-Content -Encoding utf8 (Join-Path $evidence.FullName $Name)
}

function Describe-Binary($Path) {
    $file = Get-Item -LiteralPath $Path
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    [ordered]@{
        file = $file.Name
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
        fileVersion = $file.VersionInfo.FileVersion
        signer = $signature.SignerCertificate?.Subject
        signatureStatus = [string]$signature.Status
    }
}

function Steam-ServiceStatus {
    $service = Get-Service -Name 'Steam Client Service' -ErrorAction SilentlyContinue
    if ($service) { return [string]$service.Status }
    return 'Absent'
}

function Wait-ForSteamUI {
    $deadline = [DateTime]::UtcNow.AddSeconds(115)
    do {
        $main = Get-Process -Name steam -ErrorAction SilentlyContinue
        $helpers = @(Get-Process -Name steamwebhelper -ErrorAction SilentlyContinue)
        if ($main -and $helpers.Count -gt 0 -and (Test-Path (Join-Path $installation 'SteamUI.dll'))) {
            # Allow renderer processes and the login window to appear after the first helper.
            Start-Sleep -Seconds 8
            return
        }
        Start-Sleep -Seconds 2
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Steam did not reach a client and webhelper pair before the startup deadline.'
}

function Export-Trace($TraceFile, $Stage, $StartTime) {
    $starts = [System.Collections.Generic.List[object]]::new()
    $exits = [System.Collections.Generic.List[object]]::new()
    $images = [System.Collections.Generic.List[object]]::new()
    $steamPids = [System.Collections.Generic.HashSet[int]]::new()
    $steamImages = '^(SteamSetup|steam|steamwebhelper|steamservice|steamerrorreporter64|steamerrorreporter)\.exe$'
    $records = Get-WinEvent -FilterHashtable @{ Path = $TraceFile; Id = @(1, 2, 5); ProviderName = $traceProvider } -Oldest
    foreach ($record in $records) {
        $fields = @{}
        foreach ($field in ([xml]$record.ToXml()).Event.EventData.Data) {
            $fields[$field.GetAttribute('Name')] = $field.InnerText
        }
        $image = [IO.Path]::GetFileName($fields['ImageName'])
        $processId = [int]$fields['ProcessID']
        $ms = [int64]($record.TimeCreated.ToUniversalTime() - $StartTime).TotalMilliseconds
        switch ($record.Id) {
            1 {
                if ($image -match $steamImages) {
                    $null = $steamPids.Add($processId)
                    $starts.Add([ordered]@{
                        ms = $ms; pid = $processId; parentPid = [int]$fields['ParentProcessID']; image = $image
                    })
                }
            }
            2 {
                if ($steamPids.Contains($processId)) {
                    $exits.Add([ordered]@{ ms = $ms; pid = $processId; exitCode = $fields['ExitCode'] })
                    $null = $steamPids.Remove($processId)
                }
            }
            5 {
                if ($steamPids.Contains($processId)) {
                    $images.Add([ordered]@{ ms = $ms; pid = $processId; image = $image })
                }
            }
        }
    }
    $windowProcesses = @(Get-Process -Name steam, steamwebhelper -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -ExpandProperty ProcessName -Unique)
    Save-Json "$Stage.json" ([ordered]@{
        processStarts = $starts
        processExits = $exits
        imageLoads = $images
        windowsAtEnd = $windowProcesses
        serviceAtEnd = Steam-ServiceStatus
    })
    Write-Host "Captured $($starts.Count) Steam process starts and $($images.Count) Steam image loads in $Stage."
}

function Trace-Stage($Stage, [scriptblock]$Action) {
    $session = "SteamStartup-$Stage-$PID"
    $traceFile = Join-Path $work.FullName "$Stage.etl"
    # Kernel Process keywords 0x10 (process) and 0x40 (image).
    & logman.exe start $session -p $traceProvider 0x50 5 -o $traceFile -ets
    if ($LASTEXITCODE -ne 0) { throw "Cannot start ETW for $Stage." }
    try {
        $startTime = [DateTime]::UtcNow
        & $Action
    } finally {
        & logman.exe stop $session -ets
        if ($LASTEXITCODE -ne 0) { throw "Cannot close ETW for $Stage." }
        Export-Trace $traceFile $Stage $startTime
    }
}

Invoke-WebRequest -Uri $installerUrl -OutFile $installer -TimeoutSec 60
$installerInfo = Describe-Binary $installer
Save-Json 'provenance.json' ([ordered]@{ installerUrl = $installerUrl; installer = $installerInfo })
if ($installerInfo.signatureStatus -ne 'Valid' -or
    $installerInfo.signer -notmatch '(?:^|,\s*)CN=Valve (?:Corp\.|Corporation)(?:,|$)') {
    throw 'Download does not have a valid Valve Authenticode signature.'
}

Trace-Stage 'first-start' {
    $setup = Start-Process -FilePath $installer -ArgumentList "/S /D=$installation" -PassThru
    if (!$setup.WaitForExit(90000) -or $setup.ExitCode -ne 0) { throw 'Steam installation failed.' }
    if (!(Test-Path $steam)) { throw 'The signed installer did not put steam.exe in the requested directory.' }
    if (!(Get-Process -Name steam -ErrorAction SilentlyContinue)) {
        Start-Process -FilePath $steam -WorkingDirectory $installation
    }
    Wait-ForSteamUI
}

$relativePaths = @('steam.exe', 'steamclient64.dll', 'SteamUI.dll', 'bin/cef/cef.win64/steamwebhelper.exe', 'bin/SteamService.exe')
$binaries = foreach ($relativePath in $relativePaths) {
    $path = Join-Path $installation $relativePath
    if (Test-Path $path) { Describe-Binary $path }
}
$manifest = Join-Path $installation 'package/steam_client_win64.manifest'
$version = if (Test-Path $manifest) {
    [regex]::Match((Get-Content -Raw $manifest), '"version"\s+"(\d+)"').Groups[1].Value
}
$os = Get-CimInstance Win32_OperatingSystem
Save-Json 'client-build.json' ([ordered]@{
    manifestVersion = $version
    windows = $os.Caption
    windowsBuild = $os.BuildNumber
    serviceBeforeColdStart = Steam-ServiceStatus
    binaries = @($binaries)
})

Start-Process -FilePath $steam -ArgumentList '-shutdown' -WorkingDirectory $installation
$shutdownDeadline = [DateTime]::UtcNow.AddSeconds(25)
while (Get-Process -Name steam, steamwebhelper -ErrorAction SilentlyContinue) {
    if ([DateTime]::UtcNow -ge $shutdownDeadline) { throw 'Steam did not close before the cold start.' }
    Start-Sleep -Seconds 1
}

Trace-Stage 'cold-start' {
    Start-Process -FilePath $steam -WorkingDirectory $installation
    Wait-ForSteamUI
}
