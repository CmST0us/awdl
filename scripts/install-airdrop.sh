#!/bin/sh
set -eu
exec "$(dirname "$0")/aware-build" install-airdrop "$@"
