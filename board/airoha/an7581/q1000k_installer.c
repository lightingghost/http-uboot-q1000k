// SPDX-License-Identifier: GPL-2.0+
/* Explicit HTTP installation only; all preparation and boot reads are read-only. */
#include <command.h>
#include <airoha_snand.h>
#include <dm.h>
#include <image.h>
#include <malloc.h>
#include <memalign.h>
#include <mtd.h>
#include <q1000k_installer.h>
#include <ubi_uboot.h>
#include <u-boot/schedule.h>
#include <asm/global_data.h>
#include <asm/unaligned.h>
#include <linux/ctype.h>

DECLARE_GLOBAL_DATA_PTR;

struct q1000k_install_plan {
	struct mtd_info *raw;
	struct mtd_info *ubi;
	u8 *env_block;
	u8 *factory;
	bool env_changed;
	bool prepare_ubi;
};

static int q1000k_read(struct mtd_info *mtd, loff_t off, void *buf, size_t len)
{
	size_t got = 0;
	int ret = mtd_read(mtd, off, len, &got, buf);

	if ((ret && ret != -EUCLEAN) || got != len)
		return ret ? ret : -EIO;
	schedule();
	return 0;
}

static int q1000k_get_mtd(struct mtd_info **rawp, struct mtd_info **ubip)
{
	struct mtd_info *raw, *ubi;

	mtd_probe_devices();
	raw = get_mtd_device_nm("spi-nand0");
	if (IS_ERR_OR_NULL(raw))
		return -ENODEV;
	ubi = get_mtd_device_nm("ubi");
	if (IS_ERR_OR_NULL(ubi)) {
		put_mtd_device(raw);
		return -ENODEV;
	}
	if (raw->parent || raw->size != Q1000K_NAND_SIZE ||
	    raw->erasesize != Q1000K_ERASE_SIZE ||
	    raw->writesize != Q1000K_PAGE_SIZE || raw->subpage_sft ||
	    ubi->parent != raw || ubi->offset != Q1000K_UBI_OFFSET ||
	    ubi->size != Q1000K_UBI_SIZE) {
		put_mtd_device(ubi);
		put_mtd_device(raw);
		return -EINVAL;
	}
	*rawp = raw;
	*ubip = ubi;
	return 0;
}

/* The backup establishes 250-entry v1 tables, both empty. Never rebuild them. */
static int q1000k_empty_mapping_table(const u8 *data, bool bbt)
{
	size_t header = bbt ? 12 : 20;
	size_t bytes = bbt ? 500 : 1000;
	size_t i;

	if (bbt) {
		if (memcmp(data, "RAWB", 4) || get_unaligned_le32(data + 4) != 1 ||
		    data[8] != 1 || data[9])
			return -EOPNOTSUPP;
	} else if (memcmp(data, "BMT", 3) || data[3] != 1 ||
		   data[5] || data[6] != 1) {
		return -EOPNOTSUPP;
	}
	for (i = 0; i < bytes; i++)
		if (data[header + i])
			return -EOPNOTSUPP;
	return 0;
}

static int q1000k_check_mapping(struct mtd_info *raw)
{
	const loff_t offsets[] = { 0x1e0c0000, 0x1ffe0000 };
	u8 *page = malloc_cache_aligned(Q1000K_PAGE_SIZE);
	int ret = -ENOMEM;
	loff_t off;

	if (!page)
		return ret;
	/* Reject relocated or additional table copies, including stale copies. */
	for (off = 0x1c000000; off < raw->size; off += raw->erasesize) {
		ret = mtd_block_isbad(raw, off);
		if (ret < 0)
			goto out;
		if (ret) {
			if (off == offsets[0] || off == offsets[1]) {
				ret = -EIO;
				goto out;
			}
			continue;
		}
		ret = q1000k_read(raw, off, page, Q1000K_PAGE_SIZE);
		if (ret)
			goto out;
		if (off == offsets[0] || off == offsets[1]) {
			ret = q1000k_empty_mapping_table(page, off == offsets[0]);
		} else if (!memcmp(page, "RAWB", 4) || !memcmp(page, "BMT", 3)) {
			ret = -EOPNOTSUPP;
		}
		if (ret)
			goto out;
	}
	/* A new bad block before the slot also invalidates identity mapping. */
	for (off = 0; off < Q1000K_UBI_OFFSET; off += raw->erasesize) {
		ret = mtd_block_isbad(raw, off);
		if (ret) {
			ret = ret < 0 ? ret : -EIO;
			goto out;
		}
	}
out:
	free(page);
	if (ret)
		puts("Q1000K: unsupported BBT/BMT or bad raw boot block; no installation writes\n");
	return ret;
}

