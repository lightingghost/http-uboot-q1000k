/* SPDX-License-Identifier: GPL-2.0+ */
/* Included by installer_harness.c: real UBI media validators and table reader.
 * The MTD scan adapter below models only the formatter's empty layout volume;
 * this is not a complete UBI wear-leveling or flash-driver simulation.
 */
#define be16_to_cpu bswap_16
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((long)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define vzalloc(n) calloc(1,n)
#define kfree free
#define cond_resched() ((void)0)
#define dump_stack() ((void)0)
#define dbg_gen(...) ((void)0)
#define ubi_err(ubi,fmt,...) fprintf(stderr,fmt "\n",##__VA_ARGS__)
#define ubi_warn ubi_err
#define ubi_msg ubi_err
#define ubi_dump_ec_hdr(...) ((void)0)
#define ubi_dump_vid_hdr(...) ((void)0)
#define ubi_dump_vtbl_record(...) ((void)0)
#define UBI_IO_BITFLIPS 1
#define mtd_is_eccerr(e) ((e)==-EBADMSG)

struct ubi_ainf_peb { int lnum, pnum, scrub; };
struct rb_node { struct rb_node *next; struct ubi_ainf_peb *peb; };
struct ubi_ainf_volume { int leb_count; struct rb_node *root; };
struct ubi_attach_info { int is_empty; struct ubi_ainf_volume layout; };
#define ubi_rb_for_each_entry(rb,aeb,root,member) \
    for(rb=*(root);rb&&((aeb)=rb->peb);rb=rb->next)
static struct ubi_vtbl_record empty_vtbl_record;
static struct ubi_ainf_volume *ubi_find_av(struct ubi_attach_info *ai,int id) {
    assert(id==UBI_LAYOUT_VOLUME_ID);return ai->layout.leb_count?&ai->layout:NULL;
}
static int ubi_io_read_data(struct ubi_device *ubi,void *buf,int peb,int off,int len) {
    size_t got;return mtd_read(&part,(loff_t)peb*0x20000+4096+off,len,&got,buf);
}
/* Repair is not needed for newly formatted, read-back-verified metadata. */
static int create_vtbl(struct ubi_device *ubi,struct ubi_attach_info *ai,int copy,void *table) { return -EIO; }
static struct ubi_vtbl_record *create_empty_lvol(struct ubi_device *ubi,struct ubi_attach_info *ai) { return ERR_PTR(-EINVAL); }
static int init_volumes(struct ubi_device *ubi,struct ubi_attach_info *ai,struct ubi_vtbl_record *table) { return 0; }
static int check_attaching_info(struct ubi_device *ubi,struct ubi_attach_info *ai) { return 0; }

/* These functions use the kernel-style uncomplemented CRC, not zlib's API. */
#define crc32(seed,buf,len) crc32_le(seed,(const u8 *)(buf),len)
/* INSERT REAL UBI FUNCTIONS */
#undef crc32

static int host_attach_volume_table(void) {
    struct ubi_attach_info ai={0};
    struct ubi_ainf_peb pebs[2];struct rb_node nodes[2];
    u32 sequence=0;unsigned int copies=0;
    device.min_io_size=2048;device.vid_hdr_offset=2048;device.leb_start=4096;
    device.good_peb_count=0;device.corr_peb_count=0;
    for(size_t i=0;i<ARRAY_SIZE(ec_pages);i++) {
        if(mtd_block_isbad(&part,i*0x20000))continue;
        struct ubi_ec_hdr *ec=(void*)ec_pages[i];device.good_peb_count++;
        if(!q1000k_ec_valid(ec)||validate_ec_hdr(&device,ec))return -EINVAL;
        if(!sequence)sequence=be32_to_cpu(ec->image_seq);
        if(sequence!=be32_to_cpu(ec->image_seq))return -EINVAL;
        if(!layout_pages[i])continue; /* Valid EC, erased VID: NOT ai.is_empty. */
        struct ubi_vid_hdr *vid=(void*)layout_pages[i];
        if(be32_to_cpu(vid->magic)!=UBI_VID_HDR_MAGIC || vid->version!=UBI_VERSION ||
           be32_to_cpu(vid->hdr_crc)!=crc32_le(UBI_CRC32_INIT,(u8*)vid,UBI_VID_HDR_SIZE_CRC) ||
           validate_vid_hdr(&device,vid))return -EINVAL;
        if(be32_to_cpu(vid->vol_id)!=UBI_LAYOUT_VOLUME_ID)return -EINVAL;
        unsigned int lnum=be32_to_cpu(vid->lnum);
        if(lnum>=2 || (copies&(1U<<lnum)))return -EINVAL;
        copies|=1U<<lnum;
        pebs[lnum]=(struct ubi_ainf_peb){lnum,i,0};
        nodes[lnum]=(struct rb_node){ai.layout.root,&pebs[lnum]};
        ai.layout.root=&nodes[lnum];ai.layout.leb_count++;
    }
    /* Executes the exact missing-layout error path reported on the device. */
    int ret=ubi_read_volume_table(&device,&ai);
    if(!ret){free(device.vtbl);device.vtbl=NULL;}
    return ret;
}
