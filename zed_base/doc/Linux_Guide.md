# zed_base on Linux: PetaLinux 2024.2 kernel + Ubuntu 22.04 (ZedBoard)

Companion to `Get_Started.md` (bare-metal bring-up). This guide runs the same zed_base hardware
(OV2640 → proc_slot → Video Frame Buffer Write → DDR → Video Frame Buffer Read → VGA + HDMI)
under Linux, with the bare-metal application code reused unchanged.

Status (2026-11-14): working on shell v7. Live camera on VGA and HDMI, all `jig` menu functions,
memory test bit-exact PASS at 1920x1080, OpenCV Canny as the CPU edge kernel, low-light
enhancement (kernel 4), in C (`sw/linux_c`) and Python (`sw/python`), both built from
`sw/common`.

---

## 1. Architecture and decisions

| Item | Choice | Why |
|---|---|---|
| Boot side (FSBL, U-Boot, kernel, device tree) | PetaLinux 2024.2 from the zed_base XSA | Matches Vivado/Vitis 2024.2; one toolchain |
| Root filesystem | Ubuntu 22.04 armhf (Ubuntu Base) on SD partition 2 | Real `apt`; PetaLinux rootfs has no package feed |
| PL IP access | UIO (`generic-uio`) for frame buffers, VTC, clk_wiz, proc_slot, accel | Kernel video drivers stay off; the bare-metal register sequences (graceful stop, buffer rotation) keep working |
| DMA buffers | u-dma-buf kernel module, 128 MB from CMA | Contiguous, physical address in sysfs, cached mapping with explicit sync |
| GPIO | libgpiod (kernel `gpio-xilinx`, `zynq_gpio`) | EMIO 54-56 = frame buffer resets; LEDs, switches, buttons |
| I2C | i2c-dev: `/dev/i2c-0` OV2640 (PS I2C0), `/dev/i2c-1` ADV7511 (PS I2C1) | No kernel camera/HDMI drivers needed |
| PYNQ | Not used | PYNQ v3.1 is built with 2024.1 tools; zedboard has no official image; everything needed (Jupyter, Samba, Python access) works without it |
| Xillinux | Not used | Old base, own top-level design, nothing it adds here |

Memory map (zed_base shell v7, from the XSA):

| Address | IP | DT label | Linux access |
|---|---|---|---|
| 0x41200000 | AXI GPIO LEDs (ch1, 8) + switches (ch2, 8) | `axi_gpio_ledsw` | gpio-xilinx, label `41200000.gpio`, lines 0-7 LEDs, 8-15 switches |
| 0x41210000 | AXI GPIO buttons (5) | `axi_gpio_btns` | gpio-xilinx, label `41210000.gpio` |
| 0x43C00000 | Video Frame Buffer Read (display) | `vfb_rd` | UIO, name `v_frmbuf_rd`, IRQ_F2P[0] |
| 0x43C10000 | VTC | `vtc` | UIO, name `v_tc` |
| 0x43C20000 | Video Frame Buffer Write (camera / test) | `vfb_wr` | UIO, name `v_frmbuf_wr`, IRQ_F2P[1] |
| 0x43C30000 | Clocking Wizard (pixel clock) | `clk_wiz_pix` | UIO, name `clk_wiz` |
| 0x43C40000 | Video Frame Buffer Read 2 (memory test) | `vfb_rd2` | UIO, name `v_frmbuf_rd`, IRQ_F2P[2] |
| 0x43D00000 | proc_slot v5, 64 KB | `proc_slot` | UIO, name `proc_slot`, IRQ_F2P[3] |
| 0x43E00000 | accelerator window, 64 KB (stub) | `accel` | UIO, name `accel_stub`, IRQ_F2P[4] |
| EMIO 54 / 55 / 56 | resets vfb_rd / vfb_wr / vfb_rd2 (57-61 spare) | | `zynq_gpio` lines 54/55/56 |
| PS I2C0 0xE0004000 | OV2640 at 0x30 (SCCB) | | `/dev/i2c-0` |
| PS I2C1 0xE0005000 | ADV7511 at 0x39 | | `/dev/i2c-1` |

