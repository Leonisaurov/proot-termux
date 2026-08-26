"""Read-only Android runtime bindings used by the Termux-paths preset.

This is a binding helper, not an authorization policy. The paths are mounted
before PRoot starts, so they do not consume PRCT decisions or control-fd rules.
"""
from __future__ import annotations

import os


# Android releases add/remove APEX modules and vendor libraries. Binding the
# runtime trees, instead of a fixed list of files, keeps the linker usable
# across API levels while retaining read-only access.
TERMUX_RUNTIME_BIND_PATHS = (
    "/system",
    "/system_ext",
    "/product",
    "/vendor",
    "/odm",
    "/apex",
    "/linkerconfig",
    "/dev",
    "/sys",
    "/proc",
)

# Some Android builds expose linker configuration as a file below a protected
# mount point whose parent directory cannot be stat'ed by the app.  Check these
# individually and bind the file when the app can read it.
# The Termux package prefix is read-only in the default preset, but its
# canonical temporary directory must be writable for sockets, editor state,
# and other runtime files.  Keep this narrow instead of making all of /data rw.
TERMUX_RUNTIME_RW_PATHS = (
    os.environ.get('TMPDIR', '/data/data/com.termux/files/usr/tmp'),
)

TERMUX_RUNTIME_BIND_FILES = (
    "/linkerconfig/ld.config.txt",
    "/linkerconfig/com.android.art/ld.config.txt",
    "/system/etc/ld.config.arm64.txt",
)


def termux_runtime_bindings(*, require_existing=True):
    """Return existing ``(host, guest, mode)`` Android runtime bindings."""
    result = []
    for path in TERMUX_RUNTIME_BIND_PATHS:
        if require_existing and not os.path.exists(path):
            continue
        result.append((path, path, "ro"))
    for path in TERMUX_RUNTIME_RW_PATHS:
        if require_existing and not os.path.isdir(path):
            continue
        result.append((path, path, "rw"))
    for path in TERMUX_RUNTIME_BIND_FILES:
        if require_existing and not os.path.isfile(path):
            continue
        result.append((path, path, "ro"))
    return tuple(result)


def add_termux_runtime_binds(config, *, require_existing=True):
    """Add standard Android runtime bindings to a mutable harness config.

    The config is duck-typed and must expose a mutable ``binds`` list. Existing
    bindings are preserved and duplicates are not added.
    """
    from .proot_tui.presets import Binding

    existing = {(b.host, b.guest, b.mode) for b in config.binds}
    for host, guest, mode in termux_runtime_bindings(
            require_existing=require_existing):
        key = (host, guest, mode)
        if key not in existing:
            config.binds.append(Binding(host, guest, mode))
            existing.add(key)
    return config
