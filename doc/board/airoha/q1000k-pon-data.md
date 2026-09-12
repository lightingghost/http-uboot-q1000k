# Q1000K PON data locations

Verified on 2026-09-11 using the user's 512 MiB NAND data-area backup and the
QKX001-06.00.44.00 OEM firmware. Analysis was read-only; no vendor executables
were run and no device NAND was modified. Device identifiers and authentication
values are omitted from this report.

## Factory originals in DSD

The original DSD partition spans `0x00400000..0x005fffff`.

| Data | Offset within DSD | Absolute NAND offset | Evidence |
|---|---:|---:|---|
| FSAN / PON authentication serial | Key begins at `0xcc` | `0x004000cc` | Backup has a 12-character `fsan` field; OEM startup assigns it to `network.xpon_auth.sn`. This is different from the unit's `serial_number`. |
| GPON optical BOB calibration | `0x11000` | `0x00411000` | OEM `ecnt_sys get bob dualbob_7572` reads 513 bytes here into `/tmp/en7572_bob_0.conf`; startup selects that file for GPON. |
| XGS-PON optical BOB calibration | `0x12000` | `0x00412000` | The same loader reads 513 bytes here into `/tmp/en7572_bob_1.conf`; startup selects that file for XGS-PON. |

The two calibration records differ. The 768-byte working calibration file
recovered from the backup's OEM overlay matches NAND at `0x00411000` byte for
byte, including erased-byte padding after the original calibration record.
Both factory calibration slots and FSAN are inside DSD, below the chainloader
slot at `0x00600000` and new UBI start at `0x00700000`.

## OEM working copies and configuration

The OEM `system` UBI partition starts at `0x08600000`. The supplied boot log
shows a 312 MiB runtime partition ending at `0x1be00000`, with volumes:
`rootfs_data` (ID 0), `config` (ID 1), and `log` (ID 2). This runtime boundary
differs from the older static OEM DTS and is not the new installer layout.

The backup's `rootfs_data` UBIFS overlay contains:

- `upper/etc/config/network`: an `xpon_auth` section with PON mode,
  authentication type, serial and password options. The saved serial matches
  DSD's FSAN; the saved password matches the OEM startup placeholder.
- `upper/etc/lddla/en7572_bob.conf`: the working calibration copy described above.
- Other OEM configuration, management state and credentials, outside the scope
  of this PON-location inventory.

UBI conversion erases these OEM volumes and working copies. It preserves the
factory originals in DSD. This does not establish preservation of every OEM
configuration/provisioning item or provide PON/OMCI functionality in OpenWrt.

The nominal ART reservation at `0x1bd00000` is also preserved. In this backup
it contains only UBI erase-counter header pages in its first MiB, with the
remaining area erased; it is not where the identified PON calibration resides.

## Implication for the HTTP installer

The installer preserves all 2 MiB of original DSD. Its 64 KiB `factory` UBI
volume copies WAN/LAN MACs and the device `serial_number`, plus the following
Q1000K-specific fields:

| Factory offset | Field | Copied content |
|---|---|---|
| `0x9000` | FSAN / PON authentication serial | The exact 12 ASCII bytes of DSD `fsan`, followed by NUL; distinct from unit serial at `0x8000`. |
| `0xa000` | GPON calibration | Exact 513 bytes from NAND `0x00411000`. |
| `0xb000` | XGS-PON calibration | Exact 513 bytes from NAND `0x00412000`. |

Bytes outside the defined fields remain zero. In particular, the OEM working
file's extra 255 bytes of `0xff` padding are not part of the copied calibration
record. The W1700K EEPROM and fan slots remain zero and do not overlap these
PON additions. Future PON consumers can use these documented factory fields
or the preserved raw DSD records; copying data does not implement PON drivers.

These fields are prepared entirely in RAM before opening the NAND write gate.
Preflight rejects missing, duplicate, wrong-length or non-printable FSAN,
short/failed reads, and calibration records that are entirely zero or `0xff`.
It copies calibration without decoding or claiming to verify an internal
checksum. Factory is written and read back as a complete 64 KiB volume during
explicit installation, and preserved by subsequent firmware/recovery uploads.
Old factory volumes are not automatically rewritten on boot; rerunning Update
U-Boot populates the new fields and also performs its documented UBI erase.

The [XG2010G source comparison](xg2010g-pon-preservation-review.md) confirms
that YYH's branch preserves external DSD/ART without migrating PON fields.
Naoki's fork declares a BOSA factory cell but reconstructs only MAC addresses
after erasing UBI, and uses a different UBI endpoint. Its factory handling is
not a complete PON preservation implementation to copy into Q1000K.

## Reproducible evidence

The source firmware filesystem is
`QKX001-06.00.44.00.bin_extract/3566568-40229864.squashfs_v4_le`.

- `/etc/init.d/xponconfig`, lines 15–36: FSAN to PON authentication serial.
- Same script, line 109: loads `econet_bob.ko` with `dualbob=dualbob_7572`.
- Same script, lines 200–224: selects calibration file 0 for GPON, file 1 for XGS-PON.
- `/etc/debug_ext.d/03-pon_get_logs.sh`, lines 14–15: identifies
  `/etc/lddla/en7572_bob.conf` as PON calibration data.
- `/sbin/ecnt_sys` disassembly: the `dualbob_7572` branch sets both read lengths
  to `0x201` at address `0x4011d0`; code at `0x401264..0x401298` constructs the
  first read using device `dsd`, offset `0x11000`, and file 0; code at
  `0x4012b0..0x4012d0` uses device `dsd`, offset `0x12000`, and file 1.
- `factory_bootlog.log`, lines 878–879: reads BOB information from flash;
  lines 823, 1041, 1050 identify the three OEM UBI volumes.

Temporary inspection artifacts are under `/tmp/q1000k-pon-oem-rootfs`,
`/tmp/q1000k-pon-backup-files`, and `/tmp/q1000k-pon-ecnt_sys.asm`. Extracted
backup files are protected by a mode-0700 parent directory. UBI Reader reported
unsupported device-node extraction warnings; the configuration and calibration
files used here were extracted and checked against the raw backup.
