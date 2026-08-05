# Prebuilt M4 firmware — VSPA_DBG proxy for RFNM boards

Artifact carrier only; do not merge this branch. The source is branch
**`vspa-selfhosted-debug-rfnm`** of this repository.

| | |
|---|---|
| `la9310.bin` sha256 | `c40e899269f8d55343891937829ff2ec806fe5bdfadd8b75848afbf6d2020570` |
| Source commit | `vspa-selfhosted-debug-rfnm` (proxy + sw-cmd-engine gate fix) |
| Base | `rfnm/la9310-freertos` 2026-08 stack drop (`be3536c`) |
| Toolchain | Arm GNU Toolchain 13.2.rel1 (`arm-none-eabi`) |
| Build | `ARMGCC_DIR=<toolchain> ./build_release.sh -boot_mode=pcie` |
| Verified on | RFNM board (imx8mp host), 2026-08-05 |

This is the stock RFNM firmware plus the `LA9310_SW_CMD_VSPA_DBG` mailbox
proxy, which lets a Linux host reach the VSPA debug block at `0xE0046000`
without a JTAG probe. The radio stack is unaffected: a board running this
image boots its VSPA kernel and QEC corrector exactly as before.

Verified with it: `heartbeat`, `DVR = 0x3000`, an interactive `vdbg`
source-level session, and the full 28-kernel on-target suite (28/28 PASS).

## Install

Use `tools/provision_rfnm_board.sh` from the host-tooling repository
(`la931x_vspa_common`, branch `vspa-selfhosted-debug-rfnm`), which backs up
the vendor image, installs this one, reboots and verifies:

```sh
./tools/provision_rfnm_board.sh root@<board> path/to/la9310.bin
```

Manual equivalent, if you prefer:

```sh
scp la9310.bin root@<board>:/lib/firmware/la9310.bin.vspadbg
ssh root@<board> 'cp -a /lib/firmware/la9310.bin /lib/firmware/la9310.bin.vendor
                  cp /lib/firmware/la9310.bin.vspadbg /lib/firmware/la9310.bin
                  sync && reboot'
```

Nothing is flashed — the driver loads the M4 image over PCIe at every probe,
so rollback is `cp /lib/firmware/la9310.bin.vendor /lib/firmware/la9310.bin`
plus a reboot, and the Linux host stays reachable even if the image is bad.
