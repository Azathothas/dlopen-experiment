<#
.SYNOPSIS
    The end-to-end proof: a real AppImage using a real HOST graphics driver on a
    host whose libc is not the AppImage's.

.DESCRIPTION
    run.ps1 measures the mechanism in isolation. This measures the thing users
    actually complain about, on real software:

      debian:bullseye-slim  builds foreign-dlopen.so and the probes on the glibc
                            2.31 FLOOR, so they need only old symbols
      alpine:3.22           musl host. The demo AppImage bundles glibc 2.44 and
                            must drive Alpine's musl-built Mesa. This is the
                            case the complaint is about.
      debian:trixie-slim    glibc 2.41 host, OLDER than the bundled 2.44, so
                            nothing NEEDS rewriting. The regression case: does
                            turning the feature on break what already worked?

    The demo AppImage is ~10 MB and is downloaded once into <repo>\.tmp, which
    is gitignored. Its sha256 is verified.

    Every case is measured with the feature OFF and ON, and against BOTH the
    upstream foreign-dlopen.so shipped inside the AppImage and the one built
    from src/. A single-sided result cannot tell a working fix from a fallback
    that was already happening.

.PARAMETER Engine
    Path to podman.exe or docker.exe. Auto-detected when omitted.

.PARAMETER Only
    'alpine' or 'debian' to run just one host.

.EXAMPLE
    .\appimage.ps1
#>
[CmdletBinding()]
param(
    [string]$Engine,
    [ValidateSet('alpine', 'debian', 'both')][string]$Only = 'both'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = Split-Path -Parent $Here
$Work = Join-Path $Repo '.tmp'
$Sha  = '712766f8a4dc6b5ea3193ed7bb0282b64c7b781f7334056416edd3d00e8960bd'
$Url  = 'https://github.com/Samueru-sama/Anylinux-AppImages/releases/download/demo/vkcube+glxgears-host-drivers-demo-x86_64.AppImage'

function Resolve-Engine {
    if ($Engine) {
        if (-not (Test-Path -LiteralPath $Engine)) { throw "Engine not found: $Engine" }
        return $Engine
    }
    foreach ($n in @('podman', 'docker')) {
        $c = Get-Command "$n.exe" -ErrorAction SilentlyContinue
        if ($c) { return $c.Source }
    }
    foreach ($p in @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Podman\podman.exe'),
        (Join-Path $env:ProgramFiles 'RedHat\Podman\podman.exe'),
        (Join-Path $env:ProgramFiles 'Docker\Docker\resources\bin\docker.exe'))) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    throw "No container engine found. Install podman or docker, or pass -Engine <path>."
}

function Invoke-In {
    param(
        [Parameter(Mandatory)][string]$Image,
        [Parameter(Mandatory)][string]$Script,
        [switch]$Privileged,
        [switch]$Gpu
    )
    $path = Join-Path $Here $Script
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing script: $path" }
    if ([IO.File]::ReadAllText($path).Contains("`r")) {
        throw "$Script contains CR characters. Shell scripts here must be LF-only."
    }
    Write-Host "==> $Image  ($Script)" -ForegroundColor Cyan
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        $args = @('run', '--rm',
                  '-v', "${Work}:/w",
                  '-v', "${Repo}:/repo:ro",
                  '-v', "${Here}:/scripts:ro")
        if ($Privileged) { $args += '--privileged' }
        if ($Gpu -and $script:GpuArgs) { $args += $script:GpuArgs }
        $args += @($Image, 'sh', "/scripts/$Script")
        & $engineExe @args 2>&1 | Out-Host
        $rc = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $prev }
    return $rc
}

<#
    Is a GPU reachable from a container on this machine?

    Asked by RUNNING it, not by inspecting the host. The engine may be a WSL2
    VM (podman machine), a Linux daemon or a remote socket, and only the
    container's own view of /dev/dxg and the bind-mounted vendor userspace
    decides whether the E41-E46 cases can run. A machine with no GPU is a
    supported configuration: those cases are then SKIPPED by name, never
    silently omitted (section 7).
