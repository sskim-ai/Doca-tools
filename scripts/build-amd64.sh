#!/usr/bin/env bash
set -euo pipefail
meson setup build-amd64 --wipe
meson compile -C build-amd64
