#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Execute production installer and FIT validators using host NAND/UBI models."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest
from test_nand_safety import ROOT, HTTP, function

INSTALLER = (ROOT / 'board/airoha/an7581/q1000k_installer.c').read_text()
HEADER = (ROOT / 'include/q1000k_installer.h').read_text()


def compile_model(tmp):
    code = (ROOT / 'test/q1000k/installer_harness.c').read_text()
    source = re.sub(r'^#include .*\n', '', HEADER + '\n' + INSTALLER, flags=re.M)
    code = code.replace('/* INSERT PRODUCTION CODE */', source)
    attach = (ROOT / 'test/q1000k/ubi_attach_harness.c').read_text()
    io = (ROOT / 'drivers/mtd/ubi/io.c').read_text()
    vtbl = (ROOT / 'drivers/mtd/ubi/vtbl.c').read_text()
    validators = '\n'.join([function(io, 'validate_ec_hdr'),
                            function(io, 'validate_vid_hdr'),
                            function(vtbl, 'vtbl_check'),
                            function(vtbl, 'process_lvol'),
                            function(vtbl, 'ubi_read_volume_table')])
    attach = attach.replace('/* INSERT REAL UBI FUNCTIONS */', validators)
    code = code.replace('/* INSERT UBI ATTACH VALIDATORS */', attach)
    (tmp / 'model.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-g', '-Wall', '-Werror',
                    '-Wno-unused-function', '-Wno-sign-compare', '-fsanitize=undefined',
                    '-I'+str(ROOT/'test/q1000k'),
                    str(tmp / 'model.c'), '-lz', '-o', str(tmp / 'model')], check=True)
    return tmp / 'model'


