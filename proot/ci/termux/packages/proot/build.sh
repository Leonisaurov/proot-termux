# Source is maintained directly in proot/src/ (no patches)
# To modify: edit files in proot/src/, then bump TERMUX_PKG_REVISION
TERMUX_PKG_HOMEPAGE=https://proot-me.github.io/
TERMUX_PKG_DESCRIPTION="Emulate chroot, bind mount and binfmt_misc for non-root users"
TERMUX_PKG_LICENSE="GPL-2.0"
TERMUX_PKG_MAINTAINER="@leonisaurov"
TERMUX_PKG_VERSION="5.1.107.89"
TERMUX_PKG_REVISION=93
TERMUX_PKG_SKIP_SRC_EXTRACT=true
TERMUX_PKG_DEPENDS="libtalloc"
TERMUX_PKG_SUGGESTS="proot-distro"
TERMUX_PKG_BUILD_IN_SRC=true
TERMUX_PKG_EXTRA_MAKE_ARGS="-C src"

# Install loader in libexec instead of extracting it every time
export PROOT_UNBUNDLE_LOADER=$TERMUX_PREFIX/libexec/proot

termux_step_pre_configure() {
	local proot_source_dir="$TERMUX_PKG_BUILDER_DIR/../../../../src"
	if [ -d "$proot_source_dir" ]; then
		mkdir -p "$TERMUX_PKG_SRCDIR/src"
		rsync -ac --exclude=.git "$proot_source_dir/" "$TERMUX_PKG_SRCDIR/src/"
	else
		termux_error_exit "proot source directory not found at $proot_source_dir"
	fi
	CPPFLAGS+=" -DARG_MAX=131072 -DVERSION=\\\"${TERMUX_PKG_VERSION}\\\""
}

termux_step_post_make_install() {
	if [[ -f $TERMUX_PKG_SRCDIR/doc/proot/man.1 ]]; then
		install -Dm644 $TERMUX_PKG_SRCDIR/doc/proot/man.1 $TERMUX_PREFIX/share/man/man1/proot.1
	fi

	sed -e "s|@TERMUX_PREFIX@|$TERMUX_PREFIX|g" \
		$TERMUX_PKG_BUILDER_DIR/termux-chroot \
		> $TERMUX_PREFIX/bin/termux-chroot
	chmod 700 $TERMUX_PREFIX/bin/termux-chroot
}
