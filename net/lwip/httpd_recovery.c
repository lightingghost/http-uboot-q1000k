// SPDX-License-Identifier: GPL-2.0+
/*
 * Minimal HTTP upload recovery server using lwIP httpd
 * Serves an upload page at / for firmware, recovery, chainloader and RAM boot.
 */

#include <dm.h>
#include <airoha_snand.h>
#include <q1000k_installer.h>
#include <dm/ofnode.h>
#include <env.h>
#include <image.h>
#include <log.h>
#include <malloc.h>
#include <memalign.h>
#include <command.h>
#include <mtd.h>
#include <net-lwip.h>
#include <net.h>
#include <initcall.h>
#include <ubi_uboot.h>
#include <watchdog.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <console.h>
#include <asm/gpio.h>
#include <asm/global_data.h>
#include <dt-bindings/gpio/gpio.h>

#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/ip.h>
#include <lwip/timeouts.h>
#include <lwip/udp.h>
#include <lwip/tcp.h>
#include <lwip/etharp.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/apps/httpd.h>
#include <lwip/apps/fs.h>
#include <lwip/prot/dhcp.h>
#include <lwip/prot/iana.h>
#include <version.h>
#include <timestamp.h>
#include <limits.h>
#include <miiphy.h>
#include <linux/mii.h>
#include <mtd/ubi-user.h>
#include <timer.h>
#include <asm/io.h>
#include "../../drivers/mtd/ubi/ubi.h"

DECLARE_GLOBAL_DATA_PTR;

int xr1710g_sync_factory(void);
int xr1710g_sync_factory_part(const char *part);
const char *xr1710g_detect_ubi_version(void);
ulong airoha_recovery_get_lan_activity_ms(void);

/*
 * Upload buffer
 * Use env 'recovery_addr' if set, otherwise fall back to U-Boot 'loadaddr'.
 * Avoid hard-coding a RAM address which may overlap U-Boot/lwIP memory.
 */
/* Default maximum upload size in bytes (override with env 'recovery_max') */
#define RECOVERY_UPLOAD_MAX    (32 * 1024 * 1024UL)
#define RECOVERY_MIN_FIRMWARE_SIZE (1 * 1024 * 1024UL)
#define RECOVERY_MAX_UBOOT_SIZE    (1 * 1024 * 1024UL)

/* Delay before reboot after flashing completes, to let browser finish reads */
#define REBOOT_DELAY_MS        3000
#define RECOVERY_STATIC_IPADDR           "192.168.255.1"
#define RECOVERY_STATIC_NETMASK          "255.255.255.0"
#define RECOVERY_STATIC_GATEWAY          "0.0.0.0"
#define RECOVERY_DHCP_CLIENT_IPADDR      "192.168.255.2"
#define RECOVERY_DHCP_BROADCAST_IPADDR   "192.168.255.255"
#define RECOVERY_DHCP_LEASE_SECS         86400U
#define RECOVERY_DHCP_MAX_MSG_LEN        1500
#define RECOVERY_LED_PORTS     2
#define RECOVERY_LED_POLL_MS   100
#define RECOVERY_LED_PHY_POLL_MS 500
#define RECOVERY_LED_MDIO_BACKOFF_MS 5000
#define RECOVERY_LED_ACTIVITY_MS 350
#define RECOVERY_LED_BLINK_MS  100
#define RECOVERY_STATUS_LED_MAX           8
#define RECOVERY_STATUS_SW_PWM_PERIOD_MS  20
#define RECOVERY_STATUS_HW_PWM_UPDATE_MS  40
#define RECOVERY_STATUS_HW_PWM_PERIOD_TICKS 1
#define RECOVERY_STATUS_BREATHE_PERIOD_MS 1800
#define RECOVERY_STATUS_BREATHE_HALF_MS   (RECOVERY_STATUS_BREATHE_PERIOD_MS / 2)
#define RECOVERY_STATUS_SWEEP_STEP_MS     700
#define RECOVERY_STATUS_OVERLAP_FP        (2 * 256)
#define RECOVERY_STATUS_BRIGHTNESS_FP     256
#define RECOVERY_STATUS_BREATHE_MIN_FP    64

#define RECOVERY_GPIO_SYSCTL_BASE      0x1fbf0200
#define RECOVERY_CHIP_SCU_BASE         0x1fa20000
#define RECOVERY_REG_GPIO_DATA         0x0004
#define RECOVERY_REG_GPIO_OE           0x0014
#define RECOVERY_REG_GPIO_CTRL         0x0000
#define RECOVERY_REG_GPIO_CTRL1        0x0020
#define RECOVERY_REG_GPIO_FLASH_MODE_CFG 0x0034
#define RECOVERY_REG_GPIO_CTRL2        0x0060
#define RECOVERY_REG_GPIO_CTRL3        0x0064
#define RECOVERY_REG_GPIO_DATA1        0x0070
#define RECOVERY_REG_GPIO_OE1          0x0078
#define RECOVERY_REG_GPIO_2ND_I2C_MODE 0x0214
#define RECOVERY_REG_GPIO_FLASH_MODE_CFG_EXT 0x0068
#define RECOVERY_REG_GPIO_FLASH_PRD_SET0 0x003c
#define RECOVERY_REG_GPIO_FLASH_MAP0     0x004c
#define RECOVERY_REG_GPIO_FLASH_MAP1     0x0050
#define RECOVERY_REG_CYCLE_CFG_VALUE0    0x0098

#define RECOVERY_GPIO_FLASH_MAP_BUCKET0  0x88888888
#define RECOVERY_GPIO_FLASH_DUTY_FULL    255

#define RECOVERY_GPIO_LAN0_LED0_MODE_MASK BIT(3)
#define RECOVERY_GPIO_LAN0_LED1_MODE_MASK BIT(4)
#define RECOVERY_GPIO_LAN1_LED0_MODE_MASK BIT(5)
#define RECOVERY_GPIO_LAN1_LED1_MODE_MASK BIT(6)
#define RECOVERY_GPIO43_FLASH_MODE_CFG BIT(23)
#define RECOVERY_GPIO44_FLASH_MODE_CFG BIT(24)
#define RECOVERY_UBOOTENV_SIZE (1 * 1024 * 1024UL)
#define RECOVERY_FACTORY_SIZE  (1 * 1024 * 1024UL)
#define RECOVERY_UBI_WRITE_CHUNK (1024 * 1024U)
#define RECOVERY_UBOOT_SLOT_FIT_OFFSET 0x2100U
#define RECOVERY_UBOOT_SLOT_SIZE       (1 * 1024 * 1024UL)
#define RECOVERY_UBOOT_SLOT_DEFAULT_OFS 0x600000UL
#define RECOVERY_UBOOT_SLOT_DEFAULT_DEV "spi-nand0"
#define RECOVERY_Q1000K_NAND_BYTES     0x20000000ULL
#define RECOVERY_Q1000K_UBI_OFFSET     0x00700000ULL
#define RECOVERY_Q1000K_UBI_BYTES      0x1b600000ULL
#define RECOVERY_Q1000K_UPLOAD_OFFSET  0x04000000UL
#define RECOVERY_Q1000K_RAMBOOT_OFFSET 0x09000000UL
#define RECOVERY_Q1000K_RAMBOOT_MIN_OFFSET (0x00200000UL + CONFIG_SYS_BOOTM_LEN)
#define RECOVERY_Q1000K_UPLOAD_MAX     0x10000000UL
#define RECOVERY_Q1000K_STACK_MARGIN   0x00100000UL
#define RECOVERY_NAND_BACKUP_CHUNK      (256 * 1024UL)
#define RECOVERY_NAND_BACKUP_MAGIC      0x514e424bU
#define RECOVERY_XG2010G_RUNNING_FIT_ADDR 0x81800000UL
#define RECOVERY_XG2010G_INSTALL_TOKEN "XG2010G_INSTALL"
#define RECOVERY_IH_MAGIC 0x27051956U
#define RECOVERY_FDT_MAGIC 0xd00dfeedU

static u8 *recv_base;
static u32 recv_off;
static u32 recv_total;
static int post_ok;
static void *post_connection;
static bool post_validated;
static int flash_request;
static bool ramboot_request;
static bool self_write_request;
static volatile int reboot_request;
/* Progress for /status polling */
static volatile u32 prog_total; /* combined total for backward compat */
static volatile u32 prog_done;  /* combined done for backward compat */
static volatile u32 prog_erase_total;
static volatile u32 prog_erase_done;
static volatile u32 prog_write_total;
static volatile u32 prog_write_done;
static volatile int prog_phase; /* 0 idle, 1 erase, 2 write, 3 done, 4 boot, -1 error */
static unsigned long long prog_erase_volume_base;
static unsigned long long prog_erase_volume_bytes;
static struct recovery_status_led_ctrl *prog_status_leds;
static bool recovery_httpd_started;
static int recovery_ubi_attach_error;
static unsigned int recovery_nand_backup_active;

static ulong recovery_q1000k_upload_top(void);

struct recovery_nand_backup_file {
	u32 magic;
	struct mtd_info *mtd;
	u8 *cache;
	size_t cache_capacity;
	size_t cache_len;
	size_t cache_off;
	loff_t next_offset;
	unsigned long long bytes_left;
	char http_header[256];
	size_t http_header_len;
	size_t http_header_off;
};

static void recovery_abort_tcp_list(struct tcp_pcb **list)
{
	while (*list)
		tcp_abort(*list);
}

static void recovery_close_tcp_listeners(void)
{
	while (tcp_listen_pcbs.pcbs)
		tcp_close(tcp_listen_pcbs.pcbs);
}

static void recovery_lwip_cleanup(struct netif *netif)
{
	recovery_close_tcp_listeners();
	recovery_abort_tcp_list(&tcp_active_pcbs);
	recovery_abort_tcp_list(&tcp_tw_pcbs);
	recovery_abort_tcp_list(&tcp_bound_pcbs);
	if (netif)
		etharp_cleanup_netif(netif);
	recovery_httpd_started = false;
}

static void reboot_delay_cb(void *arg)
{
    (void)arg;
    reboot_request = 1;
}

static void recovery_cancel_timeouts(void)
{
	sys_untimeout(reboot_delay_cb, NULL);
	flash_request = 0;
	ramboot_request = false;
	reboot_request = 0;
}

static void recovery_prepare_static_network(void)
{
	env_set("ipaddr", RECOVERY_STATIC_IPADDR);
	env_set("netmask", RECOVERY_STATIC_NETMASK);
	env_set("gatewayip", RECOVERY_STATIC_GATEWAY);
}

static u32 recovery_be32_to_cpu(const void *p)
{
	const u8 *b = p;

	return ((u32)b[0] << 24) | ((u32)b[1] << 16) |
	       ((u32)b[2] << 8) | b[3];
}

static bool recovery_board_is_xg2010g(void)
{
	return of_machine_is_compatible("axon,xg2010g") ||
	       of_machine_is_compatible("econet,xg2010g");
}

static bool recovery_board_is_q1000k(void)
{
	return of_machine_is_compatible("quantum,q1000k") ||
	       of_machine_is_compatible("centurylink,q1000k") ||
	       of_machine_is_compatible("lumen,q1000k");
}

static bool recovery_uboot_update_disabled(void)
{
	if (recovery_board_is_q1000k())
		return !IS_ENABLED(CONFIG_Q1000K_INSTALLER);
	return IS_ENABLED(CONFIG_AIROHA_SNFI_NAND_WRITE_GUARD) ||
	       ofnode_read_bool(ofnode_root(),
				"recovery-disable-uboot-update");
}

static bool recovery_fit_has_hashed_image(const void *fit, const char *name)
{
	int child;
	int images;
	int image;

	images = fdt_path_offset(fit, FIT_IMAGES_PATH);
	if (images < 0)
		return false;
	image = fdt_subnode_offset(fit, images, name);
	if (image < 0)
		return false;

	fdt_for_each_subnode(child, fit, image) {
		const char *child_name = fit_get_name(fit, child, NULL);

		if (!strncmp(child_name, FIT_HASH_NODENAME,
			     strlen(FIT_HASH_NODENAME)))
			return true;
	}

	return false;
}

static int recovery_validate_xg2010g_chainloader_fit(const void *fit)
{
	const char *desc = fdt_getprop(fit, 0, FIT_DESC_PROP, NULL);

	if (!desc || !strstr(desc, "XG2010G") || !strstr(desc, "chainloader") ||
	    !recovery_fit_has_hashed_image(fit, "fdt@1") ||
	    !recovery_fit_has_hashed_image(fit, "kernel@1") ||
	    !recovery_fit_has_hashed_image(fit, "uboot@1")) {
		printf("U-Boot FIT is not an XG2010G chainloader image\n");
		return -ENOEXEC;
	}

	return 0;
}

static int recovery_validate_fit(const void *fit, size_t size)
{
	int ret;

	ret = fit_check_format(fit, size);
	if (ret)
		return ret;
	if (recovery_board_is_xg2010g()) {
		ret = recovery_validate_xg2010g_chainloader_fit(fit);
		if (ret)
			return ret;
	}
	if (!fit_all_image_verify(fit)) {
		printf("XG2010G U-Boot FIT image hash verification failed\n");
		return -EBADMSG;
	}
	return 0;
}

enum upload_target {
	TARGET_FIRMWARE = 0,
	TARGET_UBOOT,
	TARGET_RECOVERY,
	TARGET_INITRAMFS,
};
static enum upload_target current_target = TARGET_FIRMWARE;
/* Bound to the accepted POST until its response is acknowledged and committed. */
static bool q1000k_uboot_only;

struct recovery_ubi_layout {
	const char *version;
	const char *part;
};

static const struct recovery_ubi_layout recovery_ubi_layouts[] = {
	{ "2.0", "ubi" },
	{ "1.5", "ubi1.5" },
	{ "1.0", "ubi1.0" },
};

static const struct recovery_ubi_layout *current_ubi_layout =
	&recovery_ubi_layouts[0];

enum recovery_backend {
	RECOVERY_BACKEND_MTD = 0,
	RECOVERY_BACKEND_UBI,
};

struct recovery_target {
	enum recovery_backend backend;
	const char *name;
	const char *ubi_part;
	struct mtd_info *mtd;
	loff_t ofs;
	unsigned long long cur_size;
	unsigned long long limit;
	bool ubi_needs_format;
};

struct recovery_gpio_pin {
	struct gpio_desc desc;
	ofnode node;
	u8 gpio;
	s8 last_on;
	bool active_low;
	bool valid;
};

struct recovery_led_ctrl {
	struct udevice *mdio_dev;
	struct recovery_gpio_pin green[RECOVERY_LED_PORTS];
	struct recovery_gpio_pin yellow[RECOVERY_LED_PORTS];
	u8 phy_addr[RECOVERY_LED_PORTS];
	ulong last_poll;
	ulong last_phy_poll;
	ulong last_mdio_error;
	bool mdio_fault;
	int port_count;
	int speed[RECOVERY_LED_PORTS];
};

struct recovery_status_led_ctrl {
	struct recovery_gpio_pin leds[RECOVERY_STATUS_LED_MAX];
	int led_count;
	ulong start_ms;
	ulong last_pwm_update;
	u32 pwm_mux_mask;
	u32 pwm_mux_mask_ext;
	u32 saved_pwm_duty;
	u32 saved_pwm_map[2];
	u32 saved_pwm_cycle;
	bool pwm_active_low;
	bool hw_pwm;
};

struct recovery_dhcp_server {
	struct udp_pcb *pcb;
	struct netif *netif;
	ip4_addr_t server_ip;
	ip4_addr_t client_ip;
	ip4_addr_t netmask;
	ip4_addr_t router;
	ip4_addr_t broadcast;
	ip4_addr_t dns;
};

static void recovery_led_ctrl_free(struct recovery_led_ctrl *ctrl)
{
	memset(ctrl, 0, sizeof(*ctrl));
}

static bool recovery_gpio_flash_mode_bit(u8 gpio, uintptr_t *reg, u32 *mask)
{
	if (gpio <= 15) {
		*reg = RECOVERY_GPIO_SYSCTL_BASE + RECOVERY_REG_GPIO_FLASH_MODE_CFG;
		*mask = BIT(gpio);
		return true;
	}

	if (gpio >= 16 && gpio <= 31) {
		*reg = RECOVERY_GPIO_SYSCTL_BASE + RECOVERY_REG_GPIO_FLASH_MODE_CFG_EXT;
		*mask = BIT(gpio - 16);
		return true;
	}

	if (gpio >= 36 && gpio <= 51) {
		*reg = RECOVERY_GPIO_SYSCTL_BASE + RECOVERY_REG_GPIO_FLASH_MODE_CFG_EXT;
		*mask = BIT(gpio - 20);
		return true;
	}

	return false;
}

static void recovery_clrsetbits_le32(uintptr_t addr, u32 clear, u32 set)
{
	u32 val = readl((void __iomem *)addr);

	val &= ~clear;
	val |= set;
	writel(val, (void __iomem *)addr);
}

static uintptr_t recovery_gpio_data_reg(u8 gpio)
{
	return RECOVERY_GPIO_SYSCTL_BASE +
	       (gpio < 32 ? RECOVERY_REG_GPIO_DATA : RECOVERY_REG_GPIO_DATA1);
}

static uintptr_t recovery_gpio_oe_reg(u8 gpio)
{
	return RECOVERY_GPIO_SYSCTL_BASE +
	       (gpio < 32 ? RECOVERY_REG_GPIO_OE : RECOVERY_REG_GPIO_OE1);
}

static uintptr_t recovery_gpio_dir_reg(u8 gpio)
{
	static const u16 dir_regs[] = {
		RECOVERY_REG_GPIO_CTRL,
		RECOVERY_REG_GPIO_CTRL1,
		RECOVERY_REG_GPIO_CTRL2,
		RECOVERY_REG_GPIO_CTRL3,
	};

	return RECOVERY_GPIO_SYSCTL_BASE + dir_regs[gpio / 16];
}

static void recovery_gpio_direction_output(u8 gpio)
{
	u32 bank_bit = BIT(gpio % 32);
	u32 dir_bit = BIT(2 * (gpio % 16));

	recovery_clrsetbits_le32(recovery_gpio_oe_reg(gpio), 0, bank_bit);
	recovery_clrsetbits_le32(recovery_gpio_dir_reg(gpio), 0, dir_bit);
}

static void recovery_gpio_prepare_output(u8 gpio)
{
	uintptr_t reg;
	u32 mask;

	if (recovery_gpio_flash_mode_bit(gpio, &reg, &mask))
		recovery_clrsetbits_le32(reg, mask, 0);

	recovery_gpio_direction_output(gpio);
}

static void recovery_gpio_set_value(u8 gpio, bool active_low, int active)
{
	u32 bit = BIT(gpio % 32);
	uintptr_t reg = recovery_gpio_data_reg(gpio);
	u32 set = active_low ? (active ? 0 : bit) : (active ? bit : 0);

	recovery_clrsetbits_le32(reg, bit, set);
}

static void recovery_led_set_pin(struct recovery_gpio_pin *pin, int on)
{
	if (pin->valid)
		recovery_gpio_set_value(pin->gpio, pin->active_low, on);
}

