"""Explicit harness presets.  Nothing here is a PRoot default."""
from __future__ import annotations

import argparse
import os
from dataclasses import dataclass, field

from .. import ProotConfig
from ..termux_paths import add_termux_runtime_binds


@dataclass(frozen=True)
class Binding:
    host: str
    guest: str
    mode: str = 'ro'

    def argument(self):
        return f'{self.host}:{self.guest}:{self.mode}'


@dataclass
class HarnessConfig:
    proot_path: str = 'proot'
    termux_paths: bool = True
    rootfs: str | None = None
    binds: list[Binding] = field(default_factory=list)
    rw_dirs: list[str] = field(default_factory=list)
    proxy: str | None = None
    proc_isolated: bool = False
    with_storage: bool = False
    net_policy: str = 'deny'
    shell: str | None = None
    command: tuple[str, ...] = ()
    cwd: str | None = None

    def summary(self):
        return {
            'mode': 'termux-paths' if self.termux_paths else 'rootfs',
            'rootfs': self.rootfs or '(Termux prefix)',
            'bindings': [b.argument() for b in self.binds],
            'rw_dirs': list(self.rw_dirs),
            'proxy': self.proxy or '(none)',
            'proc_isolated': self.proc_isolated,
            'storage': 'enabled' if self.with_storage else 'masked',
            'net_policy': self.net_policy,
        }


def parse_bind(value: str) -> Binding:
    parts = value.split(':')
    if len(parts) not in (2, 3) or not parts[0].startswith('/') or not parts[1].startswith('/'):
        raise ValueError('bind must be HOST:GUEST[:ro|rw|mask]')
    mode = parts[2] if len(parts) == 3 else 'ro'
    if mode not in ('ro', 'rw', 'mask'):
        raise ValueError('bind mode must be ro, rw, or mask')
    return Binding(parts[0], parts[1], mode)


def default_config(**overrides):
    prefix = os.environ.get('PREFIX', '/data/data/com.termux/files/usr')
    home = os.environ.get('HOME', prefix + '/home')
    cfg = HarnessConfig(
        binds=[Binding(prefix, prefix, 'ro'), Binding(home, home, 'ro')],
    )
    if cfg.termux_paths:
        add_termux_runtime_binds(cfg)
    for key, value in overrides.items():
        if value is not None:
            setattr(cfg, key, value)
    return cfg


def build_config(config: HarnessConfig) -> ProotConfig:
    """Translate a resolved harness config into generic PRoot arguments."""
    args = []
    # ``--termux-paths`` is a termux-isolated launcher option, not a PRoot
    # option.  Here the mode is represented by explicit bindings instead.
    if not config.termux_paths and config.rootfs:
        args += ['-r', config.rootfs]
    elif not config.termux_paths:
        raise ValueError('rootfs mode requires --rootfs')
    args += ['--net-policy', config.net_policy, '--net-allow', '*']
    for binding in config.binds:
        args += ['-b', binding.argument()]
    for path in config.rw_dirs:
        args += ['--rw-dir', path]
    if not config.with_storage:
        # Mask only storage mountpoints.  Masking /data would also hide the
        # Termux prefix because it lives below /data/data/... .
        host_home = os.environ.get('HOME', '/data/data/com.termux/files/home')
        if config.termux_paths:
            storage_home = host_home + '/storage'
        else:
            storage_home = '/home/storage'
        args += ['-b', f'{host_home}/storage:{storage_home}:mask',
                 '-b', '/storage/emulated/0:/storage/emulated/0:mask',
                 '-b', '/storage/self/primary:/storage/self/primary:mask',
                 '-b', '/storage/emulated/0:/sdcard:mask']
    args += ['-b', '/dev:/dev:ro']
    if config.proxy:
        args += ['--proxy', config.proxy]
    if config.proc_isolated:
        args.append('--proc-isolated')
    shell = config.shell or os.environ.get('SHELL') or '/bin/sh'
    guest_command = tuple(config.command or (shell,))
    env = dict(os.environ)
    # Keep PRoot infrastructure in Termux's host-writable tmpdir.
    # Otherwise internal scratch paths become guest path requests.
    termux_tmp = env.get('TMPDIR', '/data/data/com.termux/files/usr/tmp')
    env.setdefault('TMPDIR', termux_tmp)
    env.setdefault('PROOT_TMP_DIR', termux_tmp)
    env.setdefault('PROOT_RUNTIME_DIR', termux_tmp)
    env['TERM'] = 'xterm-kitty'
    env.setdefault('COLORTERM', 'truecolor')
    return ProotConfig(proot_path=config.proot_path, args=tuple(args),
                       guest_command=guest_command, env=env, cwd=config.cwd)


def argument_parser():
    p = argparse.ArgumentParser(description='Interactive PRCT Textual harness')
    p.add_argument('--proot', dest='proot_path', default='proot')
    p.add_argument('--termux-paths', action='store_true', default=None)
    p.add_argument('--rootfs')
    p.add_argument('--bind', action='append', type=parse_bind, default=[])
    p.add_argument('--rw-dir', action='append', default=[])
    p.add_argument('--proxy')
    p.add_argument('--proc-isolated', action='store_true', default=None)
    p.add_argument('--no-proc-isolated', action='store_false', dest='proc_isolated')
    p.add_argument('--with-storage', action='store_true')
    p.add_argument('--shell')
    p.add_argument('command', nargs=argparse.REMAINDER)
    return p


def config_from_args(argv=None):
    ns = argument_parser().parse_args(argv)
    values = vars(ns)
    command = tuple(values.pop('command'))
    if command and command[0] == '--': command = command[1:]
    values['command'] = command
    if values.get('rootfs'):
        values['termux_paths'] = False
    return default_config(**values)
