# Prebuilt M4 firmware — IQ Player pairing for RFNM Blue

Built from this repo so a board can be provisioned without setting up the ARM
toolchain. **This branch only carries the binaries — do not merge it.** The
sources are `iqplayer-mbox0` and `iqplayer-mbox0-vdbg`.

| file | size | sha256 | driver csum | debugger |
|---|---|---|---|---|
| `la9310-iqplayer.bin` | 63684 | `8ede400b4151626d6d735881de25f4ca62e2cc7f92e4cb9124da8e2a22ee401a` | `1a3fd1ee` | no |
| `la9310-iqplayer-vdbg.bin` | 63684 | `cae699c63cf64248b18f721d63a186390d1247a8e0edbf835edfd82098830e5e` | `7d86cd8e` | yes (LA9310_SW_CMD_VSPA_DBG) |
| `la9310-iqplayer-vdbg-loopback.bin` | 63684 | `b79787330f425fe3c7f765dd5cac95dd13c3326fcfe70de94cf00cfa2d19ddf1` | `9e8c1c5a` | yes, **+ AXIQ loopback** |

The *driver csum* is what `la9310shiva` logs at probe
(`loaded firmware la9310.bin: 63684 bytes, csum …`) — use it to confirm the
board actually runs the image you think it does.

## 2026-08-16 rebuild — quiet target log

All three images were rebuilt after a fix to the MBOX0 refusal path, so the
hashes above supersede any earlier copy. With `LA9310_HOST_OWNS_MBOX0` the M4
refuses every MBOX0 access, and it used to log one line per refusal —
`vLa9310MbxReceive()` polls up to 2000 times per call, so `target_log` filled
within seconds and each call burned its full ~20 ms budget on a receive that
cannot succeed.

Now the AVI layer logs only the first refusal per direction, and the three RFIC
call sites short-circuit instead of spinning. Measured on the board: `target_log`
holds **0 lines** after a full loopback run, and the loopback itself is
unchanged (RMS 0.0 idle -> 12039.7 replaying).

The switch stays provably inert when off: an OFF build is still byte-identical
to an unpatched `be3536c` build,
`49b4153515a2ab154b7380c96b825446e11b5022c1d752c0aa74baa6876fbc20`.

## What makes these different from the stock image

Both hand **VSPA mailbox 0 to the PCIe host**, which is what IQ Player needs.
The stock M4 firmware watches all four VSPA mailbox bits: its AVI ISR reads
`host_in_0_msb/lsb` and then clears `VSPA_MBOX0_STATUS`, so a reply the VSPA
posts for the host is consumed before the host can read it and `vspa_mbox`
reports `MBox:0 is not responding`. Built with
`LA9310_HOST_OWNS_MBOX0=ON`, the mask becomes MBOX1-only — visible on the
board as `IRQEN = 0xA000` instead of `0xF000`.

With the switch OFF the image is byte-identical to an unpatched `be3536c`
build, so the change is provably inert when disabled.

## The AXIQ-loopback variant

`la9310-iqplayer-vdbg-loopback.bin` is the vdbg image plus
`AXIQ_LOOPBACK_ENABLE`, which makes `vVSPAMboxInit()` write
`DBGGNCR (0xE00800EC) = 0x5e` -- "AXIQ loopback on RX1". Confirmed in the
disassembly:

```
1f804b7c:  ldr   r3, [pc, #12]      @ 0xe0080000
1f804b7e:  movs  r2, #94            @ 0x5e
1f804b80:  str.w r2, [r3, #236]     @ 0xec  -> DBGGNCR
```

The loopback is **internal to the LA9310, ahead of the RFIC**, so a TX->RX
datapath test runs with the radio untouched. Verified on the board: with the
transmitter idle, an RX1 capture is all zeros (RMS 0.0); with `iq-replay.sh`
running, the capture has RMS 12039.7 and its spectral peak sits in the same bin
at the same normalised frequency (+0.0508 cyc/sample) and the same purity
(0.757 vs 0.755) as the transmitted tone -- i.e. it *is* the transmitted
waveform. Throughout, `lsmod` showed **zero** RFNM RF modules loaded and
`/sys/kernel/rfnm_primary` did not exist, so nothing configured or powered the
RFIC.

Use this variant for datapath work; it is the only way to exercise the
host-DDR -> VSPA direction of the IQ Player EP window, which RX streaming never
touches. Note it also fixes RX1 to the loopback source, so it is **not** the
image to use for real reception.

## Trade-off — read before using

MBOX0 is RFNM's RF control channel (`rfic_cmd.c`, 10 call sites). These images
give up normal RF control by design. That is acceptable only because IQ Player
replaces RFNM's VSPA kernel anyway. **Do not run these as the default board
firmware** — keep them for IQ Player sessions and switch back afterwards.

If you need MBOX0 shared *within one boot*, don't use these: RFNM's stock
firmware already has a runtime handoff (`RF_SW_CMD_VSPA_MBOX_HANDOFF` +
`rfnm_vspa_handoff_cb`) that needs only a sysfs node in the driver and no
firmware change.

## Build

```
ARMGCC_DIR=<toolchain> LA9310_HOST_OWNS_MBOX0=ON \
    ./build_release.sh -boot_mode=pcie
```
(gcc-arm-none-eabi 13.2.1; output at `Demo/CORTEX_M4_NXP_LA9310_GCC/release/`)

## Verified on hardware, 2026-08-05

- Mailbox round-trip works with both images: `Received from VSPA:0, MBox:0`.
- Eric's self-hosted debugger under the IQ Player pairing: `dft → PASS ✓
  (698 cycles)`, bit-exact the reference value.
- On-target kernel suite: **28 PASS, 1 FAIL** (29 ELFs, `ditfft` stored
  twice). `test_fecu` also fails on the stock firmware with an identical
  signature, so it is pre-existing.

## Install

Keep a `.prev` of whatever is there, then replace `/lib/firmware/la9310.bin`,
`sync`, and reboot. `la9310shiva` leaks a module refcount and will not
`rmmod`, so a reboot is required; disable the driver autoloader first if you
want to control when the stack comes up.

⚠ After any `insmod`, check `dmesg` for `probe of … failed` **before**
touching any LA9310 register. `vspa_mbox` talks straight to `/dev/mem`, and on
a downed PCIe link that hangs the whole SoC (power cycle required).