static void recovery_led_stop(struct recovery_led_ctrl *ctrl)
{
	int i;

	if (!ctrl)
		return;

	for (i = 0; i < ctrl->port_count; i++) {
		recovery_led_set_pin(&ctrl->green[i], 0);
		recovery_led_set_pin(&ctrl->yellow[i], 0);
	}
}

static void recovery_status_led_set(struct recovery_gpio_pin *pin, int on)
{
	if (!pin->valid)
		return;

	on = !!on;
	if (pin->last_on == on)
		return;

	recovery_gpio_set_value(pin->gpio, pin->active_low, on);
	pin->last_on = on;
}

static void recovery_gpio_pin_release(struct recovery_gpio_pin *pin)
{
	if (!pin->valid)
		return;

	memset(pin, 0, sizeof(*pin));
}

static ofnode recovery_led_alias_node(const char *alias)
{
	ofnode aliases;
	const char *path;

	aliases = ofnode_path("/aliases");
	if (!ofnode_valid(aliases))
		return ofnode_null();

	path = ofnode_read_string(aliases, alias);
	if (!path)
		return ofnode_null();

	return ofnode_path(path);
}

static int recovery_node_prop_to_gpio(ofnode node, const char *prop, int index,
				      struct recovery_gpio_pin *pin)
{
	struct ofnode_phandle_args args;
	int ret;

	memset(pin, 0, sizeof(*pin));

	if (!ofnode_valid(node))
		return -ENOENT;

	ret = ofnode_parse_phandle_with_args(node, prop, "#gpio-cells", 0, index,
					     &args);
	if (ret)
		return ret;

	if (args.args_count < 1)
		return -EINVAL;

	pin->gpio = args.args[0];
	pin->active_low = args.args_count > 1 &&
			  (args.args[1] & GPIO_ACTIVE_LOW);
	pin->last_on = -1;
	pin->node = node;
	pin->valid = true;

	return 0;
}

static int recovery_led_node_to_gpio(ofnode node, struct recovery_gpio_pin *pin)
{
	return recovery_node_prop_to_gpio(node, "gpios", 0, pin);
}

static void recovery_status_led_add(struct recovery_status_led_ctrl *ctrl,
				    const struct recovery_gpio_pin *pin)
{
	int i;

	if (!pin->valid)
		return;

	for (i = 0; i < ctrl->led_count; i++) {
		if (ctrl->leds[i].gpio == pin->gpio)
			return;
	}

	if (ctrl->led_count >= ARRAY_SIZE(ctrl->leds))
		return;

	ctrl->leds[ctrl->led_count++] = *pin;
}

static void recovery_status_led_collect_alias(struct recovery_status_led_ctrl *ctrl,
					      const char *alias)
{
	struct recovery_gpio_pin pin;

	if (!recovery_led_node_to_gpio(recovery_led_alias_node(alias), &pin))
		recovery_status_led_add(ctrl, &pin);
}

static int recovery_status_led_request(struct recovery_gpio_pin *pin)
{
	recovery_gpio_prepare_output(pin->gpio);
	recovery_gpio_set_value(pin->gpio, pin->active_low, 0);
	pin->last_on = 0;

	return 0;
}

static void recovery_status_led_hw_pwm_set(struct recovery_status_led_ctrl *ctrl,
					   u32 brightness_fp)
{
	u32 high_ticks, value;

	high_ticks = (brightness_fp * RECOVERY_GPIO_FLASH_DUTY_FULL +
		      RECOVERY_STATUS_BRIGHTNESS_FP / 2) /
		     RECOVERY_STATUS_BRIGHTNESS_FP;
	if (ctrl->pwm_active_low)
		high_ticks = RECOVERY_GPIO_FLASH_DUTY_FULL - high_ticks;

	value = ((RECOVERY_GPIO_FLASH_DUTY_FULL - high_ticks) << 8) |
		high_ticks;
	recovery_clrsetbits_le32(RECOVERY_GPIO_SYSCTL_BASE +
				 RECOVERY_REG_GPIO_FLASH_PRD_SET0,
				 GENMASK(15, 0), value);
}

static int recovery_status_led_hw_pwm_init(struct recovery_status_led_ctrl *ctrl)
{
	u32 flash_mode, flash_mode_ext;
	int i;

	ctrl->pwm_active_low = ctrl->leds[0].active_low;
	for (i = 0; i < ctrl->led_count; i++) {
		uintptr_t reg;
		u32 mask;

		if (ctrl->leds[i].active_low != ctrl->pwm_active_low ||
		    !recovery_gpio_flash_mode_bit(ctrl->leds[i].gpio, &reg,
						  &mask))
			return -EOPNOTSUPP;

		if (reg == RECOVERY_GPIO_SYSCTL_BASE +
			   RECOVERY_REG_GPIO_FLASH_MODE_CFG)
			ctrl->pwm_mux_mask |= mask;
		else
			ctrl->pwm_mux_mask_ext |= mask;
	}

	flash_mode = readl((void __iomem *)(RECOVERY_GPIO_SYSCTL_BASE +
					      RECOVERY_REG_GPIO_FLASH_MODE_CFG));
	flash_mode_ext = readl((void __iomem *)(RECOVERY_GPIO_SYSCTL_BASE +
						  RECOVERY_REG_GPIO_FLASH_MODE_CFG_EXT));
	if ((flash_mode & ~ctrl->pwm_mux_mask) ||
	    (flash_mode_ext & ~ctrl->pwm_mux_mask_ext))
		return -EBUSY;

	ctrl->saved_pwm_duty = readl((void __iomem *)(RECOVERY_GPIO_SYSCTL_BASE +
						 RECOVERY_REG_GPIO_FLASH_PRD_SET0));
	ctrl->saved_pwm_map[0] = readl((void __iomem *)(RECOVERY_GPIO_SYSCTL_BASE +
						  RECOVERY_REG_GPIO_FLASH_MAP0));
	ctrl->saved_pwm_map[1] = readl((void __iomem *)(RECOVERY_GPIO_SYSCTL_BASE +
						  RECOVERY_REG_GPIO_FLASH_MAP1));
	ctrl->saved_pwm_cycle = readl((void __iomem *)(RECOVERY_GPIO_SYSCTL_BASE +
						  RECOVERY_REG_CYCLE_CFG_VALUE0));

	recovery_clrsetbits_le32(RECOVERY_GPIO_SYSCTL_BASE +
				 RECOVERY_REG_CYCLE_CFG_VALUE0,
				 GENMASK(7, 0),
				 RECOVERY_STATUS_HW_PWM_PERIOD_TICKS);
	recovery_status_led_hw_pwm_set(ctrl, 0);
	/* Extended PWM muxes use fixed GPIO0..15 channels; share one bucket. */
	writel(RECOVERY_GPIO_FLASH_MAP_BUCKET0,
	       (void __iomem *)(RECOVERY_GPIO_SYSCTL_BASE +
				RECOVERY_REG_GPIO_FLASH_MAP0));
	writel(RECOVERY_GPIO_FLASH_MAP_BUCKET0,
	       (void __iomem *)(RECOVERY_GPIO_SYSCTL_BASE +
				RECOVERY_REG_GPIO_FLASH_MAP1));
	recovery_clrsetbits_le32(RECOVERY_GPIO_SYSCTL_BASE +
				 RECOVERY_REG_GPIO_FLASH_MODE_CFG,
				 0, ctrl->pwm_mux_mask);
	recovery_clrsetbits_le32(RECOVERY_GPIO_SYSCTL_BASE +
				 RECOVERY_REG_GPIO_FLASH_MODE_CFG_EXT,
				 0, ctrl->pwm_mux_mask_ext);

	ctrl->hw_pwm = true;

	return 0;
}

static void recovery_status_led_hw_pwm_stop(struct recovery_status_led_ctrl *ctrl)
{
	int i;

	recovery_status_led_hw_pwm_set(ctrl, 0);
	recovery_clrsetbits_le32(RECOVERY_GPIO_SYSCTL_BASE +
				 RECOVERY_REG_GPIO_FLASH_MODE_CFG,
				 ctrl->pwm_mux_mask, 0);
	recovery_clrsetbits_le32(RECOVERY_GPIO_SYSCTL_BASE +
				 RECOVERY_REG_GPIO_FLASH_MODE_CFG_EXT,
				 ctrl->pwm_mux_mask_ext, 0);

	writel(ctrl->saved_pwm_map[0],
	       (void __iomem *)(RECOVERY_GPIO_SYSCTL_BASE +
				RECOVERY_REG_GPIO_FLASH_MAP0));
	writel(ctrl->saved_pwm_map[1],
	       (void __iomem *)(RECOVERY_GPIO_SYSCTL_BASE +
				RECOVERY_REG_GPIO_FLASH_MAP1));
	writel(ctrl->saved_pwm_duty,
	       (void __iomem *)(RECOVERY_GPIO_SYSCTL_BASE +
				RECOVERY_REG_GPIO_FLASH_PRD_SET0));
	writel(ctrl->saved_pwm_cycle,
	       (void __iomem *)(RECOVERY_GPIO_SYSCTL_BASE +
				RECOVERY_REG_CYCLE_CFG_VALUE0));

	ctrl->hw_pwm = false;
	for (i = 0; i < ctrl->led_count; i++) {
		ctrl->leds[i].last_on = -1;
		recovery_status_led_set(&ctrl->leds[i], 0);
	}
}

static void recovery_status_led_release(struct recovery_status_led_ctrl *ctrl)
{
	int i;

	for (i = 0; i < ctrl->led_count; i++)
		recovery_gpio_pin_release(&ctrl->leds[i]);

	memset(ctrl, 0, sizeof(*ctrl));
}

static int recovery_status_led_init(struct recovery_status_led_ctrl *ctrl)
{
	static const char *const status_aliases[] = {
		"led-boot",
		"led-running",
		"led-failsafe",
		"led-upgrade",
	};
	ofnode leds, node;
	int i, keep = 0;

	memset(ctrl, 0, sizeof(*ctrl));

	leds = ofnode_path("/leds");
	if (ofnode_valid(leds)) {
		ofnode_for_each_subnode(node, leds) {
			const char *function;
			struct recovery_gpio_pin pin;

			function = ofnode_read_string(node, "function");
			if (!function || strcmp(function, "status"))
				continue;

			if (!recovery_led_node_to_gpio(node, &pin))
				recovery_status_led_add(ctrl, &pin);
		}
	}

	for (i = 0; i < ARRAY_SIZE(status_aliases); i++)
		recovery_status_led_collect_alias(ctrl, status_aliases[i]);

	if (!ctrl->led_count)
		return -ENOENT;

	for (i = 0; i < ctrl->led_count; i++) {
		int ret;

		ret = recovery_status_led_request(&ctrl->leds[i]);
		if (ret) {
			printf("Recovery status LED gpio%u request failed: %d\n",
			       ctrl->leds[i].gpio, ret);
			recovery_gpio_pin_release(&ctrl->leds[i]);
			continue;
		}

		if (keep != i) {
			ctrl->leds[keep] = ctrl->leds[i];
			memset(&ctrl->leds[i], 0, sizeof(ctrl->leds[i]));
		}
		keep++;
	}

	ctrl->led_count = keep;

	if (!ctrl->led_count) {
		recovery_status_led_release(ctrl);
		return -ENODEV;
	}

	ctrl->start_ms = get_timer(0);
	if (ctrl->led_count == 1) {
		int pwm_ret = recovery_status_led_hw_pwm_init(ctrl);

		if (pwm_ret)
			printf("Recovery status LED hardware PWM unavailable (%d), using GPIO PWM\n",
			       pwm_ret);
	}

	printf("Recovery status LEDs:");
	for (i = 0; i < ctrl->led_count; i++)
		printf(" gpio%u%s", ctrl->leds[i].gpio,
		       ctrl->leds[i].active_low ? "(L)" : "");
	printf(" (%s PWM)\n", ctrl->hw_pwm ? "hardware" : "software");

	return 0;
}

static u32 recovery_status_led_breath_fp(ulong elapsed)
{
	ulong phase, ramp;
	u32 delta;

	phase = elapsed % RECOVERY_STATUS_BREATHE_PERIOD_MS;
	ramp = phase < RECOVERY_STATUS_BREATHE_HALF_MS ?
	       phase : RECOVERY_STATUS_BREATHE_PERIOD_MS - phase;
	delta = RECOVERY_STATUS_BRIGHTNESS_FP - RECOVERY_STATUS_BREATHE_MIN_FP;

	return RECOVERY_STATUS_BREATHE_MIN_FP +
	       (delta * ramp * ramp +
		(RECOVERY_STATUS_BREATHE_HALF_MS *
		 RECOVERY_STATUS_BREATHE_HALF_MS) / 2) /
	       (RECOVERY_STATUS_BREATHE_HALF_MS *
		RECOVERY_STATUS_BREATHE_HALF_MS);
}

static ulong recovery_status_led_position_fp(struct recovery_status_led_ctrl *ctrl,
					     ulong elapsed)
{
	ulong range_fp, phase;
	int span;

	if (ctrl->led_count <= 1)
		return 0;

	span = ctrl->led_count - 1;
	range_fp = span * RECOVERY_STATUS_BRIGHTNESS_FP;
	phase = elapsed % (2 * span * RECOVERY_STATUS_SWEEP_STEP_MS);

	if (phase <= span * RECOVERY_STATUS_SWEEP_STEP_MS)
		return phase * RECOVERY_STATUS_BRIGHTNESS_FP /
		       RECOVERY_STATUS_SWEEP_STEP_MS;

	phase -= span * RECOVERY_STATUS_SWEEP_STEP_MS;

	return range_fp - (phase * RECOVERY_STATUS_BRIGHTNESS_FP /
			   RECOVERY_STATUS_SWEEP_STEP_MS);
}

static u32 recovery_status_led_brightness_fp(struct recovery_status_led_ctrl *ctrl,
					     int idx, ulong elapsed)
{
	ulong center_fp, pwm_phase;
	u32 breath_fp, weight_fp, dist_fp;
	ulong led_pos_fp;

	center_fp = recovery_status_led_position_fp(ctrl, elapsed);
	led_pos_fp = idx * RECOVERY_STATUS_BRIGHTNESS_FP;
	dist_fp = center_fp > led_pos_fp ? center_fp - led_pos_fp :
					  led_pos_fp - center_fp;
	if (dist_fp >= RECOVERY_STATUS_OVERLAP_FP)
		return 0;

	weight_fp = (RECOVERY_STATUS_OVERLAP_FP - dist_fp) *
		    RECOVERY_STATUS_BRIGHTNESS_FP / RECOVERY_STATUS_OVERLAP_FP;
	weight_fp = weight_fp * weight_fp / RECOVERY_STATUS_BRIGHTNESS_FP;
	breath_fp = recovery_status_led_breath_fp(elapsed);
	pwm_phase = weight_fp * breath_fp;
	pwm_phase = (pwm_phase + RECOVERY_STATUS_BRIGHTNESS_FP / 2) /
		    RECOVERY_STATUS_BRIGHTNESS_FP;

	return pwm_phase;
}

static void recovery_status_led_poll(struct recovery_status_led_ctrl *ctrl)
{
	ulong elapsed, now, pwm_phase;
	u32 brightness_fp;
	int duty_ms;
	int i;

	if (!ctrl->led_count)
		return;

	now = get_timer(0);
	elapsed = now - ctrl->start_ms;
	if (ctrl->hw_pwm) {
		if (ctrl->last_pwm_update &&
		    now - ctrl->last_pwm_update < RECOVERY_STATUS_HW_PWM_UPDATE_MS)
			return;

		ctrl->last_pwm_update = now;
		recovery_status_led_hw_pwm_set(ctrl,
					       recovery_status_led_breath_fp(elapsed));
		return;
	}

	pwm_phase = elapsed % RECOVERY_STATUS_SW_PWM_PERIOD_MS;

	for (i = 0; i < ctrl->led_count; i++) {
		brightness_fp =
			recovery_status_led_brightness_fp(ctrl, i, elapsed);
		duty_ms = (brightness_fp * RECOVERY_STATUS_SW_PWM_PERIOD_MS +
			   RECOVERY_STATUS_BRIGHTNESS_FP / 2) /
			  RECOVERY_STATUS_BRIGHTNESS_FP;
		recovery_status_led_set(&ctrl->leds[i],
					duty_ms && pwm_phase < duty_ms);
	}
}

static void recovery_status_led_service(void *arg)
{
	recovery_status_led_poll(arg);
}

static void recovery_status_led_stop(struct recovery_status_led_ctrl *ctrl)
{
	int i;

	if (!ctrl->led_count)
		return;

	if (ctrl->hw_pwm) {
		recovery_status_led_hw_pwm_stop(ctrl);
		return;
	}

	for (i = 0; i < ctrl->led_count; i++)
		recovery_status_led_set(&ctrl->leds[i], 0);
}

enum recovery_dhcp_request_verdict {
	RECOVERY_DHCP_REQUEST_ACK = 0,
	RECOVERY_DHCP_REQUEST_NAK,
	RECOVERY_DHCP_REQUEST_IGNORE,
};

static int recovery_dhcp_get_option(const u8 *pkt, int pkt_len, u8 code,
				    const u8 **value, u8 *value_len)
{
	int off = DHCP_OPTIONS_OFS;

	while (off < pkt_len) {
		u8 opt, opt_len;

		opt = pkt[off++];
		if (opt == DHCP_OPTION_PAD)
			continue;
		if (opt == DHCP_OPTION_END)
			return -ENOENT;
		if (off >= pkt_len)
			break;

		opt_len = pkt[off++];
		if (off + opt_len > pkt_len)
			break;

		if (opt == code) {
			if (value)
				*value = pkt + off;
			if (value_len)
				*value_len = opt_len;
			return 0;
		}

		off += opt_len;
	}

	return -EINVAL;
}

static int recovery_dhcp_get_u8_option(const u8 *pkt, int pkt_len, u8 code,
				       u8 *value)
{
	const u8 *opt;
	u8 opt_len;
	int ret;

	ret = recovery_dhcp_get_option(pkt, pkt_len, code, &opt, &opt_len);
	if (ret)
		return ret;
	if (opt_len != sizeof(*value))
		return -EINVAL;

	*value = opt[0];
	return 0;
}

static int recovery_dhcp_get_ip4_option(const u8 *pkt, int pkt_len, u8 code,
					ip4_addr_t *addr)
{
	const u8 *opt;
	u8 opt_len;
	int ret;

	ret = recovery_dhcp_get_option(pkt, pkt_len, code, &opt, &opt_len);
	if (ret)
		return ret;
	if (opt_len != sizeof(addr->addr))
		return -EINVAL;

	memcpy(&addr->addr, opt, sizeof(addr->addr));
	return 0;
}

static int recovery_dhcp_put_option_head(u8 *options, int off, u8 code, u8 len)
{
	if (off < 0 || off + 2 + len > DHCP_OPTIONS_LEN)
		return -ENOSPC;

	options[off++] = code;
	options[off++] = len;

	return off;
}

static int recovery_dhcp_put_u8_option(u8 *options, int off, u8 code, u8 value)
{
	off = recovery_dhcp_put_option_head(options, off, code, sizeof(value));
	if (off < 0)
		return off;

	options[off++] = value;
	return off;
}

static int recovery_dhcp_put_u32_option(u8 *options, int off, u8 code, u32 value)
{
	u32 be_value = lwip_htonl(value);

	off = recovery_dhcp_put_option_head(options, off, code,
					    sizeof(be_value));
	if (off < 0)
		return off;

	memcpy(options + off, &be_value, sizeof(be_value));
	return off + sizeof(be_value);
}

