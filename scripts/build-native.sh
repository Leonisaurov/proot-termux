#!/bin/bash
set -euo pipefail

# === CONSTANTES ===
SCRIPTDIR=$(cd "$(realpath "$(dirname "$0")")"; pwd)
PROJECT_DIR=$(cd "$SCRIPTDIR/.."; pwd)
readonly SOURCE_DIR="$PROJECT_DIR/proot-source/src"
readonly TEMPLATE_CHROOT="$PROJECT_DIR/packages/proot/termux-chroot"

readonly PKG_NAME="proot"
readonly PKG_VERSION="5.1.107.87"
readonly PKG_REVISION=16
readonly PKG_ARCH="aarch64"
readonly PKG_FULLVER="${PKG_VERSION}-${PKG_REVISION}"
readonly PKG_FILENAME="${PKG_NAME}-${PKG_FULLVER}-${PKG_ARCH}.pkg.tar.xz"

# === VARIABLES CONFIGURABLES ===
JOBS="${JOBS:-2}"
INSTALL="${INSTALL:-false}"
CLEAN="${CLEAN:-false}"
SKIP_BUILD="${SKIP_BUILD:-false}"
SKIP_PACKAGE="${SKIP_PACKAGE:-false}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"

# === FUNCIONES ===

_show_usage() {
    cat << 'EOF'
Usage: ./scripts/build-native.sh [OPTIONS]

Build proot natively in Termux and optionally create a .pkg.tar.xz package.

Options:
  -j, --jobs N         Number of parallel make jobs (default: 2, safe for OOM)
  -i, --install        Install built binaries to $PREFIX (default: false)
  -c, --clean          Run make clean before building (default: false)
  --skip-build         Skip compilation, only create package from existing files
  --skip-package       Build only, don't create .pkg.tar.xz
  -h, --help           Show this help message

Environment variables:
  JOBS                 Same as --jobs
  INSTALL              Same as --install (set to "true")
  CLEAN                Same as --clean (set to "true")

Examples:
  ./scripts/build-native.sh                    # Build + package
  ./scripts/build-native.sh -j4                # Build with 4 jobs
  ./scripts/build-native.sh -i -c              # Clean build + install to system
  ./scripts/build-native.sh --skip-build       # Package from existing install
EOF
    exit 0
}

_validate_deps() {
    local missing=0
    for cmd in make gcc bsdtar xz sed; do
        if ! command -v "$cmd" &>/dev/null; then
            echo "ERROR: '$cmd' not found. Install it with: pkg install <package>"
            missing=1
        fi
    done
    # Special: gcc may be clang in Termux
    if ! command -v gcc &>/dev/null; then
        echo "ERROR: 'gcc' not found. Install clang: pkg install clang"
        missing=1
    fi
    # Check libraries
    if ! ldconfig -p 2>/dev/null | grep -q libtalloc; then
        if [ ! -f "$PREFIX/lib/libtalloc.so" ]; then
            echo "WARNING: libtalloc not found. Install: pkg install libtalloc"
        fi
    fi
    return $missing
}

_build() {
    echo ""
    echo "========================================"
    echo "  Building proot natively..."
    echo "  Jobs: $JOBS"
    echo "  Clean: $CLEAN"
    echo "========================================"
    echo ""

    cd "$SOURCE_DIR"

    if [ "$CLEAN" = "true" ]; then
        echo ">>> make clean..."
        make clean 2>/dev/null || true
        rm -f proot loader/loader loader/loader-m32 2>/dev/null || true
    fi

    # Export build variables
    export CC=gcc
    export CPPFLAGS="-D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -I. -I. -DARG_MAX=131072 -DVERSION=\"${PKG_VERSION}\""
    # Flags de optimización agresiva (-O3 + LTO + sections + NDEBUG):
    export CFLAGS="-Wall -Wextra -O3 -flto -fvisibility=hidden -ffunction-sections -fdata-sections -DNDEBUG"

    # LDFLAGS con LTO en linkeo, gc-sections e ICF:
    export LDFLAGS="-ltalloc -flto -Wl,-z,noexecstack -Wl,--gc-sections -Wl,--icf=safe"
    export PROOT_UNBUNDLE_LOADER="$PREFIX/libexec/proot"

    echo ">>> Compiling with make -j${JOBS}..."
    echo "    CC=gcc"
    echo "    CPPFLAGS=$CPPFLAGS"
    echo "    CFLAGS=$CFLAGS"
    echo "    LDFLAGS=$LDFLAGS"
    echo ""

    # Memory check before build
    local mem_avail
    mem_avail=$(free -m | awk '/^Mem:/ {print $7}')
    echo "    Memory available: ${mem_avail}MB"
    echo ""

    make -j"$JOBS" V=1

    echo ""
    echo ">>> Build complete!"

    # Verify
    if [ ! -f proot ]; then
        echo "ERROR: proot binary not found after build!"
        exit 1
    fi

    local size
    size=$(du -h proot | cut -f1)
    echo "    Binary: $size"
    file proot | head -1
}

