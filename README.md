# LibreEcho Linux 6.1 for MT8163

This repository is the current standalone Linux 6.1 kernel source for LibreEcho
MT8163 targets. It is intentionally separate from `LibreEcho-Platform`, which
owns ARM32 product tooling, initramfs, feature packaging, OTA verification, and
the historical 3.18 compatibility tree.

## Provenance

- Base kernel: Linux 6.1.178
- Base commit: `dc5c83b7f5f83ea99aea5c771b1ec77458263a15`
- Product baseline: `main`
- Upstream origin: `https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git`
- Current PRD kernel baseline: `2aaa8bfae1cc7c9aed5afe0fbe9a8e6abcbc6872`
- The public `main` branch is a clean source baseline. Post-baseline fixes are
  developed on review branches until they are verified and merged.

The tree is not a claim that the MT8163 changes are upstream-ready. A clean
kernel source baseline does not by itself constitute a complete boot image or
stable OTA release; those require the paired product tooling and independent
image verification.

## Production audio contract

> **Final hardware-verified contract (2026-08-07):** the programme is mono, but
> PCM 23 must use a **two-channel transport with identical left/right samples**.
> One-channel `MonoRight` / `DACSETUP=0x24` is superseded: live output isolation
> showed clean woofer audio but noise or silence from the tweeter.

The Radar-Puffin speaker path is:

- producer input: stereo `S16_LE`, 48 kHz;
- programme processing: mix/downmix/duck/limit once to mono;
- hardware output: duplicated mono in `S16_LE`, 48 kHz, **2 channels**;
- hardware PCM: PCM 23 / `hw:0,23`;
- board channel configuration: `Stereo`;
- codec route: normal `DACSETUP=0x14` (left slot to left DAC, right slot to right DAC);
- HP/HPL/HPR speaker routes enabled; LO/LOL/LOR line-out routes disabled;
- `Audio_DacMux_Setting=Off`;
- PRB_P2 resource class 12, calibrated crossover, and IFACE3
  `DACMOD2BCLK` are retained.

The shared audio engine renders one mono sample per frame and writes it to both
PCM channels. This is not stereo programme audio; it is the transport required
for the left/HPL tweeter and right/HPR woofer DAC paths. The userspace engine
and packaging live in the separate `LibreEcho-Platform` tooling repository.

## Build boundary

The production pipeline must pass this repository as `LIBREECHO_KERNEL_SRC`
and pass the separate tooling repository as `LIBREECHO_TOOLING_SRC`. Do not use
the tooling repository as the kernel source: it contains a Linux 3.18 tree and
will produce incompatible DTC/build results when used as a 6.1 source.

Use an external output directory, for example:

```sh
make O=/path/to/kernel-output ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- \
  libreecho_mt8163_audio_defconfig
make O=/path/to/kernel-output ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- \
  zImage dtbs -j8
```

The final LibreEcho pipeline remains the authority for the stock-v184 Android
boot envelope, initramfs, feature payloads, signing, and image verification.
This repository must not be used to flash a device directly.

## MT8163 Bluetooth HCI contract

The MT8163 Bluetooth bridge is built into the production kernel. The committed
`mt8163_arm32_defconfig` explicitly enables the complete closure:

```text
CONFIG_BT=y
CONFIG_BT_BREDR=y
CONFIG_BT_LE=y
CONFIG_MTK_BTIF=y
CONFIG_MTK_COMBO_BT=y
CONFIG_MTK_MT8163_BLUEZ_HCI=y
```

The bridge registers `hci0` before userspace WMT preparation is complete. It
therefore cancels the automatic HCI setup work queued by `hci_register_dev()`
and marks the controller for deferred setup; the first `SET_POWERED` management
command is allowed through that narrow gate only after WMT has configured the
BTIF transport. The normal HCI open/setup sequence then clears `HCI_SETUP` and
populates the controller information. Without this exception, Linux rejects the
first controller-indexed management command while the transport is deliberately
waiting for userspace preparation.

The source contract can be checked without hardware:

```sh
python3 -B tools/mt8163_bluetooth_contract_test.py
```

This static check does not replace live HCI controller-information, discovery,
pairing, and connection acceptance on the target device.