---

## 2. Host setup (Ubuntu PC, 2080 Ti machine)

1. Ubuntu 22.04 host. Keep projects on an **ext4** location (not NTFS/exFAT).
2. PetaLinux 2024.2 dependencies (AMD `plnx-env-setup.sh` or the apt list in UG1144), `sudo dpkg-reconfigure dash` → No.
3. Install as a normal user: `./petalinux-v2024.2-final-installer.run -d <install dir>`.
4. Find `settings.sh` later with `find / -name settings.sh -path "*etalinux*" 2>/dev/null`;
   keep the `source` line in `~/plnx_env.sh` (not `.bashrc`).
5. For the rootfs step: `sudo apt install qemu-user-static binfmt-support`.

---

## 3. XSA checklist (Vivado 2024.2)

Export Hardware **with bitstream**. PS7 (ZedBoard preset) must have: SD0, UART1, ENET0 (MDIO), USB0,
TTC0, I2C0 + I2C1 (EMIO), GPIO EMIO (resets 54-56), HP ports used by the frame buffers, FCLK0.
Check: `unzip -l design.xsa | grep -E "\.bit|\.hwh"`.

---

## 4. PetaLinux project

```bash
source ~/plnx_env.sh
petalinux-create project --template zynq --name zed_linux      # 2024.x syntax
cd zed_linux
petalinux-config --get-hw-description=/path/to/zed_base.xsa
```
In the menu:
- Image Packaging → root filesystem type **EXT4 (SD/eMMC/SATA/USB)**, device `/dev/mmcblk0p2`
- DTG Settings → Kernel Bootargs → manual:
  `console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait cma=256M uio_pdrv_genirq.of_id=generic-uio`
- DTG Settings → MACHINE_NAME `zedboard`

Kernel (`petalinux-config -c kernel`, search symbols with `/`):

| Symbol | Setting |
|---|---|
| `UIO_PDRV_GENIRQ` | `*` |
| `I2C_CHARDEV` | `*` |
| `USB_VIDEO_CLASS`, ChipIdea USB host | `*` (USB webcams) |
| `FPGA_MGR_ZYNQ_FPGA`, `FPGA_REGION`, `OF_FPGA_REGION` | `*` (default) |
| `CMA` | `*` (size from `cma=`) |
| `OF_OVERLAY`, `OF_CONFIGFS`, `CONFIGFS_FS` | `*` (runtime DT overlays for future designs) |
| optional `USB_USBNET`, `USB_NET_CDCETHER`, `USB_NET_CDC_NCM`, `USB_NET_RNDIS_HOST` | `M` (phone tethering) |

### 4.1 Device tree
Copy `linux/system-user.dtsi` of this repository to
`project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi`. It adds the u-dma-buf
node (128 MB) and marks the PL IP `generic-uio`. Labels come from
`components/plnx_workspace/device-tree/device-tree/pl.dtsi`
(`grep -nE "@4[0-9a-f]+ \{" .../pl.dtsi`).

### 4.2 u-dma-buf module
```bash
petalinux-create modules --name udmabuf --enable      # older syntax: -t modules
cd project-spec/meta-user/recipes-modules/udmabuf/files
rm -f udmabuf.c
wget https://raw.githubusercontent.com/ikwzm/udmabuf/master/u-dma-buf.c   # pin a release tag for reproducibility
printf 'obj-m := u-dma-buf.o\n\nSRC := $(shell pwd)\n\nall:\n\t$(MAKE) -C $(KERNEL_SRC) M=$(SRC)\n\nmodules_install:\n\t$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install\n\nclean:\n\trm -f *.o *~ core .depend .*.cmd *.ko *.mod.c *.mod *.symvers *.order\n' > Makefile
```
In `udmabuf.bb`: `SRC_URI = "file://Makefile file://u-dma-buf.c file://COPYING"`.

