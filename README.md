# Q1000K HTTP-Recovery U-Boot

This repository contains a Q1000K-specific second-stage U-Boot
chainloader. It adds HTTP recovery to the Quantum Fiber Q1000K while keeping
the vendor ECNT/AXON first-stage bootloader, FIP, BL31, certificates, DSD, ART,
and BBT/BMT data in place.

The vendor first stage loads a bare FIT chainloader from the fixed 1 MiB NAND
window at `0x00600000`. The packaged image therefore **must not exceed 1 MiB**
and must be a bare FIT at file offset zero. Do not flash `u-boot.bin`,
`u-boot.img`, FIP, BL31, DSD, ART, or a whole-NAND image.

> [!WARNING]
> Flashing boot firmware can make the ONT unbootable. Build and RAM-boot the
> image first, keep a serial console attached, and independently verify the
> NAND map and the vendor loader's commands on the exact device. This project
> deliberately limits its normal write path, but it cannot make an unverified
> installation safe.

## What it supports

- `q1000k_defconfig` for the AN7581-based Q1000K.
- A RAM-bootable FIT containing the raw U-Boot payload and matching Q1000K DTB.
- HTTP recovery at `http://192.168.255.1` after the chainloader starts.
- Separate Q1000K firmware, recovery-image, and chainloader update flows.
- Temporary HTTP boot of Q1000K OpenWrt initramfs FIT images from RAM.
- A guarded, explicit first-install operation that prepares the fixed UBI
  range only after confirmation.

The intended Q1000K flash boundaries are:

| Region | Offset | Size | Normal handling |
| --- | ---: | ---: | --- |
| Vendor boot / environment / DSD | `0x00000000` | 6 MiB | Preserved |
| Chainloader | `0x00600000` | 1 MiB | Updated only by the explicit chainloader flow |
| UBI | `0x00700000` | 438 MiB | Firmware, recovery image, and installer target |
| ART | `0x1bd00000` | 3 MiB | Preserved |
| Vendor BMT/BBT reserve | `0x1c000000` | 64 MiB | Preserved |

## Build the chainloader

Install an AArch64 cross compiler, then run the Q1000K build helper from the
repository root:

```sh
CROSS_COMPILE=aarch64-linux-gnu- JOBS=8 ./scripts/build-chainloader-fit.sh
```

For an OpenWrt SDK/toolchain, use its full compiler prefix instead:

```sh
export STAGING_DIR=/path/to/openwrt/staging_dir
export CROSS_COMPILE="$STAGING_DIR/toolchain-aarch64_cortex-a53_gcc-14.4.0_musl/bin/aarch64-openwrt-linux-musl-"
JOBS=8 ./scripts/build-chainloader-fit.sh
```

The result is `q1000k-chainload-uboot.itb`. The helper rebuilds with
`q1000k_defconfig`, verifies the required Q1000K safety options, creates the
two-image FIT, checks its 1 MiB limit, extracts both payloads again, and prints
its SHA-256. To package an already-built, known-good tree without changing its
configuration, use:

```sh
./scripts/build-chainloader-fit.sh --package-only /tmp/q1000k-test.itb
```

Inspect a candidate before any device use:

```sh
tools/dumpimage -l q1000k-chainload-uboot.itb
sha256sum q1000k-chainload-uboot.itb
```

The listing must show exactly `kernel-1` and `fdt-1`, with the kernel load and
entry address `0x81e00000` and the Q1000K device tree at `0x82000000`.

## Validate from RAM before installing

Connect a 115200 8N1 serial adapter, interrupt the vendor `ECNT>` prompt, and
transfer the **bare FIT** using the existing serial-transfer method. A typical
vendor-console validation uses `0x89000000`:

```text
loady 89000000
# Send q1000k-chainload-uboot.itb with YModem.
iminfo 89000000
bootm 89000000
```

Confirm the Q1000K second-stage banner, serial console, and recovery Ethernet
path before attempting persistent installation. `loady` syntax and available
commands are vendor-version dependent; do not substitute an untested address
or command from another router model.