static int recovery_dhcp_put_ip4_option(u8 *options, int off, u8 code,
					const ip4_addr_t *addr)
{
	off = recovery_dhcp_put_option_head(options, off, code,
					    sizeof(addr->addr));
	if (off < 0)
		return off;

	memcpy(options + off, &addr->addr, sizeof(addr->addr));
	return off + sizeof(addr->addr);
}

static void recovery_dhcp_finalize_options(struct pbuf *p, struct dhcp_msg *msg,
					   int opt_len)
{
	msg->options[opt_len++] = DHCP_OPTION_END;

	while (((opt_len < DHCP_MIN_OPTIONS_LEN) || (opt_len & 3)) &&
	       opt_len < DHCP_OPTIONS_LEN)
		msg->options[opt_len++] = DHCP_OPTION_PAD;

	pbuf_realloc(p, sizeof(*msg) - DHCP_OPTIONS_LEN + opt_len);
}

static enum recovery_dhcp_request_verdict
recovery_dhcp_classify_request(struct recovery_dhcp_server *srv,
			       const struct dhcp_msg *req,
			       const u8 *pkt, int pkt_len)
{
	ip4_addr_t option_ip, ciaddr;
	bool has_server_id, has_requested_ip;

	has_server_id = !recovery_dhcp_get_ip4_option(pkt, pkt_len,
						      DHCP_OPTION_SERVER_ID,
						      &option_ip);
	if (has_server_id) {
		if (!ip4_addr_eq(&option_ip, &srv->server_ip))
			return RECOVERY_DHCP_REQUEST_IGNORE;
	}

	has_requested_ip = !recovery_dhcp_get_ip4_option(pkt, pkt_len,
							 DHCP_OPTION_REQUESTED_IP,
							 &option_ip);
	if (has_requested_ip) {
		return ip4_addr_eq(&option_ip, &srv->client_ip) ?
			RECOVERY_DHCP_REQUEST_ACK :
			RECOVERY_DHCP_REQUEST_NAK;
	}

	ciaddr.addr = req->ciaddr.addr;
	if (!ip4_addr_isany(&ciaddr)) {
		return ip4_addr_eq(&ciaddr, &srv->client_ip) ?
			RECOVERY_DHCP_REQUEST_ACK :
			RECOVERY_DHCP_REQUEST_NAK;
	}

	return RECOVERY_DHCP_REQUEST_ACK;
}

static int recovery_dhcp_send_reply(struct recovery_dhcp_server *srv,
				    const struct dhcp_msg *req,
				    u8 message_type)
{
	struct pbuf *p;
	struct dhcp_msg *reply;
	ip_addr_t src_addr;
	ip_addr_t reply_addr;
	int opt_len;
	err_t err;

	p = pbuf_alloc(PBUF_TRANSPORT, sizeof(*reply), PBUF_RAM);
	if (!p)
		return -ENOMEM;

	reply = p->payload;
	memset(reply, 0, sizeof(*reply));

	reply->op = DHCP_BOOTREPLY;
	reply->htype = req->htype;
	reply->hlen = req->hlen;
	reply->hops = req->hops;
	reply->xid = req->xid;
	reply->secs = req->secs;
	reply->flags = req->flags | lwip_htons(0x8000);
	if (message_type != DHCP_NAK)
		reply->yiaddr.addr = srv->client_ip.addr;
	reply->siaddr.addr = 0;
	reply->giaddr = req->giaddr;
	memcpy(reply->chaddr, req->chaddr, DHCP_CHADDR_LEN);
	reply->cookie = PP_HTONL(DHCP_MAGIC_COOKIE);

	opt_len = 0;
	opt_len = recovery_dhcp_put_u8_option(reply->options, opt_len,
					      DHCP_OPTION_MESSAGE_TYPE,
					      message_type);
	opt_len = recovery_dhcp_put_ip4_option(reply->options, opt_len,
					       DHCP_OPTION_SERVER_ID,
					       &srv->server_ip);
	if (message_type != DHCP_NAK) {
		opt_len = recovery_dhcp_put_u32_option(reply->options, opt_len,
						       DHCP_OPTION_LEASE_TIME,
						       RECOVERY_DHCP_LEASE_SECS);
		opt_len = recovery_dhcp_put_ip4_option(reply->options, opt_len,
						       DHCP_OPTION_SUBNET_MASK,
						       &srv->netmask);
	}
	if (opt_len < 0) {
		pbuf_free(p);
		return opt_len;
	}

	recovery_dhcp_finalize_options(p, reply, opt_len);

	reply_addr = *IP_ADDR_BROADCAST;

	ip_addr_copy_from_ip4(src_addr, srv->server_ip);
	err = udp_sendto_if_src(srv->pcb, p, &reply_addr,
				LWIP_IANA_PORT_DHCP_CLIENT, srv->netif,
				&src_addr);
	pbuf_free(p);

	return err == ERR_OK ? 0 : -EIO;
}

static void recovery_dhcp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
			       const ip_addr_t *addr, u16_t port)
{
	struct recovery_dhcp_server *srv = arg;
	const struct dhcp_msg *req;
	u8 pkt[RECOVERY_DHCP_MAX_MSG_LEN];
	u8 message_type;
	int copy_len, pkt_len;

	(void)pcb;
	(void)addr;

	if (!p)
		return;

	if (port != LWIP_IANA_PORT_DHCP_CLIENT)
		goto out;

	copy_len = p->tot_len;
	if (copy_len > sizeof(pkt))
		copy_len = sizeof(pkt);

	pkt_len = pbuf_copy_partial(p, pkt, copy_len, 0);
	if (pkt_len < DHCP_OPTIONS_OFS)
		goto out;

	req = (const struct dhcp_msg *)pkt;
	if (req->op != DHCP_BOOTREQUEST ||
	    req->htype != LWIP_IANA_HWTYPE_ETHERNET ||
	    req->hlen != ARP_HLEN ||
	    req->cookie != PP_HTONL(DHCP_MAGIC_COOKIE))
		goto out;

	if (recovery_dhcp_get_u8_option(pkt, pkt_len, DHCP_OPTION_MESSAGE_TYPE,
					&message_type))
		goto out;

	switch (message_type) {
	case DHCP_DISCOVER:
		recovery_dhcp_send_reply(srv, req, DHCP_OFFER);
		break;
	case DHCP_REQUEST:
		switch (recovery_dhcp_classify_request(srv, req, pkt, pkt_len)) {
		case RECOVERY_DHCP_REQUEST_ACK:
			recovery_dhcp_send_reply(srv, req, DHCP_ACK);
			break;
		case RECOVERY_DHCP_REQUEST_NAK:
			recovery_dhcp_send_reply(srv, req, DHCP_NAK);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

out:
	pbuf_free(p);
}

static int recovery_dhcp_server_init(struct recovery_dhcp_server *srv,
				     struct netif *netif)
{
	char server_ip[IP4ADDR_STRLEN_MAX];
	char client_ip[IP4ADDR_STRLEN_MAX];
	char netmask[IP4ADDR_STRLEN_MAX];
	char router[IP4ADDR_STRLEN_MAX];
	char broadcast[IP4ADDR_STRLEN_MAX];
	err_t err;

	memset(srv, 0, sizeof(*srv));

	srv->netif = netif;
	ip4_addr_copy(srv->server_ip, *netif_ip4_addr(netif));
	ip4_addr_copy(srv->netmask, *netif_ip4_netmask(netif));
	ip4_addr_copy(srv->router, *netif_ip4_gw(netif));
	ip4_addr_copy(srv->dns, *netif_ip4_addr(netif));

	if (ip4_addr_isany(&srv->router))
		ip4_addr_copy(srv->router, srv->server_ip);

	if (!ip4addr_aton(RECOVERY_DHCP_CLIENT_IPADDR, &srv->client_ip))
		return -EINVAL;

	if (ip4_addr_isany(&srv->netmask)) {
		if (!ip4addr_aton(RECOVERY_DHCP_BROADCAST_IPADDR, &srv->broadcast))
			return -EINVAL;
	} else {
		srv->broadcast.addr = (srv->server_ip.addr & srv->netmask.addr) |
				      ~srv->netmask.addr;
	}

	srv->pcb = udp_new();
	if (!srv->pcb)
		return -ENOMEM;

	ip_set_option(srv->pcb, SOF_BROADCAST);

	err = udp_bind(srv->pcb, IP4_ADDR_ANY, LWIP_IANA_PORT_DHCP_SERVER);
	if (err != ERR_OK) {
		udp_remove(srv->pcb);
		srv->pcb = NULL;
		return -EIO;
	}

	udp_bind_netif(srv->pcb, netif);
	udp_recv(srv->pcb, recovery_dhcp_recv, srv);

	printf("DHCP recovery server: %s/67 -> offer %s mask %s gw %s bcast %s\n",
	       ip4addr_ntoa_r(&srv->server_ip, server_ip, sizeof(server_ip)),
	       ip4addr_ntoa_r(&srv->client_ip, client_ip, sizeof(client_ip)),
	       ip4addr_ntoa_r(&srv->netmask, netmask, sizeof(netmask)),
	       ip4addr_ntoa_r(&srv->router, router, sizeof(router)),
	       ip4addr_ntoa_r(&srv->broadcast, broadcast,
			      sizeof(broadcast)));

	return 0;
}

static void recovery_dhcp_server_stop(struct recovery_dhcp_server *srv)
{
	if (srv->pcb)
		udp_remove(srv->pcb);

	memset(srv, 0, sizeof(*srv));
}

static int recovery_led_init(struct recovery_led_ctrl *ctrl)
{
	ofnode mdio_node, root;
	int i, ret;

	memset(ctrl, 0, sizeof(*ctrl));
	root = ofnode_path("/");
	mdio_node = ofnode_parse_phandle(root, "recovery-link-mdio", 0);
	if (!ofnode_valid(mdio_node))
		return 0;

	for (i = 0; i < RECOVERY_LED_PORTS; i++) {
		u32 phy_addr;

		if (ofnode_read_u32_index(root, "recovery-link-phy-addrs", i,
					  &phy_addr) || phy_addr > U8_MAX ||
		    recovery_node_prop_to_gpio(root, "recovery-green-led-gpios",
					       i, &ctrl->green[i]) ||
		    recovery_node_prop_to_gpio(root, "recovery-yellow-led-gpios",
					       i, &ctrl->yellow[i]))
			break;

		ctrl->phy_addr[i] = phy_addr;
		ctrl->port_count++;
	}

	if (!ctrl->port_count)
		return 0;

	/* Make sure PHY LED mux is disabled so software can own the lines. */
	recovery_clrsetbits_le32(RECOVERY_CHIP_SCU_BASE + RECOVERY_REG_GPIO_2ND_I2C_MODE,
				 RECOVERY_GPIO_LAN0_LED0_MODE_MASK |
				 RECOVERY_GPIO_LAN0_LED1_MODE_MASK |
				 RECOVERY_GPIO_LAN1_LED0_MODE_MASK |
				 RECOVERY_GPIO_LAN1_LED1_MODE_MASK, 0);

	for (i = 0; i < ctrl->port_count; i++) {
		recovery_gpio_prepare_output(ctrl->green[i].gpio);
		recovery_gpio_prepare_output(ctrl->yellow[i].gpio);
		recovery_led_set_pin(&ctrl->green[i], 0);
		recovery_led_set_pin(&ctrl->yellow[i], 0);
	}

	ret = uclass_get_device_by_ofnode(UCLASS_MDIO, mdio_node, &ctrl->mdio_dev);
	if (ret)
		ctrl->mdio_dev = NULL;

	return 0;
}

static int recovery_led_phy_speed(struct recovery_led_ctrl *ctrl, int idx)
{
	int bmcr, bmsr, stat1000, ctrl1000, lpa;

	if (!ctrl->mdio_dev || idx >= ctrl->port_count)
		return 0;

	bmsr = dm_mdio_read(ctrl->mdio_dev, ctrl->phy_addr[idx],
			    MDIO_DEVAD_NONE, MII_BMSR);
	if (bmsr < 0)
		return bmsr;

	bmsr = dm_mdio_read(ctrl->mdio_dev, ctrl->phy_addr[idx],
			    MDIO_DEVAD_NONE, MII_BMSR);
	if (bmsr < 0)
		return bmsr;

	if (!(bmsr & BMSR_LSTATUS))
		return 0;

	bmcr = dm_mdio_read(ctrl->mdio_dev, ctrl->phy_addr[idx],
			    MDIO_DEVAD_NONE, MII_BMCR);
	if (bmcr < 0)
		return bmcr;

	if (!(bmcr & BMCR_ANENABLE)) {
		if (bmcr & BMCR_SPEED1000)
			return SPEED_1000;
		if (bmcr & BMCR_SPEED100)
			return SPEED_100;

		return SPEED_10;
	}

	stat1000 = dm_mdio_read(ctrl->mdio_dev, ctrl->phy_addr[idx],
				MDIO_DEVAD_NONE, MII_STAT1000);
	ctrl1000 = dm_mdio_read(ctrl->mdio_dev, ctrl->phy_addr[idx],
				MDIO_DEVAD_NONE, MII_CTRL1000);
	if (stat1000 < 0 || ctrl1000 < 0)
		return stat1000 < 0 ? stat1000 : ctrl1000;

	if (stat1000 >= 0 && ctrl1000 >= 0) {
		stat1000 &= ctrl1000 << 2;
		if (stat1000 & (PHY_1000BTSR_1000FD | PHY_1000BTSR_1000HD))
			return SPEED_1000;
	}

	lpa = dm_mdio_read(ctrl->mdio_dev, ctrl->phy_addr[idx],
			   MDIO_DEVAD_NONE, MII_ADVERTISE);
	if (lpa < 0)
		return lpa;

	bmsr = dm_mdio_read(ctrl->mdio_dev, ctrl->phy_addr[idx],
			    MDIO_DEVAD_NONE, MII_LPA);
	if (bmsr < 0)
		return bmsr;

	lpa &= bmsr;
	if (lpa & (LPA_100FULL | LPA_100HALF))
		return SPEED_100;

	return SPEED_10;
}

static void recovery_led_poll(struct recovery_led_ctrl *ctrl)
{
	ulong activity_ms;
	ulong now;
	bool activity;
	bool blink_on;
	bool poll_phys;
	int i;

	now = get_timer(0);
	if (ctrl->last_poll && now - ctrl->last_poll < RECOVERY_LED_POLL_MS)
		return;

	ctrl->last_poll = now;
	poll_phys = !ctrl->last_phy_poll ||
		    now - ctrl->last_phy_poll >= RECOVERY_LED_PHY_POLL_MS;
	if (ctrl->mdio_fault &&
	    get_timer(ctrl->last_mdio_error) < RECOVERY_LED_MDIO_BACKOFF_MS)
		poll_phys = false;

	if (poll_phys) {
		ctrl->last_phy_poll = now;
		for (i = 0; i < ctrl->port_count; i++) {
			int speed = recovery_led_phy_speed(ctrl, i);

			if (speed < 0) {
				ctrl->last_mdio_error = now;
				ctrl->mdio_fault = true;
				break;
			}

			ctrl->speed[i] = speed;
		}

		if (i == ctrl->port_count)
			ctrl->mdio_fault = false;
	}

	activity_ms = airoha_recovery_get_lan_activity_ms();
	activity = activity_ms && now - activity_ms <= RECOVERY_LED_ACTIVITY_MS;
	blink_on = !activity ||
		   ((now / RECOVERY_LED_BLINK_MS) & 1);

	for (i = 0; i < ctrl->port_count; i++) {
		recovery_led_set_pin(&ctrl->green[i],
				     ctrl->speed[i] == SPEED_1000 && blink_on);
		recovery_led_set_pin(&ctrl->yellow[i],
				     (ctrl->speed[i] == SPEED_10 ||
				      ctrl->speed[i] == SPEED_100) &&
					     blink_on);
	}
}

static const char *recovery_default_target(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_RECOVERY:
		return "recovery";
	case TARGET_FIRMWARE:
		return "fit";
	case TARGET_UBOOT:
		return "uboot";
	}

	return "fit";
}

static const char *recovery_target_env(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_RECOVERY:
		return NULL;
	case TARGET_FIRMWARE:
		return "recovery_mtd";
	case TARGET_UBOOT:
		return "recovery_mtd_uboot";
	}

	return "recovery_mtd";
}

static const char *recovery_raw_env(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_RECOVERY:
		return NULL;
	case TARGET_FIRMWARE:
		return "recovery_dev";
	case TARGET_UBOOT:
		return "recovery_dev_uboot";
	}

	return "recovery_dev";
}

static const char *recovery_size_env(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_RECOVERY:
		return NULL;
	case TARGET_FIRMWARE:
		return "recovery_size";
	case TARGET_UBOOT:
		return "recovery_size_uboot";
	}

	return "recovery_size";
}

static ulong recovery_raw_offset(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_RECOVERY:
		return 0;
	case TARGET_FIRMWARE:
		return env_get_hex("recovery_ofs", 0x050000);
	case TARGET_UBOOT:
		return env_get_hex("uboot_ofs",
				   RECOVERY_UBOOT_SLOT_DEFAULT_OFS);
	}

	return 0;
}

static const char *recovery_default_raw(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_UBOOT:
		return RECOVERY_UBOOT_SLOT_DEFAULT_DEV;
	case TARGET_FIRMWARE:
	default:
		return NULL;
	}
}

static unsigned long recovery_target_size_cap(enum upload_target tgt)
{
	ulong size_cap = env_get_hex(recovery_size_env(tgt), 0);

	if (tgt == TARGET_UBOOT &&
	    (!size_cap || size_cap > RECOVERY_MAX_UBOOT_SIZE))
		size_cap = RECOVERY_MAX_UBOOT_SIZE;

	return size_cap;
}

static const char *recovery_ubi_part(enum upload_target tgt)
{
	const char *part;

	if (recovery_board_is_q1000k())
		return "ubi";

	if (tgt == TARGET_FIRMWARE && current_ubi_layout)
		return current_ubi_layout->part;

	switch (tgt) {
	case TARGET_UBOOT:
		part = env_get("recovery_ubi_part_uboot");
		break;
	case TARGET_FIRMWARE:
	default:
		part = env_get("recovery_ubi_part");
		break;
	}

	if (!part)
		part = env_get("recovery_ubi_part");

	return part ?: "ubi";
}

static int recovery_parse_ubi_layout(const char *uri)
{
	const char *query;
	int i;

	current_ubi_layout = &recovery_ubi_layouts[0];
	query = strchr(uri, '?');
	if (!query)
		return 0;

	if (strncmp(query, "?layout=", 8))
		return -EINVAL;
	query += 8;

	for (i = 0; i < ARRAY_SIZE(recovery_ubi_layouts); i++) {
		if (!strcmp(query, recovery_ubi_layouts[i].version)) {
			if (recovery_board_is_q1000k() && i != 0)
				return -EINVAL;
			current_ubi_layout = &recovery_ubi_layouts[i];
			return 0;
		}
	}

	return -EINVAL;
}

