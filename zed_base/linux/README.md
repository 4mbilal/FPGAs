# linux/: files for the PetaLinux image and the Ubuntu root filesystem

| File | Goes to |
|---|---|
| `system-user.dtsi` | PetaLinux project: `project-spec/meta-user/recipes-bsp/device-tree/files/` (u-dma-buf 128 MB, PL IP as `generic-uio`, shell v7) |
| `99-fpga.rules` | board: `/etc/udev/rules.d/` (group `fpga` may use UIO, u-dma-buf, GPIO, I2C) |

Boot arguments (`petalinux-config` → DTG Settings → Kernel Bootargs):
`console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait cma=256M uio_pdrv_genirq.of_id=generic-uio`

The full procedure (PetaLinux project, u-dma-buf module, SD card, Ubuntu root filesystem) is in
`doc/Linux_Guide.md`. With shell v7 the image stays as it is: new hardware versions that pass
`tools/shell_check.py` are loaded as bitstreams (`tools/make_bin.sh`, `tools/load_bitstream.sh`).
