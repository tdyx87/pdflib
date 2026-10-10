<#
.SYNOPSIS
  Make sure the Visual Studio toolsets the /MT and XP builds need exist.

.DESCRIPTION
  GitHub's windows-* images carry Visual Studio 2022 but neither the v141
  (VS2017) compiler - which both static-CRT profiles pin as
  compiler.version=191 - nor the v141_xp platform toolset with the Windows
  7.1A SDK. Neither ships as a workload, only as an individual component:

    Microsoft.VisualStudio.Component.VC.v141.x86.x64   the v141 compiler
    Microsoft.VisualStudio.Component.WinXP             v141_xp + 7.1A SDK

  This script detects what is missing and, only on a GitHub-*hosted*
  runner, asks the Visual Studio Installer to add it. On a developer
  machine (or a self-hosted runner) that already has the toolset it prints
  what it found and exits without touching anything; if something is
  missing there it fails with the component names instead of silently
  modifying the machine's Visual Studio installation.

.PARAMETER Needs
  v141 - the v141 compiler only (x64 /MT build).
  xp   - the v141_xp toolset, the 7.1A SDK and therefore v141 as well.

.PARAMETER DryRun
  Print what would be installed and stop; never invokes vs_installer.

.PARAMETER ForceMissing
  Testing aid: pretend every component is absent, so the plan/refusal
  paths can be exercised on a machine that already has them.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('v141', 'xp')]
    [string]$Needs,

    [switch]$DryRun,
    [switch]$ForceMissing,

    # How long to wait for the installer to actually land the component.
    [int]$TimeoutMinutes = 25
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found at '$vswhere'" }

$vsPath = @(& $vswhere -latest -products * -property installationPath) | Select-Object -First 1
$vsVersion = @(& $vswhere -latest -products * -property installationVersion) | Select-Object -First 1
if (-not $vsPath) { throw 'vswhere found no Visual Studio installation' }

Write-Host "Visual Studio : $vsPath ($vsVersion)"
Write-Host "needs         : $Needs"

function Test-V141Tools {
    $root = Join-Path $vsPath 'VC\Tools\MSVC'
    if (-not (Test-Path $root)) { return $false }
    return [bool](Get-ChildItem $root -Directory -Filter '14.1*' -ErrorAction SilentlyContinue)
}

function Test-XpToolset {
    $direct = @(
        (Join-Path $vsPath 'Common7\IDE\VC\VCTargets\Platforms\Win32\PlatformToolsets\v141_xp'),
        (Join-Path $vsPath 'MSBuild\Microsoft\VC\v170\Platforms\Win32\PlatformToolsets\v141_xp')
    )
    foreach ($p in $direct) { if (Test-Path $p) { return $true } }
    # VS2022 keeps the platform toolsets under MSBuild\Microsoft\VC\v<NNN>.
    $vcRoot = Join-Path $vsPath 'MSBuild\Microsoft\VC'
    if (Test-Path $vcRoot) {
        foreach ($v in @(Get-ChildItem $vcRoot -Directory -ErrorAction SilentlyContinue)) {
            if (Test-Path (Join-Path $v.FullName 'Platforms\Win32\PlatformToolsets\v141_xp')) { return $true }
        }
    }
    return $false
}

function Test-Win71Sdk {
    return Test-Path (Join-Path ${env:ProgramFiles(x86)} 'Microsoft SDKs\Windows\v7.1A\Lib\kernel32.lib')
}

function Test-Needed {
    if (-not (Test-V141Tools)) { return $false }
    if ($Needs -eq 'xp') {
        if (-not (Test-XpToolset)) { return $false }
        if (-not (Test-Win71Sdk)) { return $false }
    }
    return $true
}

function Show-InstallerLogs {
    <# the installer keeps its real diagnosis in %TEMP%\dd_*.log #>
    $logs = @(Get-ChildItem -Path $env:TEMP -Filter 'dd_*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 2)
    foreach ($l in $logs) {
        Write-Host ''
        Write-Host ("--- tail of " + $l.Name) -ForegroundColor Yellow
        Get-Content -Path $l.FullName -Tail 40 -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host ("    " + $_) }
    }
}

