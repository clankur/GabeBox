# Run on a disposable Windows VM as an administrator.
param(
    [Parameter(Mandatory)]
    [string]$WorkDirectory,
    [switch]$PinnedClient,
    [switch]$BigPicture
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ((Test-Path $WorkDirectory) -or (Get-Process -Name steam, steamwebhelper, steamservice -ErrorAction SilentlyContinue)) {
    throw 'Use a clean Windows VM and a new work directory.'
}
if ($BigPicture -and (!$PinnedClient -or (Test-Path 'HKCU:\Software\Valve\Steam'))) {
    throw 'Use -PinnedClient and a fresh Windows user profile for the Big Picture reference.'
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

function Test-StagedPayload {
    if (!(Test-Path -LiteralPath $manifest)) { return $false }
    $text = Get-Content -Raw -LiteralPath $manifest
    if ([regex]::Match($text, '"version"\s+"(\d+)"').Groups[1].Value -ne $release.version) { return $false }
    foreach ($file in $stagedPayload) {
        if (!(Test-Path -PathType Leaf -LiteralPath $file.Path) -or
            (Get-FileHash -LiteralPath $file.Path -Algorithm SHA256).Hash -ne $file.Hash) {
            Write-Host "Staged file changed: $([IO.Path]::GetRelativePath($installation, $file.Path))"
            return $false
        }
    }
    return $true
}

function Trace-BigPictureWindow {
    $result = [ordered]@{
        argument = '-gamepadui'
        launchPid = $null
        steamWindowSeen = $false
        webhelperSeen = $false
        bigPictureWindowPid = $null
        bigPictureWindowOwner = $null
        pinnedManifestAtEnd = $false
        stagedFiles = $stagedPayload.Count
        pinnedPayloadBeforeLaunch = $false
        pinnedPayloadAtEnd = $false
    }
    $helper = Join-Path $installation 'bin\cef\cef.win64\steamwebhelper.exe'
    try {
        $result.pinnedPayloadBeforeLaunch = Test-StagedPayload
        if (!$result.pinnedPayloadBeforeLaunch) {
            throw 'The installed release version or staged client files changed before the Big Picture cold start.'
        }
        $launched = Start-Process -FilePath $steam -ArgumentList $result.argument -WorkingDirectory $installation -PassThru
        $result.launchPid = $launched.Id
        $deadline = [DateTime]::UtcNow.AddSeconds(115)
        do {
            foreach ($process in (Get-Process -Name steam, steamwebhelper -ErrorAction SilentlyContinue)) {
                if ($process.Path -notin @($steam, $helper)) { continue }
                if ($process.Path -eq $helper) { $result.webhelperSeen = $true }
                if ($process.MainWindowHandle -eq 0) { continue }
                $result.steamWindowSeen = $true
                if ($process.MainWindowTitle -eq 'Steam Big Picture Mode') {
                    $result.bigPictureWindowPid = $process.Id
                    $result.bigPictureWindowOwner = $process.ProcessName
                }
            }
            if ($result.webhelperSeen -and $result.bigPictureWindowPid) { break }
            Start-Sleep -Seconds 1
        } while ([DateTime]::UtcNow -lt $deadline)
        if (!$result.webhelperSeen -or !$result.bigPictureWindowPid) {
            throw 'The pinned Steam client did not expose a Big Picture main window before the deadline.'
        }
    } finally {
        if (Test-Path -LiteralPath $manifest) {
            $result.pinnedManifestAtEnd = (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash -eq $release.manifest_sha256
        }
        if ($result.launchPid) { $result.pinnedPayloadAtEnd = Test-StagedPayload }
        Save-Json 'big-picture-window.json' $result
    }
    if (!$result.pinnedPayloadAtEnd) { throw 'The installed release version or staged client files changed during the Big Picture cold start.' }
}

function Save-StartupStatus($Stage) {
    $processes = @(Get-Process -Name SteamSetup, steam, steamwebhelper, steamservice -ErrorAction SilentlyContinue |
        Select-Object Id, ProcessName, SessionId, @{ Name = 'hasWindow'; Expression = { $_.MainWindowHandle -ne 0 } })
    $log = Join-Path $installation 'logs/bootstrap_log.txt'
    $milestones = if (Test-Path $log) {
        $terms = 'version|download complete|installed|failed|error|offline|checking for available updates'
        @(Get-Content $log -Tail 120 | ForEach-Object {
            $match = [regex]::Match($_, "^\[.{0,32}\] ([A-Za-z0-9 .,()_-]{1,180})$")
            if ($match.Success -and $match.Groups[1].Value -match $terms) { $match.Groups[1].Value }
        })
    }
    Save-Json "$Stage-status.json" ([ordered]@{
        processes = $processes
        startupLogMilestones = @($milestones)
        service = Steam-ServiceStatus
    })
}

function Export-Trace($TraceFile, $Stage, $StartTime) {
    $starts = [System.Collections.Generic.List[object]]::new()
    $exits = [System.Collections.Generic.List[object]]::new()
    $images = [System.Collections.Generic.List[object]]::new()
    $steamPids = [System.Collections.Generic.HashSet[int]]::new()
    $steamImages = '^(SteamSetup|steam|steamwebhelper|steamservice|steamerrorreporter64|steamerrorreporter)\.exe$'
    $eventTypes = @{}
    Get-WinEvent -Path $TraceFile -Oldest | ForEach-Object {
        $record = $_
        $key = "$($record.ProviderName)/$($record.Id)"
        $eventTypes[$key] = 1 + [int]$eventTypes[$key]
        if ($record.Id -notin @(1, 2, 5)) { return }
        $fields = @{}
        foreach ($field in ([xml]$record.ToXml()).Event.EventData.Data) {
            $fields[$field.GetAttribute('Name')] = $field.InnerText
        }
        if (!$fields.ContainsKey('ProcessID')) { return }
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
        eventTypes = $eventTypes
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
        Save-StartupStatus $Stage
        Export-Trace $traceFile $Stage $startTime
    }
}

if ($PinnedClient) {
    & python (Join-Path $PSScriptRoot 'stage-pinned-client.py') $installation
    if ($LASTEXITCODE -ne 0) { throw 'Unable to stage the pinned client.' }
    if ($BigPicture) {
        $manifest = Join-Path $installation 'package/steam_client_win64.manifest'
        $release = Get-Content -Raw (Join-Path $PSScriptRoot 'valve-release.json') | ConvertFrom-Json
        if ((Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash -ne $release.manifest_sha256) {
            throw 'Staged Valve manifest does not match the pinned SHA-256.'
        }
        # Steam rewrites its installed manifest on first start. Compare the original
        # payload against the verified staging directory on either side of the run.
        $stagedPayload = @(Get-ChildItem -LiteralPath $installation -File -Recurse -Force |
            Where-Object { $_.FullName -ne $manifest } | Get-FileHash -Algorithm SHA256)
    }
    $clientInfo = Describe-Binary $steam
    Save-Json 'provenance.json' ([ordered]@{ distribution = 'pinned Valve CDN archives'; bootstrap = $clientInfo })
} else {
    Invoke-WebRequest -Uri $installerUrl -OutFile $installer -TimeoutSec 60
    $clientInfo = Describe-Binary $installer
    Save-Json 'provenance.json' ([ordered]@{ installerUrl = $installerUrl; installer = $clientInfo })
}
if ($clientInfo.signatureStatus -ne 'Valid' -or
    $clientInfo.signer -notmatch '(?:^|,\s*)CN=Valve (?:Corp\.|Corporation)(?:,|$)') {
    throw 'Download does not have a valid Valve Authenticode signature.'
}

Trace-Stage 'first-start' {
    if (!$PinnedClient) {
        $setup = Start-Process -FilePath $installer -ArgumentList "/S /D=$installation" -PassThru
        if (!$setup.WaitForExit(90000) -or $setup.ExitCode -ne 0) { throw 'Steam installation failed.' }
        if (!(Test-Path $steam)) { throw 'The signed installer did not put steam.exe in the requested directory.' }
    }
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
    if ($BigPicture) {
        Trace-BigPictureWindow
    } else {
        Start-Process -FilePath $steam -WorkingDirectory $installation
        Wait-ForSteamUI
    }
}