class Installer(unittest.TestCase):
    def test_nand_ubi_model(self):
        with tempfile.TemporaryDirectory(prefix='q1000k-installer-') as tmp:
            exe = compile_model(Path(tmp))
            subprocess.run([str(exe)], check=True)
            backup = os.environ.get('Q1000K_TEST_BACKUP')
            if backup:
                subprocess.run([str(exe), backup], check=True)

    def test_readonly_and_write_boundaries(self):
        boot = function(INSTALLER, 'do_q1000k_boot')
        self.assertLess(boot.index('airoha_snand_set_write_enabled(false)'), boot.index('q1000k_attach_readonly'))
        ro = function(INSTALLER, 'q1000k_attach_readonly')
        self.assertLess(ro.index('~MTD_WRITEABLE'), ro.index('ubi_part'))
        self.assertNotIn('airoha_snand_set_write_enabled(true)', INSTALLER)
        self.assertNotRegex(INSTALLER, r'\b\w*(?:markbad|mark_bad|skip_bad)\s*\(')
        commit = function(INSTALLER, 'q1000k_install_commit')
        self.assertLess(commit.index('Q1000K_CHAIN_OFFSET'), commit.index('q1000k_format_ubi'))
        self.assertLess(commit.index('ubi_detach()'), commit.index('Q1000K_ENV_OFFSET'))

    def test_upload_fit_roles(self):
        code = (ROOT / 'test/q1000k/fit_validation_harness.c').read_text()
        code = code.replace('ENUM_HELPER(fit_image_get_type, "type", "kernel", IH_TYPE_KERNEL)', '''
static int fit_image_get_type(const void *fit,int node,u8 *out) {
    const char *name=fdt_getprop(fit,node,"type",NULL);
    if(!name)return -EINVAL;
    if(!strcmp(name,"kernel"))*out=IH_TYPE_KERNEL;
    else if(!strcmp(name,"filesystem"))*out=7;
    else if(!strcmp(name,"ramdisk"))*out=3;
    else if(!strcmp(name,"flat_dt"))*out=8;
    else return -EINVAL;
    return 0;
}''')
        extra = '''
typedef unsigned long ulong;
typedef fdt32_t fdt32_t;
#define FIT_DATA_PROP "data"
#define FIT_RAMDISK_PROP "ramdisk"
#define FIT_LOADABLE_PROP "loadables"
#define IH_TYPE_RAMDISK 3
#define IH_TYPE_FILESYSTEM 7
#define IH_TYPE_FLATDT 8
#define Q1000K_CHAIN_SIZE 0x100000UL
#define Q1000K_UBI_OFFSET 0x700000UL
#define Q1000K_UBI_SIZE 0x1b600000UL
enum upload_target { TARGET_FIRMWARE, TARGET_UBOOT, TARGET_RECOVERY, TARGET_INITRAMFS };
static enum upload_target current_target;
static int fit_address(const void *fit,int node,const char *name,ulong *out) {
    int len;const fdt32_t *p=fdt_getprop(fit,node,name,&len);
    if(!p||len!=4)return -EINVAL;*out=fdt32_to_cpu(*p);return 0;
}
static int fit_image_get_load(const void *fit,int node,ulong *out) { return fit_address(fit,node,"load",out); }
static int fit_image_get_entry(const void *fit,int node,ulong *out) { return fit_address(fit,node,"entry",out); }
int recovery_validate_q1000k_fit(const void *fit,size_t size);
static int recovery_validate_firmware_image(const void *fit,size_t size) { return recovery_validate_q1000k_fit(fit,size); }
'''
        names = ['recovery_fit_has_hashed_image', 'recovery_validate_q1000k_fit',
                 'recovery_q1000k_fit_layout', 'recovery_validate_q1000k_chainloader',
                 'recovery_validate_q1000k_initramfs',
                 'recovery_validate_q1000k_upload']
        code = code.replace('/* INSERT PRODUCTION CODE */', extra + '\n'.join(function(HTTP,n) for n in names))
        code = code.replace('if (argc != 2)', 'if (argc != 3)').replace(
            'int ret = recovery_validate_q1000k_fit(data, size);',
            'current_target=atoi(argv[2]); int ret = recovery_validate_q1000k_upload(data, size);')
        with tempfile.TemporaryDirectory(prefix='q1000k-installer-fit-') as temp:
            tmp = Path(temp)
            (tmp/'test.c').write_text(code)
            libfdt = ROOT/'scripts/dtc/libfdt'
            subprocess.run(['cc','-std=gnu11','-g','-fsanitize=undefined','-I'+str(libfdt),
                            str(tmp/'test.c'),*[str(f) for f in sorted(libfdt.glob('*.c'))],
                            '-lcrypto','-lz','-o',str(tmp/'test')],check=True)
            def check(path, target, accepted):
                proc = subprocess.run([str(tmp/'test'),str(path),str(target)],capture_output=True,text=True)
                self.assertEqual(proc.returncode,0 if accepted else 1,proc.stderr)
            check(ROOT/'q1000k-chainload-uboot.itb',1,True)
            check(ROOT/'q1000k-chainload-uboot.itb',0,False)
            check(ROOT/'q1000k-chainload-uboot.itb',2,False)
            check(ROOT/'q1000k-chainload-uboot.itb',3,False)
            (tmp/'kernel.bin').write_bytes(bytes(range(256))*8192)
            (tmp/'rootfs.bin').write_bytes(b'filesystem data'*1000)
            board='/dts-v1/; / { compatible="quantum,q1000k"; partitions { compatible="fixed-partitions"; #address-cells=<1>; #size-cells=<1>; partition@700000 { label="ubi"; reg=<0x700000 SIZE>; }; }; };'
            tool_env=dict(os.environ,PATH=str(ROOT/'scripts/dtc')+os.pathsep+os.environ['PATH'])
            for size in ['0x1b600000','0x1b700000']:
                (tmp/'board.dts').write_text(board.replace('SIZE',size))
                subprocess.run([str(ROOT/'scripts/dtc/dtc'),'-I','dts','-O','dtb','-o',str(tmp/'board.dtb'),str(tmp/'board.dts')],check=True)
                for role in ['filesystem','ramdisk','embedded']:
                    prop='loadables' if role=='filesystem' else 'ramdisk'
                    its='''/dts-v1/; / { description="test"; images {
 kernel { data=/incbin/("kernel.bin"); type="kernel"; arch="arm64"; os="linux"; load=<0x80200000>; entry=<0x80200000>; compression="none"; hash {algo="sha256";}; };
 fdt { data=/incbin/("board.dtb"); type="flat_dt"; arch="arm64"; compression="none"; hash {algo="sha256";}; };
 rootfs { data=/incbin/("rootfs.bin"); type="ROLE"; arch="arm64"; os="linux"; compression="none"; hash {algo="sha256";}; };
 }; configurations { default="conf"; conf {kernel="kernel"; fdt="fdt"; PROP="rootfs";}; }; };'''
                    if role=='embedded':
                        its=re.sub(r' rootfs \{.*?\}; \};\n', '', its)
                        its=its.replace('PROP="rootfs";', '')
                    (tmp/'image.its').write_text(its.replace('ROLE',role).replace('PROP',prop))
                    for external in [False,True]:
                        subprocess.run([str(ROOT/'tools/mkimage'),*(['-E'] if external else []),'-f','image.its','image.itb'],cwd=tmp,env=tool_env,check=True,stdout=subprocess.DEVNULL)
                        check(tmp/'image.itb',0,size=='0x1b600000' and role=='filesystem')
                        check(tmp/'image.itb',2,size=='0x1b600000' and role=='ramdisk')
                        check(tmp/'image.itb',1,False)
                        check(tmp/'image.itb',3,role!='filesystem')
                        valid=(tmp/'image.itb').read_bytes()
                        damaged=bytearray(valid);at=damaged.index(bytes(range(256))*8);damaged[at]^=1
                        (tmp/'image.itb').write_bytes(damaged)
                        check(tmp/'image.itb',0,False);check(tmp/'image.itb',2,False)
                        check(tmp/'image.itb',3,False)
                        (tmp/'image.itb').write_bytes(valid[:len(valid)//2])
                        check(tmp/'image.itb',3,False)
            # Reject validly hashed kernel wrappers at the chainloader entry.
            (tmp/'image.its').write_text(its.replace('0x80200000','0x81e00000'))
            subprocess.run([str(ROOT/'tools/mkimage'),'-f','image.its','bad-entry.itb'],cwd=tmp,env=tool_env,check=True,stdout=subprocess.DEVNULL)
            check(tmp/'bad-entry.itb',3,False)
            recovery=os.environ.get('Q1000K_TEST_RECOVERY')
            if recovery:
                check(Path(recovery),2,os.environ.get("Q1000K_TEST_RECOVERY_EXPECT", "accept")=="accept")
                check(Path(recovery),0,False)
                check(Path(recovery),3,True)
            sysupgrade=os.environ.get('Q1000K_TEST_SYSUPGRADE')
            if sysupgrade:
                check(Path(sysupgrade),3,False)

if __name__=='__main__':
    unittest.main(verbosity=2)