$haveV141 = (Test-V141Tools) -and -not $ForceMissing
$haveXp = (Test-XpToolset) -and -not $ForceMissing
$haveSdk = (Test-Win71Sdk) -and -not $ForceMissing
Write-Host ("v141 compiler : {0}" -f $(if ($haveV141) { 'present' } else { 'MISSING' }))
if ($Needs -eq 'xp') {
    Write-Host ("v141_xp       : {0}" -f $(if ($haveXp) { 'present' } else { 'MISSING' }))
    Write-Host ("Windows 7.1A  : {0}" -f $(if ($haveSdk) { 'present' } else { 'MISSING' }))
}

$missing = @()
if (-not $haveV141) { $missing += 'Microsoft.VisualStudio.Component.VC.v141.x86.x64' }
if ($Needs -eq 'xp' -and (-not $haveXp -or -not $haveSdk)) {
    $missing += 'Microsoft.VisualStudio.Component.WinXP'
}
$missing = @($missing | Select-Object -Unique)

if ($missing.Count -eq 0) {
    Write-Host '[OK] every required toolset is already installed'
    exit 0
}

if (-not $DryRun -and $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw ("missing Visual Studio component(s): " + ($missing -join ', ') +
           "`nInstall them through the Visual Studio Installer (Individual components ->" +
           " 'C++ Windows XP Support for VS 2017 (v141) tools' and" +
           " 'MSVC v141 - VS 2017 C++ x64/x86 build tools'), or run on a runner that has them.")
}

$installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vs_installer.exe'
if (-not (Test-Path $installer)) { throw "vs_installer.exe not found at '$installer'" }

$installerArgs = @('modify', '--installPath', $vsPath, '--quiet', '--norestart', '--nocache', '--wait')
foreach ($c in $missing) { $installerArgs += @('--add', $c) }

Write-Host ''
Write-Host ('==> Visual Studio Installer: ' + ($missing -join ', ')) -ForegroundColor Cyan
Write-Host ('    vs_installer.exe ' + ($installerArgs -join ' ')) -ForegroundColor DarkGray
Write-Host ('    waiting up to {0} minute(s) for the component to appear' -f $TimeoutMinutes) -ForegroundColor DarkGray
if ($DryRun) {
    Write-Host '[dry-run] nothing invoked'
    exit 0
}

# vs_installer.exe only *starts* the install - it hands the work to
# vs_installershell.exe and exits within a second. Polling for the component
# is therefore the only reliable "is it done": an immediate re-check after
# the launcher returns reports failure while the install is still running
# (this cost one red CI run).
$started = Get-Date
$proc = Start-Process -FilePath $installer -ArgumentList $installerArgs -PassThru
$deadline = $started.AddMinutes($TimeoutMinutes)
$installed = $false
$idleTicks = 0
while ($true) {
    if (Test-Needed) { $installed = $true; break }
    $now = Get-Date
    if ($now -ge $deadline) { break }
    $busy = @(Get-Process -Name 'vs_installer', 'vs_installershell' -ErrorAction SilentlyContinue).Count
    # "Nothing is installing and the component still is not there" only means
    # failure once the shell has had time to show up.
    if ($busy -eq 0 -and $now -gt $started.AddSeconds(90)) {
        $idleTicks++
        if ($idleTicks -ge 6) { break }
    }
    else {
        $idleTicks = 0
    }
    Write-Host ('    ... waiting for the installer ({0} active process(es), {1:hh\:mm\:ss} left)' -f `
            $busy, ($deadline - $now))
    Start-Sleep -Seconds 10
}

$proc.Refresh()
$exit = if ($proc.HasExited) { $proc.ExitCode } else { $null }
Write-Host ('    vs_installer.exe exit: {0}' -f `
        $(if ($null -eq $exit) { 'still running' } else { $exit }))
# 0 = done, 3010 = done but a reboot is pending (runners never reboot, and the
# MSBuild toolset is usable immediately).
if ($null -ne $exit -and $exit -ne 0 -and $exit -ne 3010) {
    Show-InstallerLogs
    throw "vs_installer failed with exit code $exit"
}

# Re-check: a silent partial install would otherwise surface much later as an
# obscure MSBuild error.
if (-not $installed) {
    Show-InstallerLogs
    if (-not (Test-V141Tools)) { throw 'v141 compiler still missing after install' }
    if ($Needs -eq 'xp' -and -not (Test-XpToolset)) { throw 'v141_xp toolset still missing after install' }
    if ($Needs -eq 'xp' -and -not (Test-Win71Sdk)) { throw 'Windows 7.1A SDK still missing after install' }
}

Write-Host '[OK] Visual Studio components installed'
