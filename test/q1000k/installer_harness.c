/* SPDX-License-Identifier: GPL-2.0+ */
/* Host NAND/UBI model for executing the production installer, never real flash. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <zlib.h>
#include <byteswap.h>
#define __packed __attribute__((packed))
#define __UBOOT__
#include "../../drivers/mtd/ubi/ubi-media.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef unsigned long ulong;
#define CONFIG_Q1000K_INSTALLER 1
#define IS_ENABLED(x) (x)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define IS_ERR_OR_NULL(p) (!(p))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define div_u64(n,d) ((n)/(d))
#define malloc_cache_aligned malloc
#define vfree free
#define schedule() ((void)0)
#define get_timer(x) 123U
#define MTD_WRITEABLE 0x400
#define UBI_STATIC_VOLUME 2
#define UBI_DYNAMIC_VOLUME 1
#define UBI_READWRITE 1
#define cpu_to_be32 bswap_32
#define be32_to_cpu bswap_32
#define cpu_to_be64 bswap_64
#define be64_to_cpu bswap_64
#define CMD_RET_FAILURE 1
#define CMD_RET_SUCCESS 0
#define DECLARE_GLOBAL_DATA_PTR
#define U_BOOT_CMD(...)
struct cmd_tbl { int unused; };
static struct { ulong ram_base, ram_size, start_addr_sp; } ram, *gd = &ram;
struct mtd_info { struct mtd_info *parent; u64 size, offset; u32 erasesize, writesize, subpage_sft, flags; };
struct erase_info { loff_t addr; size_t len; };
struct ubi_device;
struct ubi_volume {
    struct ubi_device *ubi;
    int vol_id, vol_type, usable_leb_size, reserved_pebs, upd_marker, corrupted, updating;
    size_t used_bytes, received;
    char name[32]; void *upd_buf; u8 *data;
};
struct ubi_device {
    int leb_size, avail_pebs, vtbl_slots, ro_mode, min_io_size, good_peb_count;
    int vid_hdr_offset, leb_start, vtbl_size, corr_peb_count;
    struct ubi_vtbl_record *vtbl;
    struct ubi_volume *volumes[129];
};
struct ubi_volume_desc { struct ubi_volume *vol; };
static struct ubi_device device, *ubi_devices[1];
static bool write_enabled;
static int event, fail_event, short_write, corrupt_read, raw_erases, env_erases, ubi_erases;
static int bad_block = -1;
static struct mtd_info raw, part;
static u8 env_bytes[0x20000], chain[0x100000], dsd[0x12800], bbt[2048], bmt[2048];
static loff_t pon_read_fault;
static int pon_read_result;
static u8 ec_pages[3504][2048];
static u8 *layout_pages[3504]; /* Only the blocks holding the two layout LEBs. */
static int layout_write_fault, layout_read_fault, layout_short_write, layout_writes;
static int ubi_attach_calls, ubi_detach_calls;
static int host_attach_volume_table(void);
static int gate_open_calls;
static void airoha_snand_set_write_enabled(bool enabled) { write_enabled=enabled; gate_open_calls+=enabled; }
static int mutating(void) { assert(write_enabled); return ++event==fail_event ? -EIO : 0; }
static u32 crc32_le(u32 seed,const u8 *data,size_t len) { return crc32(seed^0xffffffffU,data,len)^0xffffffffU; }
static u32 get_unaligned_le32(const void *p) { u32 x; memcpy(&x,p,4); return x; }
static void put_unaligned_le32(u32 x, void *p) { memcpy(p,&x,4); }
static int hex_to_bin(int c) { if (c>='0'&&c<='9')return c-'0'; c=tolower(c); return c>='a'&&c<='f'?c-'a'+10:-1; }
static void mtd_probe_devices(void) {}
static struct mtd_info *get_mtd_device_nm(const char *name) { return !strcmp(name,"ubi")?&part:!strcmp(name,"spi-nand0")?&raw:NULL; }
static void put_mtd_device(struct mtd_info *mtd) {}
static int mtd_block_isbad(struct mtd_info *mtd, loff_t off) { return (off + mtd->offset)/0x20000 == bad_block; }
static int mtd_read(struct mtd_info *mtd, loff_t off, size_t len, size_t *got, void *buf) {
    assert(off>=0 && off+len<=mtd->size);
    if (mtd==&part) {
        size_t peb=off/0x20000, pos=off%0x20000;
        if(!pos){assert(len==2048);memcpy(buf,ec_pages[peb],len);}
        else {assert(pos>=2048&&pos+len<=0x6800&&layout_pages[peb]);memcpy(buf,layout_pages[peb]+pos-2048,len);
            if(layout_read_fault==layout_writes)((u8*)buf)[len-1]^=1;}
    } else if (off>=0x200000 && off+len<=0x220000) memcpy(buf,env_bytes+off-0x200000,len);
    else if (off>=0x600000 && off+len<=0x700000) memcpy(buf,chain+off-0x600000,len);
    else if (off>=0x400000 && off+len<=0x400000+sizeof(dsd)) memcpy(buf,dsd+off-0x400000,len);
    else if (off==0x1e0c0000 && len<=sizeof(bbt)) memcpy(buf,bbt,len);
    else if (off==0x1ffe0000 && len<=sizeof(bmt)) memcpy(buf,bmt,len);
    else memset(buf,0xff,len);
    if (corrupt_read && event && off==0x600000) ((u8 *)buf)[0]^=1;
    *got=len;
    if(off==pon_read_fault) {
        if(!pon_read_result)*got=len-1;
        return pon_read_result;
    }
    return 0;
}
static void clear_volumes(void) {
    for (int i=0;i<128;i++) if (device.volumes[i]) { free(device.volumes[i]->data); free(device.volumes[i]); }
    memset(&device,0,sizeof(device)); device.leb_size=126976;device.avail_pebs=3400;device.vtbl_slots=128;
}
static int mtd_erase(struct mtd_info *mtd, struct erase_info *e) {
    assert(e->addr>=0 && e->addr+e->len<=mtd->size && e->len==0x20000);
    int ret=mutating(); if(ret)return ret;
    if (mtd==&part) {
        if(e->addr==0)clear_volumes();
        ubi_erases++; memset(ec_pages[e->addr/0x20000],0xff,2048);
        free(layout_pages[e->addr/0x20000]);layout_pages[e->addr/0x20000]=NULL;
    } else if (e->addr==0x200000) { env_erases++; assert(ubi_erases==3504); memset(env_bytes,0xff,sizeof(env_bytes)); }
    else { assert(e->addr>=0x600000 && e->addr<0x700000);raw_erases++;memset(chain+e->addr-0x600000,0xff,e->len); }
    return 0;
}
static int mtd_write(struct mtd_info *mtd, loff_t off, size_t len, size_t *got, const void *buf) {
    int ret=mutating();if(ret)return ret;
    assert(!(off%2048)&&!(len%2048)); *got=short_write?len-1:len;
    if (mtd==&part) {
        size_t peb=off/0x20000, pos=off%0x20000;
        if(!pos){assert(len==2048);memcpy(ec_pages[peb],buf,len);}
        else {
            assert(pos==2048&&len==0x6000&&!layout_pages[peb]);layout_writes++;
            if(layout_write_fault==layout_writes)return -EIO;
            if(layout_short_write==layout_writes)*got=len-1;
            layout_pages[peb]=malloc(len);memcpy(layout_pages[peb],buf,len);
        }
    }
    else if(off==0x200000)memcpy(env_bytes,buf,len);
    else { assert(off>=0x600000&&off+len<=0x700000);memcpy(chain+off-0x600000,buf,len); }
    return 0;
}
static int ubi_detach(void) { ubi_detach_calls++;ubi_devices[0]=NULL;return 0; }
static int ubi_part(const char *name, const char *offset) {
    ubi_attach_calls++;
    assert(!strcmp(name,"ubi"));device.ro_mode=!(part.flags&MTD_WRITEABLE);
    if(!device.ro_mode)assert(write_enabled);
    int ret=host_attach_volume_table();if(ret)return ret;
    ubi_devices[0]=&device;return 0;
}
static struct ubi_volume *ubi_find_volume(const char *name) {
    for(int i=0;i<128;i++) if(device.volumes[i]&&!strcmp(device.volumes[i]->name,name))return device.volumes[i];
    return NULL;
}
static int ubi_create_vol(const char *name, long long bytes, bool dynamic, int id, bool skip) {
    int ret=mutating();if(ret)return ret;
    assert(ubi_devices[0]&&!device.ro_mode && id>=0&&id<128&&!device.volumes[id]);
    int pebs=bytes==-1?device.avail_pebs:DIV_ROUND_UP(bytes,126976);
    if(pebs<=0||pebs>device.avail_pebs)return -ENOSPC;
    struct ubi_volume *v=calloc(1,sizeof(*v));device.volumes[id]=v;
    strcpy(v->name,name);v->vol_id=id;v->vol_type=dynamic?1:2;v->reserved_pebs=pebs;
    v->usable_leb_size=126976;v->ubi=&device;device.avail_pebs-=pebs;
    v->used_bytes=dynamic?(size_t)pebs*126976:0;return 0;
}
static int ubi_remove_vol(const char *name) {
    int ret=mutating();if(ret)return ret;
    struct ubi_volume *v=ubi_find_volume(name);assert(v);device.avail_pebs+=v->reserved_pebs;
    device.volumes[v->vol_id]=NULL;free(v->data);free(v);return 0;
}
static struct ubi_volume_desc *ubi_open_volume_nm(int id,const char *name,int mode) {
    static struct ubi_volume_desc desc;desc.vol=ubi_find_volume(name);return desc.vol?&desc:NULL;
}
static void ubi_close_volume(struct ubi_volume_desc *d) {}
static int ubi_start_update(struct ubi_device *ubi,struct ubi_volume *v,size_t size) {
    int ret=mutating();if(ret)return ret;
    v->updating=1;v->upd_marker=1;v->upd_buf=malloc(126976);free(v->data);v->data=malloc(size);
    v->used_bytes=size;v->received=0;return 0;
}
static int ubi_more_update_data(struct ubi_device *ubi,struct ubi_volume *v,const void *buf,size_t len) {
    int ret=mutating();if(ret)return ret;
    memcpy(v->data+v->received,buf,len);v->received+=len;
    if(v->received==v->used_bytes){v->updating=0;v->upd_marker=0;free(v->upd_buf);v->upd_buf=NULL;}
    return 0;
}
static int ubi_volume_read(const char *name,void *buf,loff_t off,size_t len) {
    struct ubi_volume *v=ubi_find_volume(name);assert(v&&v->data&&off+len<=v->used_bytes);
    memcpy(buf,v->data+off,len);return 0;
}
static int run_commandf(const char *fmt,...) { return -EINVAL; }
int recovery_validate_q1000k_fit(const void *fit,size_t size) { return -EINVAL; }

