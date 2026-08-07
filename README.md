# LibreEcho Linux 6.1 for MT8163

This repository is the standalone Linux 6.1 kernel source for LibreEcho MT8163
Radar-Puffin/Giza targets. It is intentionally separate from the legacy
`LibreEcho-Kernel` repository, which remains the Linux 3.18 tooling, recovery
image builder, DTB tooling, and ARM32 userspace integration repository.

## Provenance

- Base kernel: Linux 6.1.178
- Base commit: `dc5c83b7f5f83ea99aea5c771b1ec77458263a15`
- Product branch: `libreecho/mt8163-6.1`
- Upstream origin: `https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git`
- Imported candidate state: existing dirty MT8163 port, audio closure, DT, Wi-Fi,
  Bluetooth, LED, thermistor, privacy, and supporting platform changes.

The first LibreEcho commit in this repository is an exact local import of the
candidate state. It is not a claim that the tree is upstream-ready or that the
current dirty candidate has passed a clean reproducible release build.

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
and packaging live in the separate `LibreEcho-Kernel` tooling repository.

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
