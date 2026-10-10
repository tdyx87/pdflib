<#
.SYNOPSIS
  Build and test one pdflibdrv configuration the way CI does.

.DESCRIPTION
  The entry point shared by .github/workflows/ci.yml and a developer shell.
  It locates Visual Studio with vswhere, activates the matching MSVC
  environment (selecting the v141 toolset when the IDE is newer than 2017,
  because both static-CRT profiles pin compiler.version=191), resolves a
  working conan CLI, and then runs the per-variant stages:

    md     x64 /MD   - all backends + pdflibdrv, full ctest + harness
    mt     x64 /MT   - static CRT, podofo only, full ctest
    x86xp  x86 + XP  - v141_xp, static CRT, ctest + XP import audit

  The three variants mirror build-driver.cmd, build-mt.cmd and
  build-x86-xp.cmd. Unlike those .cmd files this script hard-codes no IDE
  path, no drive letter and no Python install, so the same logic runs on a
  GitHub windows-* runner and on a developer machine.

.PARAMETER Variant
  md | mt | x86xp.

.PARAMETER Python
  python.exe that can "import conan". By default the script probes
  $env:CONAN_PYTHON, python, python3 and py and uses the first that has
  conan; the conan entry point itself is used when it works. Pass it
  explicitly when a machine's conan.exe launcher is broken (see
  tools/conan_main.py) and no python on PATH has conan, e.g.
  -Python D:\Python313\python.exe.

.PARAMETER RepoRoot
  Repository root. Defaults to the parent directory of this script.

.PARAMETER DryRun
  Resolve the environment and print every command without running it.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('md', 'mt', 'x86xp')]
    [string]$Variant,

    [string]$Python,
    [string]$RepoRoot,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ----------------------------------------------------------------------
# small helpers
# ----------------------------------------------------------------------

function Format-Command {
    param([string[]]$Command)
    $parts = @()
    foreach ($p in $Command) {
        if ($null -ne $p -and $p -match '[\s"]') {
            $parts += '"' + $p + '"'
        }
        else {
            $parts += $p
        }
    }
    return ($parts -join ' ')
}

function Split-Command {
    <# splits a resolved command array into exe + argument list #>
    param([string[]]$Command)
    $exe = $Command[0]
    $rest = @()
    if ($Command.Length -gt 1) { $rest = $Command[1..($Command.Length - 1)] }
    return @{ Exe = $exe; Rest = $rest }
}

function Invoke-Capture {
    <# runs a command and returns its stdout lines (never throws on exit code) #>
    param([string[]]$Command)
    $c = Split-Command $Command
    $out = & $c.Exe @($c.Rest) 2>$null
    return @($out)
}

function Invoke-Checked {
    param([string]$Label, [string[]]$Command)
    Write-Host ''
    Write-Host ("==> " + $Label) -ForegroundColor Cyan
    Write-Host ("    " + (Format-Command $Command)) -ForegroundColor DarkGray
    if ($DryRun) { return }
    $c = Split-Command $Command
    & $c.Exe @($c.Rest)
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

# ----------------------------------------------------------------------
# toolchain discovery
# ----------------------------------------------------------------------

function Resolve-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found at '$vswhere' (Visual Studio Installer missing?)"
    }
    $path = @(& $vswhere -latest -products * -property installationPath) | Select-Object -First 1
    $version = @(& $vswhere -latest -products * -property installationVersion) | Select-Object -First 1
    if (-not $path) {
        throw 'vswhere found no Visual Studio installation'
    }
    $major = [int]([string]$version).Split('.')[0]
    $generator = switch ($major) {
        15 { 'Visual Studio 15 2017' }
        16 { 'Visual Studio 16 2019' }
        17 { 'Visual Studio 17 2022' }
        18 { 'Visual Studio 18 2026' }
        default { throw "unsupported Visual Studio major version $major" }
    }
    return @{ Path = [string]$path; Version = [string]$version; Major = $major; Generator = $generator }
}

