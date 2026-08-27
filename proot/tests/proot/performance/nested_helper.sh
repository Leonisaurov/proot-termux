#!/usr/bin/env sh
set -eu

depth=$1
shift
if [ "$depth" -le 1 ]; then
	exec "$@"
fi

exec "$PROOT" --bind="$ROOT" --bind="$TMPDIR" --cwd="$ROOT" \
	"$PREFIX/bin/sh" "$0" "$((depth - 1))" "$@"