/* UBI's crc32 macro is the uncomplemented Linux CRC, unlike vendor env CRC. */
static u32 q1000k_env_crc(const u8 *data, size_t len)
{
	return crc32_le(~0U, data, len) ^ ~0U;
}

/* Return only a CRC-checked, uniquely named bootcmd in a bounded environment. */
static int q1000k_env_bootcmd(const u8 *record, size_t *pos, size_t *len)
{
	size_t off = 4;
	bool found = false;

	if (get_unaligned_le32(record) != q1000k_env_crc(record + 4, Q1000K_ENV_SIZE - 4))
		return -EBADMSG;
	while (off < Q1000K_ENV_SIZE && record[off]) {
		const u8 *end = memchr(record + off, 0, Q1000K_ENV_SIZE - off);
		size_t n;

		if (!end)
			return -EINVAL;
		n = end - record - off;
		if (!memchr(record + off, '=', n))
			return -EINVAL;
		if (n >= 8 && !memcmp(record + off, "bootcmd=", 8)) {
			if (found)
				return -EINVAL;
			*pos = off;
			*len = n;
			found = true;
		}
		off += n + 1;
	}
	if (!found || off >= Q1000K_ENV_SIZE)
		return -EINVAL;
	return 0;
}

static int q1000k_rewrite_env(u8 *block, bool *changed)
{
	const char newvar[] = "bootcmd=" Q1000K_VENDOR_BOOTCMD;
	size_t pos, len, end, tail;
	int ret = q1000k_env_bootcmd(block, &pos, &len);

	if (ret)
		return ret;
	*changed = strcmp((char *)block + pos, newvar) != 0;
	if (!*changed)
		return 0;
	end = pos + len + 1;
	while (end < Q1000K_ENV_SIZE && block[end]) {
		const u8 *nul = memchr(block + end, 0, Q1000K_ENV_SIZE - end);

		if (!nul)
			return -EINVAL;
		end = nul - block + 1;
	}
	if (end >= Q1000K_ENV_SIZE)
		return -EINVAL;
	tail = end + 1 - (pos + len + 1);
	if (pos + sizeof(newvar) + tail > Q1000K_ENV_SIZE)
		return -ENOSPC;
	memmove(block + pos + sizeof(newvar), block + pos + len + 1, tail);
	memcpy(block + pos, newvar, sizeof(newvar));
	memset(block + pos + sizeof(newvar) + tail, 0,
	       Q1000K_ENV_SIZE - pos - sizeof(newvar) - tail);
	put_unaligned_le32(q1000k_env_crc(block + 4, Q1000K_ENV_SIZE - 4), block);
	return 0;
}

static int q1000k_dsd_value(const u8 *dsd, size_t size, const char *key,
			     char *out, size_t out_size)
{
	size_t off = 0, keylen = strlen(key);
	bool found = false;

	while (off < size && dsd[off] && dsd[off] != 0xff) {
		size_t start = off, len;

		while (off < size && dsd[off] && dsd[off] != '\n' &&
		       dsd[off] != '\r' && dsd[off] != 0xff)
			off++;
		len = off - start;
		if (len > keylen && !memcmp(dsd + start, key, keylen) &&
		    dsd[start + keylen] == '=') {
			if (found)
				return -EINVAL;
			len -= keylen + 1;
			if (len >= out_size)
				return -E2BIG;
			memcpy(out, dsd + start + keylen + 1, len);
			out[len] = 0;
			found = true;
		}
		while (off < size && (dsd[off] == '\n' || dsd[off] == '\r'))
			off++;
	}
	return found ? 0 : -ENOENT;
}