### 4.3 Build and package
```bash
petalinux-build                       # first build 1-3 h
petalinux-package boot --fsbl images/linux/zynq_fsbl.elf --fpga images/linux/system.bit --u-boot --force
bootgen -arch zynq -read images/linux/BOOT.BIN | grep -iE "partition|\.bit|\.elf"   # FSBL, bitstream, U-Boot, DTB
```
After a device-tree-only change: `petalinux-build -c device-tree -x cleansstate && petalinux-build`.

---

## 5. SD card

Partition 1 FAT32 512 MB (`BOOT`), partition 2 ext4 rest (`rootfs`).
```bash
sudo parted /dev/sdX --script mklabel msdos mkpart primary fat32 1MiB 513MiB \
     mkpart primary ext4 513MiB 100% set 1 boot on
sudo mkfs.vfat -F 32 -n BOOT /dev/sdX1
sudo mkfs.ext4 -L rootfs /dev/sdX2
```
Boot files to partition 1, then verify and eject:
```bash
cp images/linux/{BOOT.BIN,image.ub,boot.scr} /media/$USER/BOOT/ && sync
md5sum images/linux/{BOOT.BIN,image.ub} /media/$USER/BOOT/{BOOT.BIN,image.ub}
udisksctl unmount -b /dev/sdX1
```
First boot test: extract PetaLinux `rootfs.tar.gz` on partition 2 (user `petalinux`); then replace it
with Ubuntu (next section).

---

## 6. Ubuntu 22.04 root filesystem (partition 2)

```bash
wget https://cdimage.ubuntu.com/ubuntu-base/releases/22.04/release/ubuntu-base-22.04.5-base-armhf.tar.gz
sudo mkfs.ext4 -F -L rootfs /dev/sdX2
R=/mnt/root; sudo mkdir -p $R && sudo mount /dev/sdX2 $R
sudo tar --numeric-owner -xpzf ubuntu-base-22.04.5-base-armhf.tar.gz -C $R
sudo tar -xzf images/linux/rootfs.tar.gz -C $R --wildcards './lib/modules/*'   # kernel modules of THIS build
sudo cp /etc/resolv.conf $R/etc/resolv.conf
for d in proc sys dev dev/pts; do sudo mount --bind /$d $R/$d; done
sudo chroot $R /bin/bash
```
Inside the chroot:
```bash
apt update
apt install -y systemd systemd-sysv udev kmod iproute2 iputils-ping openssh-server sudo nano \
               i2c-tools gpiod python3 locales apt-utils systemd-timesyncd
locale-gen en_US.UTF-8
ln -sf /usr/share/zoneinfo/Asia/Riyadh /etc/localtime
echo zed > /etc/hostname
printf "127.0.0.1 localhost\n127.0.1.1 zed\n" > /etc/hosts
printf "/dev/mmcblk0p2 / ext4 defaults,noatime 0 1\n" > /etc/fstab
mkdir -p /etc/systemd/network
printf "[Match]\nName=eth0\n\n[Network]\nDHCP=yes\n" > /etc/systemd/network/10-eth0.network
systemctl enable systemd-networkd systemd-resolved systemd-timesyncd ssh serial-getty@ttyPS0.service
groupadd -f fpga
useradd -m -s /bin/bash -G sudo,dialout,fpga zed && passwd zed
cp 99-fpga.rules /etc/udev/rules.d/     # linux/99-fpga.rules of this repository
echo u-dma-buf > /etc/modules-load.d/udmabuf.conf
depmod -a $(ls /lib/modules)
exit
```
Leave:
```bash
for d in dev/pts dev sys proc; do sudo umount $R/$d; done
sudo rm $R/etc/resolv.conf && sync && sudo umount $R
```
First boot on the board:
```bash
sudo ln -sf /run/systemd/resolve/stub-resolv.conf /etc/resolv.conf   # DNS
sudo timedatectl set-ntp true                                          # no RTC on ZedBoard
sudo apt install -y python3-numpy python3-opencv python3-libgpiod python3-smbus2 \
                    build-essential cmake git pkg-config libopencv-dev libgpiod-dev v4l-utils
```