static int recovery_select_ubi(const char *part)
{
	struct ubi_device *ubi;
	bool selected = false;
	int ret;

	if (!IS_ENABLED(CONFIG_CMD_UBI) || !IS_ENABLED(CONFIG_MTD_UBI))
		return -ENODEV;

	ubi = ubi_get_device(0);
	if (ubi) {
		selected = ubi->mtd && !strcmp(ubi->mtd->name, part);
		ubi_put_device(ubi);
	}
	if (selected)
		return 0;

	if (recovery_ubi_attach_error)
		return recovery_ubi_attach_error;

	ret = ubi_part((char *)part, NULL);
	if (ret)
		recovery_ubi_attach_error = ret;

	return ret;
}

static int recovery_try_ubi_target(enum upload_target tgt,
				       struct recovery_target *target)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	struct ubi_device *ubi;
	struct mtd_info *mtd;
	const char *volume = env_get(recovery_target_env(tgt));
	const char *part = recovery_ubi_part(tgt);

	if (recovery_board_is_q1000k())
		volume = "fit";
	if (!volume)
		volume = recovery_default_target(tgt);

	if (recovery_select_ubi(part)) {
		mtd_probe_devices();
		mtd = get_mtd_device_nm(part);
		if (IS_ERR_OR_NULL(mtd))
			return -ENODEV;

		target->backend = RECOVERY_BACKEND_UBI;
		target->name = volume;
		target->ubi_part = part;
		target->mtd = mtd;
		target->cur_size = 0;
		target->limit = mtd->size;
		target->ubi_needs_format = true;
		return 0;
	}

	desc = ubi_open_volume_nm(0, volume, UBI_READWRITE);
	if (IS_ERR_OR_NULL(desc)) {
		ubi = ubi_get_device(0);
		if (!ubi)
			return -ENODEV;

		target->backend = RECOVERY_BACKEND_UBI;
		target->name = volume;
		target->ubi_part = part;
		target->ubi_needs_format = false;
		target->cur_size = 0;
		target->limit = (unsigned long long)ubi->avail_pebs *
			(unsigned long long)ubi->leb_size;
		ubi_put_device(ubi);
		return 0;
	}

	target->backend = RECOVERY_BACKEND_UBI;
	target->name = volume;
	target->ubi_part = part;
	target->ubi_needs_format = false;
	target->cur_size = (unsigned long long)desc->vol->reserved_pebs *
			   (unsigned long long)desc->vol->usable_leb_size;
	target->limit = (unsigned long long)(desc->vol->reserved_pebs +
					     desc->vol->ubi->avail_pebs) *
			(unsigned long long)desc->vol->usable_leb_size;

	ubi_close_volume(desc);
	return 0;
#else
	return -ENODEV;
#endif
}

/* Inspect geometry only: even a UBI "read" attach may erase/program NAND. */
static int recovery_q1000k_target(enum upload_target tgt,
				   struct recovery_target *target)
{
	struct mtd_info *mtd;

	if (tgt != TARGET_FIRMWARE && !IS_ENABLED(CONFIG_Q1000K_INSTALLER))
		return -EPERM;
	if (tgt == TARGET_UBOOT) {
		memset(target, 0, sizeof(*target));
		target->name = "chainloader";
		target->limit = RECOVERY_UBOOT_SLOT_SIZE;
		return 0;
	}

	mtd_probe_devices();
	mtd = get_mtd_device_nm("ubi");
	if (IS_ERR_OR_NULL(mtd))
		return -ENODEV;

	if (!mtd->parent || mtd->parent->parent ||
	    strcmp(mtd->parent->name, RECOVERY_UBOOT_SLOT_DEFAULT_DEV) ||
	    mtd->parent->size != RECOVERY_Q1000K_NAND_BYTES ||
	    mtd->offset != RECOVERY_Q1000K_UBI_OFFSET ||
	    mtd->size != RECOVERY_Q1000K_UBI_BYTES) {
		put_mtd_device(mtd);
		return -EINVAL;
	}

	target->backend = RECOVERY_BACKEND_UBI;
	target->name = "fit";
	if (tgt == TARGET_RECOVERY)
		target->name = "recovery";
	target->ubi_part = "ubi";
	target->mtd = mtd;
	target->limit = mtd->size;
	target->ubi_needs_format = true;
	return 0;
}

static int recovery_resolve_target(enum upload_target tgt,
				   struct recovery_target *target)
{
	const char *name;
	const char *raw;
	struct mtd_info *mtd;
	ulong ofs;

	memset(target, 0, sizeof(*target));

	if (recovery_board_is_q1000k())
		return recovery_q1000k_target(tgt, target);
	if (tgt == TARGET_RECOVERY)
		return -EOPNOTSUPP;

	name = env_get(recovery_target_env(tgt));
	if (!name)
		name = recovery_default_target(tgt);

	mtd_probe_devices();

	raw = env_get(recovery_raw_env(tgt));
	if (!raw || !*raw)
		raw = recovery_default_raw(tgt);
	if (raw && *raw) {
		mtd = get_mtd_device_nm(raw);
		if (!IS_ERR_OR_NULL(mtd)) {
			ulong size_cap = recovery_target_size_cap(tgt);

			ofs = recovery_raw_offset(tgt);
			target->backend = RECOVERY_BACKEND_MTD;
			target->name = raw;
			target->mtd = mtd;
			target->ofs = ofs;
			target->limit = mtd->size;
			if (ofs && target->limit > ofs)
				target->limit -= ofs;
			if (size_cap && target->limit > size_cap)
				target->limit = size_cap;
			target->cur_size = target->limit;
			return 0;
		}
	}

	/*
	 * The XR1710G U-Boot upload target is the raw chainloader slot. Do
	 * not silently fall back to an MTD/UBI volume named "uboot"; old saved
	 * environments may lack recovery_dev_uboot, and writing there makes the
	 * browser report success while the first-stage loader still reads stale
	 * or invalid bytes from 0x600000/0x602100.
	 */
	if (tgt == TARGET_UBOOT)
		return -ENODEV;

	mtd = get_mtd_device_nm(name);
	if (!IS_ERR_OR_NULL(mtd)) {
		target->backend = RECOVERY_BACKEND_MTD;
		target->name = name;
		target->mtd = mtd;
		target->limit = mtd->size;
		target->cur_size = target->limit;
		return 0;
	}

	if (!recovery_try_ubi_target(tgt, target))
		return 0;

	if (!raw) {
		switch (tgt) {
		case TARGET_RECOVERY:
			return -EINVAL;
		case TARGET_FIRMWARE:
			raw = "nor0";
			break;
		case TARGET_UBOOT:
			raw = env_get("recovery_dev");
			if (!raw)
				raw = "nor0";
			break;
		}
	}

	mtd = get_mtd_device_nm(raw);
	if (IS_ERR_OR_NULL(mtd))
		return -ENODEV;

	ofs = recovery_raw_offset(tgt);
	target->backend = RECOVERY_BACKEND_MTD;
	target->name = raw;
	target->mtd = mtd;
	target->ofs = ofs;
	target->limit = mtd->size;
	if (ofs && target->limit > ofs)
		target->limit -= ofs;
	{
		ulong size_cap = recovery_target_size_cap(tgt);

		if (size_cap && target->limit > size_cap)
			target->limit = size_cap;
	}
	target->cur_size = target->limit;

	return 0;
}

static void recovery_release_target(struct recovery_target *target)
{
	if (target->mtd)
		put_mtd_device(target->mtd);

	target->mtd = NULL;
}

static int recovery_validate_xg2010g_uboot_target(
	const struct recovery_target *target)
{
	if (!recovery_board_is_xg2010g())
		return 0;

	if (!target || target->backend != RECOVERY_BACKEND_MTD || !target->mtd ||
	    strcmp(target->mtd->name, RECOVERY_UBOOT_SLOT_DEFAULT_DEV) ||
	    target->ofs != RECOVERY_UBOOT_SLOT_DEFAULT_OFS ||
	    target->limit != RECOVERY_UBOOT_SLOT_SIZE) {
		printf("Refusing U-Boot write outside spi-nand0 0x%lx..0x%lx\n",
		       RECOVERY_UBOOT_SLOT_DEFAULT_OFS,
		       RECOVERY_UBOOT_SLOT_DEFAULT_OFS + RECOVERY_UBOOT_SLOT_SIZE);
		return -EPERM;
	}

	return 0;
}

static int recovery_force_ubi_rebuild(struct recovery_target *target)
{
	struct mtd_info *mtd;

	if (target->backend != RECOVERY_BACKEND_UBI || !target->ubi_part)
		return -EINVAL;

	ubi_detach();
	recovery_ubi_attach_error = 0;
	if (target->mtd) {
		put_mtd_device(target->mtd);
		target->mtd = NULL;
	}

	mtd_probe_devices();
	mtd = get_mtd_device_nm(target->ubi_part);
	if (IS_ERR_OR_NULL(mtd))
		return IS_ERR(mtd) ? PTR_ERR(mtd) : -ENODEV;

	target->mtd = mtd;
	target->cur_size = 0;
	target->limit = mtd->size;
	target->ubi_needs_format = true;

	return 0;
}

static void recovery_service_runtime(struct recovery_status_led_ctrl *status_leds)
{
	struct udevice *udev = eth_get_dev();
	struct netif *netif = net_lwip_get_netif();

	if (udev && netif)
		net_lwip_rx(udev, netif);
	recovery_status_led_poll(status_leds);
	WATCHDOG_RESET();
}

static int recovery_validate_q1000k_chainloader(const void *fit, size_t size);

static int recovery_prepare_uboot_fit(const void *image, size_t size,
					const u8 **fit, size_t *fit_size)
{
	const u8 *p = image;
	u32 prefix_magic;
	u32 candidate_size;

	if (recovery_board_is_q1000k()) {
		int ret = recovery_validate_q1000k_chainloader(image, size);

		if (ret)
			return ret;
		*fit = image;
		*fit_size = size;
		return 0;
	}

	if (size < sizeof(struct fdt_header))
		return -EINVAL;

	/* Web uploads may be a bare FIT or a packaged slot. */
	if (recovery_be32_to_cpu(p) == RECOVERY_FDT_MAGIC) {
		candidate_size = fdt_totalsize(p);
		if (candidate_size < sizeof(struct fdt_header) ||
		candidate_size > size)
			return -EFBIG;
		*fit = p;
		*fit_size = candidate_size;
		return recovery_validate_fit(p, candidate_size);
	}

	/* Accept the legacy packaged slot form for web/API compatibility. */
	if (size <= RECOVERY_UBOOT_SLOT_FIT_OFFSET + sizeof(u32))
		return -EINVAL;
	prefix_magic = recovery_be32_to_cpu(p);
	if (prefix_magic != RECOVERY_IH_MAGIC ||
	    recovery_be32_to_cpu(p + RECOVERY_UBOOT_SLOT_FIT_OFFSET) !=
		    RECOVERY_FDT_MAGIC) {
		printf("Invalid XG2010G U-Boot upload: expected FIT at offset 0 or 0x%x\n",
		       RECOVERY_UBOOT_SLOT_FIT_OFFSET);
		return -EINVAL;
	}

	p += RECOVERY_UBOOT_SLOT_FIT_OFFSET;
	candidate_size = fdt_totalsize(p);
	if (candidate_size < sizeof(struct fdt_header) ||
	candidate_size > size - RECOVERY_UBOOT_SLOT_FIT_OFFSET)
		return -EFBIG;
	*fit = p;
	*fit_size = candidate_size;
	return recovery_validate_fit(p, candidate_size);
}

/*
 * Canonicalize the receive buffer to the persistent XG2010G format.  The
 * stock boot command reads the raw FIT from 0x600000 into fit-base and then
 * invokes bootm there.  Accept the older dual-entry wrapper as an input
 * convenience, but never persist its legacy uImage prefix: the chainloader
 * shim expects the FIT at the beginning of RAM.
 */
static int recovery_normalize_uboot_fit(const u8 *fit, size_t fit_size)
{
	u8 *dst = recv_base;

	if (!dst || fit_size > RECOVERY_UBOOT_SLOT_SIZE)
		return -EFBIG;

	if (fit != dst)
		memmove(dst, fit, fit_size);
	/* Keep the rest erased when the target is read as a full 1 MiB window. */
	memset(dst + fit_size, 0xff, RECOVERY_UBOOT_SLOT_SIZE - fit_size);

	return 0;
}

static bool recovery_ram_range_ok(ulong addr, size_t size)
{
	ulong ram_start = (ulong)gd->ram_base;
	ulong ram_size = (ulong)gd->ram_size;
	ulong ram_end;

	if (!ram_size || addr < ram_start || ram_size > ULONG_MAX - ram_start)
		return false;
	ram_end = ram_start + ram_size;

	return addr <= ram_end && size <= ram_end - addr;
}

static int recovery_copy_running_chainloader_fit(u8 **imagep, size_t *sizep)
{
	const ulong addr = RECOVERY_XG2010G_RUNNING_FIT_ADDR;
	const u8 *fit = (const u8 *)addr;
	const u8 *prepared_fit;
	size_t prepared_size;
	size_t fit_size;
	u8 *copy;
	int ret;

	if (!recovery_board_is_xg2010g()) {
		printf("Chainloader self-write is only available on XG2010G\n");
		return -ENODEV;
	}
	if (!recovery_ram_range_ok(addr, sizeof(struct fdt_header)) ||
	    fdt_check_header(fit)) {
		printf("No running XG2010G chainloader FIT at 0x%08lx\n", addr);
		return -ENOENT;
	}

	fit_size = fdt_totalsize(fit);
	if (fit_size < sizeof(struct fdt_header) ||
	    fit_size > RECOVERY_UBOOT_SLOT_SIZE ||
	    !recovery_ram_range_ok(addr, fit_size)) {
		printf("Running chainloader FIT size %lu is invalid\n",
		       (ulong)fit_size);
		return -EFBIG;
	}

	ret = recovery_prepare_uboot_fit(fit, fit_size, &prepared_fit,
					 &prepared_size);
	if (ret)
		return ret;
	if (prepared_fit != fit || prepared_size != fit_size)
		return -ENOEXEC;

	copy = malloc(RECOVERY_UBOOT_SLOT_SIZE);
	if (!copy)
		return -ENOMEM;
	memcpy(copy, fit, fit_size);
	memset(copy + fit_size, 0xff, RECOVERY_UBOOT_SLOT_SIZE - fit_size);

	ret = recovery_validate_fit(copy, fit_size);
	if (ret) {
		free(copy);
		return ret;
	}

	*imagep = copy;
	*sizep = fit_size;
	printf("Preserved running XG2010G chainloader FIT from 0x%08lx (%lu bytes)\n",
	       addr, (ulong)fit_size);
	return 0;
}

int recovery_validate_q1000k_fit(const void *fit, size_t size)
{
	const void *data;
	void *dtb;
	size_t len;
	uintptr_t offset;
	int images, node, hash, conf, kernel, fdt, ret;
	u8 type, arch, os, comp;

	if (fdt_check_full(fit, size))
		return -EINVAL;
	images = fdt_path_offset(fit, FIT_IMAGES_PATH);
	if (images < 0)
		return -EINVAL;
	fdt_for_each_subnode(node, fit, images) {
		/* Bound external data before any hash or DTB access. */
		if (fit_image_get_data(fit, node, &data, &len) || !len ||
		    (uintptr_t)data < (uintptr_t)fit)
			return -EINVAL;
		offset = (uintptr_t)data - (uintptr_t)fit;
		if (offset > size || len > size - offset)
			return -EFBIG;
		if (!recovery_fit_has_hashed_image(fit,
						 fit_get_name(fit, node, NULL)))
			return -EBADMSG;
		fdt_for_each_subnode(hash, fit, node) {
			if (fdt_getprop(fit, hash, "ignore", NULL))
				return -EBADMSG;
		}
	}

	conf = fit_conf_get_node(fit, NULL);
	if (conf < 0)
		return -EINVAL;
	kernel = fit_conf_get_prop_node(fit, conf, FIT_KERNEL_PROP, IH_PHASE_NONE);
	fdt = fit_conf_get_prop_node(fit, conf, FIT_FDT_PROP, IH_PHASE_NONE);
	if (kernel < 0 || fdt < 0 ||
	    fit_image_get_type(fit, kernel, &type) || type != IH_TYPE_KERNEL ||
	    fit_image_get_arch(fit, kernel, &arch) || arch != IH_ARCH_ARM64 ||
	    fit_image_get_os(fit, kernel, &os) || os != IH_OS_LINUX ||
	    fit_image_get_comp(fit, fdt, &comp) || comp != IH_COMP_NONE ||
	    fit_image_get_data(fit, fdt, &data, &len))
		return -EINVAL;
	/* FIT properties need only 4-byte alignment; libfdt requires 8. */
	dtb = malloc(len);
	if (!dtb)
		return -ENOMEM;
	memcpy(dtb, data, len);
	ret = fdt_check_full(dtb, len);
	if (!ret && fdt_node_check_compatible(dtb, 0, "quantum,q1000k") &&
	    fdt_node_check_compatible(dtb, 0, "centurylink,q1000k") &&
	    fdt_node_check_compatible(dtb, 0, "lumen,q1000k"))
		ret = -EINVAL;
	free(dtb);
	if (ret)
		return ret;

	return fit_all_image_verify(fit) ? 0 : -EBADMSG;
}

static int recovery_q1000k_fit_layout(const void *fit)
{
	const void *data;
	void *dtb;
	size_t size;
	int conf, node, depth = 0, count = 0, ret = -EINVAL;

	conf = fit_conf_get_node(fit, NULL);
	node = fit_conf_get_prop_node(fit, conf, FIT_FDT_PROP, IH_PHASE_NONE);
	if (node < 0 || fit_image_get_data(fit, node, &data, &size))
		return -EINVAL;
	dtb = malloc(size);
	if (!dtb)
		return -ENOMEM;
	memcpy(dtb, data, size);
	for (node = 0; node >= 0; node = fdt_next_node(dtb, node, &depth)) {
		const char *label;
		const fdt32_t *reg;
		int len;

		label = fdt_getprop(dtb, node, "label", &len);
		if (!label || len != 4 || memcmp(label, "ubi", 4))
			continue;
		reg = fdt_getprop(dtb, node, "reg", &len);
		if (fdt_address_cells(dtb, fdt_parent_offset(dtb, node)) != 1 ||
		    fdt_size_cells(dtb, fdt_parent_offset(dtb, node)) != 1 ||
		    !reg || len != 8 || fdt32_to_cpu(reg[0]) != Q1000K_UBI_OFFSET ||
		    fdt32_to_cpu(reg[1]) != Q1000K_UBI_SIZE)
			goto out;
		count++;
	}
	ret = count == 1 ? 0 : -EINVAL;
out:
	free(dtb);
	return ret;
}