static int q1000k_factory_mac(const char *value, u8 *mac)
{
	int i;

	if (strlen(value) != 17)
		return -EINVAL;
	for (i = 0; i < 6; i++) {
		int hi = hex_to_bin(value[i * 3]);
		int lo = hex_to_bin(value[i * 3 + 1]);

		if (hi < 0 || lo < 0 || (i < 5 && value[i * 3 + 2] != ':'))
			return -EINVAL;
		mac[i] = (hi << 4) | lo;
	}
	if ((mac[0] & 1) || !memcmp(mac, "\0\0\0\0\0\0", 6))
		return -EINVAL;
	return 0;
}

static int q1000k_copy_pon_cal(struct mtd_info *raw, loff_t dsd_offset, u8 *dst)
{
	bool all_zero = true, all_ff = true;
	size_t i;
	int ret;

	/* Copy the OEM dualbob_7572 record verbatim; no verified checksum format. */
	ret = q1000k_read(raw, Q1000K_DSD_OFFSET + dsd_offset, dst,
			   Q1000K_PON_CAL_SIZE);
	if (ret)
		return ret;
	for (i = 0; i < Q1000K_PON_CAL_SIZE; i++) {
		all_zero &= dst[i] == 0;
		all_ff &= dst[i] == 0xff;
	}
	return all_zero || all_ff ? -EINVAL : 0;
}

static int q1000k_build_factory(struct mtd_info *raw, const u8 *dsd, u8 *factory)
{
	char value[128];
	size_t i;
	int ret;

	/* No Wi-Fi EEPROM or fan on Q1000K. Keep those reference fields zero. */
	memset(factory, 0, Q1000K_FACTORY_SIZE);
	ret = q1000k_dsd_value(dsd, 0x4000, "wan_mac", value, sizeof(value));
	if (!ret)
		ret = q1000k_factory_mac(value, factory + 0x5000);
	if (ret)
		return ret;
	ret = q1000k_dsd_value(dsd, 0x4000, "lan_mac", value, sizeof(value));
	if (!ret)
		ret = q1000k_factory_mac(value, factory + 0x6000);
	if (ret)
		return ret;
	ret = q1000k_dsd_value(dsd, 0x4000, "serial_number", value, sizeof(value));
	if (ret || !*value)
		return ret ? ret : -EINVAL;
	memcpy(factory + 0x8000, value, strlen(value));
	ret = q1000k_dsd_value(dsd, 0x4000, "fsan", value, sizeof(value));
	if (ret)
		return ret;
	if (strlen(value) != Q1000K_FSAN_SIZE)
		return -EINVAL;
	for (i = 0; i < Q1000K_FSAN_SIZE; i++)
		if (value[i] < 0x21 || value[i] > 0x7e)
			return -EINVAL;
	/* Keep ASCII spelling/case and the trailing NUL, separate from unit serial. */
	memcpy(factory + Q1000K_FACTORY_FSAN_OFFSET, value, Q1000K_FSAN_SIZE + 1);
	ret = q1000k_copy_pon_cal(raw, Q1000K_DSD_GPON_OFFSET,
				factory + Q1000K_FACTORY_GPON_OFFSET);
	if (ret)
		return ret;
	return q1000k_copy_pon_cal(raw, Q1000K_DSD_XGSPON_OFFSET,
				 factory + Q1000K_FACTORY_XGSPON_OFFSET);
}

void q1000k_install_release(struct q1000k_install_plan *plan)
{
	if (!plan)
		return;
	if (plan->raw)
		put_mtd_device(plan->raw);
	if (plan->ubi)
		put_mtd_device(plan->ubi);
	free(plan->env_block);
	free(plan->factory);
	free(plan);
}

/* Refuse an additional CRC-valid environment record in the vendor partition. */
static int q1000k_check_env_copies(struct mtd_info *raw, u8 *scratch)
{
	loff_t off;

	for (off = Q1000K_ENV_OFFSET + Q1000K_ENV_SIZE; off < 0x400000;
	     off += Q1000K_ENV_SIZE) {
		int ret = q1000k_read(raw, off, scratch, Q1000K_ENV_SIZE);

		if (ret)
			return ret;
		if (get_unaligned_le32(scratch) ==
		    q1000k_env_crc(scratch + 4, Q1000K_ENV_SIZE - 4))
			return -EOPNOTSUPP;
	}
	return 0;
}

