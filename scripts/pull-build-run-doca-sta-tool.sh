#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOL_DIR="${REPO_ROOT}/doca-sta-tool"

SPDK_ROOT="${SPDK_ROOT:-/home/skhynix/spdk}"
SPDK_PKGCONFIG_DIR="${SPDK_PKGCONFIG_DIR:-${SPDK_ROOT}/build/lib/pkgconfig}"
SPDK_DPDK_LIB_DIR="${SPDK_DPDK_LIB_DIR:-${SPDK_ROOT}/dpdk/build/lib}"
SPDK_DPDK_TMP_LIB_DIR="${SPDK_DPDK_TMP_LIB_DIR:-${SPDK_ROOT}/dpdk/build-tmp/lib}"
MELLANOX_DPDK_LIB_DIR="${MELLANOX_DPDK_LIB_DIR:-/opt/mellanox/dpdk/lib/x86_64-linux-gnu}"
BDF="${1:-0000:3b:00.0}"

echo "[1/3] Updating repository"
git -C "${REPO_ROOT}" pull --ff-only

echo "[2/3] Configuring and building doca-sta-tool"
meson setup "${TOOL_DIR}/build" "${TOOL_DIR}" --wipe \
  -Dspdk_root="${SPDK_ROOT}" \
  -Dspdk_pkgconfig_dir="${SPDK_PKGCONFIG_DIR}" \
  -Dspdk_dpdk_lib_dir="${SPDK_DPDK_LIB_DIR}" \
  -Dspdk_dpdk_tmp_lib_dir="${SPDK_DPDK_TMP_LIB_DIR}" \
  -Dmellanox_dpdk_lib_dir="${MELLANOX_DPDK_LIB_DIR}"
meson compile -C "${TOOL_DIR}/build"

echo "[3/3] Running doca-sta-tool for BDF ${BDF}"
"${TOOL_DIR}/build/doca-sta-tool" "${BDF}"
