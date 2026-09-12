/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __Q1000K_INSTALLER_H
#define __Q1000K_INSTALLER_H

#include <linux/types.h>
#include <linux/errno.h>

#define Q1000K_NAND_SIZE 0x20000000UL
#define Q1000K_ERASE_SIZE 0x20000UL
#define Q1000K_PAGE_SIZE 0x800UL
#define Q1000K_ENV_OFFSET 0x200000UL
#define Q1000K_ENV_SIZE 0x4000UL
#define Q1000K_CHAIN_OFFSET 0x600000UL
#define Q1000K_CHAIN_SIZE 0x100000UL
#define Q1000K_UBI_OFFSET 0x700000UL
#define Q1000K_UBI_SIZE 0x1b600000UL
#define Q1000K_FACTORY_SIZE 0x10000UL
/* Q1000K additions; retain the W1700K MAC/serial and empty Wi-Fi/fan slots. */
#define Q1000K_FACTORY_FSAN_OFFSET 0x9000UL
#define Q1000K_FSAN_SIZE 12UL
#define Q1000K_FACTORY_GPON_OFFSET 0xa000UL
#define Q1000K_FACTORY_XGSPON_OFFSET 0xb000UL
#define Q1000K_PON_CAL_SIZE 0x201UL
#define Q1000K_DSD_OFFSET 0x400000UL
#define Q1000K_DSD_GPON_OFFSET 0x11000UL
#define Q1000K_DSD_XGSPON_OFFSET 0x12000UL
#define Q1000K_LEB_SIZE 126976UL
#define Q1000K_VENDOR_BOOTCMD \
	"flash read 0x600000 0x100000 0x89000000; bootm 0x89000000"

struct q1000k_install_plan;
typedef void (*q1000k_progress_fn)(void *ctx, bool erase, u32 done, u32 total);
int recovery_validate_q1000k_fit(const void *fit, size_t size);

#if IS_ENABLED(CONFIG_Q1000K_INSTALLER)
int q1000k_install_prepare(struct q1000k_install_plan **plan, bool prepare_ubi);
void q1000k_install_release(struct q1000k_install_plan *plan);
int q1000k_install_commit(struct q1000k_install_plan *plan, const void *image,
			 size_t size, q1000k_progress_fn progress, void *ctx);
int q1000k_ubi_check_layout(void);
int q1000k_ubi_upload(const char *name, const void *image, size_t size,
		     q1000k_progress_fn progress, void *ctx);
int q1000k_installed_boot(void);
#else
static inline int q1000k_install_prepare(struct q1000k_install_plan **plan, bool prepare_ubi)
{ return -EOPNOTSUPP; }
static inline void q1000k_install_release(struct q1000k_install_plan *plan) {}
static inline int q1000k_install_commit(struct q1000k_install_plan *plan,
	const void *image, size_t size, q1000k_progress_fn progress, void *ctx)
{ return -EOPNOTSUPP; }
static inline int q1000k_ubi_check_layout(void) { return -EOPNOTSUPP; }
static inline int q1000k_ubi_upload(const char *name, const void *image,
	size_t size, q1000k_progress_fn progress, void *ctx)
{ return -EOPNOTSUPP; }
static inline int q1000k_installed_boot(void) { return -EOPNOTSUPP; }
#endif
#endif
