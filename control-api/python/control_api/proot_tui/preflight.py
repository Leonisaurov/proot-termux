"""Fail-fast checks for the interactive application."""
from __future__ import annotations

import shutil


def _version_tuple(value):
    try:
        return tuple(int(part) for part in value.split('.')[:2])
    except (AttributeError, ValueError):
        return (0, 0)


class PreflightError(RuntimeError):
    pass


def check(config):
    if shutil.which(config.proot_path) is None and not config.proot_path.startswith('/'):
        raise PreflightError(f"PRoot no encontrado: {config.proot_path}")
    try:
        import textual
        raw_version = getattr(textual, '__version__', '0')
        version = _version_tuple(raw_version)
        if not ((7, 5) <= version < (9, 0)):
            raise PreflightError(
                f"Textual incompatible ({raw_version}); usa una versión de Textual "
                "compatible con el paquete disponible en Termux (>=7.5,<9)")
    except ImportError as exc:
        raise PreflightError(
            "Falta Textual en el Python de Termux; instala el paquete Textual "
            "ofrecido por pacman antes de ejecutar el harness") from exc