_install() {
    echo ""
    echo "========================================"
    echo "  Installing proot to $PREFIX"
    echo "========================================"
    echo ""

    cd "$SOURCE_DIR"

    # Install proot binary
    echo ">>> Installing proot..."
    install -Dm700 proot "$PREFIX/bin/proot"

    # Install loaders (rename loader-m32 → loader32 per makefile convention)
    echo ">>> Installing loaders..."
    mkdir -p "$PREFIX/libexec/proot"
    if [ -f loader/loader ]; then
        install -m700 loader/loader "$PREFIX/libexec/proot/loader"
    fi
    if [ -f loader/loader-m32 ]; then
        install -m700 loader/loader-m32 "$PREFIX/libexec/proot/loader32"
    elif [ -f "$PREFIX/libexec/proot/loader32" ]; then
        :  # already exists
    fi

    # Install termux-chroot script
    if [ -f "$TEMPLATE_CHROOT" ]; then
        echo ">>> Installing termux-chroot..."
        sed "s|@TERMUX_PREFIX@|$PREFIX|g" "$TEMPLATE_CHROOT" > "$PREFIX/bin/termux-chroot"
        chmod 700 "$PREFIX/bin/termux-chroot"
    fi

    echo ">>> Install complete!"
    ls -la "$PREFIX/bin/proot"
}