function Resolve-Conan {
    <# returns @{ Python = <exe or $null>; Conan = <command array> } #>
    $pythonExe = $null
    $candidates = @()
    if ($Python) { $candidates += $Python }
    if ($env:CONAN_PYTHON) { $candidates += $env:CONAN_PYTHON }
    $candidates += @('python', 'python3', 'py')

    foreach ($cand in $candidates) {
        $cmd = Get-Command $cand -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        $out = @(& $cmd.Source -c 'import conan' 2>$null)
        if ($LASTEXITCODE -eq 0) {
            $pythonExe = $cmd.Source
            break
        }
    }

    if ($pythonExe) {
        $conanMain = Join-Path $RepoRoot 'tools\conan_main.py'
        if (-not (Test-Path $conanMain)) { throw "tools\conan_main.py not found under $RepoRoot" }
        return @{ Python = $pythonExe; Conan = @($pythonExe, $conanMain) }
    }

    # Fall back to the conan entry point, but only if it actually runs: a
    # launcher whose baked-in interpreter path is stale exits 0 and prints
    # nothing (tools/conan_main.py exists for exactly that case).
    $cli = Get-Command conan -ErrorAction SilentlyContinue
    if ($cli) {
        $version = (Invoke-Capture @($cli.Source, '--version') | Out-String)
        if ($LASTEXITCODE -eq 0 -and $version -match 'Conan version') {
            return @{ Python = $null; Conan = @($cli.Source) }
        }
    }

    throw ('no usable conan CLI: none of ' + ($candidates -join ', ') +
           ' can "import conan", and the "conan" entry point does not run. ' +
           'Pass -Python <python.exe> (see the script help).')
}

function Resolve-PythonCommand {
    <# python usable for the repo's own tools; falls back to the conan one #>
    param([hashtable]$Conan)
    if ($Conan.Python) { return @($Conan.Python) }
    foreach ($cand in @(@($Python), @($env:CONAN_PYTHON), @('python', 'python3', 'py'))) {
        if (-not $cand) { continue }
        $cmd = Get-Command $cand -ErrorAction SilentlyContinue
        if ($cmd) { return @($cmd.Source) }
    }
    throw 'no python interpreter found for tools/*.py'
}

function Enter-MsvcEnvironment {
    param([string]$VcvarsAll, [string]$Arch, [string]$VcvarsVer)
    $vcArgs = @($Arch)
    if ($VcvarsVer) { $vcArgs += "-vcvars_ver=$VcvarsVer" }
    $call = ('call "{0}" {1} >nul 2>&1' -f $VcvarsAll, ($vcArgs -join ' '))

    Write-Host ''
    Write-Host '==> MSVC environment' -ForegroundColor Cyan
    Write-Host ("    " + $call) -ForegroundColor DarkGray
    if ($DryRun) { return }

    # A throwaway .bat avoids the quoting games of driving `cmd /c` with one
    # big string from PowerShell.
    $bat = Join-Path $env:TEMP ('pdflib-vcvars-{0}.bat' -f $PID)
    $content = "@echo off`r`n$call`r`nset`r`n"
    Set-Content -Path $bat -Value $content -NoNewline -Encoding ASCII
    try {
        $captured = @(& $bat)
        if ($LASTEXITCODE -ne 0) { throw "vcvars failed: $call" }
    }
    finally {
        Remove-Item $bat -Force -ErrorAction SilentlyContinue
    }
    foreach ($line in $captured) {
        $eq = $line.IndexOf('=')
        if ($eq -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $eq), $line.Substring($eq + 1))
        }
    }
    # vcvarsall's own chatter is redirected, so it can fail without a word and
    # leave a half-built environment. The compiler on PATH is the invariant
    # that matters; assert it right here instead of letting `cmake` discover
    # the wreckage ("CMAKE_C_COMPILER cl is not a full path and was not found
    # in the PATH") several minutes later.
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $cl) {
        throw "'$call' left no C compiler on PATH - is the requested toolset installed?"
    }
    Write-Host ("    -> cl from '" + $env:VCToolsInstallDir + "': " + $cl.Source) -ForegroundColor DarkGray
}

function Resolve-Dumpbin {
    if ($env:VCToolsInstallDir) {
        $candidate = Join-Path $env:VCToolsInstallDir 'bin\HostX86\x86\dumpbin.exe'
        if (Test-Path $candidate) { return $candidate }
    }
    $cmd = Get-Command dumpbin -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw 'dumpbin not found (the XP audit needs it; it comes with the MSVC toolset)'
}

# ----------------------------------------------------------------------
# main
# ----------------------------------------------------------------------

if (-not $RepoRoot) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}
$RepoRoot = (Resolve-Path $RepoRoot).Path
Set-Location $RepoRoot