#>
function Resolve-GpuArgs {
    $candidate = @('--device', '/dev/dxg', '-v', '/usr/lib/wsl:/usr/lib/wsl:ro')
    # One flat array, splatted once. `& $exe @a 'x' 'y', 'z'` parses the comma
    # list as a single array ARGUMENT, so the probe silently runs the wrong
    # command line and reports no GPU on a machine that has one.
    $probe = @('run', '--rm') + $candidate + @('alpine:3.22', 'sh', '-c',
              'test -e /dev/dxg && test -f /usr/lib/wsl/lib/libcuda.so.1 && echo GPU-OK')
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        $out = & $engineExe @probe 2>&1
        $ok = ($LASTEXITCODE -eq 0) -and ("$out" -match 'GPU-OK')
    }
    catch { $ok = $false }
    finally { $ErrorActionPreference = $prev }
    if ($ok) {
        Write-Host "GPU: /dev/dxg and the WSL vendor userspace are reachable" -ForegroundColor DarkGray
        return $candidate
    }
    Write-Host "GPU: no /dev/dxg with a WSL vendor userspace; E41-E46 will be SKIPPED" -ForegroundColor DarkGray
    return @()
}

$engineExe = Resolve-Engine
Write-Host "engine: $engineExe" -ForegroundColor DarkGray
New-Item -ItemType Directory -Force -Path $Work, (Join-Path $Work 'build') | Out-Null
$script:GpuArgs = Resolve-GpuArgs

# ---- the AppImage, fetched once and checksummed -------------------------
$img = Join-Path $Work 'demo.AppImage'
if (-not (Test-Path -LiteralPath $img)) {
    Write-Host "downloading the demo AppImage (~10 MB)" -ForegroundColor DarkGray
    Invoke-WebRequest -Uri $Url -OutFile $img
}
$got = (Get-FileHash -LiteralPath $img -Algorithm SHA256).Hash.ToLower()
if ($got -ne $Sha) { throw "demo.AppImage sha256 is $got, expected $Sha" }
Write-Host "demo.AppImage sha256 ok" -ForegroundColor DarkGray

if (-not (Test-Path -LiteralPath (Join-Path $Work 'AppDir'))) {
    # Extraction runs the AppImage's own ELF runtime and the payload is DwarFS,
    # so it happens inside a container, not on the host.
    $rc = Invoke-In -Image 'debian:trixie-slim' -Script '41-extract.sh' -Privileged
    if ($rc -ne 0) { throw "extraction failed (exit $rc)" }
}

# ---- build on the FLOOR, not on the newest thing available --------------
$rc = Invoke-In -Image 'debian:bullseye-slim' -Script '42-build-floor.sh'
if ($rc -ne 0) { throw "floor build failed (exit $rc)" }

# ---- and the musl half of the ABI probe, which only Alpine can produce ---
$rc = Invoke-In -Image 'alpine:3.22' -Script '45-build-musl-guest.sh'
if ($rc -ne 0) { throw "musl guest build failed (exit $rc)" }

$fail = 0
if ($Only -in @('both', 'alpine')) {
    Write-Host ""
    Write-Host "######## musl host: the case the complaint is about ########" -ForegroundColor Yellow
    if ((Invoke-In -Image 'alpine:3.22' -Script '43-host-alpine.sh' -Gpu) -ne 0) { $fail++ }
}
if ($Only -in @('both', 'debian')) {
    Write-Host ""
    Write-Host "######## glibc host: the regression case ########" -ForegroundColor Yellow
    if ((Invoke-In -Image 'debian:trixie-slim' -Script '44-host-debian.sh' -Gpu) -ne 0) { $fail++ }
}

Write-Host ""
if ($fail -eq 0) { Write-Host "ALL PREDICTIONS HELD" -ForegroundColor Green; exit 0 }
Write-Host "SOME PREDICTIONS DID NOT HOLD -- investigate, this is a finding" -ForegroundColor Yellow
exit 1
