// SPDX-License-Identifier: GPL-2.0+
/* Execute the real recovery driver with register/DMA substitutes. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned char uchar;
typedef uintptr_t dma_addr_t;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define FIELD_PREP(mask, val) (((u32)(val) << __builtin_ctz(mask)) & (mask))
#define FIELD_GET(mask, val) (((val) & (mask)) >> __builtin_ctz(mask))
#define le32_to_cpu(x) (x)
#define virt_to_phys(x) ((uintptr_t)(x))
#define DMA_FROM_DEVICE 0
#define AIROHA_RX_BUF_SIZE 2048
#define ARP_HLEN 6

/* INSERT CONSTANTS */

struct airoha_qdma_desc { u32 ctrl, addr, msg1; };
struct airoha_queue {
	struct airoha_qdma_desc *desc;
	uchar *rx_buf;
	u16 head;
	int ndesc;
};
struct airoha_qdma { struct airoha_queue q_rx[1]; u32 dma_idx, cpu_idx; };
struct airoha_eth {
	void *switch_regs;
	u32 recovery_switch_port_mask;
	struct airoha_qdma qdma[2];
	u8 last_rx_fport, last_rx_qdma, last_rx_sport, last_rx_crsn;
	bool last_rx_valid;
	u16 last_rx_index, last_rx_ppe_entry, last_rx_len;
	u32 last_rx_ctrl, last_rx_msg1;
	u8 last_rx_head[32], last_rx_head_len;
};

static u32 sw[0x8000 / 4];
static unsigned reset_count[8];
static void airoha_switch_wr(struct airoha_eth *eth, u32 reg, u32 val)
{
	assert(eth->switch_regs);
	assert(reg < sizeof(sw) && !(reg & 3));
	sw[reg / 4] = val;
}
static void airoha_switch_rmw(struct airoha_eth *eth, u32 reg, u32 mask, u32 val)
{
	airoha_switch_wr(eth, reg, (sw[reg / 4] & ~mask) | val);
}
static void airoha_switch_fdb_flush(struct airoha_eth *eth) {}
static void mdelay(int ms) {}
static u32 airoha_qdma_rr(struct airoha_qdma *qdma, u32 reg)
{
	assert(reg == REG_RX_DMA_IDX(0));
	return qdma->dma_idx;
}
static void airoha_qdma_rmw(struct airoha_qdma *qdma, u32 reg, u32 mask, u32 val)
{
	assert(reg == REG_RX_CPU_IDX(0));
	qdma->cpu_idx = (qdma->cpu_idx & ~mask) | val;
}
static void airoha_qdma_reset_rx_desc(struct airoha_queue *q, int index)
{
	q->desc[index].ctrl = 0;
	reset_count[index]++;
}
static void dma_unmap_unaligned(uintptr_t addr, unsigned size, int dir) {}
static void dma_unmap_single(dma_addr_t addr, unsigned size, int dir) {}
static u8 airoha_rx_sport_to_recovery_fport(struct airoha_eth *eth, u8 sport) { return 1; }
static void airoha_pick_tx_fport(struct airoha_eth *eth) {}
static void airoha_recovery_note_lan_activity(void) {}
static void airoha_gdm4_update_cpu_path(struct airoha_eth *eth) {}
static void airoha_recovery_copy_head(u8 *dst, u8 *len, const u8 *src, unsigned n) {}
static void airoha_peer_fport_learn(struct airoha_eth *eth, const u8 *addr, u8 port) {}
static bool airoha_recovery_accept_gdm4_rx(struct airoha_eth *eth) { return false; }
static bool airoha_recovery_accept_gdm3_rx(struct airoha_eth *eth) { return false; }
static bool airoha_sport_is_gdm4(struct airoha_eth *eth, u8 sport) { return false; }
static bool airoha_sport_is_gdm3(struct airoha_eth *eth, u8 sport) { return false; }
static bool airoha_recovery_port_is_gdm4(struct airoha_eth *eth) { return false; }
static bool airoha_recovery_port_is_gdm4_usb(struct airoha_eth *eth) { return false; }
static bool airoha_recovery_port_is_gdm3(struct airoha_eth *eth) { return false; }

/* INSERT PRODUCTION CODE */

/* Flooding is limited by both the global flood mask and ingress port matrix. */
static u32 flood_destinations(unsigned ingress, u32 field)
{
	u32 flood = (sw[SWITCH_MFC / 4] & field) >> __builtin_ctz(field);
	u32 matrix = FIELD_GET(SWITCH_PORT_MATRIX, sw[SWITCH_PCR(ingress) / 4]);

	return flood & matrix & ~BIT(ingress);
}