$vs = Resolve-VisualStudio
$conan = Resolve-Conan
# @() matters: a function that returns a one-element list hands PowerShell a
# bare string, and "$string + $array" would concatenate instead of appending.
$py = @(Resolve-PythonCommand $conan)

$arch = if ($Variant -eq 'x86xp') { 'x86' } else { 'amd64' }
$vcvarsAll = Join-Path $vs.Path 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path $vcvarsAll)) { throw "vcvarsall.bat not found: $vcvarsAll" }

# `md` builds with whatever the IDE ships - the auto-detected default conan
# profile agrees with it. `mt` and `x86xp` pin compiler.version=191, so they
# need the v141 (14.16) toolset: VS2017 has it as its default, a newer IDE
# needs it installed *and* selected explicitly. Asking a newer IDE for a
# toolset it does not have makes vcvarsall fail, and since its output is
# redirected the only symptom is "cl not found" much later - so check first.
$vcvarsVer = ''
if ($Variant -ne 'md' -and $vs.Major -ge 16) {
    $toolsRoot = Join-Path $vs.Path 'VC\Tools\MSVC'
    $v141 = @(Get-ChildItem $toolsRoot -Directory -Filter '14.16*' -ErrorAction SilentlyContinue)
    if (-not $v141) {
        $component = if ($Variant -eq 'x86xp') { 'xp' } else { 'v141' }
        throw ("variant '$Variant' needs the v141 (14.16) toolset, but '$toolsRoot' has no 14.16* " +
               "toolset. Run ci/ensure-vs-components.ps1 -Needs $component first, or use a runner that has it.")
    }
    $vcvarsVer = '14.16'
}

# NB: the extra-argument parameters below must not be called $Args: that
# name is PowerShell's automatic argument variable, and binding silently
# drops the caller's values (verified - it cost a debugging round).
function Conan-Command { param([string[]]$Extra) return , @($conan.Conan + $Extra) }
function Python-Command { param([string[]]$Extra) return , @($py + $Extra) }

Write-Host ("variant       : {0}" -f $Variant)
Write-Host ("repository    : {0}" -f $RepoRoot)
Write-Host ("Visual Studio : {0} ({1})" -f $vs.Path, $vs.Version)
Write-Host ("generator     : {0}" -f $vs.Generator)
Write-Host ("MSVC arch     : {0} {1}" -f $arch, $(if ($vcvarsVer) { "-vcvars_ver=$vcvarsVer" } else { '(IDE default v141)' }))
Write-Host ("conan         : {0}" -f (Format-Command $conan.Conan))

# Both the /MT profile and the XP profile are host profiles compiled with a
# generic x64 build profile, so a "default" conan profile must exist.
$conanHome = ((Invoke-Capture (Conan-Command @('config', 'home')) | Out-String).Trim())
if ($conanHome -and -not (Test-Path (Join-Path $conanHome 'profiles\default'))) {
    Invoke-Checked 'create default conan profile' (Conan-Command @('profile', 'detect', '--force'))
}

Enter-MsvcEnvironment -VcvarsAll $vcvarsAll -Arch $arch -VcvarsVer $vcvarsVer

