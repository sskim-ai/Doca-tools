#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOL_DIR="${REPO_ROOT}/doca-sta-rdma-tool"
BDF="${1:-}"

echo "[1/3] Updating repository"
git -C "${REPO_ROOT}" pull --ff-only

echo "[2/3] Configuring and building doca-sta-rdma-tool"
meson setup "${TOOL_DIR}/build" "${TOOL_DIR}" --wipe
meson compile -C "${TOOL_DIR}/build"

echo "[3/3] Running doca-sta-rdma-tool"
if [[ -n "${BDF}" ]]; then
  "${TOOL_DIR}/build/doca-sta-rdma-tool" "${BDF}"
else
  "${TOOL_DIR}/build/doca-sta-rdma-tool"
fi
