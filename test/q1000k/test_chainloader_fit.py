#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Exercise the Q1000K packager with real mkimage, dumpimage and dtc tools."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path('scripts/build-chainloader-fit.sh')
TEMPLATE = Path('board/airoha/an7581/q1000k-chainloader.its')


class ChainloaderFit(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='q1000k-fit-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'source tree'
        self.caller = Path(self.tmp.name) / 'caller directory'
        self.caller.mkdir()
        for path in [SCRIPT, TEMPLATE]:
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, target)
        for path in ['tools/mkimage', 'tools/dumpimage', 'scripts/dtc/dtc']:
            self.assertTrue((ROOT / path).is_file(), f'Build {path} first')
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(ROOT / path)
        config = (ROOT / 'configs/q1000k_defconfig').read_text()
        # These two symbols are normally selected by Kconfig.
        config += '\nCONFIG_ARM64=y\nCONFIG_OF_SEPARATE=y\n'
        (self.root / '.config').write_text(config)
        self.payload = b'Q1000K test U-Boot payload\0' * 64
        (self.root / 'u-boot.bin').write_bytes(self.payload)
        self.dtb = self.root / 'dts/upstream/src/arm64/airoha/q1000k.dtb'
        self.dtb.parent.mkdir(parents=True)
        subprocess.run(
            [str(ROOT / 'scripts/dtc/dtc'), '-I', 'dts', '-O', 'dtb',
             '-o', str(self.dtb)],
            input='/dts-v1/; / { compatible = "quantum,q1000k"; };',
            text=True, check=True, capture_output=True)
        self.output = self.caller / 'result image.itb'

    def package(self):
        return subprocess.run(
            [str(self.root / SCRIPT), '--package-only', self.output.name],
            cwd=self.caller, text=True, capture_output=True)

    def test_direct_boot_fit_and_payload_hashes(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        fit = self.output.read_bytes()
        self.assertEqual(fit[:4], bytes.fromhex('d00dfeed'))
        self.assertLessEqual(len(fit), 0x100000)
        listing = subprocess.check_output(
            [str(ROOT / 'tools/dumpimage'), '-l', str(self.output)], text=True)
        self.assertEqual(listing.count(' Image '), 2)
        for expected in ['(kernel-1)', '(fdt-1)', "'conf-uboot'",
                         'Kernel:       kernel-1', 'FDT:          fdt-1',
                         'Load Address: 0x81e00000',
                         'Entry Point:  0x81e00000',
                         'Load Address: 0x82000000', 'OS:           Linux',
                         'Architecture: AArch64']:
            self.assertIn(expected, listing)
        for index, expected in enumerate([self.payload, self.dtb.read_bytes()]):
            extracted = self.caller / f'image-{index}.bin'
            subprocess.run(
                [str(ROOT / 'tools/dumpimage'), '-T', 'flat_dt', '-p',
                 str(index), '-o', str(extracted), str(self.output)],
                check=True, capture_output=True)
            self.assertEqual(extracted.read_bytes(), expected)
            self.assertIn(hashlib.sha1(expected).hexdigest(), listing)
            self.assertIn(f'{zlib.crc32(expected):08x}', listing)
        self.assertIn(hashlib.sha256(fit).hexdigest(), result.stdout)

    def test_incompatible_configuration_preserves_output(self):
        config_path = self.root / '.config'
        original = config_path.read_text()
        cases = [
            ('CONFIG_Q1000K_INSTALLER=y',
             '# CONFIG_Q1000K_INSTALLER is not set', 'Required configuration'),
            ('airoha/q1000k', 'airoha/xg2010g', 'Required configuration'),
            ('CONFIG_TEXT_BASE=0x81E00000', 'CONFIG_TEXT_BASE=0x82000000',
             'CONFIG_TEXT_BASE must match'),
            ('CONFIG_AIROHA_SNFI_NAND_WRITE_GUARD=y',
             '# CONFIG_AIROHA_SNFI_NAND_WRITE_GUARD is not set',
             'Required configuration'),
        ]
        for old, new, error in cases:
            with self.subTest(setting=old):
                config_path.write_text(original.replace(old, new))
                self.output.write_bytes(b'previous verified FIT')
                result = self.package()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)
                self.assertEqual(self.output.read_bytes(), b'previous verified FIT')

    def test_oversized_fit_preserves_output_and_cleans_temporary_files(self):
        (self.root / 'u-boot.bin').write_bytes(bytes(0x100000))
        self.output.write_bytes(b'previous verified FIT')
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('the limit is 1048576', result.stderr)
        self.assertEqual(self.output.read_bytes(), b'previous verified FIT')
        self.assertEqual(list(self.caller.glob('.q1000k-fit.*')), [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
