// SPDX-License-Identifier: GPL-2.0+
/* A delayed indirect MDIO engine exposes stale reads during PHY patching. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct airoha_eth { int unused; };

/* INSERT CONSTANTS */

static u16 sds[64][32], read_data, write_data, command;
static int polls, early_reads, writes;
static int command_error, poll_error, data_error;
static bool pending, stuck;
static const char *board = "quantum,q1000k";

static bool of_machine_is_compatible(const char *compat)
{
	return !strcmp(board, compat);
}

static int airoha_mdio_c45_read(struct airoha_eth *eth, int phy,
			      int mmd, u16 reg)
{
	assert(phy == 8 && mmd == 30);
	if (reg == RTL8261_PHY_SDS_CMD) {
		if (poll_error)
			return poll_error;
		if (pending && !stuck && ++polls >= 2) {
			if (command & BIT(11))
				sds[command & 0x3f][(command >> 6) & 0x1f] = write_data;
			else
				read_data = sds[command & 0x3f][(command >> 6) & 0x1f];
			pending = false;
			command &= ~RTL8261_PHY_SDS_BUSY;
		}
		return command;
	}
	if (reg == RTL8261_PHY_SDS_RD_WR) {
		if (pending)
			early_reads++;
		return data_error ? data_error : read_data;
	}
	return 0;
}

static int airoha_mdio_c45_write(struct airoha_eth *eth, int phy,
			       int mmd, u16 reg, u16 val)
{
	assert(phy == 8 && mmd == 30);
	if (reg == RTL8261_PHY_SDS_DATA)
		write_data = val;
	if (reg == RTL8261_PHY_SDS_CMD) {
		if (command_error)
			return command_error;
		assert(!pending);
		command = val;
		pending = true;
		polls = 0;
		if (val & BIT(11))
			writes++;
	}
	return 0;
}

static void udelay(int us) {}
static void mdelay(int ms) {}

/* INSERT PRODUCTION CODE */

static const rtk_hwpatch_t patches[] = {
/* INSERT PATCH ENTRIES */
};

int main(void)
{
	struct airoha_eth eth = { 0 };
	u16 value = 0;
	const char *preserve_boards[] = {
		"quantum,q1000k", "centurylink,q1000k", "lumen,q1000k",
		"axon,xg2010g", "econet,xg2010g",
	};

	read_data = 0x1401; /* The result of the previous indirect read. */
	sds[7][16] = 0x8003;
	assert(airoha_rtl8261_sds_get(&eth, 8, 7, 16, &value) == 0);
	assert(value == 0x8003 && early_reads == 0);
	assert(airoha_rtl8261_sds_diag_get(&eth, 8, 7, 16) == 0x8003);

	/* Consecutive partial writes must preserve the latest register bits. */
	sds[7][16] = 0x0041;
	read_data = 0x1401;
	for (int i = 0; i < ARRAY_SIZE(patches); i++)
		assert(airoha_rtl8261_apply_patch(&eth, 8, &patches[i]) == 0);
	assert(sds[7][16] == 0x8003 && early_reads == 0);

	/* Read failures must not be converted into successful patch writes. */
	writes = 0;
	command_error = -EIO;
	assert(airoha_rtl8261_apply_patch(&eth, 8, &patches[0]) == -EIO);
	command_error = 0;
	poll_error = -EIO;
	assert(airoha_rtl8261_sds_get(&eth, 8, 7, 16, &value) == -EIO);
	poll_error = 0;
	pending = false;
	data_error = -EIO;
	assert(airoha_rtl8261_apply_patch(&eth, 8, &patches[0]) == -EIO);
	data_error = 0;
	stuck = true;
	assert(airoha_rtl8261_apply_patch(&eth, 8, &patches[0]) == -ETIMEDOUT);
	assert(writes == 0 && early_reads == 0);
	stuck = pending = false;

	/* Q1000K must not receive the XR1710G-specific SDS 6:3 override. */
	sds[6][3] = 0x1234;
	for (int i = 0; i < ARRAY_SIZE(preserve_boards); i++) {
		board = preserve_boards[i];
		assert(airoha_rtl8261_apply_sds_mode(&eth, 8) == 0);
		assert(writes == 0 && sds[6][3] == 0x1234);
	}
	board = "econet,xr1710g";
	assert(airoha_rtl8261_apply_sds_mode(&eth, 8) == 0);
	assert(writes == 1 && sds[6][3] == RTL8261_PHY_SDS_XR1710G_MODE);

	puts("PHY SerDes: delayed reads, patch fields, errors and board policy passed");
	return 0;
}
