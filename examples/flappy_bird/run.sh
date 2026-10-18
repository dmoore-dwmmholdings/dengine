#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bundle="$root/build/dev/examples/flappy_bird/sky_hop.app"
if [ "$(uname -s)" = Darwin ] && [ -d "$bundle" ]; then
    if [ "$#" -eq 0 ]; then
        # Launch Services registers the bundle as a normal controllable native application.
        exec /usr/bin/open -n -W "$bundle"
    fi
    # Keep command-line smoke/check exit codes and stdout directly observable.
    exec "$bundle/Contents/MacOS/sky_hop" "$@"
fi
exec "$root/build/dev/examples/flappy_bird/sky_hop" "$@"