## HTTP recovery and installation

After a validated chainloader starts, open `http://192.168.255.1` from a PC
connected to the recovery port. The recovery page distinguishes these actions:

- **Firmware Recovery** accepts a Q1000K `*-sysupgrade.itb`, replaces `fit`
  and resets `rootfs_data` while preserving the factory and recovery volumes.
- **Recovery Image** accepts a Q1000K `*-initramfs-recovery.itb`, replaces the
  recovery volume, and resets `rootfs_data`.
- **Boot from RAM** accepts a Q1000K `*-initramfs-recovery.itb` and boots it
  directly from memory. This works before installation; the bootloader does
  not attach UBI, write NAND, or save environment changes for this action.
- **Update U-Boot only** writes and verifies only the 1 MiB chainloader slot;
  it preserves the existing UBI volumes and vendor environment.
- **Install U-Boot and prepare UBI** is a deliberate first-install action. It
  erases the UBI firmware/settings area, prepares the fixed layout, and then
  updates the vendor boot command. Use it only after a successful RAM boot and
  after confirming that losing the existing UBI contents is acceptable.

Use only images built for the fixed Q1000K layout. An upload for another
AN7581 device, a raw U-Boot binary, or an oversized FIT is rejected by design.

### Temporary initramfs boot

Select **Boot from RAM**, choose the Q1000K initramfs `.itb`, and click
**Upload and Boot**. **RAM upload address** defaults to `0x89000000` and can
be changed for the current upload. Use a hexadecimal address aligned to
4 KiB. The page displays the usable RAM limit and rejects images that do not
fit at the selected address. The server independently enforces these checks.
The image may contain an embedded initramfs or a separate
FIT ramdisk. Sysupgrade images and chainloader images are rejected. Uploads
must be 1–256 MiB and fit in the available staging memory.
Addresses must also be above the full kernel decompression area: the minimum
is `0x88200000` with the current 128 MiB decompression limit. This is the FIT
upload address; Linux still loads at `0x80200000`. The choice is not saved to
flash. Firmware and recovery-image flashing continue to stage at `0x84000000`.

After validation and acknowledgement of the upload response, recovery stops
HTTP, DHCP and Ethernet, then boots Linux. The page shows **Boot requested**;
follow the serial console for the actual boot result. Linux uses the network
configuration in the initramfs, which may differ from `192.168.255.1`.
Restarting returns to the existing boot setup. The running Linux image controls
its own storage access; the bootloader's NAND write guard does not protect
against writes performed by Linux.

If `bootm` returns, the serial console reports the error. Run `http_recovery`
to retry. No persistent recovery volume is required or created.

## Regression tests

Build the host tools first, then run the Q1000K checks without a connected
device:

```sh
python3 test/q1000k/test_nand_safety.py
python3 test/q1000k/test_chainloader_fit.py
python3 test/q1000k/test_installer.py
node test/q1000k/test_installer_ui.js
```

The tests compile extracted production logic against mock NAND/UBI models and
exercise FIT validation, write gates, recovery UI routing, and packaging
checks. They are useful regression coverage, not proof that a particular ONT
is safe to flash.

## Create a release from GitHub Actions

Open the repository's **Actions** tab, select **Release Q1000K chainloader**,
choose **Run workflow**, and provide a new release tag such as
`q1000k-v2026.10-rc1`. The workflow installs the AArch64 toolchain, builds and
verifies the bare FIT, uploads the image and its SHA-256 file as a workflow
artifact, then creates a GitHub release containing both files. It defaults to
a pre-release; uncheck that option only for a release you intend to publish as
stable.

## Further documentation

The detailed design, factory-data layout, UBI lifecycle, and recovery safety
model are documented in [the Q1000K board guide](doc/board/airoha/q1000k.rst).
Read it before changing flash offsets, installer behavior, or image validation.

U-Boot is distributed under the GPL-2.0+; see [Licenses](Licenses/README).
