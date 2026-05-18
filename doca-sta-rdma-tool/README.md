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

Run the main-context probe:

```bash
./build/doca-sta-rdma-tool --pf-dev 0000:da:00.0 --sf-dev enpdas0f0s88
./build/doca-sta-rdma-tool --pf-dev 0000:da:00.0 --sf-dev mlx5_4
```

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
