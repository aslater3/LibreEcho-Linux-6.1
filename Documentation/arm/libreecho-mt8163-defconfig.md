# LibreEcho MT8163 ARM32 defconfig provenance

`arch/arm/configs/mt8163_arm32_defconfig` is generated with Linux 6.1
`make savedefconfig` from the accepted prepared build output
`pipeline/work/kernel61-bind-build/.config`. It is not copied from the legacy
Linux 3.18 `LibreEcho-Platform` defconfig.

Regenerate from a verified Linux 6.1 build output with:

```sh
make -C . O=/path/to/output ARCH=arm \
  CROSS_COMPILE=/usr/bin/arm-linux-gnueabihf- savedefconfig
cp /path/to/output/defconfig arch/arm/configs/mt8163_arm32_defconfig
```

The production audio fragment remains separate in
`arch/arm/configs/libreecho_mt8163_audio.config` and must be merged before
`olddefconfig`.