int q1000k_install_prepare(struct q1000k_install_plan **out, bool prepare_ubi)
{
	struct q1000k_install_plan *plan = calloc(1, sizeof(*plan));
	u8 *dsd = NULL;
	int ret = -ENOMEM;

	*out = NULL;
	if (!plan)
		return ret;
	plan->prepare_ubi = prepare_ubi;
	ret = q1000k_get_mtd(&plan->raw, &plan->ubi);
	if (ret)
		goto fail;
	ret = q1000k_check_mapping(plan->raw);
	if (ret)
		goto fail;
	/* A slot-only update never attaches UBI or prepares environment/factory data. */
	if (!prepare_ubi) {
		*out = plan;
		return 0;
	}
	plan->env_block = malloc_cache_aligned(Q1000K_ERASE_SIZE);
	plan->factory = malloc_cache_aligned(Q1000K_FACTORY_SIZE);
	dsd = malloc_cache_aligned(0x4000);
	if (!plan->env_block || !plan->factory || !dsd) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = q1000k_check_env_copies(plan->raw, dsd);
	if (!ret)
		ret = q1000k_read(plan->raw, Q1000K_ENV_OFFSET, plan->env_block,
				    Q1000K_ERASE_SIZE);
	if (!ret)
		ret = q1000k_rewrite_env(plan->env_block, &plan->env_changed);
	if (!ret)
		ret = q1000k_read(plan->raw, Q1000K_DSD_OFFSET, dsd, 0x4000);
	if (!ret)
		ret = q1000k_build_factory(plan->raw, dsd, plan->factory);
	if (ret)
		goto fail;
	free(dsd);
	*out = plan;
	return 0;
fail:
	printf("Q1000K installer preflight failed: %d; no installation writes\n", ret);
	free(dsd);
	q1000k_install_release(plan);
	return ret;
}

/* Raw boot regions must never skip, remap or mark blocks bad on failure. */
static int q1000k_write_boot_region(struct mtd_info *raw, loff_t offset,
				   const u8 *image, size_t size, size_t span,
				   q1000k_progress_fn progress, void *ctx)
{
	u8 *buf = malloc_cache_aligned(raw->erasesize);
	u8 *verify = malloc_cache_aligned(raw->erasesize);
	size_t off, got;
	int ret = -ENOMEM;

	if (!buf || !verify)
		goto out;
	if (!size || size > span ||
	    !((offset == Q1000K_CHAIN_OFFSET && span == Q1000K_CHAIN_SIZE) ||
	      (offset == Q1000K_ENV_OFFSET && span == Q1000K_ERASE_SIZE))) {
		ret = -EPERM;
		goto out;
	}
	for (off = 0; off < span; off += raw->erasesize) {
		struct erase_info erase = { .addr = offset + off, .len = raw->erasesize };
		size_t len = off < size ? min_t(size_t, raw->erasesize, size - off) : 0;

		ret = mtd_block_isbad(raw, offset + off);
		if (ret) {
			ret = ret < 0 ? ret : -EIO;
			goto out;
		}
		memset(buf, 0xff, raw->erasesize);
		if (len)
			memcpy(buf, image + off, len);
		ret = mtd_erase(raw, &erase);
		if (ret)
			goto out;
		progress(ctx, true, off + raw->erasesize, span);
		if (len) {
			got = 0;
			ret = mtd_write(raw, offset + off, ALIGN(len, raw->writesize),
					&got, buf);
			if (ret || got != ALIGN(len, raw->writesize)) {
				ret = ret ? ret : -EIO;
				goto out;
			}
		}
		ret = q1000k_read(raw, offset + off, verify, raw->erasesize);
		if (!ret && memcmp(buf, verify, raw->erasesize))
			ret = -EIO;
		if (ret)
			goto out;
		progress(ctx, false, min_t(size_t, off + raw->erasesize, size), size);
	}
out:
	free(buf);
	free(verify);
	return ret;
}