# Conan 2 takes the name/version from the recipe itself (podofo-local/0.9.7).
Invoke-Checked 'export the bundled podofo recipe' `
    (Conan-Command @('export', 'recipes/podofo/all'))

switch ($Variant) {
    'md' {
        Invoke-Checked 'conan install (/MD, all backends)' (Conan-Command @(
                'install', '.', '-s', 'build_type=Release', '--build=missing',
                '-of', 'build/all-backends',
                '-c', 'tools.cmake.cmaketoolchain:generator=Ninja'))

        # One layout only, but running this keeps `cmake --preset` immune to
        # a CMakeUserPresets.json left over from another variant's install.
        Invoke-Checked 'pin CMakeUserPresets.json' (Python-Command @(
                'tools/pin_user_presets.py',
                'build/all-backends/build/Release/generators/CMakePresets.json'))

        Invoke-Checked 'configure (all-backends preset + driver)' @(
            'cmake', '--preset', 'all-backends', '-DPDFLIB_BUILD_DRIVER=ON')
        Invoke-Checked 'build' @('cmake', '--build', '--preset', 'all-backends')
        Invoke-Checked 'tests' @('ctest', '--test-dir', 'build/all-backends', '--output-on-failure')
        Invoke-Checked 'driver contract tests' @(
            'ctest', '--test-dir', 'build/all-backends', '-R', 'pdflibdrv_harness', '--output-on-failure')
    }

    'mt' {
        $conf = @()
        if ($vs.Major -ge 16) {
            # compiler.version=191 would otherwise send conan looking for a
            # VS2017 install; point it at the IDE that is actually here.
            $conf = @('-c', "tools.microsoft.msbuild:vs_version=$($vs.Major)",
                '-c', 'tools.microsoft:msvc_update=6')
        }
        Invoke-Checked 'conan install (/MT, static runtime)' (Conan-Command (@(
                    'install', '.', '-of', 'build/mt',
                    '-pr:a', 'profiles/win-x64-static-release',
                    '-o', 'with_libharu=False', '-o', 'with_podofo=True',
                    '--build=missing') + $conf))

        Invoke-Checked 'pin CMakeUserPresets.json' (Python-Command @(
                'tools/pin_user_presets.py',
                'build/all-backends/build/Release/generators/CMakePresets.json'))

        Invoke-Checked 'configure (Ninja, static CRT required)' @(
            'cmake', '-S', '.', '-B', 'build/mt-cmake', '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_TOOLCHAIN_FILE=build/mt/build/Release/generators/conan_toolchain.cmake',
            '-DPDFLIB_WITH_LIBHARU=OFF', '-DPDFLIB_WITH_PODOFO=ON',
            '-DPDFLIB_BUILD_TESTS=ON', '-DPDFLIB_BUILD_EXAMPLES=ON',
            '-DPDFLIB_BUILD_DRIVER=ON', '-DPDFLIB_DRIVER_STATIC_CRT=ON')
        Invoke-Checked 'build' @('cmake', '--build', 'build/mt-cmake')
        Invoke-Checked 'tests (incl. export table + no-dynamic-CRT guard)' @(
            'ctest', '--test-dir', 'build/mt-cmake', '--output-on-failure')
    }

    'x86xp' {
        $conf = @('-c', "tools.cmake.cmaketoolchain:generator=$($vs.Generator)")
        if ($vs.Major -ge 16) {
            $conf += @('-c', "tools.microsoft.msbuild:vs_version=$($vs.Major)",
                '-c', 'tools.microsoft:msvc_update=6')
        }
        Invoke-Checked 'conan install (x86 + XP toolset + static CRT)' (Conan-Command (@(
                    'install', '.', '-of', 'build/xp',
                    '-pr:h', 'profiles/win-x86-xp-static-release', '-pr:b', 'default',
                    '-o', 'with_libharu=False', '-o', 'with_podofo=True',
                    '--build=missing') + $conf))

        Invoke-Checked 'pin CMakeUserPresets.json' (Python-Command @(
                'tools/pin_user_presets.py',
                'build/all-backends/build/Release/generators/CMakePresets.json'))

        Invoke-Checked 'configure (Win32 + v141_xp, static CRT required)' @(
            'cmake', '-S', '.', '-B', 'build/xp-cmake', '-G', $vs.Generator,
            '-A', 'Win32', '-T', 'v141_xp',
            '-DCMAKE_TOOLCHAIN_FILE=build/xp/build/generators/conan_toolchain.cmake',
            '-DPDFLIB_WITH_LIBHARU=OFF', '-DPDFLIB_WITH_PODOFO=ON',
            '-DPDFLIB_BUILD_TESTS=ON', '-DPDFLIB_BUILD_EXAMPLES=OFF',
            '-DPDFLIB_BUILD_DRIVER=ON', '-DPDFLIB_DRIVER_STATIC_CRT=ON')
        Invoke-Checked 'build' @('cmake', '--build', 'build/xp-cmake', '--config', 'Release')
        Invoke-Checked 'tests' @('ctest', '--test-dir', 'build/xp-cmake', '-C', 'Release', '--output-on-failure')

        $dumpbin = if ($DryRun) { 'dumpbin' } else { Resolve-Dumpbin }
        Invoke-Checked 'XP import audit' (Python-Command @(
                'tools/check_xp_imports.py', $dumpbin, 'build/xp-cmake/Release/pdflibdrv.dll'))
    }
}

Write-Host ''
Write-Host ("[OK] $Variant build verified") -ForegroundColor Green
