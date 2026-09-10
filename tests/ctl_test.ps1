# ProWindows - behaviour tests, driven through the control channel.
#
# tests\run.bat checks the layout arithmetic, which is pure and needs no
# desktop. This checks the other half: that the running manager actually does
# what it is told - that a workspace switch really hides the windows, that a
# layout change really takes, that a bad command is really rejected.
#
# Until prowindowsctl existed there was no way to ask those questions except by
# driving the desktop by hand and looking at it, which is why they went
# unasked. Everything here is a command in and a JSON state out.
#
#   powershell -ExecutionPolicy Bypass -File tests\ctl_test.ps1
#
# It starts its own instance, uses its own throwaway config directory so your
# real settings are never touched, and stops it again. Exit code 0 = all passed.

param(
    [string]$Exe = "$PSScriptRoot\..\build\ProWindows.exe",
    [string]$Ctl = "$PSScriptRoot\..\build\prowindowsctl.exe"
)

$ErrorActionPreference = 'Stop'
$script:Pass = 0
$script:Fail = 0

function Check($name, $got, $want) {
    if ("$got" -eq "$want") {
        Write-Host ("  PASS  {0}" -f $name) -ForegroundColor Green
        $script:Pass++
    } else {
        Write-Host ("  FAIL  {0}`n        got  '{1}'`n        want '{2}'" -f $name, $got, $want) -ForegroundColor Red
        $script:Fail++
    }
}

function Ctl { & $Ctl @args }
function State { & $Ctl get state | ConvertFrom-Json }

foreach ($p in @($Exe, $Ctl)) {
    if (-not (Test-Path $p)) { Write-Host "missing $p - run build.bat first" -ForegroundColor Red; exit 1 }
}

# --- isolate the run -------------------------------------------------------
# --config points the settings folder at a throwaway directory, so the test
# cannot disturb - or be disturbed by - real settings.
#
# Overriding %APPDATA% would NOT do this. The folder is resolved with
# SHGetKnownFolderPath, which reads the user's profile and ignores the
# environment entirely, so that approach silently runs against the real config.
$sandbox = Join-Path ([IO.Path]::GetTempPath()) ("pw-ctl-test-" + [Guid]::NewGuid().ToString("N").Substring(0,8))
New-Item -ItemType Directory -Force -Path $sandbox | Out-Null

$already = Get-Process ProWindows -ErrorAction SilentlyContinue
if ($already) {
    Write-Host "ProWindows is already running - stop it first, only one instance can hold the channel." -ForegroundColor Yellow
    exit 1
}

Write-Host "ProWindows control-channel tests"
Write-Host "  sandbox: $sandbox"
$proc = Start-Process $Exe -ArgumentList @("--tray", "--config", $sandbox) -PassThru
try {
    # Wait for the channel rather than guessing at a sleep.
    $ready = $false
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        & $Ctl get version | Out-Null
        if ($LASTEXITCODE -eq 0) { $ready = $true; break }
    }
    if (-not $ready) { Write-Host "  control channel never came up" -ForegroundColor Red; exit 1 }

    Write-Host "`n-- queries"
    $v = & $Ctl get version | ConvertFrom-Json
    Check "get version reports the app"  $v.name "ProWindows"
    $mon = & $Ctl get monitors | ConvertFrom-Json
    Check "at least one monitor"         (@($mon).Count -ge 1) "True"
    $ws = & $Ctl get workspaces | ConvertFrom-Json
    Check "nine workspaces per monitor"  (@($ws).Count) (9 * @($mon).Count)
    Check "exactly one active workspace" (@($ws | Where-Object { $_.active }).Count) @($mon).Count

    Write-Host "`n-- workspaces"
    $start = (State).activeWorkspace
    Ctl workspace 2 | Out-Null; Start-Sleep -Milliseconds 700
    $s = State
    Check "switching makes 2 active"     $s.activeWorkspace 2
    Check "windows on 1 are hidden"      (@($s.windows | Where-Object { -not $_.hidden -and $_.workspace -eq 1 }).Count) 0
    Ctl workspace 1 | Out-Null; Start-Sleep -Milliseconds 700
    $s = State
    Check "switching back restores 1"    $s.activeWorkspace 1
    Check "nothing left hidden on 1"     (@($s.windows | Where-Object { $_.hidden -and $_.workspace -eq 1 }).Count) 0
    Ctl workspace $start | Out-Null; Start-Sleep -Milliseconds 400

    Write-Host "`n-- layouts"
    foreach ($pair in @(@("master","Master"), @("grid","Grid"), @("monocle","Monocle"), @("dwindle","Dwindle"))) {
        Ctl layout $pair[0] | Out-Null; Start-Sleep -Milliseconds 400
        Check ("layout " + $pair[0])     (State).activeLayout $pair[1]
    }

    Write-Host "`n-- toggles"
    $before = (State).gaps
    Ctl togglegaps | Out-Null; Start-Sleep -Milliseconds 350
    Check "togglegaps flips it"          (State).gaps (-not [bool]::Parse($before))
    Ctl togglegaps | Out-Null; Start-Sleep -Milliseconds 350
    Check "togglegaps flips it back"     (State).gaps $before

    $before = (State).tiling
    Ctl toggletiling | Out-Null; Start-Sleep -Milliseconds 350
    Check "toggletiling flips it"        (State).tiling (-not [bool]::Parse($before))
    Ctl toggletiling | Out-Null; Start-Sleep -Milliseconds 350
    Check "toggletiling flips it back"   (State).tiling $before

    Write-Host "`n-- errors are errors"
    & $Ctl frobnicate 2>&1 | Out-Null
    Check "unknown command exits 1"      $LASTEXITCODE 1
    & $Ctl get nonsense 2>&1 | Out-Null
    Check "unknown query exits 1"        $LASTEXITCODE 1
    & $Ctl workspace 99 2>&1 | Out-Null
    Check "out-of-range workspace is safe" ((State).activeWorkspace -le 9) "True"

    Write-Host "`n-- the channel survives being hammered"
    for ($i = 0; $i -lt 40; $i++) { Ctl retile | Out-Null }
    & $Ctl get version | Out-Null
    Check "still answering after 40 commands" $LASTEXITCODE 0
}
finally {
    Ctl quit | Out-Null
    Start-Sleep -Seconds 1
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item $sandbox -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ("`n{0} passed, {1} failed" -f $script:Pass, $script:Fail)
exit ($(if ($script:Fail -gt 0) { 1 } else { 0 }))