static bool q1000k_ec_valid(const struct ubi_ec_hdr *hdr)
{
	return be32_to_cpu(hdr->magic) == UBI_EC_HDR_MAGIC &&
	       hdr->version == UBI_VERSION &&
	       be64_to_cpu(hdr->ec) < UBI_MAX_ERASECOUNTER &&
	       be32_to_cpu(hdr->hdr_crc) ==
		crc32_le(UBI_CRC32_INIT, (const u8 *)hdr, UBI_EC_HDR_SIZE_CRC);
}

static int q1000k_write_ubi_layout(struct mtd_info *mtd, const loff_t *pebs,
				 q1000k_progress_fn progress, void *ctx)
{
	size_t table_size = ALIGN(UBI_MAX_VOLUMES * UBI_VTBL_RECORD_SIZE,
				  mtd->writesize);
	size_t size = mtd->writesize + table_size, written;
	u8 *buf = malloc_cache_aligned(size);
	u8 *verify = malloc_cache_aligned(size);
	struct ubi_vid_hdr *vid = (void *)buf;
	struct ubi_vtbl_record *table;
	unsigned int copy, i;
	int ret = -ENOMEM;

	if (!buf || !verify)
		goto out;
	if (2 * mtd->writesize + table_size > mtd->erasesize) {
		ret = -EINVAL;
		goto out;
	}
	/* EC headers alone are not an attachable UBI: supply both layout LEBs. */
	memset(buf, 0xff, mtd->writesize);
	memset(vid, 0, sizeof(*vid));
	vid->magic = cpu_to_be32(UBI_VID_HDR_MAGIC);
	vid->version = UBI_VERSION;
	vid->vol_type = UBI_LAYOUT_VOLUME_TYPE;
	vid->compat = UBI_LAYOUT_VOLUME_COMPAT;
	vid->vol_id = cpu_to_be32(UBI_LAYOUT_VOLUME_ID);
	table = (void *)(buf + mtd->writesize);
	memset(table, 0, table_size);
	for (i = 0; i < UBI_MAX_VOLUMES; i++)
		table[i].crc = cpu_to_be32(crc32_le(UBI_CRC32_INIT,
			(const u8 *)&table[i], UBI_VTBL_RECORD_SIZE_CRC));

	puts("Q1000K: writing and verifying both UBI volume tables\n");
	for (copy = 0; copy < UBI_LAYOUT_VOLUME_EBS; copy++) {
		loff_t off = pebs[copy] + mtd->writesize;

		vid->lnum = cpu_to_be32(copy);
		vid->sqnum = cpu_to_be64(copy + 1);
		vid->hdr_crc = cpu_to_be32(crc32_le(UBI_CRC32_INIT, buf,
						 UBI_VID_HDR_SIZE_CRC));
		written = 0;
		ret = mtd_write(mtd, off, size, &written, buf);
		if (ret || written != size) {
			ret = ret ? ret : -EIO;
			goto out;
		}
		ret = q1000k_read(mtd, off, verify, size);
		if (!ret && memcmp(buf, verify, size))
			ret = -EIO;
		if (ret)
			goto out;
		progress(ctx, false, (copy + 1) * size, UBI_LAYOUT_VOLUME_EBS * size);
	}
out:
	free(verify);
	free(buf);
	return ret;
}

