#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$root/build/dev/examples/flappy_bird/sky_hop" "$@"
