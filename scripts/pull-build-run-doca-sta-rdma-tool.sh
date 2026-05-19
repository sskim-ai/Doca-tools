#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOL_DIR="${REPO_ROOT}/doca-sta-rdma-tool"
if [[ "${1:-}" == "--list" ]]; then
  LIST_ONLY=1
  shift
else
  LIST_ONLY=0
fi
PF_DEV="${1:-mlx5_2}"
SF_DEV="${2:-mlx5_4}"
MAX_STA_IO="${3:-1}"

echo "[1/3] Updating repository"
git -C "${REPO_ROOT}" pull --ff-only

echo "[2/3] Configuring and building doca-sta-rdma-tool"
meson setup "${TOOL_DIR}/build" "${TOOL_DIR}" --wipe
meson compile -C "${TOOL_DIR}/build"

echo "[3/3] Running doca-sta-rdma-tool"
if [[ "${LIST_ONLY}" -eq 1 ]]; then
  "${TOOL_DIR}/build/doca-sta-rdma-tool" --list
else
  "${TOOL_DIR}/build/doca-sta-rdma-tool" --pf-dev "${PF_DEV}" --sf-dev "${SF_DEV}" --max-sta-io "${MAX_STA_IO}"
fi