_create_package() {
    echo ""
    echo "========================================"
    echo "  Creating .pkg.tar.xz package"
    echo "========================================"
    echo ""

    local PKG_DIR
    PKG_DIR=$(mktemp -d "$TMPDIR/proot-pkg.XXXXXX")
    local OUTPUT_FILE="$PROJECT_DIR/$PKG_FILENAME"
    local SOURCE_DATE_EPOCH
    SOURCE_DATE_EPOCH=$(date +%s)

    # Create directory structure
    mkdir -p "$PKG_DIR/data/data/com.termux/files/usr/bin"
    mkdir -p "$PKG_DIR/data/data/com.termux/files/usr/libexec/proot"
    mkdir -p "$PKG_DIR/data/data/com.termux/files/usr/share/man/man1"
    mkdir -p "$PKG_DIR/data/data/com.termux/files/usr/share/doc/proot"

    # Copy proot
    echo ">>> Copying proot binary..."
    cp -a "$SOURCE_DIR/proot" "$PKG_DIR/data/data/com.termux/files/usr/bin/proot"
    chmod 700 "$PKG_DIR/data/data/com.termux/files/usr/bin/proot"

    # Copy loaders
    echo ">>> Copying loaders..."
    if [ -f "$SOURCE_DIR/loader/loader" ]; then
        cp -a "$SOURCE_DIR/loader/loader" "$PKG_DIR/data/data/com.termux/files/usr/libexec/proot/loader"
    fi
    if [ -f "$SOURCE_DIR/loader/loader-m32" ]; then
        cp -a "$SOURCE_DIR/loader/loader-m32" "$PKG_DIR/data/data/com.termux/files/usr/libexec/proot/loader32"
    fi
    chmod 700 "$PKG_DIR/data/data/com.termux/files/usr/libexec/proot/"* 2>/dev/null || true

    # Generate termux-chroot
    echo ">>> Generating termux-chroot..."
    if [ -f "$TEMPLATE_CHROOT" ]; then
        sed "s|@TERMUX_PREFIX@|$PREFIX|g" "$TEMPLATE_CHROOT" \
            > "$PKG_DIR/data/data/com.termux/files/usr/bin/termux-chroot"
        chmod 700 "$PKG_DIR/data/data/com.termux/files/usr/bin/termux-chroot"
    fi

    # Man page (if exists in source)
    local MAN_SOURCE="$SOURCE_DIR/doc/proot/man.1"
    if [ -f "$MAN_SOURCE" ]; then
        echo ">>> Compressing man page..."
        gzip -c "$MAN_SOURCE" > "$PKG_DIR/data/data/com.termux/files/usr/share/man/man1/proot.1.gz"
    else
        echo ">>> Man page not found, skipping..."
    fi

    # Copyright symlink
    echo ">>> Creating copyright symlink..."
    ln -sf ../../LICENSES/GPL-2.0.txt \
        "$PKG_DIR/data/data/com.termux/files/usr/share/doc/proot/copyright"

    # Calculate install size
    cd "$PKG_DIR"
    local INSTALLSIZE
    INSTALLSIZE=$(du -bs data | cut -f 1)
    echo ">>> Install size: $INSTALLSIZE bytes"

    # Create .PKGINFO
    echo ">>> Creating .PKGINFO..."
    cat > .PKGINFO << PKGEOF
pkgname = proot
pkgbase = proot
pkgver = ${PKG_FULLVER}
pkgdesc = Emulate chroot, bind mount and binfmt_misc for non-root users
url = https://proot-me.github.io/
builddate = ${SOURCE_DATE_EPOCH}
packager = @leonisaurov
size = ${INSTALLSIZE}
arch = ${PKG_ARCH}
license = GPL-2.0
depend = libandroid-shmem
depend = libtalloc
optdepend = proot-distro
PKGEOF

    # Create .BUILDINFO
    echo ">>> Creating .BUILDINFO..."
    cat > .BUILDINFO << BUIEOF
format = 2
pkgname = proot
pkgbase = proot
pkgver = ${PKG_FULLVER}
pkgarch = ${PKG_ARCH}
packager = @leonisaurov
builddate = ${SOURCE_DATE_EPOCH}
BUIEOF

    # Unify timestamps
    echo ">>> Setting timestamps..."
    find . -exec touch -h -d @$SOURCE_DATE_EPOCH {} +

    # Create .MTREE
    echo ">>> Creating .MTREE..."
    shopt -s dotglob globstar
    printf '%s\0' **/* | bsdtar -cnf - --format=mtree \
        --options='!all,use-set,type,uid,gid,mode,time,size,md5,sha256,link' \
        --null --files-from - --exclude .MTREE 2>/dev/null | \
        gzip -c -f -n > .MTREE || {
        echo "WARNING: .MTREE creation failed (non-critical), continuing..."
        touch .MTREE
    }
    touch -d @$SOURCE_DATE_EPOCH .MTREE

    # Create .pkg.tar.xz
    echo ">>> Creating $PKG_FILENAME..."
    rm -f "$OUTPUT_FILE" 2>/dev/null || true
    printf '%s\0' **/* | bsdtar --no-fflags -cnf - --null --files-from - 2>/dev/null | \
        xz -c -z - > "$OUTPUT_FILE" || {
        # Fallback to tar + xz
        printf '%s\0' **/* | tar --no-null-fflags -cnf - --null --files-from - 2>/dev/null | \
            xz -c -z - > "$OUTPUT_FILE"
    }
    shopt -u dotglob globstar

    # Cleanup
    rm -rf "$PKG_DIR"

    echo ""
    echo ">>> Package created: $OUTPUT_FILE"
    ls -la "$OUTPUT_FILE"
}

# === MAIN ===

# Parse arguments
while (( $# != 0 )); do
    case "$1" in
        -h|--help) _show_usage;;
        -j|--jobs) JOBS="$2"; shift 2;;
        -i|--install) INSTALL="true"; shift 1;;
        -c|--clean) CLEAN="true"; shift 1;;
        --skip-build) SKIP_BUILD="true"; shift 1;;
        --skip-package) SKIP_PACKAGE="true"; shift 1;;
        -*) echo "Error: Unknown option '$1'"; exit 1;;
        *) break;;
    esac
done

echo "========================================"
echo "  proot native build script"
echo "  Version: ${PKG_VERSION}-${PKG_REVISION}"
echo "========================================"

# Validate
_validate_deps || exit 1

# Build
if [ "$SKIP_BUILD" != "true" ]; then
    _build
    _install
fi

# Package
if [ "$SKIP_PACKAGE" != "true" ]; then
    _create_package
fi

echo ""
echo "========================================"
echo "  Done!"
echo "========================================"
echo ""
echo "  Binary: $PREFIX/bin/proot"
echo "  Package: $PROJECT_DIR/$PKG_FILENAME"
echo ""
echo "  Install the package with:"
echo "    pacman -U $PROJECT_DIR/$PKG_FILENAME"
echo ""
