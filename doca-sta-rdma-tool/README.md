# DOCA STA RDMA Tool

This directory contains a host-side STA bring-up probe for the path:

`host -> DPU (NIC mode, Ethernet mode) -> remote storage`

The immediate goal is narrower than remote IO. This tool verifies only that a host process can:

1. Open a PF control device.
2. Open an SF/network device.
3. Create a `doca_sta` context on the PF.
4. Add the SF/network device to that STA context.
5. Start the main STA context and wait for `RUNNING`.

Only after that baseline is stable should the code grow into:

1. Remote subsystem / namespace configuration.
2. STA IO context bring-up.
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

List what DOCA sees on the host:

```bash
./build/doca-sta-rdma-tool --list
```

Run the main-context probe with the currently validated host NIC-mode pairing:

```bash
./build/doca-sta-rdma-tool --pf-dev mlx5_2 --sf-dev mlx5_4 --max-sta-io 1
```

In the current BF3 NIC-mode setup:

- `mlx5_2` is the DPA/FlexIO-capable control device. `dpa-resource-mgmt` and the FlexIO RPC sample work on this device.
- `mlx5_4` maps to the SF traffic netdev (`enpdas0f0s88`). It is the STA network/traffic device, but it is not the DPA process owner.

The following is intentionally not the preferred start pairing now:

```bash
./build/doca-sta-rdma-tool --pf-dev 0000:da:00.0 --sf-dev mlx5_4
```

That form can open DOCA devices, but it does not distinguish the FlexIO-capable ibdev instance from the SF traffic ibdev instance when multiple DOCA devinfos share the same PCI address.

For diagnostics, a control-only start can be attempted without adding an SF traffic device:

```bash
./build/doca-sta-rdma-tool --pf-dev mlx5_2 --skip-add-dev --max-sta-io 1
```

If the control-only run succeeds but the PF+SF run fails, the failure is isolated to the STA network-device add/start path. If both fail, the failure is in STA main-context resource initialization before traffic-device use.

## Current Scope

This tool currently validates only main STA context bring-up:

- device discovery
- STA capability check
- `doca_sta` create/add_dev/start
- progress engine integration

It does not yet configure:

- backend handles
- subsystems / namespaces
- `doca_sta_io`
- QPs
- remote IO submission

Those will be added as the next stage once host-side STA bring-up is confirmed.