### 6.1 Network options
- Router: DHCP on eth0 (default above).
- Windows laptop sharing Wi-Fi: `ncpa.cpl` → Wi-Fi adapter → Properties → Sharing → allow, select the
  Ethernet adapter. Board gets `192.168.137.x`; copy files with PowerShell `scp` or WinSCP (SFTP).
- Direct cable to the Linux PC: Settings → wired → IPv4 "Shared to other computers" (`10.42.0.x`).

### 6.2 File access
- SFTP (no setup): WinSCP / FileZilla, user `zed`.
- Samba (Explorer drive): `apt install samba`, share `/home/zed`, `smbpasswd -a zed`.
- JupyterLab (browser GUI): `pip3 install jupyterlab`, `jupyter lab --ip=0.0.0.0 --no-browser`.

---

## 7. Software (on the board)

Copy the repository to the board (`git clone`, or `scp -r zed_base zed@<board>:~`). Everything
below runs from it; there is no separate copy of any source file.

### 7.1 C jig: `sw/linux_c`
```bash
cd ~/zed_base/sw/linux_c && make
sudo ./jig                                   # same keys as the UART menu ('h')
sudo ZB_IMAGE=/home/zed/img_1920x1080.raw ./jig     # 'l': memory test with this raw image
```
`make` builds `sw/common/*.c` unchanged (jig.c selects its Linux parts with `ZB_LINUX`: buffers
in the u-dma-buf region, OpenCV Canny as the CPU edge kernel, no DRAM mailbox), the unmodified
Xilinx drivers in `xlnx/` and the BSP shim in `shim/` (see `sw/linux_c/README.md`).

### 7.2 Python: `sw/python`
```bash
sudo python3 ~/zed_base/sw/python/jig.py [--load image.raw]
sudo python3 ~/zed_base/sw/python/cam_loop.py        # minimal camera loop
sudo python3 ~/zed_base/sw/python/display_test.py    # colour bars
```
- `hw.py`: hardware layer (UIO by name + address, u-dma-buf cached/uncached, libgpiod, smbus2, VTC
  sequence recorded from the 2024.2 driver, graceful frame buffer stop, ADV7511, OV2640 all sizes).
- `jig.py`: full port of `jig.c`; `sw_kernels.c` and `lle_global.c` are built once into
  `libzbmodels.so` and called through ctypes (bit-exact). Canny via OpenCV.

Never run the C and Python jig (or any two programs that drive the video IP) at the same time.

### 7.3 Low-light enhancement in the jig (kernel 4)
| Key | Action |
|---|---|
| `4` | LLE on: HW path = proc_slot curve table, updated every frame from the slot's statistics; SW path = the same on the CPU |
| `0` | LLE off (pass-through) |
| `x` | HW (proc_slot) / SW (CPU) |
| `n` | iteration count: auto (LiteIE rule from the frame mean) or forced 2 / 4 / 8 / 12 |
| `m` / `l` | memory test: two passes through proc_slot (statistics, then curve), bit-exact against `lle_global.c` |
| `s` | status line, with the frame mean and the iteration count in use |

### 7.4 Measured (2026-11-13, 1920x1080 memory test)
| Path | Time |
|---|---|
| proc_slot pass-through (150 MHz) | 20.8 ms in the slot (99.9 Mpix/s), 27.5 ms end to end |
| OpenCV Canny on the Cortex-A9 | 360 ms |
| HW vs C golden model | 0 of 2,073,600 pixels differ |
Pass-through reaches about 0.67 px/clock: the DMAs / 32-bit DDR3 limit, not the slot.

---

## 8. Gotchas (all hit during bring-up)

