# STA Host ↔ DOCA ↔ SPDK Queue Export Project

## 1. 개요

이 프로젝트는 SPDK NVMe Queue 구조(SQ/CQ + Doorbell)를 DOCA STA/NVMe-oF 데이터패스에서 활용하기 위한 **host-side PoC** 이다.

구성 환경:
- GPU Server / Host side (x86 Ubuntu, SPDK, CX 계열 NIC)
- Storage Server / DPU side (BlueField, DOCA)

핵심 목표:
- SPDK NVMe queue 구조를 DOCA에서 활용 가능하도록 export
- SQ/CQ는 RAM-backed 이므로 DOCA mmap 대상이 될 수 있음
- Doorbell MMIO는 직접 mmap 대상이 아니므로 현재는 software polling bridge fallback 사용

> 상태: 이 디렉터리는 실험용 PoC 이며, SPDK 내부 구조체에 의존한다. 빌드와 런타임 검증은 SPDK가 설치된 사내 호스트 환경에서 수행해야 한다.

## 2. 현재 구현 구조

```text
SPDK NVMe QPair (PCIe VFIO)
        ↓
internal pointer export
        ↓
DOCA mmap (SQ/CQ + dummy RAM doorbell)
        ↓
software polling bridge
        ↓
real NVMe MMIO doorbell write
```

## 3. 구현 파일

- `spdk_doca_queue_export.c`
  - SPDK 내부 qpair 메타데이터 추출
  - SQ/CQ 포인터 export
  - dummy doorbell RAM page 생성
  - real MMIO doorbell 포인터 보관
- `spdk_backend_qpair_export.c`
  - SPDK env init
  - controller probe / qpair 생성
  - backend context 관리와 cleanup
- `main_sta_integration.cpp`
  - DOCA mmap 생성/등록/시작
  - polling thread 생명주기 관리
- `doorbell_polling.c`
  - dummy RAM doorbell 값을 감시해 real MMIO doorbell로 relay

## 4. 핵심 제약

| Component | SPDK 측 | DOCA 측 |
|---|---|---|
| SQ/CQ | host RAM | mmap 가능 |
| Doorbell | PCI BAR MMIO | 직접 mmap 불가 |

즉:
- SQ/CQ → memory-backed → 가능
- Doorbell → device register → 직접 attach 불가

## 5. 현재 workaround

```text
RAM dummy doorbell
        ↓
polling thread
        ↓
real MMIO write
```

단점:
- CPU polling 필요
- latency overhead 존재
- hardware offload가 아님

## 6. 빌드 전제

이 디렉터리는 다음이 설치된 환경을 전제로 한다.

- DOCA SDK (`/opt/mellanox/doca`)
- SPDK development headers / libraries
- SPDK 내부 헤더 접근 가능 (`nvme_internal.h`, `nvme_pcie_internal.h`)

`meson.build` 는 사내 SPDK 환경에서 조정 가능한 시작점이다. 실제 환경에 따라 SPDK include/lib 경로나 정적 링크 옵션이 추가로 필요할 수 있다.

## 7. 실행

기본 BDF는 `0000:3b:00.0` 이지만, 실행 시 첫 번째 인자로 대상 NVMe BDF를 넘길 수 있다.

```bash
./build/doca-sta-tool 0000:xx:yy.z
```

사내에서 `git pull -> meson setup -> meson compile -> 실행` 을 한 번에 하려면 루트 디렉터리에서 아래 스크립트를 사용할 수 있다.

```bash
./scripts/pull-build-run-doca-sta-tool.sh 0000:xx:yy.z
```

필요하면 아래 환경변수로 SPDK/DPDK 경로를 덮어쓸 수 있다.

```bash
SPDK_ROOT=/home/skhynix/spdk
SPDK_PKGCONFIG_DIR=/home/skhynix/spdk/build/lib/pkgconfig
SPDK_DPDK_LIB_DIR=/home/skhynix/spdk/dpdk/build/lib
SPDK_DPDK_TMP_LIB_DIR=/home/skhynix/spdk/dpdk/build-tmp/lib
MELLANOX_DPDK_LIB_DIR=/opt/mellanox/dpdk/lib/x86_64-linux-gnu
```

## 8. 알려진 한계

- SPDK public API가 아니라 internal header에 의존하므로 SPDK 버전에 취약하다.
- 기본 BDF 값은 `0000:3b:00.0` 이지만, 실제 검증 시에는 실행 인자로 변경해야 할 수 있다.
- doorbell polling은 현재 fallback일 뿐, 최종 구조가 아니다.
- 현재 PoC는 STA 데이터패스 전체를 완성한 것이 아니라, host-side queue export 경로를 검증하기 위한 기반 코드다.

## 9. 장기 목표

- VFIO dma-buf 기반 BAR export
- `doca_mmap_set_dmabuf_memrange()` 또는 동등한 안전한 메커니즘 활용
- polling 없는 doorbell 경로