/* INSERT PRODUCTION CODE */

/* INSERT UBI ATTACH VALIDATORS */

static void progress(void *ctx,bool erase,u32 done,u32 total) { assert(done<=total); }
static void reset(void) {
    write_enabled=false;event=fail_event=short_write=corrupt_read=raw_erases=env_erases=ubi_erases=gate_open_calls=0;bad_block=-1;
    pon_read_fault=-1;pon_read_result=0;
    layout_write_fault=layout_read_fault=layout_short_write=layout_writes=0;
    ubi_attach_calls=ubi_detach_calls=0;
    for(size_t i=0;i<ARRAY_SIZE(layout_pages);i++){free(layout_pages[i]);layout_pages[i]=NULL;}
    raw=(struct mtd_info){NULL,0x20000000,0,0x20000,2048,0,MTD_WRITEABLE};
    part=(struct mtd_info){&raw,0x1b600000,0x700000,0x20000,2048,0,MTD_WRITEABLE};
    memset(env_bytes,0xff,sizeof(env_bytes));memset(env_bytes,0,0x4000);
    const char vars[]="alpha=keep\0bootcmd=flash imgread 2048;bootm\0omega=keep=too\0";
    memcpy(env_bytes+4,vars,sizeof(vars));put_unaligned_le32(crc32(0,env_bytes+4,0x3ffc),env_bytes);
    memset(chain,0x55,sizeof(chain));memset(ec_pages,0xff,sizeof(ec_pages));
    memset(dsd,0,sizeof(dsd));strcpy((char*)dsd,"wan_mac=02:11:22:33:44:50\nlan_mac=02:11:22:33:44:51\nserial_number=Q1000K_TEST_SERIAL\nfsan=TEST1234ABcd\n");
    for(size_t i=0;i<0x201;i++){dsd[0x11000+i]=(u8)(i+0x31);dsd[0x12000+i]=(u8)(i+0x87);}
    memset(bbt,0xff,sizeof(bbt));memcpy(bbt,"RAWB",4);put_unaligned_le32(1,bbt+4);bbt[8]=1;bbt[9]=0;memset(bbt+12,0,500);
    memset(bmt,0xff,sizeof(bmt));memcpy(bmt,"BMT",3);bmt[3]=1;bmt[5]=0;bmt[6]=1;memset(bmt+20,0,1000);
    clear_volumes();ubi_devices[0]=NULL;
}
static void assert_env_preserved(const u8 *before,const u8 *after) {
    size_t pos,len;assert(!q1000k_env_bootcmd(after,&pos,&len));
    assert(!strcmp((const char*)after+pos,"bootcmd=" Q1000K_VENDOR_BOOTCMD));
    for(size_t i=4;before[i];i+=strlen((const char*)before+i)+1) {
        if(!memcmp(before+i,"bootcmd=",8))continue;
        bool found=false;
        for(size_t j=4;after[j];j+=strlen((const char*)after+j)+1)
            if(!strcmp((const char*)before+i,(const char*)after+j))found=true;
        assert(found);
    }
    assert(!memcmp(before+0x4000,after+0x4000,0x1c000));
}
static void assert_factory_pon(const u8 *factory) {
    const char *fsan=strstr((const char*)dsd,"fsan=");assert(fsan);
    assert(!memcmp(factory+0x9000,fsan+5,12));assert(!factory[0x900c]);
    assert(!memcmp(factory+0xa000,dsd+0x11000,0x201));
    assert(!memcmp(factory+0xb000,dsd+0x12000,0x201));
    assert(memcmp(factory+0xa000,factory+0xb000,0x201));
    assert(strcmp((const char*)factory+0x8000,(const char*)factory+0x9000));
    for(size_t i=0;i<0x5000;i++)assert(!factory[i]);
    for(size_t i=0x7000;i<0x8000;i++)assert(!factory[i]);
    for(size_t i=0x900d;i<0xa000;i++)assert(!factory[i]);
    for(size_t i=0xa201;i<0xb000;i++)assert(!factory[i]);
    for(size_t i=0xb201;i<0x10000;i++)assert(!factory[i]);
}
int main(int argc,char **argv) {
    reset();
    assert(q1000k_env_crc((const u8 *)"123456789",9)==0xcbf43926U);
    assert(crc32_le(UBI_CRC32_INIT,(const u8 *)"123456789",9)==0x340bc6d9U);
    if(argc==2) {
        FILE *f=fopen(argv[1],"rb");assert(f);
        fseek(f,0x200000,SEEK_SET);assert(fread(env_bytes,1,sizeof(env_bytes),f)==sizeof(env_bytes));
        fseek(f,0x400000,SEEK_SET);assert(fread(dsd,1,sizeof(dsd),f)==sizeof(dsd));
        fseek(f,0x1e0c0000,SEEK_SET);assert(fread(bbt,1,sizeof(bbt),f)==sizeof(bbt));
        fseek(f,0x1ffe0000,SEEK_SET);assert(fread(bmt,1,sizeof(bmt),f)==sizeof(bmt));fclose(f);
    }
    u8 original[0x20000];memcpy(original,env_bytes,sizeof(original));
    u32 original_dsd_crc=crc32(0,dsd,sizeof(dsd));
    struct q1000k_install_plan *plan=NULL;
    assert(!q1000k_install_prepare(&plan, true));assert(!event&&!write_enabled&&!gate_open_calls);
    assert_env_preserved(original,plan->env_block);
    assert(plan->factory[0x5000]==2 || argc==2);
    for(size_t i=0;i<0x5000;i++)assert(!plan->factory[i]);
    for(size_t i=0x7000;i<0x8000;i++)assert(!plan->factory[i]);
    assert(plan->factory[0x8000]);
    assert_factory_pon(plan->factory);
    assert(crc32(0,dsd,sizeof(dsd))==original_dsd_crc);
    bool changed=true;assert(!q1000k_rewrite_env(plan->env_block,&changed)&&!changed);
    q1000k_install_release(plan);
    if(argc==2){puts("PASS: real backup environment, empty BBT/BMT, FSAN and both PON calibration copies in 64 KiB factory (read-only)");return 0;}
    for(int fault=0;fault<8;fault++) {
        reset();
        if(fault==0)bbt[12]=1;
        if(fault==1)bmt[5]=1;
        if(fault==2)env_bytes[99]^=1;
        if(fault==3)dsd[8]='z';
        if(fault==4)bad_block=48;
        if(fault==5)part.offset=0;
        if(fault==6)raw.subpage_sft=1;
        if(fault==7)memcpy(env_bytes+0x4000,env_bytes,0x4000);
        assert(q1000k_install_prepare(&plan, true));assert(!plan&&!event&&!write_enabled);
    }
    /* Reject incomplete/ambiguous PON data while NAND is still read-only. */
    for(int fault=0;fault<15;fault++) {
        reset();char *fsan=strstr((char*)dsd,"fsan=");assert(fsan);
        if(fault==0)memcpy(fsan,"none",4);
        if(fault==1)strcpy(fsan,"fsan=\n");
        if(fault==2)strcpy(fsan,"fsan=TEST1234ABC\n");
        if(fault==3)strcpy(fsan,"fsan=TEST1234ABCDE\n");
        if(fault==4)fsan[5]=' ';
        if(fault==5)fsan[5]=0x80;
        if(fault==6)strcat((char*)dsd,"fsan=DIFF87654321\n");
        if(fault==7)memset(dsd+0x11000,0xff,0x201);
        if(fault==8)memset(dsd+0x11000,0,0x201);
        if(fault==9)memset(dsd+0x12000,0xff,0x201);
        if(fault==10)memset(dsd+0x12000,0,0x201);
        if(fault>=11){pon_read_fault=(fault&1)?0x411000:0x412000;pon_read_result=fault<13?-EBADMSG:0;}
        original_dsd_crc=crc32(0,dsd,sizeof(dsd));
        assert(q1000k_install_prepare(&plan, true));
        assert(!plan&&!event&&!write_enabled&&!gate_open_calls);
        assert(crc32(0,dsd,sizeof(dsd))==original_dsd_crc);
    }
    for(int which=0;which<2;which++) {
        reset();pon_read_fault=which?0x412000:0x411000;pon_read_result=-EUCLEAN;
        assert(!q1000k_install_prepare(&plan, true));assert_factory_pon(plan->factory);
        assert(!event&&!write_enabled&&!gate_open_calls);q1000k_install_release(plan);
    }
    /* Full transaction: exact slot, exact partition, factory and numbered volumes. */
    reset();memcpy(original,env_bytes,sizeof(original));assert(!q1000k_install_prepare(&plan, true));
    u8 image[0x40001];memset(image,0xa7,sizeof(image));
    struct ubi_ec_hdr *old=(void*)ec_pages[0];memset(old,0,sizeof(*old));
    old->magic=cpu_to_be32(UBI_EC_HDR_MAGIC);old->version=UBI_VERSION;
    old->ec=cpu_to_be64(7);old->vid_hdr_offset=cpu_to_be32(2048);old->data_offset=cpu_to_be32(4096);
    old->hdr_crc=cpu_to_be32(crc32_le(UBI_CRC32_INIT,(const u8*)old,UBI_EC_HDR_SIZE_CRC));
    airoha_snand_set_write_enabled(true);
    assert(!q1000k_install_commit(plan,image,sizeof(image),progress,NULL));
    assert(raw_erases==8&&env_erases==1&&ubi_erases==3504);
    assert(!memcmp(chain,image,sizeof(image)));
    for(size_t i=sizeof(image);i<sizeof(chain);i++)assert(chain[i]==0xff);
    assert_env_preserved(original,env_bytes);
    for(int i=0;i<6;i++)assert(device.volumes[i]&&device.volumes[i]->vol_id==i);
    assert(device.volumes[0]->reserved_pebs==1&&device.volumes[1]->reserved_pebs==1);
    assert(device.volumes[2]->vol_type==2&&device.volumes[2]->used_bytes==65536);
    assert(!memcmp(device.volumes[2]->data,plan->factory,65536));
    assert(layout_writes==2);
    assert_factory_pon(device.volumes[2]->data);
    for(int i=0;i<3504;i++) {
        struct ubi_ec_hdr *h=(void*)ec_pages[i];assert(q1000k_ec_valid(h));
        assert(be64_to_cpu(h->ec)==8&&be32_to_cpu(h->vid_hdr_offset)==2048&&be32_to_cpu(h->data_offset)==4096);
    }
    q1000k_install_release(plan);airoha_snand_set_write_enabled(false);
    assert(!q1000k_ubi_check_layout());assert(device.ro_mode&&!ubi_devices[0]&&(part.flags&MTD_WRITEABLE));
    /* Updating an installed chainloader preserves populated UBI and the exact environment. */
    memcpy(original,env_bytes,sizeof(original));
    struct ubi_device saved_device=device;
    u32 saved_ec_crc=crc32(0,(u8*)ec_pages,sizeof(ec_pages));
    int saved_attaches=ubi_attach_calls,saved_detaches=ubi_detach_calls,saved_writes=event;
    int saved_raw_erases=raw_erases,saved_ubi_erases=ubi_erases,saved_env_erases=env_erases;
    assert(!q1000k_install_prepare(&plan,false));
    assert(!plan->prepare_ubi&&!plan->env_block&&!plan->factory&&!write_enabled);
    assert(event==saved_writes);
    airoha_snand_set_write_enabled(true);
    assert(!q1000k_install_commit(plan,image,sizeof(image),progress,NULL));
    assert(raw_erases==saved_raw_erases+8&&event==saved_writes+11);
    assert(ubi_erases==saved_ubi_erases&&env_erases==saved_env_erases);
    assert(ubi_attach_calls==saved_attaches&&ubi_detach_calls==saved_detaches);
    assert(!memcmp(&device,&saved_device,sizeof(device)));
    assert(!memcmp(original,env_bytes,sizeof(original))&&!memcmp(chain,image,sizeof(image)));
    assert(saved_ec_crc==crc32(0,(u8*)ec_pages,sizeof(ec_pages)));
    q1000k_install_release(plan);airoha_snand_set_write_enabled(false);
    /* Firmware upload preserves all other volumes and allows failed-upload retries. */
    struct ubi_volume *factory=device.volumes[2],*recovery=device.volumes[3],*env0=device.volumes[0];
    u8 saved_factory[65536];memcpy(saved_factory,factory->data,sizeof(saved_factory));
    airoha_snand_set_write_enabled(true);
    assert(!q1000k_ubi_upload("fit",image,sizeof(image),progress,NULL));
    assert(device.volumes[0]==env0&&device.volumes[2]==factory&&device.volumes[3]==recovery);
    assert(!memcmp(factory->data,saved_factory,sizeof(saved_factory)));
    assert(!memcmp(device.volumes[4]->data,image,sizeof(image)));
    assert(device.volumes[5]&&device.volumes[5]->reserved_pebs>1);
    int saved_event=event;
    assert(q1000k_ubi_upload("fit",image,0x70000000,progress,NULL)==-ENOSPC);assert(event==saved_event);
    /* An update-marker or missing target/overlay is retryable. */
    device.volumes[4]->upd_marker=1;
    airoha_snand_set_write_enabled(false);assert(!q1000k_ubi_check_layout());
    airoha_snand_set_write_enabled(true);assert(!q1000k_ubi_upload("fit",image,sizeof(image),progress,NULL));
    assert(!ubi_part("ubi",NULL));assert(!ubi_remove_vol("fit"));assert(!ubi_remove_vol("rootfs_data"));ubi_detach();
    airoha_snand_set_write_enabled(false);assert(!q1000k_ubi_check_layout());
    airoha_snand_set_write_enabled(true);assert(!q1000k_ubi_upload("fit",image,sizeof(image),progress,NULL));
    assert(!q1000k_ubi_upload("recovery",image,sizeof(image),progress,NULL));
    assert(device.volumes[2]==factory&&device.volumes[0]==env0);
    assert(!memcmp(factory->data,saved_factory,sizeof(saved_factory)));
    strcpy(device.volumes[3]->name,"wrong");airoha_snand_set_write_enabled(false);assert(q1000k_ubi_check_layout());
    /* Every slot-only erase/write failure stops without touching UBI or environment. */
    reset();memset(env_bytes,0x12,sizeof(env_bytes));memcpy(original,env_bytes,sizeof(original));
    memset(dsd,0,sizeof(dsd));memset(ec_pages,0x59,sizeof(ec_pages));
    saved_ec_crc=crc32(0,(u8*)ec_pages,sizeof(ec_pages));
    assert(!q1000k_install_prepare(&plan,false));
    airoha_snand_set_write_enabled(true);
    assert(!q1000k_install_commit(plan,image,sizeof(image),progress,NULL));
    assert(!ubi_erases&&!env_erases&&!ubi_attach_calls&&!ubi_detach_calls);
    assert(!memcmp(original,env_bytes,sizeof(original)));
    assert(saved_ec_crc==crc32(0,(u8*)ec_pages,sizeof(ec_pages)));
    q1000k_install_release(plan);
    for(int failure=1;failure<=13;failure++) {
        reset();memcpy(original,env_bytes,sizeof(original));
        assert(!q1000k_install_prepare(&plan,false));
        airoha_snand_set_write_enabled(true);
        fail_event=failure<=11?failure:0;
        short_write=failure==12;corrupt_read=failure==13;
        assert(q1000k_install_commit(plan,image,sizeof(image),progress,NULL)==-EIO);
        assert(!ubi_erases&&!env_erases&&!ubi_attach_calls&&!ubi_detach_calls);
        assert(!memcmp(original,env_bytes,sizeof(original)));
        q1000k_install_release(plan);
    }
    /* Slot-only preflight still refuses unsupported mapping and bad boot blocks. */
    for(int failure=0;failure<2;failure++) {
        reset();if(failure)bad_block=0x600000/0x20000;else bmt[5]=1;
        assert(q1000k_install_prepare(&plan,false));
        assert(!plan&&!event&&!write_enabled&&!ubi_attach_calls);
    }
    /* Failures before vendor-env commit never select an unverified chainloader. */
    const int failures[]={1,2,6,12,100,7019,7020,7021,7022,7023,7024,7025,7026,7027,7028,7029};
    for(size_t f=0;f<ARRAY_SIZE(failures);f++) {
        reset();memcpy(original,env_bytes,sizeof(original));assert(!q1000k_install_prepare(&plan, true));
        airoha_snand_set_write_enabled(true);fail_event=failures[f];
        int ret=q1000k_install_commit(plan,image,sizeof(image),progress,NULL);
        assert(ret==-EIO);assert(!env_erases);assert(!memcmp(original,env_bytes,sizeof(original)));
        assert(!ubi_devices[0]);q1000k_install_release(plan);
    }
    for(int f=0;f<2;f++) {
        reset();assert(!q1000k_install_prepare(&plan, true));airoha_snand_set_write_enabled(true);
        short_write=f==0;corrupt_read=f==1;
        assert(q1000k_install_commit(plan,image,sizeof(image),progress,NULL)==-EIO);
        assert(!env_erases&&!ubi_erases);q1000k_install_release(plan);
    }
    /* Either layout copy's write/readback failure must prevent vendor bootcmd update. */
    for(int f=0;f<6;f++) {
        reset();memcpy(original,env_bytes,sizeof(original));assert(!q1000k_install_prepare(&plan, true));
        airoha_snand_set_write_enabled(true);
        if(f<2)layout_write_fault=f+1;
        else if(f<4)layout_read_fault=f-1;
        else layout_short_write=f-3;
        assert(q1000k_install_commit(plan,image,sizeof(image),progress,NULL)==-EIO);
        assert(!env_erases&&!memcmp(original,env_bytes,sizeof(original))&&!ubi_devices[0]);
        q1000k_install_release(plan);
    }
    /* Reproduce the reported EC-only medium with the real UBI table reader. */
    reset();airoha_snand_set_write_enabled(true);
    assert(!q1000k_format_ubi(&part,progress,NULL));ubi_detach();
    for(size_t i=0;i<ARRAY_SIZE(layout_pages);i++){free(layout_pages[i]);layout_pages[i]=NULL;}
    assert(ubi_part("ubi",NULL)==-EINVAL);
    assert(!q1000k_format_ubi(&part,progress,NULL));ubi_detach();
    assert(layout_writes==4);
    for(int i=0;i<3504;i++)assert(be64_to_cpu(((struct ubi_ec_hdr*)ec_pages[i])->ec)==2);
    /* The real table reader rejects corrupt record CRCs in both copies. */
    layout_pages[0][2048]^=1;layout_pages[1][2048]^=1;
    assert(ubi_part("ubi",NULL)==-EINVAL);
    layout_pages[0][2048]^=1;layout_pages[1][2048]^=1;
    assert(!ubi_part("ubi",NULL));ubi_detach();
    /* Layout copies use distinct good PEBs even when the first UBI PEB is bad. */
    reset();bad_block=0x700000/0x20000;airoha_snand_set_write_enabled(true);
    assert(!q1000k_format_ubi(&part,progress,NULL));
    assert(!layout_pages[0]&&layout_pages[1]&&layout_pages[2]&&layout_writes==2);
    reset();puts("PASS: installer preflight, exact writes, verification failures, UBI layout, preservation, read-only attach and retry");return 0;
}