static int recovery_validate_q1000k_chainloader(const void *fit, size_t size)
{
	const fdt32_t *version;
	int ret, conf, node, images, len, count = 0;
	ulong load, entry;
	u8 comp;

	if (size > Q1000K_CHAIN_SIZE || size < sizeof(struct fdt_header))
		return -EFBIG;
	ret = recovery_validate_q1000k_fit(fit, size);
	if (ret || fdt_totalsize(fit) != size)
		return ret ? ret : -EINVAL;
	version = fdt_getprop(fit, 0, "q1000k,installer-version", &len);
	if (!version || len != 4 || fdt32_to_cpu(*version) != 1)
		return -ENOEXEC;
	images = fdt_path_offset(fit, FIT_IMAGES_PATH);
	fdt_for_each_subnode(node, fit, images) {
		if (!fdt_getprop(fit, node, FIT_DATA_PROP, NULL))
			return -EINVAL;
		count++;
	}
	conf = fit_conf_get_node(fit, "conf-uboot");
	if (conf < 0 || conf != fit_conf_get_node(fit, NULL) || count != 2)
		return -EINVAL;
	node = fit_conf_get_prop_node(fit, conf, FIT_KERNEL_PROP, IH_PHASE_NONE);
	if (fit_image_get_load(fit, node, &load) || load != 0x81e00000 ||
	    fit_image_get_entry(fit, node, &entry) || entry != load ||
	    fit_image_get_comp(fit, node, &comp) || comp != IH_COMP_NONE)
		return -EINVAL;
	node = fit_conf_get_prop_node(fit, conf, FIT_FDT_PROP, IH_PHASE_NONE);
	if (fit_image_get_load(fit, node, &load) || load != 0x82000000)
		return -EINVAL;
	return recovery_q1000k_fit_layout(fit);
}

static int recovery_validate_firmware_image(const void *image, size_t size)
{
	const char *offset_env = env_get("recovery_firmware_fit_offset");
	const u8 *fit;
	ulong fit_offset;
	u32 fit_size;
	int ret;

	/* Q1000K validation cannot be disabled by changing an environment var. */
	if (recovery_board_is_q1000k())
		offset_env = "0";

	/* Legacy UBI recovery images predate an explicit FIT offset contract. */
	if (!offset_env)
		return 0;

	fit_offset = recovery_board_is_q1000k() ? 0 :
		     env_get_hex("recovery_firmware_fit_offset", ULONG_MAX);
	if (fit_offset == ULONG_MAX || fit_offset > size ||
	    size - fit_offset < sizeof(struct fdt_header)) {
		printf("Firmware FIT offset 0x%lx is outside the %lu-byte upload\n",
		       fit_offset, (unsigned long)size);
		return -EINVAL;
	}

	fit = (const u8 *)image + fit_offset;
	ret = fdt_check_header(fit);
	if (ret) {
		printf("Firmware has no valid FIT at offset 0x%lx: %d\n",
		       fit_offset, ret);
		return -ENOEXEC;
	}

	fit_size = fdt_totalsize(fit);
	if (fit_size < sizeof(struct fdt_header) || fit_size > size - fit_offset) {
		printf("Firmware FIT size 0x%x exceeds upload data after offset 0x%lx\n",
		       fit_size, fit_offset);
		return -EFBIG;
	}

	if (recovery_board_is_q1000k() && fdt_check_full(fit, size - fit_offset))
		return -EINVAL;

	ret = fit_check_format(fit, fit_size);
	if (ret) {
		printf("Firmware FIT format validation failed: %d\n", ret);
		return ret;
	}

	if (recovery_board_is_q1000k())
		return recovery_validate_q1000k_fit(fit, size - fit_offset);

	return 0;
}

static bool recovery_mtd_is_block_aligned(struct mtd_info *mtd, u64 value)
{
	return mtd->erasesize && !(value % mtd->erasesize);
}

static int recovery_mtd_validate_region(struct mtd_info *mtd, loff_t ofs,
					size_t len)
{
	if (!mtd || !mtd->erasesize)
		return -EINVAL;

	if (!len)
		return 0;

	if (ofs < 0 || ofs >= mtd->size ||
	    (u64)len > (u64)(mtd->size - ofs)) {
		printf("MTD region 0x%llx..+0x%lx is outside '%s' size 0x%llx\n",
		       (unsigned long long)ofs, (unsigned long)len,
		       mtd->name, (unsigned long long)mtd->size);
		return -EINVAL;
	}

	if (!recovery_mtd_is_block_aligned(mtd, ofs) ||
	    !recovery_mtd_is_block_aligned(mtd, len)) {
		printf("MTD region 0x%llx..+0x%lx is not eraseblock aligned (0x%x)\n",
		       (unsigned long long)ofs, (unsigned long)len,
		       mtd->erasesize);
		return -EINVAL;
	}

	return 0;
}

static void recovery_update_erase_progress(loff_t done)
{
	prog_erase_done = done > prog_erase_total ? prog_erase_total : done;
	prog_done = prog_erase_done + prog_write_done;
}

static int recovery_validate_q1000k_initramfs(const void *fit, size_t size)
{
	int ret, conf, kernel, ramdisk, images, node;
	ulong load, entry;
	u8 type, arch, os;

	ret = recovery_validate_firmware_image(fit, size);
	if (ret)
		return ret;
	if (fdt_getprop(fit, 0, "q1000k,installer-version", NULL))
		return -ENOEXEC;
	conf = fit_conf_get_node(fit, NULL);
	kernel = fit_conf_get_prop_node(fit, conf, FIT_KERNEL_PROP, IH_PHASE_NONE);
	/* The OpenWrt Linux entry differs from the chainloader's Linux wrapper. */
	if (fit_image_get_load(fit, kernel, &load) || load != 0x80200000 ||
	    fit_image_get_entry(fit, kernel, &entry) || entry != load ||
	    fdt_getprop(fit, conf, FIT_LOADABLE_PROP, NULL))
		return -ENOEXEC;
	images = fdt_path_offset(fit, FIT_IMAGES_PATH);
	fdt_for_each_subnode(node, fit, images) {
		if (fit_image_get_type(fit, node, &type) ||
		    (type != IH_TYPE_KERNEL && type != IH_TYPE_FLATDT &&
		     type != IH_TYPE_RAMDISK))
			return -ENOEXEC;
	}
	/* OpenWrt also builds kernel+DTB FITs with the initramfs inside Linux. */
	if (!fdt_getprop(fit, conf, FIT_RAMDISK_PROP, NULL))
		return 0;
	ramdisk = fit_conf_get_prop_node(fit, conf, FIT_RAMDISK_PROP, IH_PHASE_NONE);
	if (ramdisk < 0 || fit_image_get_type(fit, ramdisk, &type) ||
	    type != IH_TYPE_RAMDISK ||
	    fit_image_get_arch(fit, ramdisk, &arch) || arch != IH_ARCH_ARM64 ||
	    fit_image_get_os(fit, ramdisk, &os) || os != IH_OS_LINUX)
		return -ENOEXEC;
	return 0;
}

static int recovery_validate_q1000k_upload(const void *fit, size_t size)
{
	int ret, conf, node;
	u8 type;

	if (current_target == TARGET_UBOOT)
		return recovery_validate_q1000k_chainloader(fit, size);
	if (current_target == TARGET_INITRAMFS)
		return recovery_validate_q1000k_initramfs(fit, size);
	ret = recovery_validate_firmware_image(fit, size);
	if (ret)
		return ret;
	if (fdt_getprop(fit, 0, "q1000k,installer-version", NULL))
		return -ENOEXEC;
	ret = recovery_q1000k_fit_layout(fit);
	if (ret)
		return ret;
	conf = fit_conf_get_node(fit, NULL);
	node = fit_conf_get_prop_node(fit, conf,
		current_target == TARGET_RECOVERY ? FIT_RAMDISK_PROP : FIT_LOADABLE_PROP,
		IH_PHASE_NONE);
	if (node < 0 || fit_image_get_type(fit, node, &type) ||
	    type != (current_target == TARGET_RECOVERY ? IH_TYPE_RAMDISK : IH_TYPE_FILESYSTEM))
		return -ENOEXEC;
	return 0;
}

static int recovery_mtd_mark_bad(struct mtd_info *mtd, loff_t addr)
{
	int ret;

	ret = mtd_block_markbad(mtd, addr);
	if (ret)
		printf("Warning: failed to mark bad block at 0x%llx on '%s': %d\n",
		       (unsigned long long)addr, mtd->name, ret);
	else
		printf("Marked bad block at 0x%llx on '%s'\n",
		       (unsigned long long)addr, mtd->name);

	return ret;
}

static int recovery_mtd_verify_write(struct mtd_info *mtd, loff_t addr,
				     const u8 *src, size_t len, u8 *buf)
{
	size_t retlen = 0;
	int ret;

	ret = mtd_read(mtd, addr, len, &retlen, buf);
	if ((ret && ret != -EUCLEAN) || retlen != len) {
		printf("mtd_read verify failed: ret=%d retlen=%lu at 0x%llx\n",
		       ret, (unsigned long)retlen, (unsigned long long)addr);
		return ret ? ret : -EIO;
	}

	if (memcmp(buf, src, len)) {
		printf("mtd_read verify mismatch at 0x%llx\n",
		       (unsigned long long)addr);
		return -EIO;
	}

	return 0;
}

static void recovery_ubi_progress(struct ubi_volume *vol, int done, int total)
{
	unsigned long long bytes_done = prog_erase_volume_base;

	(void)vol;

	if (total > 0)
		bytes_done += (prog_erase_volume_bytes * (unsigned long long)done) /
			      (unsigned long long)total;

	if (bytes_done > prog_erase_total)
		bytes_done = prog_erase_total;

	prog_erase_done = bytes_done;
	prog_done = prog_erase_done + prog_write_done;

	if (prog_status_leds)
		recovery_service_runtime(prog_status_leds);
}

static int recovery_erase_mtd_region(struct mtd_info *mtd, loff_t ofs,
				     size_t len,
				     struct recovery_status_led_ctrl *status_leds)
{
	struct erase_info ei = { 0 };
	int ret;

	if (!mtd || !len)
		return 0;

	ret = recovery_mtd_validate_region(mtd, ofs, len);
	if (ret)
		return ret;

	ei.addr = ofs;
	ei.len = len;

	ret = mtd_unlock(mtd, ei.addr, ei.len);
	if (ret && ret != -EOPNOTSUPP) {
		printf("Warning: initial mtd_unlock 0x%llx..+0x%llx failed: %d\n",
		       (unsigned long long)ei.addr,
		       (unsigned long long)ei.len, ret);
	}

	for (loff_t addr = 0; addr < len; addr += mtd->erasesize) {
		struct erase_info e = {
			.addr = ofs + addr,
			.len = mtd->erasesize,
		};
		int tries = 0;

		ret = mtd_block_isbad(mtd, e.addr);
		if (ret < 0) {
			printf("Failed to query bad block at 0x%llx: %d\n",
			       (unsigned long long)e.addr, ret);
			return ret;
		}
		if (ret > 0) {
			printf("Skipping bad block at 0x%llx\n",
			       (unsigned long long)e.addr);
			recovery_update_erase_progress(addr + mtd->erasesize);
			recovery_service_runtime(status_leds);
			continue;
		}

		do {
			ret = mtd_erase(mtd, &e);
			if (!ret)
				break;
			if (ret == -EROFS || ret == -EACCES)
				mtd_unlock(mtd, e.addr, e.len);
			else
				break;
		} while (++tries < 2);

		if (ret) {
			printf("mtd_erase failed at 0x%llx: %d\n",
			       (unsigned long long)e.addr, ret);
			if (recovery_mtd_mark_bad(mtd, e.addr))
				return ret;
		}

		recovery_update_erase_progress(addr + mtd->erasesize);
		recovery_service_runtime(status_leds);
	}

	return 0;
}

static int recovery_write_mtd_region(struct mtd_info *mtd, loff_t ofs,
				     size_t region_len, const void *image,
				     size_t image_size, bool verify,
				     struct recovery_status_led_ctrl *status_leds)
{
	const u8 *src = image;
	u8 *verify_buf = NULL;
	u32 written = 0;
	int ret;

	if (!image_size)
		return 0;

	ret = recovery_mtd_validate_region(mtd, ofs, region_len);
	if (ret)
		return ret;

	if (verify) {
		verify_buf = malloc_cache_aligned(mtd->erasesize);
		if (!verify_buf)
			return -ENOMEM;
	}

	for (loff_t addr = ofs; addr < ofs + region_len && written < image_size;
	     addr += mtd->erasesize) {
		size_t chunk = min((size_t)mtd->erasesize,
				   image_size - (size_t)written);
		size_t retlen = 0;

		ret = mtd_block_isbad(mtd, addr);
		if (ret < 0) {
			printf("Failed to query bad block at 0x%llx: %d\n",
			       (unsigned long long)addr, ret);
			goto out;
		}
		if (ret > 0) {
			printf("Skipping bad block at 0x%llx\n",
			       (unsigned long long)addr);
			recovery_service_runtime(status_leds);
			continue;
		}

		ret = mtd_write(mtd, addr, chunk, &retlen, src + written);
		if (ret || retlen != chunk) {
			printf("mtd_write failed: ret=%d retlen=%lu at 0x%llx\n",
			       ret, (unsigned long)retlen,
			       (unsigned long long)addr);
			if (recovery_mtd_mark_bad(mtd, addr)) {
				if (!ret)
					ret = -EIO;
				goto out;
			}
			recovery_service_runtime(status_leds);
			continue;
		}

		if (verify_buf) {
			ret = recovery_mtd_verify_write(mtd, addr,
							src + written, chunk,
							verify_buf);
			if (ret) {
				if (recovery_mtd_mark_bad(mtd, addr))
					goto out;
				recovery_service_runtime(status_leds);
				continue;
			}
		}

		written += chunk;
		prog_write_done = written;
		prog_done = prog_erase_done + prog_write_done;
		recovery_service_runtime(status_leds);
	}

	if (written < image_size) {
		printf("Not enough good eraseblocks in MTD target: wrote %u/%lu bytes\n",
		       written, (unsigned long)image_size);
		ret = -ENOSPC;
		goto out;
	}

	ret = 0;

out:
	free(verify_buf);
	return ret;
}

static int recovery_install_running_chainloader(
	struct recovery_status_led_ctrl *status_leds)
{
	struct recovery_target target;
	u8 *image = NULL;
	size_t image_size = 0;
	int ret;

	memset(&target, 0, sizeof(target));
	ret = recovery_copy_running_chainloader_fit(&image, &image_size);
	if (ret)
		goto out;

	ret = recovery_resolve_target(TARGET_UBOOT, &target);
	if (ret) {
		printf("Cannot resolve the XG2010G chainloader target: %d\n", ret);
		goto out;
	}
	ret = recovery_validate_xg2010g_uboot_target(&target);
	if (ret)
		goto out;

	prog_phase = 1;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = RECOVERY_UBOOT_SLOT_SIZE;
	prog_write_done = 0;
	prog_write_total = image_size;
	prog_total = prog_erase_total + prog_write_total;

	printf("Installing running chainloader into spi-nand0 0x%lx..0x%lx\n",
	       RECOVERY_UBOOT_SLOT_DEFAULT_OFS,
	       RECOVERY_UBOOT_SLOT_DEFAULT_OFS + RECOVERY_UBOOT_SLOT_SIZE);
	ret = recovery_erase_mtd_region(target.mtd, target.ofs,
					RECOVERY_UBOOT_SLOT_SIZE, status_leds);
	if (ret)
		goto out;

	prog_phase = 2;
	ret = recovery_write_mtd_region(target.mtd, target.ofs,
					RECOVERY_UBOOT_SLOT_SIZE, image,
					image_size, true, status_leds);
	if (ret)
		goto out;

	prog_phase = 3;
	printf("Running chainloader installed and read-back verified (%lu bytes)\n",
	       (ulong)image_size);

out:
	if (ret)
		prog_phase = -1;
	recovery_release_target(&target);
	free(image);
	return ret;
}

int xg2010g_install_running_chainloader(void)
{
	struct recovery_status_led_ctrl status_leds = { 0 };

	return recovery_install_running_chainloader(&status_leds);
}

static bool recovery_preserve_ubi_volume(const char *name)
{
	if (!name || !*name)
		return true;

	if (!strcmp(name, "ubootenv") || !strcmp(name, "ubootenv2"))
		return true;

	if (of_machine_is_compatible("econet,xr1710g") ||
	    of_machine_is_compatible("econet,xr1710g-ubi") ||
	    of_machine_is_compatible("gemtek,xr1710g") ||
	    of_machine_is_compatible("gemtek,xr1710g-ubi"))
		return !strcmp(name, "factory");

	if (of_machine_is_compatible("gemtek,w1700k") ||
	    of_machine_is_compatible("gemtek,w1700k-ubi"))
		return !strcmp(name, "factory");

	return false;
}

static unsigned long long
recovery_calc_rebuild_ubi_limit(struct recovery_target *target)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_device *ubi;
	unsigned long long limit;
	int i;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return target->limit;

	if (target->ubi_needs_format)
		return target->limit;

	if (recovery_select_ubi(target->ubi_part))
		return target->limit;

	ubi = ubi_get_device(0);
	if (!ubi)
		return target->limit;

	limit = (unsigned long long)ubi->avail_pebs *
		(unsigned long long)ubi->leb_size;

	for (i = 0; i < ubi->vtbl_slots; i++) {
		struct ubi_volume *vol = ubi->volumes[i];

		if (!vol || vol->vol_id >= UBI_INTERNAL_VOL_START)
			continue;
		if (recovery_preserve_ubi_volume(vol->name))
			continue;

		limit += (unsigned long long)vol->reserved_pebs *
			 (unsigned long long)vol->usable_leb_size;
	}

	ubi_put_device(ubi);
	return limit;
#else
	return target->limit;
#endif
}

static int recovery_create_ubi_volume(const char *name, size_t size, int vol_type)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_mkvol_req req;
	struct ubi_device *ubi;
	int ret;

	if (!name || !*name)
		return -EINVAL;

	ubi = ubi_get_device(0);
	if (!ubi)
		return -ENODEV;

	if (!size)
		size = (size_t)ubi->avail_pebs * (size_t)ubi->leb_size;

	memset(&req, 0, sizeof(req));
	req.vol_id = UBI_VOL_NUM_AUTO;
	req.alignment = 1;
	req.bytes = size;
	req.vol_type = vol_type;
	req.name_len = strlen(name);
	if (req.name_len > UBI_VOL_NAME_MAX) {
		ubi_put_device(ubi);
		return -ENAMETOOLONG;
	}
	memcpy(req.name, name, req.name_len);
	req.name[req.name_len] = '\0';

	mutex_lock(&ubi->device_mutex);
	ret = ubi_create_volume(ubi, &req);
	mutex_unlock(&ubi->device_mutex);
	ubi_put_device(ubi);

	return ret;
#else
	return -ENODEV;
#endif
}

static int recovery_remove_ubi_volume(const char *name)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	int ret;

	desc = ubi_open_volume_nm(0, name, UBI_EXCLUSIVE);
	if (IS_ERR_OR_NULL(desc))
		return IS_ERR(desc) ? PTR_ERR(desc) : -ENODEV;

	mutex_lock(&desc->vol->ubi->device_mutex);
	ret = ubi_remove_volume(desc, 0);
	mutex_unlock(&desc->vol->ubi->device_mutex);
	ubi_close_volume(desc);

	return ret;
#else
	return -ENODEV;
#endif
}

static size_t recovery_rootfs_data_size(void)
{
	const char *value = env_get("rootfs_data_max");
	unsigned long long parsed;
	char *end;

	if (!value || !*value)
		return 0;

	parsed = simple_strtoull(value, &end, 0);
	if (*end || !parsed || parsed > (size_t)-1) {
		printf("Ignoring invalid rootfs_data_max '%s'\n", value);
		return 0;
	}

	return parsed;
}

