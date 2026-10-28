<#
.SYNOPSIS
    Build proot-termux artifacts from a Windows host (WSL2 required).

.DESCRIPTION
    proot is a Linux tool: it traces tracees with ptrace and installs seccomp
    filters, so it cannot run natively on Windows.  Windows can still be a
    perfectly good build host.  This script drives WSL2 and produces:

      * linux   - native x86_64 Linux binary built from proot/src (Route A)
      * android - aarch64 Android PIE cross-built with the Android NDK r30
                  (Route B)

    Every heavy step runs inside the distribution through
    "wsl -d <Distro> -u <user> -- bash <script>".  The bash text is written
    to a file in the WSL build root first, so paths that contain spaces are
    never passed through shell quoting games.

    The script is idempotent.  The NDK, talloc and the object files are
    cached in the WSL build root, so re-running only rebuilds what changed.

.PARAMETER Target
    Which artifact to build: "linux", "android" or "all" (default: all).

.PARAMETER Distro
    WSL distribution used for the build (default: Ubuntu-24.04).

.PARAMETER DistroUser
    User inside the distribution.  The default "root" is the one used by the
    verified route; another user only works with passwordless sudo.

.PARAMETER Jobs
    Parallel make jobs (default: 1).  More than one job needs at least 4 GB
    of free RAM, so the script warns when the host reports less.

.PARAMETER NdkVersion
    NDK revision used to name the cache directory (default: r30).

.PARAMETER NdkUrl
    Android NDK archive to download (default: the official r30 linux zip).

.PARAMETER NdkSha256
    Expected SHA-256 of the NDK archive.  The default is the value pinned
    in proot/ci/termux/scripts/setup-android-sdk.sh.

.PARAMETER TallocVersion
    talloc release used for the static aarch64 archive (default: 2.4.3).

.PARAMETER TallocUrl
    talloc source tarball used for the static aarch64 archive.

.PARAMETER TallocSha256
    Expected SHA-256 of the talloc tarball.  The default is the value pinned
    in proot/ci/termux/packages/libtalloc/build.sh.

.PARAMETER OutputDir
    Where the artifacts and the build reports are written.  Relative paths
    are resolved against the repository root (default: proot\artifacts\windows).

.PARAMETER BuildRoot
    WSL directory used for the build caches (NDK, talloc, source copies).
    Default: $HOME/proot-build inside the distribution.

.PARAMETER SkipDeps
    Skip the apt-get step that installs the build dependencies.

.EXAMPLE
    .\proot\scripts\build-windows.ps1 -Target linux

.EXAMPLE
    .\proot\scripts\build-windows.ps1 -Target android -Distro Ubuntu-24.04

.EXAMPLE
    .\proot\scripts\build-windows.ps1 -Target all -Jobs 2 -Verbose

.NOTES
    Exit codes:
      0  success
      1  unexpected error
      2  preflight failed (no WSL, unknown distribution, missing sources)
      3  a build or verification step failed
