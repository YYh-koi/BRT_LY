# Usage examples:
# .\tools\avrcp_fast_toggle_test.ps1
# .\tools\avrcp_fast_toggle_test.ps1 -Mode PauseFinal -IntervalMs 250
# .\tools\avrcp_fast_toggle_test.ps1 -Backend KeyEvent -Serial DEVICE_ID
# .\tools\avrcp_fast_toggle_test.ps1 -DryRun

[CmdletBinding()]
param(
    [string]$AdbPath = 'adb',
    [string]$Serial,
    [ValidateSet('PauseFinal', 'PlayFinal', 'Both')]
    [string]$Mode = 'Both',
    [ValidateSet('Media', 'KeyEvent')]
    [string]$Backend = 'Media',
    [ValidateRange(1, 60000)]
    [int]$IntervalMs = 250,
    [ValidateRange(1, 10000)]
    [int]$Cycles = 15,
    [ValidateRange(0, 3600)]
    [int]$StableSeconds = 3,
    [ValidateRange(0, 3600)]
    [int]$TailSeconds = 5,
    [ValidateRange(0, 3600)]
    [int]$BetweenRoundsSeconds = 10,
    [switch]$DryRun,
    [string]$TimelineLog
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$script:AdbExecutable = $null
$script:TimelineLogPath = $null
$script:AsciiEncoding = New-Object System.Text.ASCIIEncoding

function Write-Timeline {
    param([Parameter(Mandatory = $true)][string]$Message)

    if ($null -eq $script:TimelineLogPath) {
        return
    }

    $line = '{0} {1}{2}' -f (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff'), $Message, [Environment]::NewLine
    [System.IO.File]::AppendAllText($script:TimelineLogPath, $line, $script:AsciiEncoding)
}

function Initialize-Timeline {
    if ([string]::IsNullOrWhiteSpace($TimelineLog)) {
        return
    }

    $fullPath = [System.IO.Path]::GetFullPath($TimelineLog)
    $parent = [System.IO.Path]::GetDirectoryName($fullPath)
    if (-not [string]::IsNullOrEmpty($parent) -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    $script:TimelineLogPath = $fullPath
}

function ConvertTo-OutputText {
    param([object[]]$Items)

    $textItems = @($Items | ForEach-Object { [string]$_ })
    return [string]::Join([Environment]::NewLine, $textItems).Trim()
}

function Resolve-AdbExecutable {
    $commands = @(Get-Command -Name $AdbPath -CommandType Application -ErrorAction SilentlyContinue)
    if ($commands.Count -eq 0) {
        throw "ADB executable not found: $AdbPath"
    }

    return $commands[0].Source
}

function Invoke-Adb {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [AllowNull()][string]$StandardInput
    )

    try {
        if ($PSBoundParameters.ContainsKey('StandardInput')) {
            $output = @($StandardInput | & $script:AdbExecutable @Arguments 2>&1)
        } else {
            $output = @(& $script:AdbExecutable @Arguments 2>&1)
        }
        $exitCode = $LASTEXITCODE
    } catch {
        throw "Failed to start adb: $($_.Exception.Message)"
    }

    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = ConvertTo-OutputText -Items $output
    }
}

function Get-SelectedDevice {
    $devicesResult = Invoke-Adb -Arguments @('devices')
    if ($devicesResult.ExitCode -ne 0) {
        throw "adb devices failed with exit code $($devicesResult.ExitCode): $($devicesResult.Output)"
    }

    $rows = @()
    foreach ($line in @($devicesResult.Output -split '\r?\n')) {
        $trimmed = $line.Trim()
        if ([string]::IsNullOrEmpty($trimmed) -or
            $trimmed -eq 'List of devices attached' -or
            $trimmed.StartsWith('*')) {
            continue
        }

        if ($trimmed -match '^(\S+)\s+(.+)$') {
            $rows += [pscustomobject]@{
                Serial = $Matches[1]
                State = $Matches[2].Trim()
            }
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($Serial)) {
        $stateResult = Invoke-Adb -Arguments @('-s', $Serial, 'get-state')
        $stateLines = @($stateResult.Output -split '\r?\n' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $state = if ($stateLines.Count -gt 0) { $stateLines[-1].Trim() } else { 'unknown' }
        if ($stateResult.ExitCode -ne 0 -or $state -ne 'device') {
            $listed = @($rows | Where-Object { $_.Serial -eq $Serial } | Select-Object -First 1)
            if ($listed.Count -gt 0) {
                $state = $listed[0].State
            }
            throw "ADB serial '$Serial' is not ready (state=$state, exit=$($stateResult.ExitCode))."
        }
        return $Serial
    }

    $notReady = @($rows | Where-Object { $_.State -ne 'device' })
    if ($notReady.Count -gt 0) {
        $details = [string]::Join(', ', @($notReady | ForEach-Object { "$($_.Serial)($($_.State))" }))
        throw "ADB device(s) not ready: $details. Authorize the device or restore it online."
    }

    $ready = @($rows | Where-Object { $_.State -eq 'device' })
    if ($ready.Count -eq 0) {
        throw 'No ADB device is in state=device.'
    }
    if ($ready.Count -gt 1) {
        $serials = [string]::Join(', ', @($ready | ForEach-Object { $_.Serial }))
        throw "Multiple ADB devices are ready: $serials. Specify -Serial."
    }

    return $ready[0].Serial
}

function Test-DevicePrerequisites {
    param([Parameter(Mandatory = $true)][string]$DeviceSerial)

    $tool = if ($Backend -eq 'Media') { 'media' } else { 'input' }
    $toolCheck = Invoke-Adb -Arguments @('-s', $DeviceSerial, 'shell', 'sh') -StandardInput "command -v $tool >/dev/null 2>&1"
    if ($toolCheck.ExitCode -ne 0) {
        throw "Required device command '$tool' is unavailable for Backend=$Backend."
    }

    $sleepCheck = Invoke-Adb -Arguments @('-s', $DeviceSerial, 'shell', 'sh') -StandardInput 'sleep 0.01'
    if ($sleepCheck.ExitCode -ne 0) {
        throw 'Device shell does not support fractional sleep (sleep 0.01). Use a shell with fractional sleep support.'
    }
}

function Get-BackendCommands {
    if ($Backend -eq 'Media') {
        return [pscustomobject]@{
            Play = 'media dispatch play'
            Pause = 'media dispatch pause'
        }
    }

    return [pscustomobject]@{
        Play = 'input keyevent 126'
        Pause = 'input keyevent 127'
    }
}

function ConvertTo-IntervalSeconds {
    return ([double]$IntervalMs / 1000.0).ToString('0.###', [System.Globalization.CultureInfo]::InvariantCulture)
}

function New-RemoteRoundScript {
    param([Parameter(Mandatory = $true)][ValidateSet('PauseFinal', 'PlayFinal')][string]$RoundMode)

    $commands = Get-BackendCommands
    $interval = ConvertTo-IntervalSeconds
    $cyclesText = $Cycles.ToString([System.Globalization.CultureInfo]::InvariantCulture)
    $stableText = $StableSeconds.ToString([System.Globalization.CultureInfo]::InvariantCulture)
    $tailText = $TailSeconds.ToString([System.Globalization.CultureInfo]::InvariantCulture)

    if ($RoundMode -eq 'PauseFinal') {
        $template = @'
set -eu
__PAUSE__
sleep __STABLE__
i=0
while [ "$i" -lt __CYCLES__ ]; do
    __PLAY__
    sleep __INTERVAL__
    __PAUSE__
    sleep __INTERVAL__
    i=$((i + 1))
done
__PAUSE__
sleep __TAIL__
'@
    } else {
        $template = @'
set -eu
__PLAY__
sleep __STABLE__
i=0
while [ "$i" -lt __CYCLES__ ]; do
    __PAUSE__
    sleep __INTERVAL__
    __PLAY__
    sleep __INTERVAL__
    i=$((i + 1))
done
__PLAY__
sleep __TAIL__
'@
    }

    $scriptText = $template.Replace('__PLAY__', $commands.Play).
        Replace('__PAUSE__', $commands.Pause).
        Replace('__INTERVAL__', $interval).
        Replace('__CYCLES__', $cyclesText).
        Replace('__STABLE__', $stableText).
        Replace('__TAIL__', $tailText)

    return $scriptText.Trim() + [Environment]::NewLine
}

function Get-RequestedRounds {
    if ($Mode -eq 'PauseFinal' -or $Mode -eq 'Both') {
        [pscustomobject]@{
            Label = 'A'
            Name = 'PauseFinal'
            Initial = 'pause'
            Final = 'pause'
        }
    }
    if ($Mode -eq 'PlayFinal' -or $Mode -eq 'Both') {
        [pscustomobject]@{
            Label = 'B'
            Name = 'PlayFinal'
            Initial = 'play'
            Final = 'play'
        }
    }
}

function Write-RoundPlan {
    param([Parameter(Mandatory = $true)]$Round)

    $fastCommands = 2 * $Cycles
    $estimated = [double]$StableSeconds + [double]$TailSeconds +
        ((2.0 * [double]$Cycles * [double]$IntervalMs) / 1000.0)
    $estimatedText = $estimated.ToString('0.###', [System.Globalization.CultureInfo]::InvariantCulture)
    Write-Host ("Round {0} ({1}) plan: initial={2}, final={3}, fastCommands={4}, intervalMs={5}, estimatedSeconds={6}" -f
        $Round.Label, $Round.Name, $Round.Initial, $Round.Final, $fastCommands, $IntervalMs, $estimatedText)
}

function Invoke-Round {
    param(
        [Parameter(Mandatory = $true)]$Round,
        [Parameter(Mandatory = $true)][string]$DeviceSerial
    )

    Write-RoundPlan -Round $Round
    $remoteScript = New-RemoteRoundScript -RoundMode $Round.Name
    Write-Timeline ("ROUND_START label={0} mode={1}" -f $Round.Label, $Round.Name)

    $result = Invoke-Adb -Arguments @('-s', $DeviceSerial, 'shell', 'sh') -StandardInput $remoteScript
    if ($result.ExitCode -ne 0) {
        throw "Round $($Round.Label) ($($Round.Name)) failed with adb exit code $($result.ExitCode): $($result.Output)"
    }

    Write-Timeline ("ROUND_END label={0} mode={1} exit=0" -f $Round.Label, $Round.Name)
    Write-Host ("Round {0} ({1}) completed." -f $Round.Label, $Round.Name)
}

try {
    Initialize-Timeline
    Write-Timeline ("PARAMS Mode={0} Backend={1} IntervalMs={2} Cycles={3} StableSeconds={4} TailSeconds={5} BetweenRoundsSeconds={6} DryRun={7}" -f
        $Mode, $Backend, $IntervalMs, $Cycles, $StableSeconds, $TailSeconds, $BetweenRoundsSeconds, [bool]$DryRun)

    $rounds = @(Get-RequestedRounds)

    if ($DryRun) {
        foreach ($round in $rounds) {
            Write-RoundPlan -Round $round
            Write-Host ("--- Round {0} ({1}) remote sh ---" -f $round.Label, $round.Name)
            Write-Output (New-RemoteRoundScript -RoundMode $round.Name)
        }
        exit 0
    }

    $script:AdbExecutable = Resolve-AdbExecutable
    $deviceSerial = Get-SelectedDevice
    Write-Host "Using ADB device: $deviceSerial"
    Test-DevicePrerequisites -DeviceSerial $deviceSerial

    for ($roundIndex = 0; $roundIndex -lt $rounds.Count; $roundIndex++) {
        Invoke-Round -Round $rounds[$roundIndex] -DeviceSerial $deviceSerial
        if ($roundIndex -lt ($rounds.Count - 1)) {
            Write-Host "Waiting $BetweenRoundsSeconds second(s) between rounds."
            Start-Sleep -Seconds $BetweenRoundsSeconds
        }
    }

    Write-Host 'All requested rounds completed.'
    exit 0
} catch {
    $message = $_.Exception.Message -replace '[\r\n]+', ' '
    Write-Timeline ("ERROR $message")
    Write-Error $message
    exit 1
}