static int recovery_ensure_rootfs_data(struct recovery_target *target)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	size_t size;
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return 0;

	if (recovery_select_ubi(target->ubi_part))
		return -ENODEV;

	desc = ubi_open_volume_nm(0, "rootfs_data", UBI_READWRITE);
	if (!IS_ERR_OR_NULL(desc)) {
		ubi_close_volume(desc);
		return 0;
	}

	size = recovery_rootfs_data_size();
	ret = recovery_create_ubi_volume("rootfs_data", size,
					 UBI_DYNAMIC_VOLUME);
	if (ret == -ENOSPC && size) {
		printf("rootfs_data_max does not fit; using all available PEBs\n");
		ret = recovery_create_ubi_volume("rootfs_data", 0,
						 UBI_DYNAMIC_VOLUME);
	}
	if (ret && ret != -EEXIST) {
		printf("Failed to create UBI volume 'rootfs_data': %d\n", ret);
		return ret;
	}

	return 0;
#else
	return -ENODEV;
#endif
}

static int recovery_prepare_ubi_target(struct recovery_target *target,
				       struct recovery_status_led_ctrl *status_leds,
				       size_t image_size, bool *reformatted)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_device *ubi;
	loff_t erase_len;
	int ret;

	if (reformatted)
		*reformatted = false;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (!target->ubi_needs_format)
		return recovery_select_ubi(target->ubi_part);

	if (!target->mtd)
		return -ENODEV;

	erase_len = ALIGN(target->mtd->size, target->mtd->erasesize);
	prog_phase = 1;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = erase_len;
	prog_write_done = 0;
	prog_write_total = image_size;
	prog_total = prog_erase_total + prog_write_total;

	printf("UBI partition '%s' is invalid, erasing it to recreate layout...\n",
	       target->ubi_part);
	ret = recovery_erase_mtd_region(target->mtd, 0, target->mtd->size,
					status_leds);
	if (ret)
		return ret;

	recovery_ubi_attach_error = 0;
	ret = recovery_select_ubi(target->ubi_part);
	if (ret)
		return ret;

	ubi = ubi_get_device(0);
	if (ubi) {
		target->limit = (unsigned long long)ubi->avail_pebs *
				(unsigned long long)ubi->leb_size;
		ubi_put_device(ubi);
	}

	target->cur_size = 0;
	target->ubi_needs_format = false;
	if (reformatted)
		*reformatted = true;

	return 0;
#else
	return -ENODEV;
#endif
}

static int recovery_ensure_ubi_volume_named(const char *name, size_t size,
					    int vol_type)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	int ret;

	if (!name || !*name)
		return -EINVAL;

	desc = ubi_open_volume_nm(0, name, UBI_READWRITE);
	if (!IS_ERR_OR_NULL(desc)) {
		ubi_close_volume(desc);
		return 0;
	}

	ret = recovery_create_ubi_volume(name, size, vol_type);
	if (ret && ret != -EEXIST) {
		printf("Failed to create UBI volume '%s': %d\n", name, ret);
		return ret;
	}

	return 0;
#else
	return -ENODEV;
#endif
}

static int recovery_ensure_preserved_ubi_volumes(struct recovery_target *target)
{
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return 0;

	if (recovery_select_ubi(target->ubi_part))
		return -ENODEV;

	ret = recovery_ensure_ubi_volume_named("ubootenv",
					       RECOVERY_UBOOTENV_SIZE,
					       UBI_DYNAMIC_VOLUME);
	if (ret)
		return ret;

	ret = recovery_ensure_ubi_volume_named("ubootenv2",
					       RECOVERY_UBOOTENV_SIZE,
					       UBI_DYNAMIC_VOLUME);
	if (ret)
		return ret;

	if (of_machine_is_compatible("econet,xr1710g") ||
	    of_machine_is_compatible("econet,xr1710g-ubi") ||
	    of_machine_is_compatible("gemtek,xr1710g") ||
	    of_machine_is_compatible("gemtek,xr1710g-ubi") ||
	    of_machine_is_compatible("gemtek,w1700k") ||
	    of_machine_is_compatible("gemtek,w1700k-ubi")) {
		ret = recovery_ensure_ubi_volume_named("factory",
						       RECOVERY_FACTORY_SIZE,
						       UBI_STATIC_VOLUME);
		if (ret)
			return ret;
	}

	return 0;
}

static int recovery_cleanup_ubi_firmware(struct recovery_target *target,
					 struct recovery_status_led_ctrl *status_leds,
					 size_t image_size)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_device *ubi;
	unsigned long long erase_total = 0;
	unsigned long long erase_done = 0;
	int ret = 0;
	int i;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (recovery_select_ubi(target->ubi_part))
		return -ENODEV;

	ubi = ubi_get_device(0);
	if (!ubi)
		return -ENODEV;

	for (i = 0; i < ubi->vtbl_slots; i++) {
		struct ubi_volume *vol = ubi->volumes[i];

		if (!vol || vol->vol_id >= UBI_INTERNAL_VOL_START)
			continue;
		if (recovery_preserve_ubi_volume(vol->name))
			continue;

		erase_total += (unsigned long long)vol->reserved_pebs *
			       (unsigned long long)vol->usable_leb_size;
	}

	prog_phase = 1;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = erase_total > UINT_MAX ? UINT_MAX : erase_total;
	prog_write_done = 0;
	prog_write_total = image_size;
	prog_total = prog_erase_total + prog_write_total;
	prog_status_leds = status_leds;
	ubi_set_progress_callback(recovery_ubi_progress);

	for (i = 0; i < ubi->vtbl_slots; i++) {
		struct ubi_volume *vol = ubi->volumes[i];
		unsigned long long vol_size;
		char name[UBI_VOL_NAME_MAX + 1];

		if (!vol || vol->vol_id >= UBI_INTERNAL_VOL_START)
			continue;
		if (recovery_preserve_ubi_volume(vol->name))
			continue;

		vol_size = (unsigned long long)vol->reserved_pebs *
			   (unsigned long long)vol->usable_leb_size;
		strlcpy(name, vol->name, sizeof(name));
		prog_erase_volume_base = erase_done;
		prog_erase_volume_bytes = vol_size;

		printf("Removing UBI volume '%s' before flashing '%s'...\n",
		       name, target->name);
		ret = recovery_remove_ubi_volume(name);
		if (ret) {
			printf("Failed to remove UBI volume '%s': %d\n", name, ret);
			break;
		}

		erase_done += vol_size;
		prog_erase_done = erase_done > prog_erase_total ?
				  prog_erase_total : erase_done;
		prog_done = prog_erase_done + prog_write_done;
		recovery_service_runtime(status_leds);
	}

	ubi_set_progress_callback(NULL);
	prog_status_leds = NULL;
	ubi_put_device(ubi);
	if (ret)
		return ret;

	return recovery_try_ubi_target(current_target, target);
#else
	return -ENODEV;
#endif
}

static void recovery_abort_ubi_update(struct ubi_volume *vol)
{
	if (!vol || !vol->updating)
		return;

	if (vol->upd_buf) {
		vfree(vol->upd_buf);
		vol->upd_buf = NULL;
	}
	vol->updating = 0;
	vol->upd_bytes = 0;
	vol->upd_received = 0;
	vol->upd_ebs = 0;
}

static int recovery_write_ubi_target(struct recovery_target *target,
				      struct recovery_status_led_ctrl *status_leds,
				      const void *image, size_t image_size)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	struct ubi_volume *vol;
	struct ubi_device *ubi;
	const u8 *src = image;
	size_t reserved_bytes;
	u32 written = 0;
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (recovery_select_ubi(target->ubi_part))
		return -ENODEV;

	desc = ubi_open_volume_nm(0, target->name, UBI_READWRITE);
	if (IS_ERR_OR_NULL(desc))
		return IS_ERR(desc) ? PTR_ERR(desc) : -ENODEV;

	vol = desc->vol;
	ubi = vol->ubi;
	reserved_bytes = (size_t)vol->reserved_pebs *
			 (size_t)(ubi->leb_size - vol->data_pad);
	if (image_size > reserved_bytes) {
		ret = -EFBIG;
		goto out_close;
	}

	ret = ubi_start_update(ubi, vol, image_size);
	if (ret)
		goto out_close;

	/* Flush one runtime cycle so the browser can observe phase 2 before
	 * the actual UBI data write loop starts.
	 */
	recovery_service_runtime(status_leds);

	while (written < image_size) {
		u32 chunk = image_size - written;

		if (chunk > RECOVERY_UBI_WRITE_CHUNK)
			chunk = RECOVERY_UBI_WRITE_CHUNK;

		ret = ubi_more_update_data(ubi, vol, src + written, chunk);
		if (ret < 0)
			goto out_close;

		written += chunk;
		prog_write_done = written > prog_write_total ?
				  prog_write_total : written;
		prog_done = prog_erase_done + prog_write_done;
		recovery_service_runtime(status_leds);
	}

	ret = ubi_check_volume(ubi, vol->vol_id);
	if (ret < 0) {
		ret = -ret;
		goto out_close;
	}

	if (ret) {
		ubi_warn(ubi, "volume %d on UBI device %d is corrupt",
			 vol->vol_id, ubi->ubi_num);
		vol->corrupted = 1;
	}

	vol->checked = 1;
	ubi_gluebi_updated(vol);
	ret = 0;

out_close:
	if (ret)
		recovery_abort_ubi_update(vol);
	ubi_close_volume(desc);
	return ret;
#else
	return -ENODEV;
#endif
}

static int recovery_resize_ubi_target(struct recovery_target *target,
				      size_t new_size)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	struct ubi_volume *vol;
	int needed_pebs;
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (recovery_select_ubi(target->ubi_part))
		return -ENODEV;

	desc = ubi_open_volume_nm(0, target->name, UBI_EXCLUSIVE);
	if (IS_ERR_OR_NULL(desc))
		return IS_ERR(desc) ? PTR_ERR(desc) : -ENODEV;

	vol = desc->vol;
	needed_pebs = DIV_ROUND_UP(new_size, vol->usable_leb_size);

	mutex_lock(&vol->ubi->device_mutex);
	ret = ubi_resize_volume(desc, needed_pebs);
	mutex_unlock(&vol->ubi->device_mutex);

	if (!ret) {
		target->cur_size = (unsigned long long)needed_pebs *
				   (unsigned long long)vol->usable_leb_size;
		target->limit = (unsigned long long)(needed_pebs +
						     vol->ubi->avail_pebs) *
			(unsigned long long)vol->usable_leb_size;
	}

	ubi_close_volume(desc);
	return ret;
#else
	return -ENODEV;
#endif
}

static int recovery_create_ubi_target(struct recovery_target *target,
				      size_t new_size)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_mkvol_req req;
	struct ubi_device *ubi;
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (recovery_select_ubi(target->ubi_part))
		return -ENODEV;

	ubi = ubi_get_device(0);
	if (!ubi)
		return -ENODEV;

	memset(&req, 0, sizeof(req));
	req.vol_id = UBI_VOL_NUM_AUTO;
	req.alignment = 1;
	req.bytes = new_size;
	req.vol_type = UBI_STATIC_VOLUME;
	req.name_len = strlen(target->name);
	if (req.name_len > UBI_VOL_NAME_MAX) {
		ubi_put_device(ubi);
		return -ENAMETOOLONG;
	}
	memcpy(req.name, target->name, req.name_len);
	req.name[req.name_len] = '\0';

	mutex_lock(&ubi->device_mutex);
	ret = ubi_create_volume(ubi, &req);
	mutex_unlock(&ubi->device_mutex);
	ubi_put_device(ubi);
	if (ret)
		return ret;

	return recovery_try_ubi_target(current_target, target);
#else
	return -ENODEV;
#endif
}

/* Determine maximum payload size based on selected target and DTS-defined MTD
 * layout. Returns 0 on error. */
static unsigned long recovery_calc_target_max(enum upload_target tgt,
					      loff_t *p_ofs)
{
	struct recovery_target target;
	unsigned long limit = 0;
	unsigned long long effective_limit;

	if (recovery_resolve_target(tgt, &target))
		return 0;

	if (p_ofs)
		*p_ofs = target.ofs;

	effective_limit = target.limit;
	if (tgt == TARGET_FIRMWARE)
		effective_limit = recovery_calc_rebuild_ubi_limit(&target);

	limit = (effective_limit > ULONG_MAX) ? ULONG_MAX :
		(unsigned long)effective_limit;
	recovery_release_target(&target);

	return limit;
}

/* Only dynamic endpoints here; static files come from fsdata */

static const char recovery_page_ok[] =
	"HTTP/1.0 200 OK\r\n"
	"Content-Type: text/plain\r\n"
	"Cache-Control: no-store\r\n"
	"Content-Length: 2\r\n"
	"Connection: close\r\n"
	"\r\n"
	"OK";

static int recovery_open_custom_response(struct fs_file *file,
					 const char *content_type,
					 const char *body, int body_len)
{
	char header[160];
	char *page;
	int header_len;
	int total_len;

	if (body_len < 0)
		return 0;

	header_len = snprintf(header, sizeof(header),
			      "HTTP/1.0 200 OK\r\n"
			      "Content-Type: %s\r\n"
			      "Cache-Control: no-store\r\n"
			      "Content-Length: %d\r\n"
			      "Connection: close\r\n"
			      "\r\n",
			      content_type, body_len);
	if (header_len < 0 || header_len >= (int)sizeof(header))
		return 0;

	total_len = header_len + body_len;
	page = malloc(total_len + 1);
	if (!page)
		return 0;

	memcpy(page, header, header_len);
	memcpy(page + header_len, body, body_len);
	page[total_len] = '\0';

	file->data = page;
	file->len = total_len;
	file->index = file->len;
	file->pextension = NULL;
	file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
	return 1;
}

static int recovery_open_http_error(struct fs_file *file, const char *status,
				    const char *message)
{
	char header[192];
	char *page;
	int header_len;
	int message_len;

	message_len = strlen(message);
	header_len = snprintf(header, sizeof(header),
			      "HTTP/1.0 %s\r\n"
			      "Content-Type: text/plain\r\n"
			      "Cache-Control: no-store\r\n"
			      "Content-Length: %d\r\n"
			      "Connection: close\r\n\r\n",
			      status, message_len);
	if (header_len < 0 || header_len >= sizeof(header))
		return 0;

	page = malloc(header_len + message_len + 1);
	if (!page)
		return 0;

	memcpy(page, header, header_len);
	memcpy(page + header_len, message, message_len);
	page[header_len + message_len] = '\0';
	file->data = page;
	file->len = header_len + message_len;
	file->index = file->len;
	file->pextension = NULL;
	file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
	return 1;
}

static bool recovery_uses_raw_firmware_slot(void)
{
	const char *raw = env_get("recovery_dev");

	return raw && *raw;
}

static const char *recovery_board_name(void)
{
	if (of_machine_is_compatible("axon,xg2010g") ||
	    of_machine_is_compatible("econet,xg2010g"))
		return "XG2010G";

	if (of_machine_is_compatible("quantum,q1000k") ||
	    of_machine_is_compatible("centurylink,q1000k") ||
	    of_machine_is_compatible("lumen,q1000k"))
		return "Q1000K";

	if (of_machine_is_compatible("econet,xr1710g") ||
	    of_machine_is_compatible("econet,xr1710g-ubi") ||
	    of_machine_is_compatible("gemtek,xr1710g") ||
	    of_machine_is_compatible("gemtek,xr1710g-ubi"))
		return "XR1710G";

	return "AN7581";
}

static int recovery_open_about_response(struct fs_file *file)
{
	char json[576];
	bool raw_slot = recovery_uses_raw_firmware_slot();
	bool nand_backup = recovery_board_is_q1000k();
	bool ramboot = nand_backup && IS_ENABLED(CONFIG_Q1000K_INSTALLER);
	const char *layout = raw_slot ? "raw-slot" :
			     xr1710g_detect_ubi_version();
	int json_len;

#ifdef U_BOOT_DATE
	json_len = snprintf(json, sizeof(json),
			    "{\"u_boot\":\"%s (%s - %s %s)\","
			    "\"board\":\"%s\",\"firmware_mode\":\"%s\","
			    "\"detected_layout\":\"%s\","
			    "\"chainloader_update\":%s,"
			    "\"ramboot\":%s,"
			    "\"ramboot_addr\":\"0x%lx\",\"ramboot_min_addr\":\"0x%lx\","
			    "\"ramboot_end\":\"0x%lx\","
			    "\"nand_backup\":%s,\"nand_backup_bytes\":%llu}\n",
			    U_BOOT_VERSION, U_BOOT_DATE, U_BOOT_TIME, U_BOOT_TZ,
			    recovery_board_name(), raw_slot ? "raw" : "ubi",
			    layout, !recovery_uboot_update_disabled() ? "true" : "false",
			    ramboot ? "true" : "false",
			    (ulong)gd->ram_base + RECOVERY_Q1000K_RAMBOOT_OFFSET,
			    (ulong)gd->ram_base + RECOVERY_Q1000K_RAMBOOT_MIN_OFFSET,
			    recovery_q1000k_upload_top(),
			    nand_backup ? "true" : "false",
			    nand_backup ? RECOVERY_Q1000K_NAND_BYTES : 0ULL);
#else
	json_len = snprintf(json, sizeof(json),
			    "{\"u_boot\":\"%s\",\"board\":\"%s\","
			    "\"firmware_mode\":\"%s\","
			    "\"detected_layout\":\"%s\","
			    "\"chainloader_update\":%s,"
			    "\"ramboot\":%s,"
			    "\"ramboot_addr\":\"0x%lx\",\"ramboot_min_addr\":\"0x%lx\","
			    "\"ramboot_end\":\"0x%lx\","
			    "\"nand_backup\":%s,\"nand_backup_bytes\":%llu}\n",
			    U_BOOT_VERSION, recovery_board_name(),
			    raw_slot ? "raw" : "ubi", layout,
			    !recovery_uboot_update_disabled() ? "true" : "false",
			    ramboot ? "true" : "false",
			    (ulong)gd->ram_base + RECOVERY_Q1000K_RAMBOOT_OFFSET,
			    (ulong)gd->ram_base + RECOVERY_Q1000K_RAMBOOT_MIN_OFFSET,
			    recovery_q1000k_upload_top(),
			    nand_backup ? "true" : "false",
			    nand_backup ? RECOVERY_Q1000K_NAND_BYTES : 0ULL);
#endif
	if (json_len < 0)
		return 0;
	if (json_len >= (int)sizeof(json))
		json_len = (int)sizeof(json) - 1;

	return recovery_open_custom_response(file, "application/json", json,
					     json_len);
}

static int recovery_nand_backup_fill_cache(
	struct recovery_nand_backup_file *backup)
{
	size_t read_len;
	size_t retlen = 0;
	int ret;

	if (!backup->bytes_left)
		return 0;

	read_len = min_t(unsigned long long, backup->bytes_left,
				 backup->cache_capacity);
	ret = mtd_read(backup->mtd, backup->next_offset, read_len, &retlen,
		       backup->cache);
	if ((ret && ret != -EUCLEAN) || retlen != read_len) {
		printf("httpd: NAND backup read failed on '%s' at 0x%llx: ret=%d retlen=%lu\n",
		       backup->mtd->name,
		       (unsigned long long)backup->next_offset, ret,
		       (unsigned long)retlen);
		backup->bytes_left = 0;
		backup->cache_len = 0;
		backup->cache_off = 0;
		return ret ? ret : -EIO;
	}