#>
[CmdletBinding()]
param(
    [ValidateSet('linux', 'android', 'all')]
    [string] $Target = 'all',

    [string] $Distro = 'Ubuntu-24.04',

    [string] $DistroUser = 'root',

    [ValidateRange(1, 32)]
    [int] $Jobs = 1,

    [string] $NdkVersion = 'r30',

    [string] $NdkUrl = 'https://dl.google.com/android/repository/android-ndk-r30-linux.zip',

    [string] $NdkSha256 = '753611f410d002cfcd3f3dc2ef49aad532089d3180b436c060a90bf0fcb64df2',

    [string] $TallocVersion = '2.4.3',

    [string] $TallocUrl = 'https://www.samba.org/ftp/talloc/talloc-2.4.3.tar.gz',

    [string] $TallocSha256 = 'dc46c40b9f46bb34dd97fe41f548b0e8b247b77a918576733c528e83abd854dd',

    [string] $OutputDir = 'proot\artifacts\windows',

    [string] $BuildRoot = '',

    [switch] $SkipDeps
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# --------------------------------------------------------------------------
# Constants
# --------------------------------------------------------------------------

$Script:RepoRoot   = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Script:ProotDir  = Join-Path $Script:RepoRoot 'proot'
$Script:SrcDir    = Join-Path $Script:ProotDir 'src'
$Script:BuildDate = Get-Date -Format 'yyyy-MM-dd HH:mm:ss K'
$Script:Results   = New-Object System.Collections.ArrayList
$Script:LogFile   = $null
$Script:WorkRoot  = $null
$Script:OutputFull = $null
$Script:BuildRoot = ''

# --------------------------------------------------------------------------
# Logging helpers
# --------------------------------------------------------------------------

function Write-Info {
    param([string] $Message)
    Write-Host ('[info] ' + $Message) -ForegroundColor Cyan
}

function Write-Step {
    param([string] $Message)
    Write-Host ('==> ' + $Message) -ForegroundColor Green
}

function Write-Warn {
    param([string] $Message)
    Write-Host ('[warn] ' + $Message) -ForegroundColor Yellow
}

function Write-Fail {
    param([string] $Message)
    Write-Host ('[fail] ' + $Message) -ForegroundColor Red
}

function Write-Detail {
    param([string] $Message)
    if ($VerbosePreference -ne 'SilentlyContinue') {
        Write-Host ('       ' + $Message) -ForegroundColor DarkGray
    }
}

# --------------------------------------------------------------------------
# Path and template helpers
# --------------------------------------------------------------------------

function ConvertTo-ShellLiteral {
    <#
        Wraps a value in single quotes so that bash treats it as one literal
        argument, whatever it contains.
    #>
    param([AllowEmptyString()][string] $Value)
    $escape = "'" + '\' + "'"
    return "'" + $Value.Replace("'", $escape) + "'"
}

function ConvertTo-WslPath {
    <#
        Maps a Windows path to its WSL /mnt/<drive>/... counterpart.
    #>
    param([Parameter(Mandatory = $true)][string] $Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^([A-Za-z]):[\\/](.*)$') {
        throw ('Cannot convert this Windows path to a WSL path: ' + $Path)
    }
    $drive = $Matches[1].ToLowerInvariant()
    $rest  = $Matches[2] -replace '\\', '/'
    return '/mnt/' + $drive + '/' + $rest
}

function New-Template {
    <#
        Replaces __TOKEN__ placeholders in a bash template with shell
        literals.
    #>
    param(
        [Parameter(Mandatory = $true)][string] $Template,
        [hashtable] $Values
    )
    $result = $Template
    foreach ($key in $Values.Keys) {
        $result = $result.Replace($key, (ConvertTo-ShellLiteral ([string]$Values[$key])))
    }
    return $result
}

# --------------------------------------------------------------------------
# WSL helpers
# --------------------------------------------------------------------------

function Test-WslDistro {
    <#
        Returns $true when the distribution is registered.  "wsl -l -q" is
        emitted as UTF-16, so the comparison ignores blanks and NUL bytes.
    #>
    param([Parameter(Mandatory = $true)][string] $Name)
    $raw = & wsl.exe -l -q 2>$null
    if ($LASTEXITCODE -ne 0) { return $false }
    $normalized = (($raw | ForEach-Object { $_ -replace '[\s\0]', '' }) -join '')
    $wanted = $Name -replace '[\s\0]', ''
    return $normalized.Contains($wanted)
}

function Write-WslFile {
    <#
        Streams a bash script into the WSL build root and returns its path.
        The path has no spaces, so it can be handed to "bash" as one word.
    #>
    param(
        [Parameter(Mandatory = $true)][string] $Name,
        [Parameter(Mandatory = $true)][string] $Content
    )
    $path = $Script:WorkRoot + '/' + $Name
    $payload = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($Content))
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & wsl.exe -d $Distro -u $DistroUser -- mkdir -p $Script:WorkRoot | Out-Null
    try {
        & wsl.exe -d $Distro -u $DistroUser -- sh -c "echo $payload | base64 -d > $path" | Out-Null
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    if ($code -ne 0) {
        throw ('Cannot write the WSL script ' + $path)
    }
    return $path
}

function Invoke-Wsl {
    <#
        Runs a bash script inside the distribution and returns its exit
        code.  The output is echoed and appended to the build log.
    #>
    param(
        [Parameter(Mandatory = $true)][string] $Script,
        [Parameter(Mandatory = $true)][string] $Name,
        [switch] $AllowFailure
    )
    $body = $Script.TrimEnd("`n")
    $statements = @($body -split "`n" | Where-Object { $_.Trim() -ne '' })
    $last = ''
    if ($statements.Count -gt 0) { $last = $statements[$statements.Count - 1] }
    if ($last -notmatch '^\s*exit\b') { $body = $body + "`n" + 'exit 0' }
    $path = Write-WslFile -Name ($Name + '.sh') -Content ($body + "`n")
    Write-Step ('WSL step: ' + $Name)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & wsl.exe -d $Distro -u $DistroUser -- bash $path 2>&1 |
            Tee-Object -FilePath $Script:LogFile -Append |
            ForEach-Object { Write-Host ('    ' + "$_") }
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    if ($code -ne 0 -and -not $AllowFailure) {
        throw ('WSL step ' + $Name + ' failed with exit code ' + $code + '.')
    }
    return $code
}

function Get-WslValue {
    <#
        Runs a single WSL command and returns its standard output.
    #>
    param(
        [Parameter(Mandatory = $true)][string] $Command,
        [string] $Argument = ''
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ([string]::IsNullOrEmpty($Argument)) {
            $out = & wsl.exe -d $Distro -u $DistroUser -- $Command 2>$null
        }
        else {
            $out = & wsl.exe -d $Distro -u $DistroUser -- $Command $Argument 2>$null
        }
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    if ($code -ne 0) { return '' }
    return (($out | Out-String).Trim())
}
# --------------------------------------------------------------------------
# Preflight
# --------------------------------------------------------------------------

function Test-Preflight {
    Write-Step 'Preflight'

    if (-not (Get-Command 'wsl.exe' -ErrorAction SilentlyContinue)) {
        Write-Fail 'wsl.exe was not found in PATH. Install WSL2 and try again.'
        exit 2
    }
    Write-Info ('WSL command: ' + (Get-Command 'wsl.exe').Source)

    if (-not (Test-WslDistro -Name $Distro)) {
        Write-Fail ('WSL distribution ' + $Distro + ' is not registered. Run: wsl --install -d ' + $Distro)
        exit 2
    }
    Write-Info ('WSL distribution: ' + $Distro + ' (user: ' + $DistroUser + ')')

    $kernel = Get-WslValue -Command 'uname' -Argument '-srm'
    if ([string]::IsNullOrWhiteSpace($kernel)) {
        Write-Fail ('Cannot run commands inside ' + $Distro + ' as ' + $DistroUser + '.')
        exit 2
    }
    Write-Info ('Distribution kernel: ' + $kernel)

    $required = @(
        (Join-Path $Script:SrcDir 'GNUmakefile'),
        (Join-Path $Script:ProotDir 'ci\termux\packages\libtalloc\build.sh'),
        (Join-Path $Script:ProotDir 'ci\termux\packages\proot\build.sh'),
        (Join-Path $Script:ProotDir 'ci\termux\scripts\setup-android-sdk.sh')
    )
    foreach ($file in $required) {
        if (-not (Test-Path -LiteralPath $file)) {
            Write-Fail ('Missing repository file: ' + $file)
            exit 2
        }
    }
    Write-Info ('Repository root: ' + $Script:RepoRoot)

    $os = Get-CimInstance Win32_OperatingSystem
    $freeMb = [int]($os.FreePhysicalMemory / 1024)
    $totalMb = [int]($os.TotalVisibleMemorySize / 1024)
    Write-Info ('Free physical memory: ' + $freeMb + ' MB of ' + $totalMb + ' MB')
    if ($Jobs -gt 1) {
        if ($freeMb -lt 4096) {
            Write-Warn ('-Jobs ' + $Jobs + ' was requested but only ' + $freeMb + ' MB of RAM are free. Use -Jobs 1 to avoid the OOM killer.')
        }
        else {
            Write-Info ('Parallel jobs: ' + $Jobs)
        }
    }
    else {
        Write-Info 'Parallel jobs: 1 (safe on hosts with less than 4 GB of RAM)'
    }

    $driveRoot = [System.IO.Path]::GetPathRoot([System.IO.Path]::GetFullPath($Script:OutputFull))
    $drive = New-Object System.IO.DriveInfo($driveRoot)
    $freeGb = [math]::Round($drive.AvailableFreeSpace / 1GB, 1)
    Write-Info ('Free space on ' + $driveRoot + ': ' + $freeGb + ' GB')
    if ($Target -ne 'linux' -and $freeGb -lt 2) {
        Write-Warn 'The Android route needs about 2 GB of free space for the NDK archive.'
    }

    Write-Info 'Plan:'
    Write-Host ('       repository : ' + $Script:RepoRoot)
    Write-Host ('       output     : ' + $Script:OutputFull)
    Write-Host ('       build root : ' + $Script:BuildRoot)
    Write-Host ('       target     : ' + $Target)
}

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------

function Install-Dependencies {
    if ($SkipDeps) {
        Write-Info 'Skipping the dependency step (-SkipDeps).'
        return
    }
    Write-Step 'Installing the build dependencies inside the distribution'

    $sudo = ''
    if ($DistroUser -ne 'root') {
        $sudo = 'sudo -n '
        $probe = Get-WslValue -Command 'sudo' -Argument '-n true'
        if ($probe -ne 'true') {
            Write-Fail 'The selected user needs passwordless sudo to install the dependencies.'
            exit 3
        }
    }

    $bash = @(
        'set -euo pipefail',
        'export DEBIAN_FRONTEND=noninteractive',
        '__SUDO__apt-get update',
        '__SUDO__apt-get install -y --no-install-recommends build-essential libtalloc-dev gcc-multilib g++-multilib unzip file curl ca-certificates',
        'echo DEPENDENCIES_OK'
    ) -join "`n"
    $bash = New-Template -Template $bash -Values @{ '__SUDO__' = $sudo }
    Invoke-Wsl -Script $bash -Name 'deps' | Out-Null
}

# --------------------------------------------------------------------------
# Target: native Linux binary
# --------------------------------------------------------------------------

function Invoke-LinuxTarget {
    Write-Step 'Route A: native Linux build (x86_64)'

    $srcWsl = ConvertTo-WslPath $Script:SrcDir

    $build = @(
        'set -euo pipefail',
        'SRC=__SRC__',
        'cd "$SRC"',
        'make -j__JOBS__ V=1',
        'test -x proot',
        'echo BUILD_OK'
    ) -join "`n"
    $build = New-Template -Template $build -Values @{ '__SRC__' = $srcWsl; '__JOBS__' = "$Jobs" }
    Invoke-Wsl -Script $build -Name 'build-linux' | Out-Null

    $verify = @(
        'set -u',
        'SRC=__SRC__',
        'PROBE=/tmp/proot-build-windows-probe',
        'fail=0',
        'check() {',
        '  if [ "$1" = "0" ]; then echo "SMOKE: PASS $2"; else echo "SMOKE: FAIL $2"; fail=1; fi',
        '}',
        'cd "$SRC" || exit 1',
        'rm -rf "$PROBE"',
        'mkdir -p "$PROBE"',
        'run_proot() {',
        '  env -i PATH=/bin:/usr/bin PROOT_RUNTIME_DIR="$PROBE" TMPDIR="$PROBE" "$@"',
        '}',
        'out=$(run_proot ./proot -r / -b /proc -b /dev -b /sys /bin/echo SMOKE_OK 2>/dev/null)',
        'rc=$?',
        'if [ "$rc" = "0" ] && [ "$out" = "SMOKE_OK" ]; then check 0 "basic sandbox (echo SMOKE_OK)"; else check 1 "basic sandbox (rc=$rc out=$out)"; fi',
        'out=$(run_proot ./proot -r / -b /proc -b /dev -b /sys --net-policy deny --net-deny-bind 443 /bin/echo NETPOLICY_OK 2>/dev/null)',
        'rc=$?',
        'if [ "$rc" = "0" ] && [ "$out" = "NETPOLICY_OK" ]; then check 0 "static network policy (--net-policy deny --net-deny-bind 443)"; else check 1 "static network policy (rc=$rc out=$out)"; fi',
        'if command -v python3 >/dev/null 2>&1; then',
        '  run_proot ./proot -r / -b /proc -b /dev -b /sys --proxy WINPROBE /usr/bin/python3 -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_STREAM);s.bind((''127.0.0.1'',8080));print(''PROXY_OK'')" >/dev/null 2>&1',
        '  rc=$?',
        '  reg="$PROBE/proot-net/WINPROBE/registry.lock"',
        '  if [ "$rc" = "0" ] && [ -f "$reg" ]; then',
        '    magic=$(od -An -tx1 -N4 "$reg" | tr -d '' \n'')',
        '    size=$(stat -c %s "$reg")',
        '    if [ "$magic" = "4e4f5250" ]; then check 0 "virtual network registry ($size bytes, magic 0x50524F4E)"; else check 1 "virtual network registry magic=$magic"; fi',
        '  else',
        '    check 1 "virtual network registry missing (rc=$rc)"',
        '  fi',
        'else',
        '  echo "SMOKE: SKIP virtual network registry (python3 not available)"',
        'fi',
        'host_pids=$(ls /proc | grep -c ''^[0-9]'')',
        'guest_list=$(run_proot ./proot -r / -b /proc -b /dev -b /sys --proc-isolated /bin/sh -c ''ls /proc'' 2>/dev/null)',
        'guest_pids=$(printf ''%s\n'' "$guest_list" | grep -c ''^[0-9]'')',
        'missing=""',
        'for f in self thread-self cpuinfo meminfo loadavg uptime stat; do',
        '  printf ''%s\n'' "$guest_list" | grep -qx "$f" || missing="$missing $f"',
        'done',
        'if [ "$guest_pids" -le 8 ] && [ "$guest_pids" -lt "$host_pids" ] && [ -z "$missing" ]; then',
        '  check 0 "proc isolation ($guest_pids guest pids vs $host_pids host pids)"',
        'else',
        '  check 1 "proc isolation (guest=$guest_pids host=$host_pids missing=$missing)"',
        'fi',
        'file proot',
        './proot --version',
        './proot --about',
        'exit $fail'
    ) -join "`n"
    $verify = New-Template -Template $verify -Values @{ '__SRC__' = $srcWsl }
    $code = Invoke-Wsl -Script $verify -Name 'verify-linux' -AllowFailure
    if ($code -ne 0) {
        throw 'The Linux binary was produced but the smoke tests failed.'
    }

    $targetDir = Join-Path $Script:OutputFull 'linux'
    New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
    $collect = @(
        'set -euo pipefail',
        'SRC=__SRC__',
        'DEST=__DEST__',
        'mkdir -p "$DEST"',
        'cp -f "$SRC/proot" "$DEST/proot"',
        'chmod 0755 "$DEST/proot"',
        'sha256sum "$DEST/proot"',
        'stat -c %s "$DEST/proot"'
    ) -join "`n"
    $collect = New-Template -Template $collect -Values @{ '__SRC__' = $srcWsl; '__DEST__' = (ConvertTo-WslPath $targetDir) }
    Invoke-Wsl -Script $collect -Name 'collect-linux' | Out-Null

    $artifact = Join-Path $targetDir 'proot'
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $artifact).Length
    [void]$Script:Results.Add([pscustomobject]@{
            Target   = 'linux'
            Artifact = $artifact
            Size     = $size
            Sha256   = $hash
            Status   = 'OK'
        })
    Write-Info ('Artifact: ' + $artifact + ' (' + $size + ' bytes)')
    Write-Info ('SHA-256:  ' + $hash)
}
# --------------------------------------------------------------------------
# Target: Android aarch64 binary
# --------------------------------------------------------------------------

function Invoke-AndroidTarget {
    Write-Step 'Route B: Android aarch64 cross build (NDK)'

    $cache = $Script:BuildRoot + '/cache'
    $srcWsl = ConvertTo-WslPath $Script:SrcDir
    $libtallocSh = ConvertTo-WslPath (Join-Path $Script:ProotDir 'ci\termux\packages\libtalloc\build.sh')
    $prootPkgSh = ConvertTo-WslPath (Join-Path $Script:ProotDir 'ci\termux\packages\proot\build.sh')

    # ---- NDK -------------------------------------------------------------
    $ndk = @(
        'set -euo pipefail',
        'CACHE=__CACHE__',
        'NDK_VERSION=__NDKVERSION__',
        'NDK_URL=__NDKURL__',
        'NDK_SHA256=__NDKSHA256__',
        'NDK_DIR="$CACHE/android-ndk-$NDK_VERSION"',
        'NDK_ZIP="$CACHE/android-ndk-$NDK_VERSION-linux.zip"',
        'mkdir -p "$CACHE"',
        'if [ -f "$NDK_DIR/source.properties" ]; then',
        '  echo "NDK: reusing $NDK_DIR"',
        'else',
        '  if [ -f "$NDK_ZIP" ] && ! echo "$NDK_SHA256  $NDK_ZIP" | sha256sum -c - >/dev/null 2>&1; then',
        '    rm -f "$NDK_ZIP"',
        '  fi',
        '  if [ ! -f "$NDK_ZIP" ]; then',
        '    echo "Downloading $NDK_URL ..."',
        '    curl -fL --retry 3 --retry-delay 5 -o "$NDK_ZIP.part" "$NDK_URL"',
        '    mv "$NDK_ZIP.part" "$NDK_ZIP"',
        '  fi',
        '  echo "$NDK_SHA256  $NDK_ZIP" | sha256sum -c -',
        '  rm -rf "$NDK_DIR.unpack"',
        '  mkdir -p "$NDK_DIR.unpack"',
        '  unzip -q "$NDK_ZIP" -d "$NDK_DIR.unpack"',
        '  extracted=$(find "$NDK_DIR.unpack" -mindepth 1 -maxdepth 1 -type d | head -1)',
        '  test -n "$extracted"',
        '  rm -rf "$NDK_DIR"',
        '  mv "$extracted" "$NDK_DIR"',
        '  rm -rf "$NDK_DIR.unpack"',
        'fi',
        'NDK_BIN=""',
        'for candidate in "$NDK_DIR"/toolchain/llvm/prebuilt/*/bin "$NDK_DIR"/toolchains/llvm/prebuilt/*/bin "$NDK_DIR"/prebuilt/*/bin; do',
        '  if [ -x "$candidate/aarch64-linux-android24-clang" ]; then NDK_BIN="$candidate"; break; fi',
        'done',
        'if [ -z "$NDK_BIN" ]; then echo "ERROR: the NDK clang was not found under $NDK_DIR" >&2; exit 1; fi',
        'echo "NDK_BIN=$NDK_BIN"',
        'grep Pkg.Revision "$NDK_DIR/source.properties"',
        'printf ''NDK_DIR=%s\nNDK_BIN=%s\n'' "$NDK_DIR" "$NDK_BIN" > "$CACHE/ndk.env"',
        'echo NDK_READY'
    ) -join "`n"
    $ndk = New-Template -Template $ndk -Values @{
        '__CACHE__'      = $cache
        '__NDKVERSION__' = $NdkVersion
        '__NDKURL__'     = $NdkUrl
        '__NDKSHA256__'  = $NdkSha256
    }
    Invoke-Wsl -Script $ndk -Name 'ndk' | Out-Null

    # ---- talloc (static, aarch64) ---------------------------------------
    $talloc = @(
        'set -euo pipefail',
        'CACHE=__CACHE__',
        'TALLOC_VERSION=__TALLOCVERSION__',
        'TALLOC_URL=__TALLOCURL__',
        'TALLOC_SHA256=__TALLOCSHA256__',
        'LIBTALLOC_SH=__LIBTALLOC_SH__',
        '. "$CACHE/ndk.env"',
        'BUILD=__BUILDROOT__',
        'TDIR="$BUILD/talloc"',
        'TSRC="$TDIR/talloc-$TALLOC_VERSION"',
        'PREFIX="$BUILD/talloc-install"',
        'TARBALL="$TDIR/talloc-$TALLOC_VERSION.tar.gz"',
        'mkdir -p "$TDIR"',
        'if [ ! -f "$TARBALL" ]; then',
        '  curl -fL --retry 3 --retry-delay 5 -o "$TARBALL.part" "$TALLOC_URL"',
        '  mv "$TARBALL.part" "$TARBALL"',
        'fi',
        'echo "$TALLOC_SHA256  $TARBALL" | sha256sum -c -',
        'if [ ! -f "$PREFIX/lib/libtalloc.a" ]; then',
        '  rm -rf "$TSRC"',
        '  tar -xzf "$TARBALL" -C "$TDIR"',
        '  sed -n ''/^[[:space:]]*cat <<EOF > cross-answers.txt/,/^EOF$/p'' "$LIBTALLOC_SH" | sed ''1d;$d'' > "$TSRC/cross-answers.txt"',
        '  test -s "$TSRC/cross-answers.txt"',
        '  cd "$TSRC"',
        '  export CC="$NDK_BIN/aarch64-linux-android24-clang"',
        '  export AR="$NDK_BIN/llvm-ar"',
        '  export RANLIB="$NDK_BIN/llvm-ranlib"',
        '  ./configure --prefix="$PREFIX" --host=aarch64-linux-android --disable-rpath --disable-python --cross-compile --cross-answers=cross-answers.txt',
        '  make || echo "WARNING: the talloc build reported an error, the objects are checked next"',
        '  mkdir -p "$PREFIX/lib" "$PREFIX/include"',
        '  objects=$(ls bin/default/talloc*.6.o 2>/dev/null || true)',
        '  if [ -z "$objects" ]; then objects=$(ls bin/default/talloc*.o 2>/dev/null || true); fi',
        '  if [ -z "$objects" ]; then echo "ERROR: no talloc object was produced" >&2; exit 1; fi',
        '  echo "talloc objects: $objects"',
        '  rm -f "$PREFIX/lib/libtalloc.a"',
        '  "$NDK_BIN/llvm-ar" rcu "$PREFIX/lib/libtalloc.a" $objects',
        '  "$NDK_BIN/llvm-ranlib" "$PREFIX/lib/libtalloc.a"',
        '  cp talloc.h "$PREFIX/include/talloc.h"',
        'fi',
        'ls -l "$PREFIX/lib/libtalloc.a" "$PREFIX/include/talloc.h"',
        'echo TALLOC_READY'
    ) -join "`n"
    $talloc = New-Template -Template $talloc -Values @{
        '__CACHE__'         = $cache
        '__BUILDROOT__'     = $Script:BuildRoot
        '__TALLOCVERSION__' = $TallocVersion
        '__TALLOCURL__'     = $TallocUrl
        '__TALLOCSHA256__'  = $TallocSha256
        '__LIBTALLOC_SH__'  = $libtallocSh
    }
    Invoke-Wsl -Script $talloc -Name 'talloc' | Out-Null

    # ---- proot -----------------------------------------------------------
    $build = @(
        'set -euo pipefail',
        'CACHE=__CACHE__',
        'PROOT_PKG_SH=__PROOT_PKG_SH__',
        '. "$CACHE/ndk.env"',
        'BUILD=__BUILDROOT__',
        'SRC=__SRC__',
        'DEST="$BUILD/android/src"',
        'rm -rf "$DEST"',
        'mkdir -p "$DEST"',
        'tar -C "$SRC" --exclude=''*.o'' --exclude=''*.d'' --exclude=''*.tmp'' --exclude=''./proot'' --exclude=''./build.h'' --exclude=''./loader/loader'' --exclude=''./loader/loader-m32'' --exclude=''./loader/loader-info.c'' --exclude=''.check_*.res'' -cf - . | tar -C "$DEST" -xf -',
        'cd "$DEST"',
        'VERSION=$(sed -n ''s/^TERMUX_PKG_VERSION="\(.*\)"$/\1/p'' "$PROOT_PKG_SH")',
        'test -n "$VERSION"',
        'echo "proot version: $VERSION"',
        'export CC="$NDK_BIN/aarch64-linux-android24-clang"',
        'export STRIP="$NDK_BIN/llvm-strip"',
        'export OBJCOPY="$NDK_BIN/llvm-objcopy"',
        'export OBJDUMP="$NDK_BIN/llvm-objdump"',
        'export CPPFLAGS="-DARG_MAX=131072 -DVERSION=\\\"$VERSION\\\" -I$BUILD/talloc-install/include"',
        'export LDFLAGS="-L$BUILD/talloc-install/lib"',
        'if grep -q ffat-lto-objects GNUmakefile; then',
        '  echo "The makefile already handles the clang LTO objects"',
        'else',
        '  export OPTIMIZATION_FLAGS="-O3 -flto -fvisibility=hidden -ffunction-sections -fdata-sections -DNDEBUG -ffat-lto-objects"',
        '  echo "Adding -ffat-lto-objects for the clang-like compiler"',
        'fi',
        'make -j__JOBS__ V=1 READELF="$NDK_BIN/llvm-readelf"',
        'test -x proot',
        'echo BUILD_OK'
    ) -join "`n"
    $build = New-Template -Template $build -Values @{
        '__CACHE__'      = $cache
        '__BUILDROOT__'  = $Script:BuildRoot
        '__SRC__'        = $srcWsl
        '__PROOT_PKG_SH__' = $prootPkgSh
        '__JOBS__'       = "$Jobs"
    }
    Invoke-Wsl -Script $build -Name 'build-android' | Out-Null

    # ---- verification ----------------------------------------------------
    $verify = @(
        'set -u',
        'CACHE=__CACHE__',
        '. "$CACHE/ndk.env"',
        'BUILD=__BUILDROOT__',
        'cd "$BUILD/android/src" || exit 1',
        'fail=0',
        'check() {',
        '  if [ "$1" = "0" ]; then echo "SMOKE: PASS $2"; else echo "SMOKE: FAIL $2"; fail=1; fi',
        '}',
        'has() {',
        '  # grep -c reads the whole stream: grep -q would close the pipe and',
        '  # trip "pipefail" with a SIGPIPE on the producer side.',
        '  if grep -c "$1" > /dev/null; then check 0 "$2"; else check 1 "$2"; fi',
        '}',
        'header=$("$NDK_BIN/llvm-readelf" -h proot)',
        'echo "$header" | grep -E ''Type:|Machine:|Entry point'' || true',
        'echo "$header" | has AArch64 "AArch64 machine"',
        'echo "$header" | has ''Type:.*DYN'' "dynamic executable (PIE)"',
        'needed=$("$NDK_BIN/llvm-readelf" -d proot | grep NEEDED || true)',
        'echo "$needed"',
        'echo "$needed" | has ''libc.so'' "NEEDED libc.so"',
        'echo "$needed" | has ''libdl.so'' "NEEDED libdl.so"',
        'if "$NDK_BIN/llvm-readelf" -s proot | has _talloc_ "talloc linked statically"; then :; else check 1 "talloc symbols not found"; fi',
        'symbols=$("$NDK_BIN/llvm-readelf" -s proot | grep -E ''_binary_loader(_m32)?_exe_size'' || true)',
        'echo "$symbols"',
        'if [ -n "$symbols" ]; then check 0 "embedded loaders present"; else check 1 "embedded loaders missing"; fi',
        'for loader in loader/loader loader/loader-m32; do',
        '  if [ -f "$loader" ]; then',
        '    echo "--- $loader"',
        '    "$NDK_BIN/llvm-readelf" -h "$loader" | grep -E ''Type:|Machine:|Entry point'' || true',
        '  fi',
        'done',
        'interpreter=$("$NDK_BIN/llvm-readelf" -l proot | grep -m1 interpreter || true)',
        'echo "interpreter: $interpreter"',
        'echo "$interpreter" | has ''/system/bin/linker64'' "bionic interpreter /system/bin/linker64"',
        'stat -c ''size: %s bytes'' proot',
        'sha256sum proot',
        'exit $fail'
    ) -join "`n"
    $verify = New-Template -Template $verify -Values @{
        '__CACHE__'     = $cache
        '__BUILDROOT__' = $Script:BuildRoot
    }
    $code = Invoke-Wsl -Script $verify -Name 'verify-android' -AllowFailure
    if ($code -ne 0) {
        throw 'The Android binary was produced but the verification failed.'
    }

    $targetDir = Join-Path $Script:OutputFull 'android'
    New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
    $collect = @(
        'set -euo pipefail',
        'BUILD=__BUILDROOT__',
        'DEST=__DEST__',
        'mkdir -p "$DEST"',
        'cp -f "$BUILD/android/src/proot" "$DEST/proot"',
        'chmod 0755 "$DEST/proot"',
        'sha256sum "$DEST/proot"',
        'stat -c %s "$DEST/proot"'
    ) -join "`n"
    $collect = New-Template -Template $collect -Values @{
        '__BUILDROOT__' = $Script:BuildRoot
        '__DEST__'      = (ConvertTo-WslPath $targetDir)
    }
    Invoke-Wsl -Script $collect -Name 'collect-android' | Out-Null

    $artifact = Join-Path $targetDir 'proot'
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $artifact).Length
    [void]$Script:Results.Add([pscustomobject]@{
            Target   = 'android'
            Artifact = $artifact
            Size     = $size
            Sha256   = $hash
            Status   = 'OK'
        })
    Write-Info ('Artifact: ' + $artifact + ' (' + $size + ' bytes)')
    Write-Info ('SHA-256:  ' + $hash)
    Write-Warn 'The Android PIE needs a real device or an Android system image: the bionic linker is not part of the NDK sysroot.'
}

