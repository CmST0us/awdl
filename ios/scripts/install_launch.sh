#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
if [[ -n "${AWARE_LOCAL_ENV:-}" ]]; then source "$AWARE_LOCAL_ENV"; fi
: "${AWARE_DEVICE_UDID:?}"
export AWARE_DEVICE_UDID
if [[ -n "${AWARE_DERIVED_DATA:-}" ]]; then export AWARE_DERIVED_DATA; fi
exec python3 "$ROOT/ios/scripts/install_launch.py" "$@"