**Tools and versions**
- PYNQ v3.1 needs Vivado/Vitis/PetaLinux **2024.1**; with 2024.2 tools use this Linux route instead.
- No 2024.x ZedBoard BSP: create the project from the XSA (`--template zynq`).
- `petalinux-create` 2024.x syntax: `petalinux-create project ...` / `petalinux-create modules ...`.
- PetaLinux `settings.sh` path depends on the install; find it, do not assume `/tools/Xilinx/...`.

**SD card and boot**
- No console output and no blue DONE LED = `BOOT.BIN` not readable. Cause found: the copy on the card
  differed from the build (`md5sum` mismatch). Always `sync`, compare md5, eject before removing.
- Ubuntu auto-mounts the card under `/media/$USER/BOOT` and `/media/$USER/rootfs`; an unmounted
  `/mnt/boot` is just a folder on the PC.
- A compressed bitstream in `BOOT.BIN` is ~1 MB instead of ~4 MB; that is fine.
- PetaLinux FSBL is silent by default: no output means U-Boot never started.
- After every kernel rebuild, copy `lib/modules` from the new `rootfs.tar.gz` to the Ubuntu partition
  and run `depmod -a`; otherwise u-dma-buf (and other modules) fail to load.

**Ubuntu base**
- `/etc/resolv.conf` must link to `/run/systemd/resolve/stub-resolv.conf` (name resolution fails otherwise).
- No RTC: enable `systemd-timesyncd`, or apt signatures and TLS fail with a 1970 clock.
- `tzdata` asks for the time zone during apt (Asia → Riyadh); `apt-utils` missing only prints a warning.
- `python3-opencv` has no headers: C++ needs `libopencv-dev` and `pkg-config`.
- Default MAC `00:0a:35:00:1e:53` (Xilinx); set a unique one if two boards share a network.
- Link negotiated 100 Mbps on the router port/cable used; gigabit needs a better cable/port.

**Kernel / hardware access**
- PetaLinux binds kernel drivers to PL IP it knows; mark the video IP `generic-uio` so they do not
  reconfigure it. `uio_pdrv_genirq.of_id=generic-uio` must be in the boot arguments.
- `uioN` numbering changes with the design and two IPs are named `v_frmbuf_rd`: always find UIO
  devices by address (the name follows the device-tree node, e.g. `accel_stub`).
- u-dma-buf physical address can change between boots: read `/sys/class/u-dma-buf/udmabuf0/phys_addr`.
- u-dma-buf `sync_*` sysfs files are root-only unless the udev RUN rule (`linux/99-fpga.rules`) is installed.
- `i2cdetect` on the Cadence PS I2C skips most addresses (no SMBus quick write); use `i2cdetect -y -r N`.
- The EMIO reset lines are held by the process that requested them; when a Python script exits,
  the frame buffer cores drop back into reset and the picture disappears. Keep the process running
  (a device-tree GPIO hog is not an option: the reset must stay pulsable).
- Loading a bitstream at run time resets the PL: OV2640 setup, frame buffers and video output stop;
  start the jig again afterwards.
- busybox (PetaLinux rootfs) prints `driver` for unbound devices in `readlink | basename` loops.
- `sudo` resets `HOME` to `/root`: give absolute paths to scripts run with sudo.

**Video IP**
- VTC: the control register must end as `0x00000006` (register update + generator enable), polarity
  register `0x70` for the negative-sync VESA modes. A hand-written setup with source-select bits
  (`0x03F5EF07`) gave "no signal" on both outputs while the DMA ran. `hw.py` replays the exact
  register writes of `XVtc_SetGeneratorTiming` (2024.2) per mode.
- Frame buffer video format code for BGRX8 is 27 (`0x1b`).
- Keep the graceful-stop rule from `Get_Started.md`: let the write frame buffer finish its frame and go
  idle before pulsing its reset (otherwise the 32 px seam returns).

**Code**
- `ov2640_regs.h` must be the current split-table version (`OV2640_SZ` sizes); the older flattened
  VGA-only header does not work with `zb_platform.c`.
