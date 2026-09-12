# XG2010G PON factory-data preservation review

Reviewed on 2026-09-11. This is a source review of the normal configured HTTP
update paths, not a hardware test or a guarantee for arbitrary console commands,
changed environments, or different device trees. No device NAND was modified.

Sources pinned for reproduction:

- [YYH2913/http-uboot, xg2010g, dbd8efb9dacc](https://github.com/YYH2913/http-uboot/tree/dbd8efb9dacc87c8d9822b12edc64c93441e9412).
  The remote branch head was checked with `git ls-remote` and matches the local
  `origin/xg2010g` reviewed for the Q1000K installer comparison.
- [naoki66/XG2010G-http-uboot, master, d76dcf1972d6](https://github.com/naoki66/XG2010G-http-uboot/tree/d76dcf1972d6d331c4be4d7f626aeefd45bdc64b).
  Reviewed from a fresh shallow checkout.

## Conclusion

YYH preserves the external DSD and ART regions by keeping them outside its
438 MiB firmware erase range. It does not migrate PON data into UBI.

Naoki also leaves raw DSD outside its default update ranges, but its UBI factory
handling does not preserve optical calibration. It forcibly erases UBI during
firmware recovery and reconstructs `factory` using only two MAC addresses. Its
439 MiB firmware range also overlaps the first MiB of YYH's ART reservation.
It should not be treated as a reference implementation for preserving all PON
factory data.

## Flash ranges

All ends below are exclusive.

| Area | YYH xg2010g | Naoki master |
|---|---|---|
| DSD | `0x00400000..0x00600000`, read-only partition | Same range, without a `read-only` property |
| Chainloader update | `0x00600000..0x00700000` | Same default raw update window |
| Firmware UBI | `ubi`, `0x00700000..0x1bd00000`, 438 MiB | `system`, `0x00700000..0x1be00000`, 439 MiB |
| ART | `0x1bd00000..0x1c000000`, read-only | No separate ART partition; its first MiB lies inside `system` |
| Tail reservation | `0x1c000000..0x20000000`, read-only | `0x1be00000..0x20000000`, read-only |

These are explicit declarations in the respective board device trees:
[YYH DTS](https://github.com/YYH2913/http-uboot/blob/dbd8efb9dacc87c8d9822b12edc64c93441e9412/dts/upstream/src/arm64/airoha/xg2010g.dts#L141-L221),
[Naoki DTS](https://github.com/naoki66/XG2010G-http-uboot/blob/d76dcf1972d6d331c4be4d7f626aeefd45bdc64b/dts/upstream/src/arm64/airoha/xg2010g.dts#L168-L214).
The overlap is a verified layout disagreement, not proof that those bytes
contain optical calibration on every XG2010G. No XG2010G NAND dump was supplied
for this review.

## YYH behavior

`xg2010g_get_dsd_ethaddrs()` reads DSD and parses `lan_mac` and `wan_mac`.
The XR1710G factory synchronization routine returns immediately on XG2010G;
the HTTP factory-volume creation conditions also exclude XG2010G. There is no
XG-specific FSAN/BOB migration in these paths.
[Board code](https://github.com/YYH2913/http-uboot/blob/dbd8efb9dacc87c8d9822b12edc64c93441e9412/board/airoha/an7581/an7581_rfb.c#L352-L389),
[factory guard](https://github.com/YYH2913/http-uboot/blob/dbd8efb9dacc87c8d9822b12edc64c93441e9412/board/airoha/an7581/an7581_rfb.c#L554-L562),
[HTTP volume creation](https://github.com/YYH2913/http-uboot/blob/dbd8efb9dacc87c8d9822b12edc64c93441e9412/net/lwip/httpd_recovery.c#L2573-L2609).

Firmware recovery calls `recovery_force_ubi_rebuild()` and erases the whole
selected UBI partition. Existing configuration, working calibration copies or
credentials stored there do not survive; the external DSD/ART regions do.
[Forced rebuild](https://github.com/YYH2913/http-uboot/blob/dbd8efb9dacc87c8d9822b12edc64c93441e9412/net/lwip/httpd_recovery.c#L3396-L3407),
[erase](https://github.com/YYH2913/http-uboot/blob/dbd8efb9dacc87c8d9822b12edc64c93441e9412/net/lwip/httpd_recovery.c#L2485-L2520).

## Naoki inconsistencies

1. **The declared BOSA cell conflicts with the writer.** The DTS declares
   `bosa-calibration@5000` as a 4 KiB cell at factory offset `0x5000`.
   `xg2010g_sync_factory_part()` constructs a `0x6006`-byte buffer filled with
   `0xff`, inserts WAN MAC at `0x5000` and LAN MAC at `0x6000`, and writes that
   buffer to the factory volume if it differs. It never copies calibration or
   FSAN. Thus the declared BOSA cell becomes six WAN-MAC bytes followed by
   4,090 bytes of `0xff`.
   [Cell declaration](https://github.com/naoki66/XG2010G-http-uboot/blob/d76dcf1972d6d331c4be4d7f626aeefd45bdc64b/dts/upstream/src/arm64/airoha/xg2010g.dts#L218-L228),
   [offset constants](https://github.com/naoki66/XG2010G-http-uboot/blob/d76dcf1972d6d331c4be4d7f626aeefd45bdc64b/board/airoha/an7581/an7581_rfb.c#L46-L50),
   [factory writer](https://github.com/naoki66/XG2010G-http-uboot/blob/d76dcf1972d6d331c4be4d7f626aeefd45bdc64b/board/airoha/an7581/an7581_rfb.c#L562-L624).

2. **The factory-preservation helper does not protect firmware recovery.**
   `recovery_preserve_ubi_volume()` recognizes `factory`, but every firmware
   upload targeting UBI first forces a full rebuild. That sets
   `ubi_needs_format = true`; preparation erases the entire MTD partition.
   The volume-cleanup helper that skips factory is only used when no reformat
   occurred. Factory is recreated and synchronized after the firmware write.
   There is no calibration backup/restore around that erase.
   [Preserve helper](https://github.com/naoki66/XG2010G-http-uboot/blob/d76dcf1972d6d331c4be4d7f626aeefd45bdc64b/net/lwip/httpd_recovery.c#L2059-L2075),
   [forced rebuild call](https://github.com/naoki66/XG2010G-http-uboot/blob/d76dcf1972d6d331c4be4d7f626aeefd45bdc64b/net/lwip/httpd_recovery.c#L3055-L3106),
   [whole-partition erase](https://github.com/naoki66/XG2010G-http-uboot/blob/d76dcf1972d6d331c4be4d7f626aeefd45bdc64b/net/lwip/httpd_recovery.c#L2212-L2251),
   [post-write factory sync](https://github.com/naoki66/XG2010G-http-uboot/blob/d76dcf1972d6d331c4be4d7f626aeefd45bdc64b/net/lwip/httpd_recovery.c#L3161-L3168).

3. **DSD is outside the default write ranges, but is not marked read-only.**
   This is weaker partition protection than YYH provides. The different UBI
   endpoint also means the ART-preservation claim from YYH cannot be applied
   to this fork unchanged.

## Verification and Q1000K implications

A host C harness extracted Naoki's actual `xg2010g_sync_factory_part()` and
stubbed its UBI/DSD calls. Given a factory buffer seeded with synthetic
calibration bytes, it replaced every byte except the two MAC slots with `0xff`.
The harness made no hardware calls; it is retained at
`/tmp/q1000k-pon-naoki-factory-harness.c`. Both device-tree firmware endpoints
were also calculated directly from their `reg` properties.

Neither project provides a complete PON provisioning migration to copy into
Q1000K. Keep Q1000K's independently verified raw DSD originals and 438 MiB UBI
boundary. As documented in [the Q1000K PON audit](q1000k-pon-data.md), FSAN and
both BOB calibration records reside in DSD on the supplied Q1000K backup.
The Q1000K installer now also copies FSAN at factory offset `0x9000` and the
513-byte GPON/XGS-PON records at `0xa000`/`0xb000`, preserving the 64 KiB size
and existing MAC/unit-serial offsets. This is a Q1000K extension based on its
verified DSD data, not a migration inherited from either XG2010G project.
Retaining raw DSD does not retain all OEM settings erased during UBI conversion.