	backup->next_offset += read_len;
	backup->bytes_left -= read_len;
	backup->cache_len = read_len;
	backup->cache_off = 0;
	WATCHDOG_RESET();
	return 1;
}

static int recovery_read_nand_backup(struct recovery_nand_backup_file *backup,
				     char *buffer, int count)
{
	int copied = 0;

	while (copied < count) {
		size_t available;
		size_t todo;
		int ret;

		if (backup->http_header_off < backup->http_header_len) {
			available = backup->http_header_len - backup->http_header_off;
			todo = min_t(size_t, available, count - copied);
			memcpy(buffer + copied,
			       backup->http_header + backup->http_header_off, todo);
			backup->http_header_off += todo;
			copied += todo;
			continue;
		}

		if (backup->cache_off >= backup->cache_len) {
			ret = recovery_nand_backup_fill_cache(backup);
			if (ret <= 0)
				break;
		}

		available = backup->cache_len - backup->cache_off;
		todo = min_t(size_t, available, count - copied);
		memcpy(buffer + copied, backup->cache + backup->cache_off, todo);
		backup->cache_off += todo;
		copied += todo;
	}

	return copied ? copied : FS_READ_EOF;
}

static int recovery_open_q1000k_nand_backup(struct fs_file *file)
{
	struct recovery_nand_backup_file *backup;
	struct mtd_info *mtd;
	int header_len;

	if (!recovery_board_is_q1000k())
		return recovery_open_http_error(file, "404 Not Found",
					"NAND backup is unavailable on this board.\n");
	if (recovery_nand_backup_active)
		return recovery_open_http_error(file, "409 Conflict",
					"A NAND backup stream is already active.\n");
	if (post_ok || flash_request || ramboot_request || reboot_request ||
	    (prog_phase > 0 && prog_phase < 3))
		return recovery_open_http_error(file, "409 Conflict",
					"A recovery write operation is active.\n");

	mtd_probe_devices();
	mtd = get_mtd_device_nm(RECOVERY_UBOOT_SLOT_DEFAULT_DEV);
	if (IS_ERR_OR_NULL(mtd))
		return recovery_open_http_error(file, "503 Service Unavailable",
					"The Q1000K NAND device is unavailable.\n");
	if (mtd_is_partition(mtd) || mtd->size != RECOVERY_Q1000K_NAND_BYTES) {
		put_mtd_device(mtd);
		return recovery_open_http_error(file, "503 Service Unavailable",
					"The Q1000K full NAND device is unavailable.\n");
	}

	backup = calloc(1, sizeof(*backup));
	if (!backup) {
		put_mtd_device(mtd);
		return recovery_open_http_error(file, "503 Service Unavailable",
					"Cannot allocate NAND backup state.\n");
	}

	backup->cache = memalign(ARCH_DMA_MINALIGN, RECOVERY_NAND_BACKUP_CHUNK);
	if (!backup->cache) {
		put_mtd_device(mtd);
		free(backup);
		return recovery_open_http_error(file, "503 Service Unavailable",
					"Cannot allocate NAND backup buffer.\n");
	}

	header_len = snprintf(backup->http_header, sizeof(backup->http_header),
			      "HTTP/1.0 200 OK\r\n"
			      "Content-Type: application/octet-stream\r\n"
			      "Content-Disposition: attachment; "
			      "filename=\"q1000k-nand-backup.bin\"\r\n"
			      "Cache-Control: no-store\r\n"
			      "Content-Length: %llu\r\n"
			      "Connection: close\r\n\r\n",
			      RECOVERY_Q1000K_NAND_BYTES);
	if (header_len < 0 || header_len >= sizeof(backup->http_header)) {
		put_mtd_device(mtd);
		free(backup->cache);
		free(backup);
		return 0;
	}

	backup->magic = RECOVERY_NAND_BACKUP_MAGIC;
	backup->mtd = mtd;
	backup->cache_capacity = RECOVERY_NAND_BACKUP_CHUNK;
	backup->next_offset = 0;
	backup->bytes_left = RECOVERY_Q1000K_NAND_BYTES;
	backup->http_header_len = header_len;
	recovery_nand_backup_active++;

	file->data = NULL;
	file->len = INT_MAX;
	file->index = 0;
	file->pextension = (fs_file_extension *)backup;
	file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
	printf("httpd: streaming complete Q1000K NAND backup (%llu bytes)\n",
	       RECOVERY_Q1000K_NAND_BYTES);
	return 1;
}

/* lwIP httpd custom file hooks: serve only dynamic endpoints; static files via fsdata */
int fs_open_custom(struct fs_file *file, const char *name)
{
	const char *p;

	if (!file || !name)
		return 0;
	memset(file, 0, sizeof(*file));

	/* Normalize leading slash to make httpd default filenames work */
	p = name;
	if (*p == '/')
		p++;
	if (!strcmp(p, "backup/q1000k-nand.bin"))
		return recovery_open_q1000k_nand_backup(file);

	if (!strcmp(p, "status")) {
		char json[256];
		int ok = prog_phase == 3;
		int err = prog_phase == -1;
		int json_len;
		json_len = snprintf(json, sizeof(json),
				    "{\"in_progress\":%d,\"done\":%u,"
				    "\"total\":%u,\"erase_done\":%u,"
				    "\"erase_total\":%u,\"write_done\":%u,"
				    "\"write_total\":%u,\"ok\":%d,"
				    "\"error\":%d,\"phase\":%d}\n",
				    prog_phase > 0 && prog_phase < 3,
				    (unsigned int)prog_done,
				    (unsigned int)prog_total,
				    (unsigned int)prog_erase_done,
				    (unsigned int)prog_erase_total,
				    (unsigned int)prog_write_done,
				    (unsigned int)prog_write_total,
				    ok, err, prog_phase);
		if (json_len < 0)
			return 0;
		if (json_len >= (int)sizeof(json))
			json_len = (int)sizeof(json) - 1;

		return recovery_open_custom_response(file, "application/json",
						     json, json_len);
	}

	if (!strcmp(p, "ok")) {
		file->data = recovery_page_ok;
		file->len = sizeof(recovery_page_ok) - 1;
		file->index = file->len;
		file->pextension = NULL;
		file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
		return 1;
	}

	if (!strcmp(p, "about"))
		return recovery_open_about_response(file);

	/* Static files are served by fsdata. */
	return 0;
}

void fs_close_custom(struct fs_file *file)
{
	struct recovery_nand_backup_file *backup;

	if (!file)
		return;
	backup = (struct recovery_nand_backup_file *)file->pextension;
	if (backup && backup->magic == RECOVERY_NAND_BACKUP_MAGIC) {
		backup->magic = 0;
		if (recovery_nand_backup_active)
			recovery_nand_backup_active--;
		put_mtd_device(backup->mtd);
		free(backup->cache);
		free(backup);
		file->pextension = NULL;
		return;
	}
	if (file->data && file->data != recovery_page_ok) {
		free((void *)file->data);
		file->data = NULL;
	}
}

int fs_read_custom(struct fs_file *file, char *buffer, int count)
{
	struct recovery_nand_backup_file *backup;
	u32_t left;
	int read;

	if (!file || !buffer || count <= 0)
		return FS_READ_EOF;
	backup = (struct recovery_nand_backup_file *)file->pextension;
	if (backup && backup->magic == RECOVERY_NAND_BACKUP_MAGIC) {
		read = recovery_read_nand_backup(backup, buffer, count);
		if (read < 0 || (!backup->bytes_left &&
				 backup->cache_off >= backup->cache_len &&
				 backup->http_header_off >= backup->http_header_len))
			file->index = file->len;
		return read;
	}
	left = file->len - file->index;
	if (left <= 0)
		return FS_READ_EOF;
	if ((u32_t)count > left)
		count = left;
	memcpy(buffer, file->data + file->index, count);
	file->index += count;
	return count;
}

/* Complete custom responses are supplied in file->data by fs_open_custom(). */

/* HTTP POST handlers */
static ulong recovery_q1000k_upload_top(void)
{
	ulong start = gd->ram_base;
	ulong top;

	if (!gd->ram_size || gd->ram_size > ULONG_MAX - start ||
	    gd->start_addr_sp <= start)
		return 0;

	/* All relocated code, malloc, GD and FDT reservations are above SP. */
	top = min_t(ulong, start + gd->ram_size, gd->start_addr_sp);
	if (top < RECOVERY_Q1000K_STACK_MARGIN)
		return 0;
	return top - RECOVERY_Q1000K_STACK_MARGIN;
}

static int recovery_q1000k_upload_buffer(ulong base, size_t size, u8 **buffer)
{
	ulong start = gd->ram_base;
	ulong top = recovery_q1000k_upload_top();

	if (!size || size > RECOVERY_Q1000K_UPLOAD_MAX ||
	    start > ULONG_MAX - RECOVERY_Q1000K_UPLOAD_OFFSET ||
	    base < start + RECOVERY_Q1000K_UPLOAD_OFFSET)
		return -ENOMEM;
	if (base >= top || size > top - base)
		return -ENOMEM;

	/* Leave ATF, the chainload image and low-memory Ethernet DMA alone. */
	*buffer = (u8 *)base;
	return 0;
}

/* Only an optional addr=0x... parameter is accepted; never evaluate it as code. */
static int recovery_parse_ramboot_addr(const char *uri, ulong *base)
{
	const char *p = strchr(uri, '?');
	ulong value = 0;
	unsigned int digit;

	if (gd->ram_base > ULONG_MAX - RECOVERY_Q1000K_RAMBOOT_OFFSET ||
	    gd->ram_base > ULONG_MAX - RECOVERY_Q1000K_RAMBOOT_MIN_OFFSET)
		return -EINVAL;
	*base = gd->ram_base + RECOVERY_Q1000K_RAMBOOT_OFFSET;
	if (p) {
		if (strncmp(p, "?addr=0x", 8) && strncmp(p, "?addr=0X", 8))
			return -EINVAL;
		p += 8;
		if (!*p)
			return -EINVAL;
		for (; *p; p++) {
			if (*p >= '0' && *p <= '9')
				digit = *p - '0';
			else if (*p >= 'a' && *p <= 'f')
				digit = *p - 'a' + 10;
			else if (*p >= 'A' && *p <= 'F')
				digit = *p - 'A' + 10;
			else
				return -EINVAL;
			if (value > (ULONG_MAX - digit) / 16)
				return -ERANGE;
			value = value * 16 + digit;
		}
		*base = value;
	}
	/* Keep the entire maximum kernel decompression range below the FIT. */
	if (*base < gd->ram_base + RECOVERY_Q1000K_RAMBOOT_MIN_OFFSET ||
	    (*base & 0xfff))
		return -EINVAL;
	return 0;
}

err_t httpd_post_begin(void *connection, const char *uri, const char *http_request,
                       u16_t http_request_len, int content_len, char *response_uri,
                       u16_t response_uri_len, u8_t *post_auto_wnd)
{
    ulong upload_addr = gd->ram_base + RECOVERY_Q1000K_UPLOAD_OFFSET;

    (void)http_request; (void)http_request_len;
    /*
     * Throttle large uploads explicitly: XR1710G recovery accepts firmware
     * images that are much larger than a typical lwIP POST body and manual
     * window updates avoid over-optimistic receive windows.
     */
    if (post_auto_wnd)
        *post_auto_wnd = 0;

    if (recovery_nand_backup_active || post_ok || flash_request || ramboot_request ||
        reboot_request || prog_phase > 0) {
        printf("httpd: rejecting upload while recovery flash is busy (phase=%d)\n",
               prog_phase);
        strlcpy(response_uri, "/fail.html", response_uri_len);
        return ERR_ARG;
    }

    post_ok = 0;
    post_connection = NULL;
    post_validated = false;
    self_write_request = false;
    q1000k_uboot_only = false;
    recovery_ubi_attach_error = 0;
    recv_off = 0;
    recv_total = 0;
    /* Reset progress state. */
    prog_phase = 0;
    prog_done = 0;
    prog_total = 0;
    prog_erase_done = 0;
    prog_erase_total = 0;
    prog_write_done = 0;
    prog_write_total = 0;
    /* Accept optional query parameters after the target path. */
    if ((!strcmp(uri, "/action/self-write") || !strcmp(uri, "/upload/uboot-only") ||
         (!strncmp(uri, "/upload/uboot", 13) &&
          (uri[13] == '\0' || uri[13] == '?'))) &&
        recovery_uboot_update_disabled()) {
        printf("httpd: chainloader updates are disabled for this board\n");
        prog_phase = -1;
        strlcpy(response_uri, "/fail.html", response_uri_len);
        return ERR_ARG;
    }

    if (!strcmp(uri, "/action/self-write") && recovery_board_is_q1000k()) {
        prog_phase = -1;
        strlcpy(response_uri, "/fail.html", response_uri_len);
        return ERR_ARG;
    }
    if (!strcmp(uri, "/upload/uboot-only") && recovery_board_is_q1000k() &&
        IS_ENABLED(CONFIG_Q1000K_INSTALLER)) {
        current_target = TARGET_UBOOT;
        q1000k_uboot_only = true;
    } else if (!strncmp(uri, "/upload/initramfs", 17) &&
        (uri[17] == '\0' || uri[17] == '?') && recovery_board_is_q1000k() &&
        IS_ENABLED(CONFIG_Q1000K_INSTALLER)) {
        current_target = TARGET_INITRAMFS;
        if (recovery_parse_ramboot_addr(uri, &upload_addr)) {
            printf("httpd: invalid or unsafe RAM boot address\n");
            prog_phase = -1;
            strlcpy(response_uri, "/fail.html", response_uri_len);
            return ERR_ARG;
        }
    } else if (!strcmp(uri, "/upload/recovery") && recovery_board_is_q1000k() &&
        IS_ENABLED(CONFIG_Q1000K_INSTALLER)) {
        current_target = TARGET_RECOVERY;
    } else if (!strcmp(uri, "/action/self-write")) {
        current_target = TARGET_UBOOT;
        self_write_request = true;
    } else if (!strncmp(uri, "/upload/firmware", 16) &&
        (uri[16] == '\0' || uri[16] == '?')) {
        current_target = TARGET_FIRMWARE;
        if (recovery_parse_ubi_layout(uri)) {
            printf("httpd: rejecting unknown UBI layout in '%s'\n", uri);
            prog_phase = -1;
            strlcpy(response_uri, "/fail.html", response_uri_len);
            return ERR_ARG;
        }
    } else if (!strncmp(uri, "/upload/uboot", 13) &&
               (uri[13] == '\0' || uri[13] == '?')) {
        current_target = TARGET_UBOOT;
    } else if (!strncmp(uri, "/upload", 7) &&
               (uri[7] == '\0' || uri[7] == '?')) {
        current_target = TARGET_FIRMWARE;
        if (recovery_parse_ubi_layout(uri)) {
            prog_phase = -1;
            strlcpy(response_uri, "/fail.html", response_uri_len);
            return ERR_ARG;
        }
    } else {
        prog_phase = -1;
        strlcpy(response_uri, "/fail.html", response_uri_len);
        return ERR_ARG;
    }

    {
        ulong min = 0;
        ulong env_max = env_get_hex("recovery_max", 0);
        loff_t tmpofs = 0;
        /* RAM boot works before installation and never resolves a flash target. */
        ulong dts_max = current_target == TARGET_INITRAMFS ?
            RECOVERY_Q1000K_UPLOAD_MAX : recovery_calc_target_max(current_target, &tmpofs);
        ulong max = dts_max ? dts_max : RECOVERY_UPLOAD_MAX;

        if (current_target == TARGET_FIRMWARE || current_target == TARGET_RECOVERY ||
            current_target == TARGET_INITRAMFS)
            min = RECOVERY_MIN_FIRMWARE_SIZE;
        else if (current_target == TARGET_UBOOT &&
                 max > RECOVERY_MAX_UBOOT_SIZE)
            max = RECOVERY_MAX_UBOOT_SIZE;

        if (env_max && env_max < max)
            max = env_max; /* allow env to further cap */
        if (content_len <= 0 || (ulong)content_len > max ||
            (min && (ulong)content_len < min)) {
            prog_phase = -1;
            if (min && (ulong)content_len < min) {
                printf("httpd: content_len %d below allowed min %lu for target %d\n",
                       content_len, min, current_target);
            } else {
                printf("httpd: content_len %d exceeds allowed max %lu (target limit %lu, ofs 0x%llx)\n",
                       content_len, max, dts_max, (unsigned long long)tmpofs);
            }
            strlcpy(response_uri, "/fail.html", response_uri_len);
            return ERR_ARG;
        }
    }

    recv_total = content_len;

    if (recovery_board_is_q1000k()) {
        if (recovery_q1000k_upload_buffer(upload_addr, recv_total, &recv_base)) {
            printf("httpd: upload at 0x%lx (%u bytes) exceeds usable RAM\n",
                   upload_addr, recv_total);
            prog_phase = -1;
            strlcpy(response_uri, "/fail.html", response_uri_len);
            return ERR_MEM;
        }
    } else {
        /*
         * Pick a stable upload buffer:
         * recovery_addr -> loadaddr -> CONFIG_SYS_LOAD_ADDR -> RAM fallback.
         */
        ulong ram_start = (ulong)gd->ram_base;
        ulong ram_end = (ulong)gd->ram_base + (ulong)gd->ram_size;
        ulong base = env_get_hex("recovery_addr", 0);
        if (!base)
            base = env_get_hex("loadaddr", 0);
        if (!base)
            base = CONFIG_SYS_LOAD_ADDR;

        /* Keep the buffer inside usable RAM, including normalization space. */
		/* Leave room for NAND page alignment after FIT normalization. */
		ulong buffer_need = current_target == TARGET_UBOOT ?
			RECOVERY_UBOOT_SLOT_SIZE : recv_total;
		if (base < ram_start || base >= ram_end ||
		    buffer_need > (ram_end - base) - 0x2000) {
            ulong fallback = ram_start + 0x01000000UL;
            if (fallback >= ram_start && fallback < ram_end &&
                buffer_need <= (ram_end - fallback)) {
                base = fallback;
            } else {
                prog_phase = -1;
                printf("httpd: no sufficient RAM for upload (%u bytes)\n", recv_total);
                strlcpy(response_uri, "/fail.html", response_uri_len);
                return ERR_MEM;
            }
        }
        recv_base = (u8 *)base;
    }

    post_ok = 1;
    post_connection = connection;
    /* Leave response_uri untouched here so the POST can complete normally. */
    if (self_write_request)
        printf("httpd: accepting XG2010G chainloader self-write request\n");
    else if (current_target == TARGET_FIRMWARE)
        printf("httpd: accepting %u-byte firmware for UBI %s\n",
               recv_total, current_ubi_layout->version);
    else if (current_target == TARGET_INITRAMFS)
        printf("httpd: accepting %u-byte initramfs at 0x%lx\n", recv_total, (ulong)recv_base);
    else
        printf("httpd: accepting %u-byte U-Boot image\n", recv_total);
    return ERR_OK;
}