- The BSP shim `xil_io.h` must include `xstatus.h` (`XST_SUCCESS` is used through it in `jig.c`).
- The Linux C build compiles the Xilinx drivers with `-U__linux__` (their standalone paths), so
  `jig.c` uses its own switch `ZB_LINUX` for Linux-only code. `xil_printf` is a macro in the shim:
  no `#if` inside its arguments.

---

## 9. Backup and future changes

- After any working state: `sudo dd if=/dev/sdX of=zed_sd_$(date +%F).img bs=4M status=progress`.
- New bitstream with the same shell: no image change (section 10).
- New IP at new addresses: runtime device-tree overlay (`.dtbo` with `generic-uio` nodes) through
  configfs; enabled in the kernel, workflow to be documented with the first new design.

---

## 10. Shell v7: the last planned PetaLinux build

zed_base v7 freezes everything this image depends on, so later hardware versions are
bitstream-only updates (details: `Get_Started.md` section 16).

| Item | v6 | v7 |
|---|---|---|
| Clocks | FCLK0 100, FCLK1 150, FCLK2 25 MHz (camera XCLK) | + FCLK3 200 MHz (spare) |
| HP ports | HP0-HP2 | + HP3 (accelerator window) |
| EMIO GPIO | 54-56 | 54-61 (57-61 spare) |
| Fabric interrupts | 3 | 16: 0 vfb_rd, 1 vfb_wr, 2 vfb_rd2, 3 proc_slot, 4 accel |
| proc_slot | 4 KB, no interrupt | 64 KB at 0x43D00000, ID 0x534C0005, level interrupt |
| accelerator window | none | `accel` 64 KB at 0x43E00000 (stub, ID 0x41430000) |

One-time steps (done 2026-11-13):
1. `petalinux-config --get-hw-description=<v7 xsa> --silentconfig`.
2. `system-user.dtsi` from `linux/` (includes `&accel { compatible = "generic-uio"; };`).
3. `petalinux-build`, `petalinux-package boot ...`, copy the boot files; if the kernel was rebuilt,
   copy `lib/modules` and run `depmod -a`.
4. On the board: `sudo python3 tools/shell_test.py` checks both UIO windows, IDs, sizes and interrupts.
5. On the PC: `python3 tools/shell_check.py freeze <v7 xsa> tools/shell_v7.json`, commit the file.

For every later build:
```bash
python3 tools/shell_check.py check zed_base/system_wrapper.xsa tools/shell_v7.json   # PC
sh tools/make_bin.sh zed_base/zed_base.runs/impl_1/system_wrapper.bit               # PC, on SHELL MATCH
sudo sh tools/load_bitstream.sh system_wrapper.bit.bin                             # board, no PL user running
cat /sys/class/fpga_manager/fpga0/state                                            # operating
```
UIO interrupts: write 1 to `/dev/uioN` to enable, `read()`/`select()` to wait, clear the source
before re-enabling.

### 10.1 v7 results (2026-11-14)
- `shell_check.py freeze`: 9 address ranges, IRQ_F2P[0..4] = vfb_rd, vfb_wr, vfb_rd2, proc_slot, accel,
  [5..15] tied 0. `shell_test.py`: PASS (both windows, IDs, 64 KB maps, both interrupts through UIO).
- First v7 build failed timing (FCLK1 WNS −2.961 ns, 81 endpoints, all in proc_slot: input mux,
  luma adder, table read and mode mux in one cycle; pixel clock −0.022 ns in hdmi_pack). Fixed by a
  three-stage proc_slot pipeline and one more hdmi_pack stage (bitstream-only update):
  WNS +0.293 ns, WHS +0.024 ns, all constraints met.
- Utilisation (xc7z020): LUT 10,349 (19.5 %), FF 16,548 (15.6 %), BRAM 10 tiles (7.1 %), DSP 4.
  proc_slot v5: 810 LUT, 1,605 FF, 3 RAMB18 (the curve tables), 0 DSP.