# --------------------------------------------------------------------------
# Reports
# --------------------------------------------------------------------------

function Write-BuildReport {
    param(
        [Parameter(Mandatory = $true)][string] $Target,
        [Parameter(Mandatory = $true)][string] $Artifact,
        [Parameter(Mandatory = $true)][string] $Sha256,
        [Parameter(Mandatory = $true)][int] $Size
    )
    $report = Join-Path (Split-Path -Parent $Artifact) 'build-report.txt'
    $lines = New-Object System.Collections.ArrayList
    [void]$lines.Add('proot-termux build report')
    [void]$lines.Add('generated: ' + $Script:BuildDate)
    [void]$lines.Add('host: ' + $env:COMPUTERNAME + ' (' + [System.Environment]::OSVersion.VersionString + ')')
    [void]$lines.Add('distribution: ' + $Distro + ' (user ' + $DistroUser + ')')
    [void]$lines.Add('kernel: ' + (Get-WslValue -Command 'uname' -Argument '-srm'))
    [void]$lines.Add('target: ' + $Target)
    [void]$lines.Add('artifact: ' + $Artifact)
    [void]$lines.Add('size: ' + $Size + ' bytes')
    [void]$lines.Add('sha256: ' + $Sha256)
    [void]$lines.Add('jobs: ' + $Jobs)
    if ($Target -eq 'android') {
        [void]$lines.Add('ndk: ' + $NdkUrl)
        [void]$lines.Add('ndk sha256: ' + $NdkSha256)
        [void]$lines.Add('talloc: ' + $TallocVersion + ' (' + $TallocUrl + ')')
        [void]$lines.Add('talloc sha256: ' + $TallocSha256)
        [void]$lines.Add('note: the binary needs a real device or an Android system image to run.')
    }
    [void]$lines.Add('log: ' + $Script:LogFile)
    $text = ($lines -join "`n") + "`n"
    [System.IO.File]::WriteAllText($report, $text, (New-Object System.Text.UTF8Encoding($false)))
    Write-Info ('Report: ' + $report)
}