static void test_switch(void)
{
	struct airoha_eth eth = { .switch_regs = sw };
	u32 masks[] = { BIT(1) | BIT(2), BIT(4), BIT(0) | BIT(1) | BIT(4) };
	unsigned i, port, restart;

	for (i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
		eth.recovery_switch_port_mask = masks[i];
		for (restart = 0; restart < 2; restart++) {
			airoha_switch_recovery_runtime_init(&eth);
			/* OFFER/ACK broadcasts from CPU must reach every recovery LAN. */
			assert(flood_destinations(6, SWITCH_BC_FFP) == masks[i]);
			/* Client broadcasts reach CPU, without bridging LAN ports. */
			for (port = 0; port < 6; port++)
				assert(flood_destinations(port, SWITCH_BC_FFP) ==
				       ((masks[i] & BIT(port)) ? BIT(6) : 0));
			assert(FIELD_GET(SWITCH_CPU_PMAP, sw[SWITCH_CFC / 4]) == BIT(6));
			airoha_switch_recovery_quiesce(&eth);
			for (port = 0; port <= 6; port++)
				assert(flood_destinations(port, SWITCH_BC_FFP) == 0);
		}
	}
	puts("Switch: DHCP broadcasts reach LAN; port isolation/re-entry preserved");
}

static void test_rx(void)
{
	struct airoha_qdma_desc desc[8] = { 0 };
	u8 buffers[8 * AIROHA_RX_BUF_SIZE] = { 0 }, *packet = NULL;
	struct airoha_eth eth = { 0 };
	struct airoha_qdma *qdma = &eth.qdma[0];
	struct airoha_queue *q = &qdma->q_rx[0];
	unsigned i;

	q->desc = desc;
	q->rx_buf = buffers;
	q->ndesc = 8;
	/* Retain the hardware startup-index workaround. */
	qdma->dma_idx = 2;
	airoha_qdma_sync_rx_head(qdma, q, 0);
	assert(q->head == 2);
	desc[2].ctrl = QDMA_DESC_DONE_MASK | 342;
	assert(airoha_eth_recv_qdma(&eth, qdma, &packet) == 342);
	assert(packet == buffers + 2 * AIROHA_RX_BUF_SIZE);
	airoha_qdma_recycle_rx_desc(qdma, q, 0);
	assert(q->head == 3 && reset_count[2] == 0);
	/* Slot 2 stays DONE until slot 3 arrives. It must never be replayed. */
	for (i = 0; i < 100; i++) {
		assert(airoha_eth_recv_qdma(&eth, qdma, &packet) == -EAGAIN);
		assert(q->head == 3);
	}
	desc[3].ctrl = QDMA_DESC_DONE_MASK | 343;
	assert(airoha_eth_recv_qdma(&eth, qdma, &packet) == 343);
	airoha_qdma_recycle_rx_desc(qdma, q, 0);
	assert(reset_count[2] == 1 && reset_count[3] == 1 && q->head == 4);
	/* Do not jump past the current head, even if a later slot is DONE. */
	desc[5].ctrl = QDMA_DESC_DONE_MASK | 345;
	assert(airoha_eth_recv_qdma(&eth, qdma, &packet) == -EAGAIN);
	assert(q->head == 4);
	desc[4].ctrl = QDMA_DESC_DONE_MASK | 344;
	assert(airoha_eth_recv_qdma(&eth, qdma, &packet) == 344);
	airoha_qdma_recycle_rx_desc(qdma, q, 0);
	assert(airoha_eth_recv_qdma(&eth, qdma, &packet) == 345);
	airoha_qdma_recycle_rx_desc(qdma, q, 0);
	/* Several complete ring wraps, including a discarded even descriptor. */
	for (i = 0; i < 32; i++) {
		unsigned index = q->head;
		bool drop = i == 2;

		desc[index].ctrl = QDMA_DESC_DONE_MASK | (350 + i) |
			(drop ? QDMA_DESC_DROP_MASK : 0);
		assert(airoha_eth_recv_qdma(&eth, qdma, &packet) ==
		       (drop ? -EAGAIN : (int)(350 + i)));
		if (!drop) {
			assert(packet == buffers + index * AIROHA_RX_BUF_SIZE);
			airoha_qdma_recycle_rx_desc(qdma, q, 0);
		}
		assert(q->head == (index + 1) % 8);
		assert(airoha_eth_recv_qdma(&eth, qdma, &packet) == -EAGAIN);
	}
	puts("RX: no replay, ordered delivery, paired recycling and ring wrap passed");
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "switch"))
		test_switch();
	else if (!strcmp(argv[1], "rx"))
		test_rx();
	else
		return 1;
	return 0;
}