err_t httpd_post_receive_data(void *connection, struct pbuf *p)
{
	struct pbuf *q;
	u16_t recved = 0;

	if (!post_ok || connection != post_connection || post_validated) {
		pbuf_free(p);
		return ERR_ARG;
	}

	/* Copy request payload into RAM and defer flash work to the main loop. */
	for (q = p; q != NULL; q = q->next) {
        size_t avail = recv_total - recv_off;
        size_t clen = q->len;
	        if (clen > avail)
	            clen = avail;
	        memcpy(recv_base + recv_off, q->payload, clen);
	        recv_off += clen;
    }
    recved = p->tot_len;
    pbuf_free(p);

#if LWIP_HTTPD_POST_MANUAL_WND
    if (recved)
        httpd_post_data_recved(connection, recved);
#endif

    return ERR_OK;
}

void httpd_post_finished(void *connection, char *response_uri, u16_t response_uri_len)
{
	const u8 *fit_image;
	u8 *running_fit = NULL;
	size_t fit_size;
	bool complete;
	int ret = 0;

	if (connection != post_connection) {
		strlcpy(response_uri, "/fail.html", response_uri_len);
		return;
	}
	post_validated = false;
	printf("httpd: post finished, %u/%u bytes received\n", recv_off, recv_total);
	complete = post_ok && recv_total && (recv_off >= recv_total);

	/*
	 * Validate while httpd can still choose the POST response. flash_image()
	 * repeats the same checks immediately before touching flash, but waiting
	 * until then would make a malformed image receive the success page first.
	 */
	if (complete) {
		if (recovery_board_is_q1000k() && IS_ENABLED(CONFIG_Q1000K_INSTALLER)) {
			ret = recovery_validate_q1000k_upload(recv_base, recv_off);
		} else if (self_write_request) {
			if (recv_off != strlen(RECOVERY_XG2010G_INSTALL_TOKEN) ||
			    memcmp(recv_base, RECOVERY_XG2010G_INSTALL_TOKEN,
				   strlen(RECOVERY_XG2010G_INSTALL_TOKEN))) {
				printf("httpd: invalid XG2010G self-write confirmation\n");
				ret = -EPERM;
			} else {
				ret = recovery_copy_running_chainloader_fit(&running_fit,
								      &fit_size);
				free(running_fit);
			}
		} else if (current_target == TARGET_UBOOT) {
			ret = recovery_prepare_uboot_fit(recv_base, recv_off,
							  &fit_image, &fit_size);
		} else {
			ret = recovery_validate_firmware_image(recv_base, recv_off);
		}

		if (ret) {
			printf("httpd: upload validation failed for target %d: %d\n",
			       current_target, ret);
			post_ok = 0;
			self_write_request = false;
			prog_phase = -1;
			complete = false;
		}
	}

	if (complete) {
		post_validated = true;
		strlcpy(response_uri, "/ok", response_uri_len);
		printf("httpd: upload received; waiting for POST response ACK\n");
	} else {
		strlcpy(response_uri, "/fail.html", response_uri_len);
		post_ok = 0;
		prog_phase = -1;
	}
}

#if LWIP_HTTPD_POST_RESPONSE_ACK
void httpd_post_response_complete(void *connection, err_t result)
{
	if (connection != post_connection || !post_validated ||
	    !post_ok || !recv_total || recv_off != recv_total)
		return;
	post_connection = NULL;

	if (result != ERR_OK) {
		printf("httpd: POST response not acknowledged (%d); operation cancelled\n",
		       result);
		post_ok = 0;
		post_validated = false;
		prog_phase = -1;
		return;
	}

	if (current_target == TARGET_INITRAMFS) {
		printf("httpd: POST response acknowledged; RAM boot may start\n");
		ramboot_request = true;
	} else {
		printf("httpd: POST response acknowledged; flashing may start\n");
		flash_request = 1;
	}
}
#endif

static int flash_image(struct recovery_status_led_ctrl *status_leds)
{
	struct recovery_target target;
	const u8 *image = recv_base;
	const u8 *fit_image = image;
	u32 image_size = recv_off;
	size_t fit_size = image_size;
	int ret;

	post_ok = 0;

	if (!image_size) {
		printf("No data received to flash\n");
		prog_phase = -1;
		return -EINVAL;
	}

	if (current_target == TARGET_UBOOT) {
		ret = recovery_prepare_uboot_fit(image, image_size,
						  &fit_image, &fit_size);
		if (ret) {
			prog_phase = -1;
			return ret;
		}
	} else {
		ret = recovery_validate_firmware_image(image, image_size);
		if (ret) {
			prog_phase = -1;
			return ret;
		}
	}

	if (current_target == TARGET_UBOOT) {
		/* Keep the raw target available for normalization below. */
		image = fit_image;
		image_size = fit_size;
	}

	ret = recovery_resolve_target(current_target, &target);
	if (ret) {
		printf("No flash target found for upload type %d\n", current_target);
		prog_phase = -1;
		return ret;
	}

	if (current_target == TARGET_UBOOT) {
		ret = recovery_validate_xg2010g_uboot_target(&target);
		if (ret) {
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

		if (RECOVERY_UBOOT_SLOT_SIZE > target.limit) {
			recovery_release_target(&target);
			prog_phase = -1;
			return -EFBIG;
		}

		ret = recovery_normalize_uboot_fit(fit_image, fit_size);
		if (ret) {
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}
		printf("Normalized U-Boot upload to raw FIT: %u bytes at slot offset 0x0 (window %lu bytes)\n",
		       (unsigned int)fit_size, RECOVERY_UBOOT_SLOT_SIZE);
		image = recv_base;
		image_size = fit_size;
	}

	if (current_target == TARGET_FIRMWARE &&
	    target.backend == RECOVERY_BACKEND_UBI) {
		ret = recovery_force_ubi_rebuild(&target);
		if (ret) {
			printf("Failed to select UBI %s rebuild target: %d\n",
			       current_ubi_layout->version, ret);
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}
	}

	if (current_target == TARGET_FIRMWARE &&
	    target.backend == RECOVERY_BACKEND_UBI)
		target.limit = recovery_calc_rebuild_ubi_limit(&target);

	if (image_size > target.limit) {
		printf("Image size %u exceeds target size %llu\n",
		       image_size, target.limit);
		recovery_release_target(&target);
		prog_phase = -1;
		return -EFBIG;
	}

	if (target.backend == RECOVERY_BACKEND_UBI) {
		bool reformatted = false;

		if (current_target == TARGET_FIRMWARE)
			printf("Recovery mode: recreating UBI %s on '%s'.\n",
			       current_ubi_layout->version, target.ubi_part);

		ret = recovery_prepare_ubi_target(&target, status_leds, image_size,
						      &reformatted);
		if (ret) {
			printf("Failed to prepare UBI target '%s' on '%s': %d\n",
			       target.name, target.ubi_part, ret);
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

		if (!reformatted) {
			ret = recovery_cleanup_ubi_firmware(&target, status_leds,
							    image_size);
			if (ret) {
				printf("Failed to clean UBI firmware volumes for '%s': %d\n",
				       target.name, ret);
				recovery_release_target(&target);
				prog_phase = -1;
				return ret;
			}
		}

		if (!target.cur_size) {
			printf("Creating missing UBI volume '%s' for %u bytes...\n",
			       target.name, image_size);
			ret = recovery_create_ubi_target(&target, image_size);
			if (ret) {
				printf("ubi_create_volume failed for '%s': %d\n",
				       target.name, ret);
				recovery_release_target(&target);
				prog_phase = -1;
				return ret;
			}
		} else if (image_size > target.cur_size) {
			printf("Resizing UBI volume '%s' from %llu to fit %u bytes...\n",
			       target.name, target.cur_size, image_size);
			ret = recovery_resize_ubi_target(&target, image_size);
			if (ret) {
				printf("ubi_resize_volume failed for '%s': %d\n",
				       target.name, ret);
				recovery_release_target(&target);
				prog_phase = -1;
				return ret;
			}
		}

		ret = recovery_ensure_preserved_ubi_volumes(&target);
		if (ret) {
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

		ret = recovery_ensure_rootfs_data(&target);
		if (ret) {
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

		printf("Writing %u bytes to UBI volume '%s' on '%s'...\n",
		       image_size, target.name, target.ubi_part);
		prog_phase = 2;
		prog_done = prog_erase_total;
		prog_write_done = 0;
		recovery_service_runtime(status_leds);
		ret = recovery_write_ubi_target(&target, status_leds,
						image, image_size);
		if (ret) {
			printf("UBI write failed for '%s': %d\n",
			       target.name, ret);
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

		prog_write_done = image_size;
		prog_done = prog_erase_total + image_size;
		recovery_release_target(&target);
		prog_phase = 3;
		if (current_target == TARGET_FIRMWARE)
			xr1710g_sync_factory_part(target.ubi_part);
		return 0;
	}

	{
		struct mtd_info *mtd = target.mtd;
		loff_t ofs = target.ofs;
		loff_t erase_len = target.limit;

		prog_phase = 1;
		prog_done = 0;
		prog_erase_done = 0;
		prog_erase_total = erase_len;
		prog_write_done = 0;
		prog_write_total = image_size;
		prog_total = prog_erase_total + prog_write_total;

		printf("Erasing entire target '%s' (%llu bytes) and writing %u bytes...\n",
		       target.name, (unsigned long long)target.limit, image_size);
		ret = recovery_erase_mtd_region(mtd, ofs, target.limit,
						status_leds);
		if (ret) {
			printf("mtd_erase failed: %d\n", ret);
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

		prog_phase = 2;
		ret = recovery_write_mtd_region(mtd, ofs, target.limit, image,
						image_size,
						current_target == TARGET_UBOOT,
						status_leds);
		if (ret) {
			printf("mtd_write failed: %d\n", ret);
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}
	}

	recovery_release_target(&target);
	prog_phase = 3;
	if (current_target == TARGET_FIRMWARE)
		xr1710g_sync_factory();
	return 0;
}

static void recovery_installer_progress(void *ctx, bool erase, u32 done, u32 total)
{
	prog_phase = erase ? 1 : 2;
	if (erase) {
		prog_erase_total = total;
		prog_erase_done = done;
	} else {
		prog_write_total = total;
		prog_write_done = done;
	}
	prog_total = prog_erase_total + prog_write_total;
	prog_done = prog_erase_done + prog_write_done;
	recovery_service_runtime(ctx);
}

static int recovery_flash_uploaded_image(struct recovery_status_led_ctrl *leds)
{
	struct q1000k_install_plan *plan = NULL;
	bool installer = IS_ENABLED(CONFIG_Q1000K_INSTALLER) && recovery_board_is_q1000k();
	int ret = -EPERM;

	if (current_target == TARGET_INITRAMFS ||
	    !post_validated || !post_ok || !recv_total ||
	    recv_off != recv_total || post_connection)
		goto out;
	post_validated = false;
	if (installer) {
		ret = recovery_validate_q1000k_upload(recv_base, recv_off);
		if (ret)
			goto out;
		/* No further requests may enter while read-only preflight runs. */
		prog_phase = 1;
		if (current_target == TARGET_UBOOT)
			ret = q1000k_install_prepare(&plan, !q1000k_uboot_only);
		else
			ret = q1000k_ubi_check_layout();
		if (ret)
			goto out;
		airoha_snand_set_write_enabled(true);
		if (current_target == TARGET_UBOOT)
			ret = q1000k_install_commit(plan, recv_base, recv_off,
						   recovery_installer_progress, leds);
		else
			ret = q1000k_ubi_upload(current_target == TARGET_RECOVERY ? "recovery" : "fit",
				recv_base, recv_off, recovery_installer_progress, leds);
		goto out;
	}

	if (IS_ENABLED(CONFIG_AIROHA_SNFI_NAND_WRITE_GUARD)) {
		if (!recovery_board_is_q1000k() ||
		    current_target != TARGET_FIRMWARE)
			goto out;
		ret = recovery_validate_firmware_image(recv_base, recv_off);
		if (ret)
			goto out;
	}

	/* One synchronous commit; close the window on every flash return path. */
	airoha_snand_set_write_enabled(true);
	ret = flash_image(leds);
out:
	airoha_snand_set_write_enabled(false);
	q1000k_install_release(plan);
	post_ok = 0;
	post_validated = false;
	if (ret)
		prog_phase = -1;
	else if (installer)
		prog_phase = 3;
	return ret;
}

/* Consume a single acknowledged RAM upload without ever opening the write gate. */
static int recovery_prepare_ramboot(void)
{
	u8 *checked_base;
	int ret = -EPERM;

	airoha_snand_set_write_enabled(false);
	if (!IS_ENABLED(CONFIG_Q1000K_INSTALLER) || !recovery_board_is_q1000k() ||
	    current_target != TARGET_INITRAMFS || !ramboot_request ||
	    !post_validated || !post_ok || !recv_total ||
	    recv_off != recv_total || post_connection)
		goto out;
	ret = recovery_q1000k_upload_buffer((ulong)recv_base, recv_total, &checked_base);
	if (ret)
		goto out;
	ret = recovery_validate_q1000k_upload(recv_base, recv_off);
out:
	ramboot_request = false;
	post_ok = 0;
	post_validated = false;
	prog_phase = ret ? -1 : 4;
	return ret;
}

static int recovery_boot_initramfs(void)
{
	const char *args = env_get("bootargs");
	char *saved_args = args ? strdup(args) : NULL;
	int ret;

	if (args && !saved_args)
		return -ENOMEM;
	/* A kernel without /init must not fall back to the installed NAND root. */
	ret = env_set("bootargs", "console=ttyS0,115200 earlycon root=/dev/ram0 rdinit=/init");
	if (!ret) {
		printf("Q1000K: booting initramfs at 0x%lx; bootloader NAND writes locked\n",
		       (ulong)recv_base);
		ret = run_commandf("bootm 0x%lx", (ulong)recv_base);
	}
	env_set("bootargs", saved_args);
	free(saved_args);
	/* Linux does not return on success. Leave failures at the serial prompt. */
	printf("Q1000K: RAM boot returned (%d); run http_recovery to retry\n", ret);
	return ret ? ret : -EIO;
}

int run_http_recovery(void)
{
	struct udevice *udev;
	struct netif *netif;
	struct recovery_led_ctrl leds;
	struct recovery_status_led_ctrl status_leds;
	struct recovery_dhcp_server dhcp;
	bool use_status_leds = false;
	bool boot_from_ram = false;
	int rc;

	airoha_snand_set_write_enabled(false);
	recovery_cancel_timeouts();
	recv_off = recv_total = 0;
	post_ok = 0;
	post_connection = NULL;
	post_validated = false;
	flash_request = 0;
	self_write_request = false;
	q1000k_uboot_only = false;
	reboot_request = 0;
	prog_phase = 0;
	prog_done = 0;
	prog_total = 0;
	prog_erase_done = 0;
	prog_erase_total = 0;
	prog_write_done = 0;
	prog_write_total = 0;
	recovery_ubi_attach_error = 0;
	memset(&leds, 0, sizeof(leds));
	rc = recovery_status_led_init(&status_leds);
	if (!rc) {
		use_status_leds = true;
	} else {
		printf("Recovery status LEDs unavailable (%d), fallback to link LEDs\n",
		       rc);
	}
	recovery_led_init(&leds);
	recovery_prepare_static_network();

	rc = net_lwip_eth_start();
	if (rc < 0) {
		printf("Failed to start Ethernet: %d\n", rc);
		recovery_lwip_cleanup(NULL);
		recovery_status_led_stop(&status_leds);
		recovery_status_led_release(&status_leds);
		recovery_led_stop(&leds);
		recovery_led_ctrl_free(&leds);
		return rc;
	}

	udev = eth_get_dev();
	if (!udev) {
		printf("No active net device\n");
		recovery_lwip_cleanup(NULL);
		recovery_status_led_stop(&status_leds);
		recovery_status_led_release(&status_leds);
		recovery_led_stop(&leds);
		recovery_led_ctrl_free(&leds);
		net_lwip_eth_stop();
		return -ENODEV;
	}

	netif = net_lwip_new_netif(udev);
	if (!netif) {
		recovery_lwip_cleanup(NULL);
		recovery_status_led_stop(&status_leds);
		recovery_status_led_release(&status_leds);
		recovery_led_stop(&leds);
		recovery_led_ctrl_free(&leds);
		net_lwip_eth_stop();
		return -ENODEV;
	}

	rc = recovery_dhcp_server_init(&dhcp, netif);
	if (rc)
		printf("Failed to start recovery DHCP server: %d\n", rc);
	else
		net_lwip_set_recovery_dhcp_hook(recovery_dhcp_recv, &dhcp);

	if (!recovery_httpd_started) {
		httpd_init();
		recovery_httpd_started = true;
	}
	printf("HTTP recovery server listening on http://%s/\n",
	       ip4addr_ntoa(netif_ip4_addr(netif)));
	if (IS_ENABLED(CONFIG_AIROHA_ETH))
		printf("Press d for live Ethernet diagnostics, Ctrl-C to stop.\n");
	if (use_status_leds)
		net_lwip_set_recovery_poll_hook(recovery_status_led_service, &status_leds);

	while (1) {
		if (tstc()) {
			int c = getchar();

			if (c == 0x03) { /* Ctrl-C */
				printf("Abort by user\n");
				break;
			}
			if (IS_ENABLED(CONFIG_AIROHA_ETH) && c == 'd')
				run_command("rtl8261_diag", 0);
		}
		/* net_lwip_rx() already runs sys_check_timeouts(). */
		net_lwip_rx(udev, netif);
		if (use_status_leds)
			recovery_status_led_poll(&status_leds);
		recovery_led_poll(&leds);
		if (ramboot_request) {
			rc = recovery_prepare_ramboot();
			if (!rc) {
				boot_from_ram = true;
				break;
			}
			printf("RAM boot validation failed: %d. Keeping server running.\n", rc);
		}
		if (flash_request) {
			flash_request = 0;
			if (self_write_request) {
				printf("Self-write confirmed, installing running chainloader...\n");
				rc = recovery_install_running_chainloader(&status_leds);
				self_write_request = false;
			} else {
				printf("Upload done, flashing...\n");
				rc = recovery_flash_uploaded_image(&status_leds);
			}
			if (!rc) {
				printf("Flashing complete. Rebooting in %dms...\n",
				       REBOOT_DELAY_MS);
				reboot_request = 0;
				sys_timeout(REBOOT_DELAY_MS, reboot_delay_cb, NULL);
			} else {
				printf("Flashing failed: %d. Keeping server running.\n",
				       rc);
			}
		}
		if (reboot_request)
			do_reset(NULL, 0, 0, NULL);
		WATCHDOG_RESET();
	}

	net_lwip_set_recovery_poll_hook(NULL, NULL);
	recovery_cancel_timeouts();
	recovery_dhcp_server_stop(&dhcp);
	net_lwip_set_recovery_dhcp_hook(NULL, NULL);
	recovery_lwip_cleanup(netif);
	net_lwip_remove_netif(netif);
	net_lwip_eth_stop();
	recovery_status_led_stop(&status_leds);
	recovery_status_led_release(&status_leds);
	recovery_led_stop(&leds);
	recovery_led_ctrl_free(&leds);
	/* Stop HTTP, DHCP, Ethernet DMA and LED hooks before bootm takes over. */
	return boot_from_ram ? recovery_boot_initramfs() : 0;
}
