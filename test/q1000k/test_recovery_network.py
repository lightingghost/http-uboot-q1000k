#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Run recovery switch/RX regressions using the production driver functions."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r'^static [\w *]+\b' + name + r'\([^;]*?\)\n\{', source, re.M)
    if not match:
        raise AssertionError(f'Function not found: {name}')
    return source[match.start():source.index('\n}', match.end()) + 2]


class RecoveryNetwork(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'drivers/net/airoha_eth.c').read_text()
        prefixes = ('SWITCH_', 'AIROHA_RECOVERY_SWITCH_', 'QDMA_DESC_',
                    'QDMA_ETH_RXMSG_', 'AIROHA_FPORT_', 'REG_RX_CPU_IDX',
                    'REG_RX_DMA_IDX', 'RX_RING_CPU_IDX_MASK', 'RX_RING_DMA_IDX_MASK')
        macros = '\n'.join(line for line in source.replace('\\\n', '').splitlines()
                           if line.startswith('#define ') and
                           line.split()[1].startswith(prefixes))
        names = ['airoha_switch_recovery_pmcr', 'airoha_switch_recovery_program_ports',
                 'airoha_switch_recovery_quiesce', 'airoha_switch_recovery_runtime_init',
                 'airoha_qdma_sync_rx_head', 'airoha_qdma_recycle_rx_desc',
                 'airoha_eth_recv_qdma']
        code = (ROOT / 'test/q1000k/recovery_network_harness.c').read_text()
        code = code.replace('/* INSERT CONSTANTS */', macros)
        code = code.replace('/* INSERT PRODUCTION CODE */',
                            '\n\n'.join(function(source, name) for name in names))
        cls.tmp = tempfile.TemporaryDirectory(prefix='q1000k-network-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cfile = Path(cls.tmp.name) / 'test.c'
        cls.exe = Path(cls.tmp.name) / 'test'
        cfile.write_text(code)
        subprocess.run(['cc', '-std=gnu11', '-g', '-Wall', '-Werror',
                        '-fsanitize=undefined', str(cfile), '-o', str(cls.exe)], check=True)

    def test_broadcast_forwarding_and_port_isolation(self):
        subprocess.run([str(self.exe), 'switch'], check=True)

    def test_rx_descriptors_delivered_once_in_order(self):
        subprocess.run([str(self.exe), 'rx'], check=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