function Write-Summary {
    Write-Step 'Summary'
    if ($Script:Results.Count -eq 0) {
        Write-Warn 'No artifact was produced.'
        return
    }
    $rows = @()
    foreach ($result in $Script:Results) {
        $rows += [pscustomobject]@{
            Target   = $result.Target
            Size     = $result.Size
            Sha256   = $result.Sha256
            Artifact = $result.Artifact
            Status   = $result.Status
        }
    }
    $rows | Format-Table -AutoSize | Out-String | Write-Host
    Write-Info ('Log: ' + $Script:LogFile)
    Write-Info ('Output: ' + $Script:OutputFull)
}

# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------

function Invoke-Main {
    $exitCode = 0
    try {
        if ([System.IO.Path]::IsPathRooted($OutputDir)) {
            $output = $OutputDir
        }
        else {
            $output = Join-Path $Script:RepoRoot $OutputDir
        }
        $Script:OutputFull = [System.IO.Path]::GetFullPath($output)
        New-Item -ItemType Directory -Path $Script:OutputFull -Force | Out-Null
        $Script:LogFile = Join-Path $Script:OutputFull ('build-windows-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')

        Write-Host ''
        Write-Host 'proot-termux build from Windows' -ForegroundColor White
        Write-Host ('  repository : ' + $Script:RepoRoot)
        Write-Host ('  output     : ' + $Script:OutputFull)
        Write-Host ('  log        : ' + $Script:LogFile)
        Write-Host ''

        if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
            $wslHome = Get-WslValue -Command 'printenv' -Argument 'HOME'
            if ([string]::IsNullOrWhiteSpace($wslHome)) { $wslHome = '/root' }
            $BuildRoot = $wslHome + '/proot-build'
        }
        $Script:BuildRoot = $BuildRoot
        $Script:WorkRoot = $Script:BuildRoot + '/work'

        Test-Preflight

        $prepare = @(
            'set -euo pipefail',
            'mkdir -p __WORKROOT__',
            'echo WORKROOT_READY'
        ) -join "`n"
        $prepare = New-Template -Template $prepare -Values @{ '__WORKROOT__' = $Script:WorkRoot }
        Invoke-Wsl -Script $prepare -Name 'prepare' | Out-Null

        Install-Dependencies

        if ($Target -eq 'linux' -or $Target -eq 'all') {
            Invoke-LinuxTarget
        }
        if ($Target -eq 'android' -or $Target -eq 'all') {
            Invoke-AndroidTarget
        }

        foreach ($result in $Script:Results) {
            Write-BuildReport -Target $result.Target -Artifact $result.Artifact -Sha256 $result.Sha256 -Size $result.Size
        }
        Write-Summary
    }
    catch {
        Write-Host ''
        Write-Fail $_.Exception.Message
        if ($_.InvocationInfo -and $_.InvocationInfo.ScriptLineNumber) {
            Write-Host ('  at line ' + $_.InvocationInfo.ScriptLineNumber) -ForegroundColor DarkGray
        }
        $exitCode = 1
    }
    exit $exitCode
}

Invoke-Main