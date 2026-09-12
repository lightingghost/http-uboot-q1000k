# Q1000K installer behavior and source comparison

Sources inspected:

- [http-uboot master, 53b73174c0f](https://github.com/YYH2913/http-uboot/tree/53b73174c0fcb192a355e79710cdef5be61b14af): README, HTTP writer, factory reconstruction.
- [http-uboot xg2010g, dbd8efb9dac](https://github.com/YYH2913/http-uboot/tree/dbd8efb9dacc87c8d9822b12edc64c93441e9412): README, HTTP writer, self-installer, boot environment.
- [w1700k-ubi-installer, 46df1f5282f](https://github.com/hurrian/w1700k-ubi-installer/tree/46df1f5282f53c6757752354d7b6702f6fa54ec5): README and `files/installer/install.sh`.
- [OpenWrt PR #17869](https://github.com/openwrt/openwrt/pull/17869), the local OpenWrt Q1000K/W1700K device trees, and `nand_upgrade_fit()` / `nand_upgrade_prepare_ubi()`.

The W1700K **complete installation procedure** has two parts. Vendor-console commands install the chainloader and change the vendor environment. The Linux installer then migrates UBI and installs recovery and production firmware. The Linux script itself does not write the chainloader or vendor environment.

| Operation | W1700K procedure | http-uboot master | http-uboot xg2010g |
|---|---|---|---|
| Validate board/factory data | Serial prefix, EEPROM magic, MACs, fan ID, serial, required image files | XR factory reconstruction; legacy chainloader header checks | XG-specific chainloader description/image names and FIT hashes |
| Install chainloader | Vendor `flash erase/write`, 1 MiB at `0x600000` | HTTP raw chainloader slot; legacy wrapper accepted | HTTP or self-install; bare FIT normalized to offset zero, read-back verification |
| Change vendor `bootcmd` | Separate vendor-console `saveenv` | Separate manual procedure | Separate manual procedure after verified self-install |
| Prepare UBI | `ubiformat`, attach, create numbered volumes | Firmware upload erases/recreates selected UBI partition | Firmware upload erases/recreates fixed UBI partition |
| Factory migration | Build and write 64 KiB static `factory` volume | Reserve 1 MiB; reconstruct EEPROM and two MACs, without the installer’s complete 64 KiB record | DSD/ART remain external; no XG factory volume |
| Recovery Linux image | Write `recovery` volume | Not installed by HTTP firmware recovery | Not installed by HTTP firmware recovery |
| Production Linux image | `nand_upgrade_fit()` writes `fit` | HTTP writes `fit` | HTTP writes `fit` |
| Writable overlay | Remaining capacity in `rootfs_data`, or environment cap | Remaining capacity | Environment cap supported; XG defaults to `0x15800000` |
| Reboot | After installation | After successful upload | After successful upload/self-install |

## W1700K UBI structure

With the observed 128 KiB eraseblocks and 2 KiB pages, each LEB is 126,976 bytes.

| Volume ID | Name | Type | Requested size / content |
|---:|---|---|---|
| 0 | `ubootenv` | dynamic | 126,976 bytes; initially empty |
| 1 | `ubootenv2` | dynamic | 126,976 bytes; initially empty |
| 2 | `factory` | static | 65,536 bytes |
| 3 | `recovery` | dynamic | Recovery FIT length, rounded to LEBs |
| 4 | `fit` | dynamic | Initially one LEB; replaced with a volume sized for the production FIT |
| 5 | `rootfs_data` | dynamic | Remaining available LEBs, unless capped |

The Linux installer explicitly assigns IDs 0–4. `nand_upgrade_fit()` removes and recreates `fit`, then creates `rootfs_data`; in the fresh layout they receive IDs 4 and 5. Both HTTP branches instead create `fit` first with an automatically selected ID, and reserve **1 MiB each** for their environment volumes. They therefore do not reproduce this layout exactly.

The W1700K factory record is zero-filled and contains EEPROM at `0x0000` (7,680 bytes), WAN MAC at `0x5000`, LAN MAC at `0x6000`, fan ID at `0x7000`, and serial at `0x8000`. The source EEPROM is sought at `0x405000` and the corresponding location in up to three following eraseblocks.

## Q1000K evidence and decisions

The user selected a two-step installation: **Update U-Boot installs the chainloader, prepares UBI and changes vendor bootcmd; firmware is uploaded afterward.** Recovery firmware must also be separately uploadable if the Linux recovery volume is to contain a usable image. An empty reserved volume is not an installed recovery system.

The inspected NAND data-area backup is 536,870,912 bytes; SHA-256:
`41f08f7e71c5c08835fd1f923a925e10bf56add1239b1e35ad35df6da8e76c50`.

- The vendor environment at `0x200000` has a 4-byte little-endian CRC32 followed by NUL-separated variables in a **16 KiB** record. Its CRC validates. There is one valid record in the 2 MiB environment partition; it is not a redundant UBI environment. The rest of its first 128 KiB eraseblock is erased.
- The original `bootcmd` is `flash imgread 2048;bootm`. A direct chainloader command must explicitly read the 1 MiB slot and use the RAM address verified on this board, `0x89000000`.
- The valid 250-entry factory BBT is at `0x1e0c0000`, and the valid 250-entry BMT is at `0x1ffe0000`. Both are version 1 with **zero entries**. For this state, raw physical offsets and vendor logical offsets coincide. A writer must recheck this on the live device and refuse unsupported mappings; skipping bad blocks alone is not equivalent to vendor remapping.
- The user confirmed that Q1000K has neither Wi-Fi nor a fan. Create the same **64 KiB static factory** record, copying DSD WAN/LAN MACs and serial to the reference offsets while leaving EEPROM/fan fields zero. Q1000K additions copy the 12-byte FSAN plus NUL at `0x9000`, GPON calibration at `0xa000`, and XGS-PON calibration at `0xb000`; both calibration records are exactly 513 bytes from DSD offsets `0x11000`/`0x12000`. Preserve DSD and ART in place. See [PON data details](q1000k-pon-data.md).
- Preserve Q1000K’s **438 MiB** UBI range, `0x00700000` through `0x1bcfffff`, ART at `0x1bd00000..0x1bffffff`, and BMT reservation at `0x1c000000..0x1fffffff`. The local W1700K/master 439 MiB boundary would erase the first MiB of Q1000K ART. Older W1700K layouts are larger still.
- Preserve all unrelated vendor environment variables, and update `bootcmd` only after chainloader read-back verification and successful UBI preparation. This improves on the reference README’s environment-before-image order. A single-copy NAND environment cannot offer atomic, power-loss-proof replacement.
- Keep browsing, backup, validation and boot reads read-only. UBI environment volumes can match the reference structure while this chainloader retains RAM-only settings. Automatically attaching writable UBI to load settings would violate the original no-write-at-startup requirement.

The PR’s example boot command reads `0x1000000` bytes (16 MiB), while the installer README reads `0x100000` (1 MiB). Use the actual 1 MiB Q1000K chainloader slot size. Do not copy the PR’s larger read count or a different board’s environment/flash boundaries.

## Implemented Q1000K flow and limits

- The full Install U-Boot and Prepare UBI operation requires an uploaded installer-capable Q1000K chainloader FIT. It verifies the raw slot, formats UBI, creates IDs 0–5 and writes factory, then updates and verifies vendor bootcmd last.
- The two-step choice leaves one-LEB **empty placeholders** for recovery and fit. Separate validated uploads populate these volumes. The final logical structure matches the W1700K layout; the first step alone does not install OpenWrt.
- Firmware uploads replace fit and reset rootfs_data, preserving env/factory/recovery. Recovery uploads replace recovery and reset rootfs_data, preserving env/factory/fit. Both use all remaining space for the overlay, with no XG size cap.
- Startup settings stay in RAM despite the two compatible UBI environment volumes. Installed boot uses read-only UBI, tries fit then recovery, then falls back to HTTP; Reset forces HTTP.
- Runtime preflight supports the verified empty-table format only, scans for additional/relocated tables and environment copies, and refuses incompatible mappings or raw boot bad blocks. It never updates vendor BBT/BMT. A single-copy vendor environment and the overall multi-stage conversion are not power-loss atomic.
- Both upload targets reject old vendor-layout Q1000K images. The September 9 recovery FIT found in Downloads has tclinux/tclinux_slave/system partitions, not the fixed UBI partition, and is correctly rejected. Rebuild recovery and sysupgrade from the Q1000K UBI device profile.

Host tests cover the controller gate/ACK policy, raw and UBI write boundaries, failure ordering, factory migration using the actual backup, retry after incomplete updates, and FIT roles/layouts. The first user installation exposed missing UBI layout-volume metadata after EC-header formatting. The formatter now writes and verifies both empty volume-table copies before attachment, matching the required `ubiformat` behavior. Tests use U-Boot's actual header validators and table reader to reproduce that failure and check the correction; full UBI runtime and hardware installation remain to be verified.

The Update U-Boot page now defaults to updating only the chainloader slot. This mode preserves all UBI volumes and the vendor environment, performs no UBI attachment, and uses a separate `/upload/uboot-only` endpoint. The full W1700K-style preparation remains an explicit installer choice.
