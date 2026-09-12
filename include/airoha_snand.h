/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __AIROHA_SNAND_H
#define __AIROHA_SNAND_H

#include <stdbool.h>

#ifdef CONFIG_AIROHA_SNFI_NAND_WRITE_GUARD
/* Only the HTTP firmware commit path may enable writes. Defaults to false. */
void airoha_snand_set_write_enabled(bool enabled);
#else
static inline void airoha_snand_set_write_enabled(bool enabled)
{
}
#endif

#endif