static int q1000k_format_ubi(struct mtd_info *mtd,
			      q1000k_progress_fn progress, void *ctx)
{
	u8 *page = malloc_cache_aligned(mtd->writesize);
	struct ubi_ec_hdr *hdr = (void *)page;
	u64 sum = 0, mean, ec;
	u32 valid = 0, sequence = get_timer(0) | 1;
	loff_t layout_pebs[UBI_LAYOUT_VOLUME_EBS];
	unsigned int layout_count = 0;
	loff_t off;
	size_t written;
	int ret = -ENOMEM;

	if (!page)
		return ret;
	ubi_detach();
	/* Retain valid erase counters; use their mean for non-UBI blocks. */
	for (off = 0; off < mtd->size; off += mtd->erasesize) {
		ret = mtd_block_isbad(mtd, off);
		if (ret < 0)
			goto out;
		if (ret)
			continue;
		if (layout_count < UBI_LAYOUT_VOLUME_EBS)
			layout_pebs[layout_count++] = off;
		ret = q1000k_read(mtd, off, page, mtd->writesize);
		if (ret)
			goto out;
		if (q1000k_ec_valid(hdr)) {
			sum += be64_to_cpu(hdr->ec);
			valid++;
		}
		progress(ctx, true, 0, mtd->size);
	}
	if (layout_count != UBI_LAYOUT_VOLUME_EBS) {
		ret = -ENOSPC;
		goto out;
	}
	mean = valid ? div_u64(sum, valid) : 0;
	for (off = 0; off < mtd->size; off += mtd->erasesize) {
		struct erase_info erase = { .addr = off, .len = mtd->erasesize };

		ret = mtd_block_isbad(mtd, off);
		if (ret < 0)
			goto out;
		if (ret)
			continue;
		ret = q1000k_read(mtd, off, page, mtd->writesize);
		if (ret)
			goto out;
		ec = q1000k_ec_valid(hdr) ? be64_to_cpu(hdr->ec) : mean;
		memset(page, 0xff, mtd->writesize);
		memset(hdr, 0, sizeof(*hdr));
		hdr->magic = cpu_to_be32(UBI_EC_HDR_MAGIC);
		hdr->version = UBI_VERSION;
		hdr->ec = cpu_to_be64(ec + 1);
		hdr->vid_hdr_offset = cpu_to_be32(mtd->writesize);
		hdr->data_offset = cpu_to_be32(2 * mtd->writesize);
		hdr->image_seq = cpu_to_be32(sequence);
		hdr->hdr_crc = cpu_to_be32(crc32_le(UBI_CRC32_INIT, page, UBI_EC_HDR_SIZE_CRC));
		ret = mtd_erase(mtd, &erase);
		if (ret)
			goto out;
		written = 0;
		ret = mtd_write(mtd, off, mtd->writesize, &written, page);
		if (ret || written != mtd->writesize) {
			ret = ret ? ret : -EIO;
			goto out;
		}
		progress(ctx, true, off + mtd->erasesize, mtd->size);
	}
	ret = q1000k_write_ubi_layout(mtd, layout_pebs, progress, ctx);
	if (!ret)
		ret = ubi_part("ubi", NULL);
out:
	free(page);
	return ret;
}

static int q1000k_ubi_write_verify(const char *name, const void *image, size_t size,
				 q1000k_progress_fn progress, void *ctx)
{
	struct ubi_volume_desc *desc;
	u8 *buf;
	size_t off;
	int ret;

	desc = ubi_open_volume_nm(0, name, UBI_READWRITE);
	if (IS_ERR_OR_NULL(desc))
		return -ENODEV;
	buf = malloc_cache_aligned(Q1000K_ERASE_SIZE);
	if (!buf) {
		ubi_close_volume(desc);
		return -ENOMEM;
	}
	ret = ubi_start_update(desc->vol->ubi, desc->vol, size);
	for (off = 0; !ret && off < size; off += Q1000K_ERASE_SIZE) {
		size_t len = min_t(size_t, Q1000K_ERASE_SIZE, size - off);

		ret = ubi_more_update_data(desc->vol->ubi, desc->vol, image + off, len);
		if (ret > 0)
			ret = 0;
		progress(ctx, false, off + len, size);
	}
	if (ret && desc->vol->updating) {
		vfree(desc->vol->upd_buf);
		desc->vol->upd_buf = NULL;
		desc->vol->updating = 0;
	}
	ubi_close_volume(desc);
	for (off = 0; !ret && off < size; off += Q1000K_ERASE_SIZE) {
		size_t len = min_t(size_t, Q1000K_ERASE_SIZE, size - off);

		ret = ubi_volume_read(name, buf, off, len);
		if (!ret && memcmp(buf, image + off, len))
			ret = -EIO;
		progress(ctx, false, size, size);
	}
	free(buf);
	return ret;
}

