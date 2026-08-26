#!/usr/bin/env python3
"""Declarative PRoot launcher used by the ``proot-exec`` command.

The configuration format is TOML.  Values are converted directly to an
argv; no shell is involved when the guest command is executed.
"""
from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping

try:
    import tomllib
except ModuleNotFoundError as exc:  # pragma: no cover - Termux has 3.11+
    raise RuntimeError("proot-exec requires Python 3.11 or newer") from exc


class ConfigError(ValueError):
    """A configuration cannot be represented safely as a PRoot command."""


_VAR = re.compile(r"\$(?:\{(?P<braced>[A-Za-z_][A-Za-z0-9_]*)\}|(?P<plain>[A-Za-z_][A-Za-z0-9_]*))")
_MODES = {"ro", "rw", "wo", "mask"}


def _string(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise ConfigError(f"{name} must be a non-empty string")
    return value


def _bool(value: Any, name: str, default: bool = False) -> bool:
    if value is None:
        return default
    if not isinstance(value, bool):
        raise ConfigError(f"{name} must be boolean")
    return value


def _expand(value: str, variables: Mapping[str, str], name: str) -> str:
    """Expand only $NAME/${NAME}; fail on an unset variable."""

    def replace(match: re.Match[str]) -> str:
        key = match.group("braced") or match.group("plain")
        if key not in variables:
            raise ConfigError(f"{name} references unset variable ${key}")
        return variables[key]

    return _VAR.sub(replace, value)


def _expanded(value: Any, variables: Mapping[str, str], name: str) -> str:
    return _expand(_string(value, name), variables, name)


def _host_path(value: Any, variables: Mapping[str, str], name: str, base: Path) -> str:
    path = Path(_expanded(value, variables, name)).expanduser()
    if not path.is_absolute():
        path = base / path
    return str(path)


def _executable(value: Any, variables: Mapping[str, str], name: str, base: Path) -> str:
    raw = _expanded(value, variables, name)
    if "/" not in raw:
        return raw
    return _host_path(raw, variables, name, base)


def _guest_path(value: Any, variables: Mapping[str, str], name: str) -> str:
    path = _expanded(value, variables, name)
    if not path.startswith("/") or "\0" in path:
        raise ConfigError(f"{name} must be an absolute guest path")
    return path


@dataclass(frozen=True)
class Binding:
    host: str
    guest: str
    mode: str | None = None

    def argument(self) -> str:
        value = f"{self.host}:{self.guest}"
        return f"{value}:{self.mode}" if self.mode else value


@dataclass(frozen=True)
class ExecConfig:
    proot: str
    rootfs: str | None
    cwd: str | None
    binds: tuple[Binding, ...]
    proxy: str | None
    net_policy: str | None
    net_allow: tuple[str, ...]
    control_fd: int | None
    flags: tuple[str, ...]
    extra_args: tuple[str, ...]
    command: tuple[str, ...]
    environment: Mapping[str, str]
    inherit_environment: bool
    launcher_cwd: str | None
    config_dir: Path

    @classmethod
    def from_file(cls, filename: str | os.PathLike[str]) -> "ExecConfig":
        config_path = Path(filename).expanduser().resolve()
        try:
            with config_path.open("rb") as stream:
                raw = tomllib.load(stream)
        except OSError as exc:
            raise ConfigError(f"cannot read {config_path}: {exc}") from exc
        except tomllib.TOMLDecodeError as exc:
            raise ConfigError(f"invalid TOML in {config_path}: {exc}") from exc

        section = raw.get("proot", raw)
        if not isinstance(section, dict):
            raise ConfigError("[proot] must be a table")
        base = config_path.parent
        variables = {key: str(value) for key, value in os.environ.items()}

        proot = _executable(section.get("executable", section.get("proot", "proot")), variables, "proot.executable", base)
        rootfs_value = section.get("rootfs")
        rootfs = _host_path(rootfs_value, variables, "proot.rootfs", base) if rootfs_value is not None else None
        cwd_value = section.get("cwd")
        cwd = _guest_path(cwd_value, variables, "proot.cwd") if cwd_value is not None else None

        raw_binds = raw.get("binds", section.get("binds", []))
        if not isinstance(raw_binds, list):
            raise ConfigError("binds must be an array of tables")
        binds: list[Binding] = []
        for index, item in enumerate(raw_binds):
            if not isinstance(item, dict):
                raise ConfigError(f"binds[{index}] must be a table")
            host = _host_path(item.get("host"), variables, f"binds[{index}].host", base)
            guest = _guest_path(item.get("guest"), variables, f"binds[{index}].guest")
            mode = item.get("mode")
            if mode is not None:
                mode = _string(mode, f"binds[{index}].mode").lower()
                if mode not in _MODES:
                    raise ConfigError(f"binds[{index}].mode must be one of {sorted(_MODES)}")
            binds.append(Binding(host, guest, mode))

        proxy_value = section.get("proxy")
        proxy = _expanded(proxy_value, variables, "proot.proxy") if proxy_value not in (None, "") else None
        policy_value = section.get("net_policy")
        net_policy = _expanded(policy_value, variables, "proot.net_policy") if policy_value is not None else None
        if net_policy is not None and net_policy not in {"off", "deny", "allow"}:
            raise ConfigError("proot.net_policy must be off, deny, or allow")

        raw_allow = section.get("net_allow", [])
        if not isinstance(raw_allow, list) or not all(isinstance(x, str) for x in raw_allow):
            raise ConfigError("proot.net_allow must be an array of strings")
        net_allow = tuple(_expand(x, variables, f"proot.net_allow[{i}]") for i, x in enumerate(raw_allow))

        control_value = section.get("control_fd")
        if isinstance(control_value, str):
            control_value = _expand(control_value, variables, "proot.control_fd")
        if control_value is None or control_value in {"", "none", "off"}:
            control_fd = None
        else:
            try:
                control_fd = int(control_value)
            except (TypeError, ValueError) as exc:
                raise ConfigError("proot.control_fd must be an integer or none") from exc
            if control_fd < 0:
                raise ConfigError("proot.control_fd must be non-negative")

        flags = section.get("flags", [])
        extra_args = section.get("extra_args", [])
        for name, values in (("proot.flags", flags), ("proot.extra_args", extra_args)):
            if not isinstance(values, list) or not all(isinstance(x, str) and x for x in values):
                raise ConfigError(f"{name} must be an array of non-empty strings")
        flags_tuple = tuple(_expand(x, variables, "proot.flags") for x in flags)
        extra_tuple = tuple(_expand(x, variables, "proot.extra_args") for x in extra_args)

        raw_command = raw.get("command")
        if isinstance(raw_command, dict):
            raw_command = raw_command.get("argv", raw_command.get("cmd"))
        elif raw_command is None:
            raw_command = raw.get("cmd")
        if raw_command is None:
            raw_command = section.get("command", section.get("cmd", ["/bin/sh"]))
        if not isinstance(raw_command, list) or not raw_command or not all(isinstance(x, str) and x for x in raw_command):
            raise ConfigError("command must be a non-empty array of strings")
        command = tuple(_expand(x, variables, f"command[{i}]") for i, x in enumerate(raw_command))

        raw_env = raw.get("env", section.get("env", {}))
        if not isinstance(raw_env, dict):
            raise ConfigError("env must be a table")
        configured_env: dict[str, str] = {}
        for key, value in raw_env.items():
            if not isinstance(key, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
                raise ConfigError(f"invalid environment variable name: {key!r}")
            configured_env[key] = _expand(_string(value, f"env.{key}"), {**variables, **configured_env}, f"env.{key}")

        launcher_value = section.get("launcher_cwd")
        launcher_cwd = _host_path(launcher_value, variables, "proot.launcher_cwd", base) if launcher_value is not None else None
        return cls(
            proot=proot, rootfs=rootfs, cwd=cwd, binds=tuple(binds),
            proxy=proxy, net_policy=net_policy,
            net_allow=net_allow, control_fd=control_fd,
            flags=flags_tuple, extra_args=extra_tuple, command=command,
            environment=configured_env,
            inherit_environment=_bool(section.get("inherit_environment"), "proot.inherit_environment", True),
            launcher_cwd=launcher_cwd, config_dir=base,
        )

    def argv(self, command_override: list[str] | None = None) -> list[str]:
        args = [self.proot]
        if self.rootfs is not None:
            args += [f"--rootfs={self.rootfs}"]
        if self.cwd is not None:
            args += [f"--cwd={self.cwd}"]
        for bind in self.binds:
            args += [f"--bind={bind.argument()}"]
        if self.proxy is not None:
            args += ["--proxy", self.proxy]
        if self.net_policy is not None:
            args += ["--net-policy", self.net_policy]
        for destination in self.net_allow:
            args += ["--net-allow", destination]
        if self.control_fd is not None:
            args += ["--control-fd", str(self.control_fd)]
        args += list(self.flags) + list(self.extra_args)
        args += list(command_override if command_override is not None else self.command)
        return args

    def environment_for_exec(self) -> dict[str, str]:
        env = dict(os.environ) if self.inherit_environment else {}
        env.update(self.environment)
        return env

    def validate_runtime(self) -> None:
        if self.control_fd is not None:
            try:
                os.fstat(self.control_fd)
            except OSError as exc:
                raise ConfigError(f"control fd {self.control_fd} is not open: {exc}") from exc
        if self.rootfs is not None and not os.path.isdir(self.rootfs):
            raise ConfigError(f"rootfs is not a directory: {self.rootfs}")
        if self.launcher_cwd is not None and not os.path.isdir(self.launcher_cwd):
            raise ConfigError(f"launcher_cwd is not a directory: {self.launcher_cwd}")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Build and execute a PRoot command from TOML")
    parser.add_argument("-c", "--config", default="proot-exec.conf", help="TOML configuration file")
    parser.add_argument("--print", action="store_true", dest="print_command", help="print the generated argv and environment")
    parser.add_argument("--dry-run", action="store_true", help="validate and print without executing")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="optional guest command override after --")
    return parser


def main(argv: list[str] | None = None) -> int:
    ns = _parser().parse_args(argv)
    try:
        config = ExecConfig.from_file(ns.config)
        override = list(ns.command)
        if override and override[0] == "--":
            override = override[1:]
        command = override or None
        config.validate_runtime()
        args = config.argv(command)
        env = config.environment_for_exec()
        if ns.print_command or ns.dry_run:
            print("argv:", shlex.join(args))
            print("launcher_cwd:", config.launcher_cwd or os.getcwd())
            print("inherit_environment:", config.inherit_environment)
            print("environment_overrides:", shlex.join(
                f"{k}={v}" for k, v in sorted(config.environment.items())))
        if ns.dry_run:
            return 0
        if ns.print_command:
            return 0
        pass_fds = (config.control_fd,) if config.control_fd is not None else ()
        completed = subprocess.run(
            args, env=env, cwd=config.launcher_cwd, pass_fds=pass_fds
        )
        return completed.returncode
    except (ConfigError, OSError, subprocess.SubprocessError) as exc:
        print(f"proot-exec: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
