#!/bin/sh
exec "$(dirname -- "$0")/aware-build" install-driver "$@"
