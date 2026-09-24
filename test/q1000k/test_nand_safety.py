#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Host regression tests for the Q1000K NAND-preservation control flow.

Run: python3 test/q1000k/test_nand_safety.py
Extract the production C functions verbatim and execute them with mocked hardware,
RAM allocation and firmware validation. No board or flash device is accessed.
The firmware parser itself is checked separately with real FIT fixtures.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HTTP = (ROOT / 'net/lwip/httpd_recovery.c').read_text()
SPI = (ROOT / 'drivers/spi/airoha_snfi_spi.c').read_text()


def function(source, name):
    match = re.search(r'^(?:static )?[\w *]+\b' + name + r'\([^;]*?\)\n\{', source, re.M)
    if not match:
        raise AssertionError(f'Function not found: {name}')
    # Function closing braces are at column zero in these production sources.
    end = source.index('\n}', match.end()) + 2
    return source[match.start():end]


class NandSafety(unittest.TestCase):
    def test_phy_serdes_access(self):
        eth = (ROOT / 'drivers/net/airoha_eth.c').read_text()
        constants = eth[eth.index('#define RTL8261_PHY5_ADDR'):
                        eth.index('#include "rtl8261be_patch_table-stock-v28.inc"')]
        names = ['airoha_rtl8261_sds_wait_ready', 'airoha_rtl8261_mask',
                 'airoha_rtl8261_field_set', 'airoha_rtl8261_phy_reg_convert',
                 'airoha_rtl8261_top_get', 'airoha_rtl8261_top_set',
                 'airoha_rtl8261_sds_get', 'airoha_rtl8261_sds_diag_get',
                 'airoha_rtl8261_sds_write', 'airoha_rtl8261_apply_patch',
                 'airoha_rtl8261_apply_sds_mode']
        code = (ROOT / 'test/q1000k/phy_serdes_harness.c').read_text()
        code = code.replace('/* INSERT CONSTANTS */', constants)
        code = code.replace('/* INSERT PRODUCTION CODE */',
                            '\n\n'.join(function(eth, name) for name in names))
        # Use the actual patch entries that set Q1000K's expected SDS 7:10.
        table = (ROOT / 'drivers/net/rtl8261be_patch_table-stock-v28.inc').read_text()
        entries = [line for line in table.splitlines()
                   if 'RTK_PATCH_OP_PSDS0, 0x0f, 0x0007, 0x0010,' in line]
        self.assertEqual(len(entries), 2)
        code = code.replace('/* INSERT PATCH ENTRIES */', '\n'.join(entries))
        with tempfile.TemporaryDirectory(prefix='q1000k-serdes-') as tmp:
            cfile = Path(tmp) / 'test.c'
            exe = Path(tmp) / 'test'
            cfile.write_text(code)
            subprocess.run(['cc', '-std=gnu11', '-g', '-Wall', '-Werror',
                            '-fsanitize=undefined', str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_ethernet_initialization(self):
        eth = (ROOT / 'drivers/net/airoha_eth.c').read_text()
        names = ['SCU_SSR3', 'SCU_USB0_SERDES_SEL', 'SCU_ETH_XSI_SEL',
                 'SCU_ETH_XSI_USXGMII', 'SCU_WAN_CONF', 'SCU_WAN_SEL',
                 'SCU_WAN_SEL_USXGMII', 'RTL8261_PHY_SPEED_2500M']
        macros = '\n'.join(line for line in eth.splitlines()
                           if re.match(r'#define (' + '|'.join(names) + r')\b', line))
        code = (ROOT / 'test/q1000k/eth_init_harness.c').read_text()
        code = code.replace('/* INSERT PRODUCTION CODE */',
                            macros + '\n' + function(eth, 'airoha_hw_init'))
        with tempfile.TemporaryDirectory(prefix='q1000k-eth-') as tmp:
            cfile = Path(tmp) / 'test.c'
            exe = Path(tmp) / 'test'
            cfile.write_text(code)
            subprocess.run(['cc', '-std=gnu11', '-g', '-Wall', '-Werror',
                            '-fsanitize=undefined', str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_startup_and_controller_wiring(self):
        config = (ROOT / 'configs/q1000k_defconfig').read_text()
        env = (ROOT / 'board/airoha/an7581/q1000k.env').read_text()
        self.assertIn('CONFIG_ENV_IS_NOWHERE=y', config)
        self.assertIn('# CONFIG_ENV_IS_IN_UBI is not set', config)
        self.assertIn('CONFIG_AIROHA_SNFI_NAND_WRITE_GUARD=y', config)
        self.assertIn('bootcmd=q1000k_boot || http_recovery\n', env)
        self.assertNotIn('ubi part', env)
        self.assertNotIn('bootm', env)
        for name, first_io in [('airoha_snand_exec_op', 'airoha_snand_set_mode'),
                               ('airoha_snand_dirmap_write', 'spi_mem_exec_op')]:
            body = function(SPI, name)
            self.assertLess(body.index('airoha_snand_check_write'), body.index(first_io))
        resolve = function(HTTP, 'recovery_resolve_target')
        self.assertLess(resolve.index('return recovery_q1000k_target'),
                        resolve.index('recovery_try_ubi_target'))
        target = function(HTTP, 'recovery_q1000k_target')
        self.assertNotRegex(target, r'\bubi_(part|open|attach)\w*\(')
        self.assertIn('target->ubi_needs_format = true', target)
        backup = function(HTTP, 'recovery_nand_backup_fill_cache')
        self.assertNotRegex(backup, r'\b(mtd_write|mtd_erase|ubi_\w+|.*markbad)\(')
        self.assertEqual(HTTP.count('airoha_snand_set_write_enabled(true)'), 2)
        run = function(HTTP, 'run_http_recovery')
        self.assertIn('rc = recovery_flash_uploaded_image(&status_leds)', run)
        self.assertNotIn('rc = flash_image(', run)

    def test_real_fit_validation(self):
        tool_env = dict(os.environ, PATH=str(ROOT / 'scripts/dtc') + os.pathsep + os.environ['PATH'])
        code = (ROOT / 'test/q1000k/fit_validation_harness.c').read_text()
        code = code.replace('/* INSERT PRODUCTION CODE */',
                            function(HTTP, 'recovery_fit_has_hashed_image') + '\n' +
                            function(HTTP, 'recovery_validate_q1000k_fit'))
        with tempfile.TemporaryDirectory(prefix='q1000k-fit-') as tmp:
            tmp = Path(tmp)
            (tmp / 'test.c').write_text(code)
            libfdt = ROOT / 'scripts/dtc/libfdt'
            subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=undefined',
                            '-I' + str(libfdt), str(tmp / 'test.c'),
                            *[str(f) for f in sorted(libfdt.glob('*.c'))],
                            '-lcrypto', '-lz', '-o', str(tmp / 'test')], check=True)
            (tmp / 'board.dts').write_text('/dts-v1/; / { compatible = "quantum,q1000k"; };')
            subprocess.run([str(ROOT / 'scripts/dtc/dtc'), '-I', 'dts', '-O', 'dtb',
                            '-o', str(tmp / 'board.dtb'), str(tmp / 'board.dts')], check=True)
            (tmp / 'kernel.bin').write_bytes(bytes(range(256)) * 8192)
            its = r'''/dts-v1/;
/ {
    description = "Q1000K validation fixture";
    images {
        kernel-1 {
            data = /incbin/("kernel.bin"); type = "kernel";
            arch = "arm64"; os = "linux"; compression = "none";
            hash-1 { algo = "sha256"; };
        };
        fdt-1 {
            data = /incbin/("board.dtb"); type = "flat_dt";
            arch = "arm64"; compression = "none";
            hash-1 { algo = "sha256"; };
        };
    };
    configurations {
        default = "config-1";
        config-1 { kernel = "kernel-1"; fdt = "fdt-1"; };
    };
};'''
            (tmp / 'image.its').write_text(its)
            for external in [False, True]:
                image = tmp / 'image.itb'
                subprocess.run([str(ROOT / 'tools/mkimage'),
                                *(['-E'] if external else []), '-f', 'image.its', str(image)],
                               cwd=tmp, env=tool_env, check=True, stdout=subprocess.DEVNULL)
                valid = image.read_bytes()
                self.assertEqual(subprocess.run([str(tmp / 'test'), str(image)]).returncode, 0)
                # Corrupt image data without updating its hash.
                damaged = bytearray(valid)
                data_at = damaged.index(bytes(range(256)) * 4)
                damaged[data_at + 5] ^= 1
                image.write_bytes(damaged)
                self.assertEqual(subprocess.run([str(tmp / 'test'), str(image)]).returncode, 1)
                # Truncated external payload / truncated inline FDT must fail.
                image.write_bytes(valid[:len(valid) // 2])
                self.assertEqual(subprocess.run([str(tmp / 'test'), str(image)]).returncode, 1)
            # Re-hash a structurally valid FIT for a different board: still reject it.
            (tmp / 'board.dts').write_text('/dts-v1/; / { compatible = "other,board"; };')
            subprocess.run([str(ROOT / 'scripts/dtc/dtc'), '-I', 'dts', '-O', 'dtb',
                            '-o', str(tmp / 'board.dtb'), str(tmp / 'board.dts')], check=True)
            subprocess.run([str(ROOT / 'tools/mkimage'), '-f', 'image.its', 'wrong.itb'],
                           cwd=tmp, env=tool_env, check=True, stdout=subprocess.DEVNULL)
            self.assertEqual(subprocess.run([str(tmp / 'test'), str(tmp / 'wrong.itb')]).returncode, 1)

    def test_production_control_flow(self):
        macros = '\n'.join(line for line in SPI.splitlines()
                           if line.startswith('#define SPI_NAND_OP_'))
        macros += '\n#define Q1000K_CHAIN_SIZE 0x100000UL\n' + '\n'.join(line for line in HTTP.splitlines()
                                   if re.match(r'#define RECOVERY_(UPLOAD_MAX|MIN_FIRMWARE_SIZE|MAX_UBOOT_SIZE|NAND_BACKUP_CHUNK|XG2010G_INSTALL_TOKEN|Q1000K_\w+|UBOOT_SLOT_\w+)\b', line))
        names = ['recovery_q1000k_upload_top', 'recovery_q1000k_upload_buffer',
                 'recovery_parse_ramboot_addr', 'httpd_post_begin',
                 'httpd_post_receive_data', 'httpd_post_finished',
                 'httpd_post_response_complete', 'recovery_flash_uploaded_image',
                 'recovery_prepare_ramboot', 'recovery_boot_initramfs',
                 'recovery_q1000k_target', 'recovery_nand_backup_fill_cache',
                 'recovery_read_nand_backup']
        code = (ROOT / 'test/q1000k/nand_safety_harness.c').read_text()
        code = code.replace('/* INSERT PRODUCTION CODE */', macros + '\n' +
                            function(SPI, 'airoha_snand_set_write_enabled') + '\n' +
                            function(SPI, 'airoha_snand_check_write') + '\n' +
                            '\n\n'.join(function(HTTP, name) for name in names))
        with tempfile.TemporaryDirectory(prefix='q1000k-safety-') as tmp:
            cfile = Path(tmp) / 'test.c'
            exe = Path(tmp) / 'test'
            cfile.write_text(code)
            for installer in (0, 1):
                subprocess.run(['cc', '-std=gnu11', '-g', '-Wall', '-Werror',
                                '-Wno-unused-function', '-Wno-unused-variable',
                                f'-DTEST_INSTALLER={installer}',
                                '-fsanitize=undefined', str(cfile), '-o', str(exe)], check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
