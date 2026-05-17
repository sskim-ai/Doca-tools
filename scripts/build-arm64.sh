#!/usr/bin/env bash
set -euo pipefail
meson setup build-arm64 --wipe
meson compile -C build-arm64
