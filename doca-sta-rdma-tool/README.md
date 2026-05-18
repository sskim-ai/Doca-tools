# DOCA STA RDMA Tool

This directory contains a fresh host-side PoC for the path:

`host -> DPU (NIC mode, Ethernet mode) -> remote storage`

The goal of this PoC is not local SPDK NVMe queue export. The first milestone is to verify that a host process can:

1. Open a DOCA device exposed by a BlueField in NIC mode.
2. Confirm that the device reports STA capability.
3. Create and start a `doca_sta` control context.
4. Create and start a `doca_sta_io` context on the same progress engine.

Only after that baseline is stable should the code grow into:

1. Remote subsystem / namespace configuration.
2. STA IO path bring-up to a remote storage target.
3. Synthetic host-side IO submission.
4. GPU-originated IO submission.

## Preconditions

- BlueField is configured in NIC mode.
- Ethernet mode is enabled for the path used by the host.
- The host can see a DOCA-capable device backed by the BlueField NIC-mode exposure.
- DOCA SDK is installed on the host.

## Build

```bash
meson setup build --wipe
meson compile -C build
```

## Run

Without arguments, the tool selects the first DOCA device that reports STA capability.

```bash
./build/doca-sta-rdma-tool
```

To force a specific host-visible DOCA device by PCI address:

```bash
./build/doca-sta-rdma-tool 0000:xx:yy.z
```

## Current Scope

This tool currently validates only host-side DOCA STA bring-up:

- device discovery
- STA capability check
- `doca_sta` create/add_dev/start
- `doca_sta_io` create/start
- progress engine integration

It does not yet configure:

- backend handles
- subsystems / namespaces
- QPs
- remote IO submission

Those will be added as the next stage once host-side STA bring-up is confirmed.
