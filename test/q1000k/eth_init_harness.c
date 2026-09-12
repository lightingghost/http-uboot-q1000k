// SPDX-License-Identifier: GPL-2.0+
/* Exercise the real hardware-init sequence without accessing hardware. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define FIELD_PREP(mask, val) (((val) << __builtin_ctz(mask)) & (mask))

struct udevice { int unused; };
struct airoha_qdma { int unused; };
struct airoha_eth {
	bool has_switch_rst, gdm4_usb_hsgmii, gdm4_dual_hsgmii;
	int switch_rst, rsts, xsi_rsts;
	void *scu_regmap, *pcie1_pcs_xfi_mac;
	struct airoha_qdma qdma[2];
};

static int gdm3_calls, gdm4_calls, fe_calls, qdma_calls;
static int gdm3_error, gdm4_error, qdma_error, reset_error;

static int reset_assert(int *rst) { return reset_error; }
static int reset_assert_bulk(int *rst) { return reset_error; }
static int reset_deassert(int *rst) { return reset_error; }
static int reset_deassert_bulk(int *rst) { return reset_error; }
static void regmap_update_bits(void *map, int reg, int mask, int val) {}
static void mdelay(int ms) {}

static int airoha_eth_gdm4_pcs_init(struct airoha_eth *eth)
{
	gdm4_calls++;
	return gdm4_error;
}

static int airoha_eth_gdm3_pcs_init(struct airoha_eth *eth)
{
	gdm3_calls++;
	/* The real PCS routine rejects an absent PCIe1 register mapping. */
	return eth->pcie1_pcs_xfi_mac ? gdm3_error : -ENODEV;
}

static int airoha_fe_init(struct airoha_eth *eth)
{
	fe_calls++;
	return 0;
}

static void airoha_eth_gdm4_set_frag_size(struct airoha_eth *eth, int speed) {}
static void airoha_eth_gdm4_set_usb_frag_size(struct airoha_eth *eth) {}

static int airoha_qdma_init(struct udevice *dev, struct airoha_eth *eth,
			   struct airoha_qdma *qdma)
{
	qdma_calls++;
	return qdma_error;
}

/* INSERT PRODUCTION CODE */

static void reset_test(void)
{
	gdm3_calls = gdm4_calls = fe_calls = qdma_calls = 0;
	gdm3_error = gdm4_error = qdma_error = reset_error = 0;
}

int main(void)
{
	struct udevice dev = { 0 };
	struct airoha_eth eth = { .has_switch_rst = true };

	/* Q1000K: GDM4 exists, GDM3 is absent. Reach FE and both QDMAs. */
	assert(airoha_hw_init(&dev, &eth) == 0);
	assert(gdm4_calls == 1 && gdm3_calls == 0);
	assert(fe_calls == 1 && qdma_calls == 2);

	/* XG2010G: populated GDM3 must still be initialized. */
	reset_test();
	eth.pcie1_pcs_xfi_mac = &eth;
	assert(airoha_hw_init(&dev, &eth) == 0);
	assert(gdm3_calls == 1 && gdm4_calls == 1 && qdma_calls == 2);

	/* A populated GDM3 that fails must still abort initialization. */
	reset_test();
	gdm3_error = -ETIMEDOUT;
	assert(airoha_hw_init(&dev, &eth) == -ETIMEDOUT);
	assert(gdm3_calls == 1 && fe_calls == 0 && qdma_calls == 0);

	/* Keep Q1000K GDM4, DMA and reset failures visible to the caller. */
	reset_test();
	eth.pcie1_pcs_xfi_mac = NULL;
	gdm4_error = -EIO;
	assert(airoha_hw_init(&dev, &eth) == -EIO);
	assert(gdm3_calls == 0 && fe_calls == 0 && qdma_calls == 0);

	reset_test();
	qdma_error = -ENOMEM;
	assert(airoha_hw_init(&dev, &eth) == -ENOMEM);
	assert(fe_calls == 1 && qdma_calls == 1);

	reset_test();
	reset_error = -EIO;
	assert(airoha_hw_init(&dev, &eth) == -EIO);
	assert(gdm4_calls == 0 && gdm3_calls == 0 && qdma_calls == 0);

	puts("Ethernet init: optional GDM3 and error propagation passed");
	return 0;
}