int q1000k_install_commit(struct q1000k_install_plan *plan, const void *image,
			 size_t size, q1000k_progress_fn progress, void *ctx)
{
	int ret;

	if (!plan || !image || !progress || !size || size > Q1000K_CHAIN_SIZE)
		return -EINVAL;
	if (!plan->prepare_ubi)
		puts("Q1000K: U-Boot only; preserving UBI and vendor environment\n");
	puts("Q1000K: installing and verifying chainloader\n");
	ret = q1000k_write_boot_region(plan->raw, Q1000K_CHAIN_OFFSET, image, size,
				      Q1000K_CHAIN_SIZE, progress, ctx);
	if (ret || !plan->prepare_ubi)
		return ret;
	puts("Q1000K: formatting fixed UBI partition\n");
	ret = q1000k_format_ubi(plan->ubi, progress, ctx);
	if (!ret)
		ret = ubi_create_vol("ubootenv", Q1000K_LEB_SIZE, true, 0, false);
	if (!ret)
		ret = ubi_create_vol("ubootenv2", Q1000K_LEB_SIZE, true, 1, false);
	if (!ret)
		ret = ubi_create_vol("factory", Q1000K_FACTORY_SIZE, false, 2, false);
	if (!ret)
		ret = q1000k_ubi_write_verify("factory", plan->factory,
					     Q1000K_FACTORY_SIZE, progress, ctx);
	if (!ret)
		ret = ubi_create_vol("recovery", Q1000K_LEB_SIZE, true, 3, false);
	if (!ret)
		ret = ubi_create_vol("fit", Q1000K_LEB_SIZE, true, 4, false);
	if (!ret)
		ret = ubi_create_vol("rootfs_data", -1, true, 5, false);
	/* Finish all UBI cleanup before modifying the vendor boot selector. */
	ubi_detach();
	if (ret)
		return ret;
	if (plan->env_changed) {
		puts("Q1000K: updating and verifying vendor bootcmd\n");
		ret = q1000k_write_boot_region(plan->raw, Q1000K_ENV_OFFSET,
			plan->env_block, Q1000K_ERASE_SIZE, Q1000K_ERASE_SIZE, progress, ctx);
	}
	return ret;
}

static int q1000k_attach_readonly(void)
{
	struct mtd_info *raw, *mtd;
	u32 flags;
	int ret = q1000k_get_mtd(&raw, &mtd);

	if (ret)
		return ret;
	ubi_detach();
	flags = mtd->flags;
	mtd->flags &= ~MTD_WRITEABLE;
	ret = ubi_part("ubi", NULL);
	mtd->flags = flags;
	put_mtd_device(mtd);
	put_mtd_device(raw);
	return ret;
}

static int q1000k_check_volumes(void)
{
	const char *names[] = { "ubootenv", "ubootenv2", "factory", "recovery", "fit", "rootfs_data" };
	struct ubi_device *ubi = ubi_devices[0];
	int i;

	if (!ubi || ubi->leb_size != Q1000K_LEB_SIZE)
		return -EINVAL;
	for (i = 0; i < ubi->vtbl_slots; i++) {
		struct ubi_volume *vol = ubi->volumes[i];

		if (!vol) {
			if (i < 3)
				return -EINVAL;
			continue;
		}
		if (i >= ARRAY_SIZE(names) || strcmp(vol->name, names[i]) ||
		    vol->vol_id != i ||
		    vol->vol_type != (i == 2 ? UBI_STATIC_VOLUME : UBI_DYNAMIC_VOLUME) ||
		    vol->usable_leb_size != Q1000K_LEB_SIZE ||
		    (i < 3 && (vol->reserved_pebs != 1 || vol->upd_marker || vol->corrupted)) ||
		    (i == 2 && vol->used_bytes != Q1000K_FACTORY_SIZE))
			return -EINVAL;
	}
	/* Missing/damaged firmware or overlay after an interrupted update is repairable. */
	return 0;
}

int q1000k_ubi_check_layout(void)
{
	int ret = q1000k_attach_readonly();

	if (!ret)
		ret = q1000k_check_volumes();
	ubi_detach();
	if (ret)
		puts("Q1000K: choose Install U-Boot and Prepare UBI first\n");
	return ret;
}

