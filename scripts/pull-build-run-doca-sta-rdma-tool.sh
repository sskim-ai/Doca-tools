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
HOLD_SECONDS="${4:-10}"
START_IO="${5:-0}"
SUBSYSTEM_NQN="${6:-}"
LISTEN_TRADDR="${7:-}"
LISTEN_TRSVCID="${8:-4420}"
LISTEN_SECONDS="${9:-30}"

export STA_COMP_EU_MASK_P0="${STA_COMP_EU_MASK_P0:-0-3}"
export STA_TX_EU_MASK_P0="${STA_TX_EU_MASK_P0:-4-7}"
export STA_BE_Q_EU_MASK="${STA_BE_Q_EU_MASK:-8-11}"
export STA_DOCA_RDMA_ENABLE="${STA_DOCA_RDMA_ENABLE:-1}"
export STA_MAX_QPS_NUM="${STA_MAX_QPS_NUM:-1024}"
unset STA_COMP_EU_MASK_P1
unset STA_TX_EU_MASK_P1

echo "[1/3] Updating repository"
git -C "${REPO_ROOT}" pull --ff-only

echo "[2/3] Configuring and building doca-sta-rdma-tool"
meson setup "${TOOL_DIR}/build" "${TOOL_DIR}" --wipe
meson compile -C "${TOOL_DIR}/build"

echo "[3/3] Running doca-sta-rdma-tool"
if [[ "${LIST_ONLY}" -eq 1 ]]; then
  "${TOOL_DIR}/build/doca-sta-rdma-tool" --list
else
  EXTRA_ARGS=(--hold-seconds "${HOLD_SECONDS}")
  if [[ "${START_IO}" == "1" || "${START_IO}" == "true" || "${START_IO}" == "yes" ]]; then
    EXTRA_ARGS+=(--start-io)
  fi
  if [[ -n "${SUBSYSTEM_NQN}" || -n "${LISTEN_TRADDR}" ]]; then
    if [[ -z "${SUBSYSTEM_NQN}" || -z "${LISTEN_TRADDR}" ]]; then
      echo "SUBSYSTEM_NQN and LISTEN_TRADDR must be provided together" >&2
      exit 1
    fi
    EXTRA_ARGS+=(--subsystem-nqn "${SUBSYSTEM_NQN}" --listen-traddr "${LISTEN_TRADDR}" --listen-trsvcid "${LISTEN_TRSVCID}" --listen-seconds "${LISTEN_SECONDS}")
  fi
  "${TOOL_DIR}/build/doca-sta-rdma-tool" --pf-dev "${PF_DEV}" --sf-dev "${SF_DEV}" --max-sta-io "${MAX_STA_IO}" "${EXTRA_ARGS[@]}"
fi
