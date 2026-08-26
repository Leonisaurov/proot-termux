from __future__ import annotations

import sys

from .app import run_app
from .presets import config_from_args
from .preflight import PreflightError, check


def main(argv=None):
    config = config_from_args(sys.argv[1:] if argv is None else argv)
    if not config.termux_paths and not config.rootfs:
        raise SystemExit('--rootfs is required when --termux-paths is disabled')
    try:
        check(config)
    except PreflightError as exc:
        raise SystemExit(f'preflight: {exc}')
    print('PRCT harness configuration:', config.summary(), file=sys.stderr)
    run_app(config)


if __name__ == '__main__':
    main()