int q1000k_ubi_upload(const char *name, const void *image, size_t size,
		     q1000k_progress_fn progress, void *ctx)
{
	struct ubi_device *ubi;
	struct ubi_volume *vol, *overlay;
	int id, ret;
	u64 capacity;

	if (!name || !image || !size || !progress)
		return -EINVAL;
	if (!strcmp(name, "fit"))
		id = 4;
	else if (!strcmp(name, "recovery"))
		id = 3;
	else
		return -EINVAL;
	ret = ubi_part("ubi", NULL);
	if (ret)
		return ret;
	ret = q1000k_check_volumes();
	if (ret)
		goto out;
	vol = ubi_find_volume(name);
	overlay = ubi_find_volume("rootfs_data");
	ubi = ubi_devices[0];
	capacity = (u64)(ubi->avail_pebs + (vol ? vol->reserved_pebs : 0) +
			 (overlay ? overlay->reserved_pebs : 0)) * ubi->leb_size;
	/* Leave at least one LEB for the writable overlay. */
	if (!size || (DIV_ROUND_UP(size, ubi->leb_size) + 1) * (u64)ubi->leb_size > capacity) {
		ret = -ENOSPC;
		goto out;
	}
	if (overlay) {
		ret = ubi_remove_vol("rootfs_data");
		if (ret)
			goto out;
	}
	ret = vol ? ubi_remove_vol(name) : 0;
	if (!ret)
		ret = ubi_create_vol(name, size, true, id, false);
	if (!ret)
		ret = q1000k_ubi_write_verify(name, image, size, progress, ctx);
	if (!ret)
		ret = ubi_create_vol("rootfs_data", -1, true, 5, false);
out:
	ubi_detach();
	return ret;
}

int q1000k_installed_boot(void)
{
	struct mtd_info *raw, *mtd;
	u8 *record;
	size_t pos, len;
	int ret;

	ret = q1000k_get_mtd(&raw, &mtd);
	if (ret)
		return ret;
	record = malloc_cache_aligned(Q1000K_ENV_SIZE);
	ret = record ? q1000k_read(raw, Q1000K_ENV_OFFSET, record,
				  Q1000K_ENV_SIZE) : -ENOMEM;
	put_mtd_device(mtd);
	put_mtd_device(raw);
	if (!ret)
		ret = q1000k_env_bootcmd(record, &pos, &len);
	if (!ret && strcmp((char *)record + pos, "bootcmd=" Q1000K_VENDOR_BOOTCMD))
		ret = -ENOENT;
	free(record);
	return ret;
}

/* Boot only after explicit installation has selected our vendor boot command. */
static int do_q1000k_boot(struct cmd_tbl *cmdtp, int flag, int argc,
			 char *const argv[])
{
	const char *names[] = { "fit", "recovery" };
	const ulong addr = 0x84000000, max_size = 0x10000000;
	int i, ret;

	airoha_snand_set_write_enabled(false);
	if (q1000k_installed_boot() || q1000k_attach_readonly())
		return CMD_RET_FAILURE;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct ubi_volume *vol = ubi_find_volume(names[i]);
		size_t size;

		if (!vol || vol->upd_marker)
			continue;
		size = min_t(u64, vol->used_bytes, max_size);
		if (!size || addr < gd->ram_base ||
		    addr - gd->ram_base >= gd->ram_size ||
		    size > gd->ram_size - (addr - gd->ram_base) ||
		    gd->start_addr_sp <= addr + 0x100000 ||
		    size > gd->start_addr_sp - addr - 0x100000)
			continue;
		ret = ubi_volume_read(names[i], (void *)addr, 0, size);
		if (ret || recovery_validate_q1000k_fit((void *)addr, size))
			continue;
		printf("Q1000K: booting UBI volume %s with NAND writes locked\n", names[i]);
		ret = run_commandf("bootm 0x%lx", addr);
		if (!ret)
			return CMD_RET_SUCCESS;
	}
	ubi_detach();
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(q1000k_boot, 1, 0, do_q1000k_boot,
	   "boot installed Q1000K firmware using read-only UBI", "");
