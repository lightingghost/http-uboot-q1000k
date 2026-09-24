/* SPDX-License-Identifier: GPL-2.0+ */
/* Hardware substitutes for test_nand_safety.py; not part of the firmware. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>

typedef uint8_t u8;
typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32;
typedef unsigned long ulong;
typedef int err_t;
#define CONFIG_AIROHA_SNFI_NAND_WRITE_GUARD 1
#define CONFIG_Q1000K_INSTALLER TEST_INSTALLER
#define CONFIG_SYS_BOOTM_LEN 0x08000000UL
#define IS_ENABLED(x) (x)
#define BIT(n) (1U << (n))
#define REG_BLOCK_LOCK 0xa0
#define REG_CFG 0xb0
#define CFG_OTP_ENABLE BIT(6)
#define SPI_MEM_DATA_OUT 1
#define ERR_ARG -1
#define ERR_MEM -2
#define ERR_OK 0
#define FS_READ_EOF -1
#define CONFIG_SYS_LOAD_ADDR ((ulong)upload_buffer)
#define LWIP_HTTPD_POST_MANUAL_WND 1
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define WATCHDOG_RESET() do {} while (0)
#define IS_ERR_OR_NULL(p) (!(p))
static int quiet_printf(const char *format, ...) { return 0; }
#define printf quiet_printf
#define strlcpy test_strlcpy

struct spi_mem_op {
    struct { unsigned opcode; } cmd;
    struct { unsigned nbytes; uint64_t val; } addr;
    struct { unsigned nbytes; int dir; union { const void *out; void *in; } buf; } data;
};
struct pbuf { struct pbuf *next; void *payload; unsigned len, tot_len; };
struct mtd_info { struct mtd_info *parent; const char *name; uint64_t size, offset; };
struct recovery_status_led_ctrl { int unused; };
struct recovery_ubi_layout { const char *version; };
struct recovery_target {
    int backend; const char *name, *ubi_part; struct mtd_info *mtd;
    unsigned long long limit; bool ubi_needs_format;
};
struct recovery_nand_backup_file {
    struct mtd_info *mtd;
    u8 *cache; size_t cache_capacity, cache_len, cache_off;
    loff_t next_offset; unsigned long long bytes_left;
    char http_header[256]; size_t http_header_len, http_header_off;
};
enum upload_target { TARGET_FIRMWARE, TARGET_UBOOT, TARGET_RECOVERY, TARGET_INITRAMFS };
#define RECOVERY_BACKEND_UBI 1
static enum upload_target current_target;
static struct recovery_ubi_layout layout = { "2.0" }, *current_ubi_layout = &layout;
static struct { ulong ram_base, ram_size, start_addr_sp; } ram, *gd = &ram;
static struct mtd_info master = { NULL, "spi-nand0", 0x20000000, 0 };
static struct mtd_info partition = { &master, "ubi", 0x1b600000, 0x700000 };
/* Model both staging addresses with an untouched, sparse 79 MiB gap. */
static struct {
    u8 flash[1024 * 1024];
    u8 gap[79 * 1024 * 1024];
    u8 ramboot[2 * 1024 * 1024];
} staging __attribute__((aligned(4096)));
#define upload_buffer staging.flash
#define ramboot_buffer staging.ramboot
static u8 *recv_base;
static u32 recv_off, recv_total;
static int post_ok, flash_request, reboot_request, prog_phase;
static unsigned prog_done, prog_total, prog_erase_done, prog_erase_total;
static unsigned prog_write_done, prog_write_total, recovery_nand_backup_active;
static int recovery_ubi_attach_error;
static bool self_write_request, post_validated, airoha_snand_write_enabled;
static bool q1000k_uboot_only, prepared_ubi;
static bool ramboot_request;
static char bootargs[256];
static bool have_bootargs;
static int boot_calls, boot_result, boot_env_result;
static void *post_connection;
static int validation_result, flash_result, flash_calls, validation_calls;
static int get_target_calls, read_result;
static uint64_t read_next, read_bytes;
static bool board_q1000k = true;
static void airoha_snand_set_write_enabled(bool enabled);
static int airoha_snand_check_write(const struct spi_mem_op *op);
static int recovery_q1000k_target(enum upload_target, struct recovery_target *);
static bool recovery_board_is_q1000k(void) { return board_q1000k; }
static bool recovery_uboot_update_disabled(void) { return !TEST_INSTALLER; }
static int recovery_parse_ubi_layout(const char *uri) { return strchr(uri, '?') ? -EINVAL : 0; }
static void mtd_probe_devices(void) {}
static struct mtd_info *get_mtd_device_nm(const char *name) {
    assert(!strcmp(name, "ubi")); get_target_calls++; return &partition;
}
static void put_mtd_device(struct mtd_info *mtd) {}
static ulong recovery_calc_target_max(enum upload_target target, loff_t *ofs) {
    struct recovery_target t = { 0 };
    return recovery_q1000k_target(target, &t) ? 0 : t.limit;
}
static ulong env_get_hex(const char *name, ulong fallback) { return fallback; }
static const char *env_get(const char *name) {
    assert(!strcmp(name, "bootargs")); return have_bootargs ? bootargs : NULL;
}
static int env_set(const char *name, const char *value) {
    assert(!strcmp(name, "bootargs"));
    if (value && strstr(value, "rdinit=") && boot_env_result) return boot_env_result;
    have_bootargs = value != NULL;
    if (value) snprintf(bootargs, sizeof(bootargs), "%s", value);
    return 0;
}
static int run_commandf(const char *format, ...) {
    va_list ap;
    assert(!strcmp(format, "bootm 0x%lx"));
    va_start(ap, format); assert(va_arg(ap, ulong) == (ulong)recv_base); va_end(ap);
    assert(!airoha_snand_write_enabled);
    assert(strstr(bootargs, "rdinit=/init") && strstr(bootargs, "root=/dev/ram0"));
    assert(!strstr(bootargs, "ubi.block="));
    boot_calls++; return boot_result;
}
static bool recovery_ram_range_ok(ulong addr, size_t len) { return true; }
static size_t strlcpy(char *dst, const char *src, size_t size) {
    snprintf(dst, size, "%s", src); return strlen(src);
}
static void pbuf_free(struct pbuf *p) {}
static void httpd_post_data_recved(void *connection, u16_t count) {}
static int recovery_copy_running_chainloader_fit(u8 **image, size_t *size) { return -EPERM; }
static int recovery_prepare_uboot_fit(const void *image, size_t size,
                                    const u8 **fit, size_t *fit_size) { return -EPERM; }
static int recovery_validate_firmware_image(const void *image, size_t size) {
    validation_calls++; return validation_result;
}
static int flash_image(struct recovery_status_led_ctrl *leds) {
    struct spi_mem_op op = { .cmd.opcode = 0xd8 };
    assert(airoha_snand_check_write(&op) == 0);
    flash_calls++; return flash_result;
}
static int mtd_read(struct mtd_info *mtd, loff_t offset, size_t len, size_t *retlen, u8 *buf) {
    struct spi_mem_op op = { .cmd.opcode = 0x13 };
    assert(!airoha_snand_write_enabled);
    assert(airoha_snand_check_write(&op) == 0);
    assert((uint64_t)offset == read_next);
    if (read_result && read_result != -EUCLEAN) { *retlen = 0; return read_result; }
    memset(buf, (offset / 0x40000) & 255, len);
    read_next += len; read_bytes += len; *retlen = len;
    return read_result;
}

struct q1000k_install_plan { int unused; };
static int preflight_result;
static int recovery_validate_q1000k_upload(const void *buf, size_t size) {
    return recovery_validate_firmware_image(buf, size);
}
static void recovery_installer_progress(void *ctx, bool erase, u32 done, u32 total) {}
static int q1000k_install_prepare(struct q1000k_install_plan **plan, bool prepare_ubi) {
    assert(!airoha_snand_write_enabled); prepared_ubi=prepare_ubi; return preflight_result;
}
static void q1000k_install_release(struct q1000k_install_plan *plan) {}
static int q1000k_ubi_check_layout(void) {
    assert(!airoha_snand_write_enabled); return preflight_result;
}
static int q1000k_install_commit(struct q1000k_install_plan *plan,
    const void *image, size_t size, void (*progress)(void *, bool, u32, u32), void *ctx) {
    return flash_image(ctx);
}
static int q1000k_ubi_upload(const char *name, const void *image, size_t size,
    void (*progress)(void *, bool, u32, u32), void *ctx) {
    assert(!strcmp(name, "fit") || !strcmp(name, "recovery")); return flash_image(ctx);
}

/* INSERT PRODUCTION CODE */

static void reset_state(void) {
    gd->ram_base = (ulong)upload_buffer - 0x04000000UL;
    gd->ram_size = 0x20000000UL;
    gd->start_addr_sp = gd->ram_base + 0x1f000000UL;
    post_ok = post_validated = self_write_request = false;
    q1000k_uboot_only = prepared_ubi = false;
    ramboot_request = false;
    flash_request = reboot_request = prog_phase = 0;
    recv_off = recv_total = 0;
    post_connection = NULL;
    recovery_nand_backup_active = 0;
    validation_result = flash_result = flash_calls = validation_calls = preflight_result = 0;
    airoha_snand_set_write_enabled(false);
}
static char response[64];
static int owner, stranger;
static void begin_upload(void) {
    u8 wnd;
    assert(httpd_post_begin(&owner, "/upload/firmware", "", 0,
                           sizeof(upload_buffer), response, sizeof(response), &wnd) == ERR_OK);
    assert(wnd == 0);
}
static void complete_upload(void) {
    struct pbuf data = { NULL, upload_buffer, sizeof(upload_buffer), sizeof(upload_buffer) };
    begin_upload();
    assert(httpd_post_receive_data(&owner, &data) == 0);
    httpd_post_finished(&owner, response, sizeof(response));
}
static void check_blocked(void) {
    unsigned mutations[] = { 0x06, 0x02, 0x32, 0x84, 0x34, 0x10, 0xd8, 0xa1 };
    for (unsigned i = 0; i < sizeof(mutations)/sizeof(*mutations); i++) {
        struct spi_mem_op op = { .cmd.opcode = mutations[i] };
        assert(airoha_snand_check_write(&op) == -EROFS);
    }
}
int main(void) {
    /* Both array and OOB programming require the blocked NAND opcodes. */
    reset_state(); check_blocked();
    u8 *buffer;
    assert(recovery_q1000k_upload_buffer((ulong)upload_buffer, 0x10000000, &buffer) == 0);
    assert(buffer == upload_buffer);
    assert(recovery_q1000k_upload_buffer((ulong)upload_buffer, 0x10000001, &buffer) == -ENOMEM);
    gd->start_addr_sp = (ulong)upload_buffer + 0x10000000 + 0x100000 - 1;
    assert(recovery_q1000k_upload_buffer((ulong)upload_buffer, 0x10000000, &buffer) == -ENOMEM);
    gd->start_addr_sp = 0;
    assert(recovery_q1000k_upload_buffer((ulong)upload_buffer, 0x100000, &buffer) == -ENOMEM);
    reset_state();
    /* Exact physical defaults, kernel headroom, alignment, syntax and top bound. */
    gd->ram_base = 0x80000000UL;
    gd->start_addr_sp = 0x9f000000UL;
    ulong address;
    assert(!recovery_parse_ramboot_addr("/upload/initramfs", &address));
    assert(address == 0x89000000UL);
    assert(!recovery_q1000k_upload_buffer(address, 0x10000000, &buffer));
    assert(!recovery_parse_ramboot_addr("/upload/initramfs?addr=0x90000000", &address));
    assert(address == 0x90000000UL);
    assert(!recovery_parse_ramboot_addr("/upload/initramfs?addr=0X88200000", &address));
    const char *bad_addresses[] = {
        "", "0x", "0x84000000", "0x80200000", "0x89000001", "0x89000100",
        "-1", "89000000", "0x10000000000000000", "0x89000000;reset",
        "0x89000000&addr=0x90000000", "0x89000000%00", "0x89000000 ", "0x8900000g"
    };
    char uri[128];
    for (unsigned i = 0; i < sizeof(bad_addresses)/sizeof(*bad_addresses); i++) {
        snprintf(uri, sizeof(uri), "/upload/initramfs?addr=%s", bad_addresses[i]);
        assert(recovery_parse_ramboot_addr(uri, &address) < 0);
    }
    assert(recovery_parse_ramboot_addr("/upload/initramfs?foo=0x89000000", &address) < 0);
    assert(recovery_parse_ramboot_addr("/upload/initramfs?", &address) < 0);
    ulong top = recovery_q1000k_upload_top();
    assert(top == 0x9ef00000UL);
    assert(!recovery_q1000k_upload_buffer(top - 0x100000, 0x100000, &buffer));
    assert(recovery_q1000k_upload_buffer(top - 0x100000, 0x100001, &buffer) < 0);
    assert(recovery_q1000k_upload_buffer(0xa0000000UL, 0x100000, &buffer) < 0);
    assert(recovery_q1000k_upload_buffer(ULONG_MAX, 0x100000, &buffer) < 0);
    gd->ram_base = ULONG_MAX - 0x1000;
    assert(recovery_parse_ramboot_addr("/upload/initramfs", &address) < 0);
    assert(recovery_q1000k_upload_top() == 0);
    reset_state();
    unsigned reads[] = { 0x13, 0x03, 0x0b, 0x3b, 0xbb, 0x6b, 0xeb, 0x9f, 0x0f, 0xff };
    for (unsigned i = 0; i < sizeof(reads)/sizeof(*reads); i++) {
        struct spi_mem_op op = { .cmd.opcode = reads[i] };
        assert(airoha_snand_check_write(&op) == 0);
    }
    u8 cfg = 0x18;
    struct spi_mem_op setup = { .cmd.opcode = 0x1f, .addr = { 1, 0xb0 },
                               .data = { 1, SPI_MEM_DATA_OUT, { .out = &cfg } } };
    assert(airoha_snand_check_write(&setup) == 0);
    cfg = 0x80; assert(airoha_snand_check_write(&setup) == -EROFS);
    cfg = 0x40; assert(airoha_snand_check_write(&setup) == -EROFS);

    /* No data, forbidden endpoints, and concurrent backup never arm a write. */
    u8 wnd;
    assert(httpd_post_begin(&owner, "/upload/firmware", "", 0, 0,
                           response, sizeof(response), &wnd) == ERR_ARG);
    httpd_post_response_complete(&owner, ERR_OK);
    assert(!flash_request); check_blocked();
    reset_state();
    assert(httpd_post_begin(&owner, "/action/self-write", "", 0, 16,
                           response, sizeof(response), &wnd) == ERR_ARG);
    assert(httpd_post_begin(&owner, "/upload/uboot", "", 0, 1048576,
                           response, sizeof(response), &wnd) == (TEST_INSTALLER ? ERR_OK : ERR_ARG));
    reset_state();
    assert(httpd_post_begin(&owner, "/upload/uboot-only", "", 0, 1048576,
                           response, sizeof(response), &wnd) == (TEST_INSTALLER ? ERR_OK : ERR_ARG));
    reset_state();
    recovery_nand_backup_active = 1;
    assert(httpd_post_begin(&owner, "/upload/firmware", "", 0, 1048576,
                           response, sizeof(response), &wnd) == ERR_ARG);
    check_blocked();

    reset_state(); begin_upload();
    httpd_post_finished(&owner, response, sizeof(response));
    httpd_post_response_complete(&owner, ERR_OK);
    assert(!flash_request && !post_validated); check_blocked();
    reset_state(); validation_result = -EINVAL; complete_upload();
    httpd_post_response_complete(&owner, ERR_OK);
    assert(!flash_request && !post_validated); check_blocked();

    reset_state(); complete_upload();
    httpd_post_response_complete(&stranger, ERR_OK);
    assert(!flash_request && post_connection == &owner); check_blocked();
    httpd_post_response_complete(&owner, -EIO);
    assert(!flash_request); check_blocked();

    /* A stale connection must not finish or replace the active upload. */
    reset_state(); begin_upload();
    struct pbuf one = { NULL, upload_buffer, 1, 1 };
    assert(httpd_post_receive_data(&stranger, &one) == ERR_ARG);
    assert(recv_off == 0);
    httpd_post_finished(&stranger, response, sizeof(response));
    assert(post_ok && post_connection == &owner);

    /* Correct ACK opens exactly one synchronous commit and always relocks. */
    for (int error = 0; error < 2; error++) {
        reset_state(); complete_upload(); check_blocked();
        assert(post_validated && !flash_request);
        httpd_post_response_complete(&owner, ERR_OK);
        assert(flash_request); check_blocked();
        flash_result = error ? -EIO : 0;
        assert(recovery_flash_uploaded_image(NULL) == flash_result);
        assert(flash_calls == 1); check_blocked();
        assert(recovery_flash_uploaded_image(NULL) == -EPERM);
        assert(flash_calls == 1); check_blocked();
    }
    reset_state(); complete_upload(); httpd_post_response_complete(&owner, ERR_OK);
    validation_result = -EBADMSG;
    assert(recovery_flash_uploaded_image(NULL) == -EBADMSG);
    assert(flash_calls == 0); check_blocked();

    /* Fail closed if an environment/FDT override substitutes another region. */
    struct recovery_target target = { 0 };
    assert(recovery_q1000k_target(TARGET_FIRMWARE, &target) == 0);
    partition.offset = 0;
    assert(recovery_q1000k_target(TARGET_FIRMWARE, &target) == -EINVAL);
    partition.offset = 0x700000;
    assert(recovery_q1000k_target(TARGET_UBOOT, &target) == (TEST_INSTALLER ? 0 : -EPERM));

    if (TEST_INSTALLER) {
        reset_state(); complete_upload(); httpd_post_response_complete(&owner, ERR_OK);
        preflight_result = -EOPNOTSUPP;
        assert(recovery_flash_uploaded_image(NULL) == -EOPNOTSUPP);
        assert(!flash_calls); check_blocked();
        const char *routes[] = { "/upload/uboot", "/upload/recovery", "/upload/uboot-only" };
        for (unsigned i = 0; i < 3; i++) {
            for (unsigned error = 0; error < 2; error++) {
                reset_state();
                struct pbuf data = { NULL, upload_buffer, sizeof(upload_buffer), sizeof(upload_buffer) };
                assert(httpd_post_begin(&owner, routes[i], "", 0, sizeof(upload_buffer),
                       response, sizeof(response), &wnd) == ERR_OK);
                assert(q1000k_uboot_only==(i==2));
                /* A second request cannot change the operation of an accepted POST. */
                assert(httpd_post_begin(&stranger, i==2?"/upload/uboot":"/upload/uboot-only", "", 0,
                       sizeof(upload_buffer), response, sizeof(response), &wnd) == ERR_ARG);
                assert(q1000k_uboot_only==(i==2));
                assert(httpd_post_receive_data(&owner, &data) == 0);
                httpd_post_finished(&owner, response, sizeof(response));
                assert(post_validated); check_blocked();
                httpd_post_response_complete(&stranger, ERR_OK);
                assert(!flash_request); check_blocked();
                httpd_post_response_complete(&owner, ERR_OK);
                assert(flash_request); check_blocked();
                flash_result = error ? -EIO : 0;
                assert(recovery_flash_uploaded_image(NULL) == flash_result);
                assert(flash_calls == 1); check_blocked();
                if(i!=1)assert(prepared_ubi==(i==0));
            }
        }
        /* Rejected/truncated update-only uploads never commit; mode resets on the next POST. */
        for(int failure=0;failure<4;failure++) {
            reset_state();
            struct pbuf data={NULL,upload_buffer,sizeof(upload_buffer),sizeof(upload_buffer)};
            assert(httpd_post_begin(&owner,"/upload/uboot-only","",0,sizeof(upload_buffer),
                   response,sizeof(response),&wnd)==ERR_OK);
            if(failure==0)validation_result=-EBADMSG;
            if(failure!=1)assert(!httpd_post_receive_data(&owner,&data));
            httpd_post_finished(&owner,response,sizeof(response));
            httpd_post_response_complete(&owner,failure==2?-EIO:ERR_OK);
            if(failure==3)preflight_result=-EOPNOTSUPP;
            flash_request=0; /* The server loop consumes the request before committing. */
            assert(recovery_flash_uploaded_image(NULL)<0);
            assert(!flash_calls);check_blocked();
            assert(httpd_post_begin(&owner,"/upload/uboot","",0,sizeof(upload_buffer),
                   response,sizeof(response),&wnd)==ERR_OK);
            assert(!q1000k_uboot_only);
        }
        reset_state();board_q1000k=false;
        assert(httpd_post_begin(&owner,"/upload/uboot-only","",0,sizeof(upload_buffer),
               response,sizeof(response),&wnd)==ERR_ARG);
    board_q1000k=true;
    }

    /* RAM uploads never resolve a NAND target or enter the write path. */
    for (int failure = 0; failure < 5; failure++) {
        reset_state();
        get_target_calls = 0;
        struct pbuf data = {NULL, upload_buffer, sizeof(upload_buffer), sizeof(upload_buffer)};
        assert(httpd_post_begin(&owner, "/upload/initramfs", "", 0, sizeof(upload_buffer),
               response, sizeof(response), &wnd) == (TEST_INSTALLER ? ERR_OK : ERR_ARG));
        assert(!get_target_calls);
        if (!TEST_INSTALLER) continue;
        assert(current_target == TARGET_INITRAMFS);
        assert(recv_base == ramboot_buffer);
        assert(httpd_post_begin(&stranger, "/upload/firmware", "", 0, sizeof(upload_buffer),
               response, sizeof(response), &wnd) == ERR_ARG);
        assert(current_target == TARGET_INITRAMFS);
        if (failure == 1) validation_result = -EBADMSG;
        if (failure != 2) assert(!httpd_post_receive_data(&owner, &data));
        httpd_post_finished(&owner, response, sizeof(response));
        httpd_post_response_complete(&stranger, ERR_OK);
        assert(!ramboot_request && !flash_request);
        httpd_post_response_complete(&owner, failure == 3 ? -EIO : ERR_OK);
        assert(!flash_request); check_blocked();
        if (failure == 4) validation_result = -EBADMSG;
        assert((recovery_prepare_ramboot() == 0) == (failure == 0));
        assert(!ramboot_request && !post_validated);
        assert(recovery_prepare_ramboot() == -EPERM);
        assert(!flash_calls); check_blocked();
    }
    /* Even an acknowledged RAM upload passed to the flash helper is rejected. */
    reset_state();
    current_target = TARGET_INITRAMFS;
    post_validated = true;
    post_ok = 1;
    recv_total = recv_off = sizeof(upload_buffer);
    assert(recovery_flash_uploaded_image(NULL) == -EPERM);
    assert(!flash_calls); check_blocked();
    reset_state(); board_q1000k = false;
    assert(httpd_post_begin(&owner, "/upload/initramfs", "", 0, sizeof(upload_buffer),
           response, sizeof(response), &wnd) == ERR_ARG);
    board_q1000k = true;

    if (TEST_INSTALLER) {
        /* Custom address reaches receive, validation and boot without resolving NAND. */
        reset_state(); get_target_calls = 0;
        snprintf(uri, sizeof(uri), "/upload/initramfs?addr=0x%lx", (ulong)ramboot_buffer + 4096);
        assert(httpd_post_begin(&owner, uri, "", 0, sizeof(upload_buffer),
               response, sizeof(response), &wnd) == ERR_OK);
        assert(recv_base == ramboot_buffer + 4096);
        snprintf(uri, sizeof(uri), "/upload/initramfs?addr=0x%lx", (ulong)ramboot_buffer);
        assert(httpd_post_begin(&stranger, uri, "", 0, sizeof(upload_buffer),
               response, sizeof(response), &wnd) == ERR_ARG);
        assert(recv_base == ramboot_buffer + 4096);
        memset(upload_buffer, 0x5a, sizeof(upload_buffer));
        struct pbuf data = {NULL, upload_buffer, sizeof(upload_buffer), sizeof(upload_buffer)};
        assert(!httpd_post_receive_data(&owner, &data));
        assert(!memcmp(recv_base, upload_buffer, sizeof(upload_buffer)));
        httpd_post_finished(&owner, response, sizeof(response));
        httpd_post_response_complete(&owner, ERR_OK);
        assert(!recovery_prepare_ramboot());
        boot_result = boot_env_result = boot_calls = 0;
        assert(recovery_boot_initramfs() < 0 && boot_calls == 1);
        assert(!get_target_calls && !flash_calls); check_blocked();
        reset_state();
        assert(httpd_post_begin(&owner, "/upload/initramfs", "", 0, sizeof(upload_buffer),
               response, sizeof(response), &wnd) == ERR_OK);
        assert(recv_base == ramboot_buffer); /* Omitted address returns to default. */
        reset_state(); begin_upload(); assert(recv_base == upload_buffer);

        reset_state();
        snprintf(uri, sizeof(uri), "/upload/initramfs?addr=0x%lx", recovery_q1000k_upload_top());
        assert(httpd_post_begin(&owner, uri, "", 0, sizeof(upload_buffer),
               response, sizeof(response), &wnd) == ERR_MEM);
        assert(!post_ok && !ramboot_request); check_blocked();
    }

    /* Handoff uses the staged FIT and restores temporary bootargs on return. */
    for (int saved = 0; saved < 2; saved++) {
        for (int failure = 0; failure < 3; failure++) {
            reset_state(); boot_calls = 0;
            boot_result = failure == 1 ? -EINVAL : 0;
            boot_env_result = failure == 2 ? -ENOMEM : 0;
            const char *original = saved ? "console=ttyS0 root=/dev/fit0 ubi.block=0,fit" : NULL;
            env_set("bootargs", original);
            assert(recovery_boot_initramfs() < 0);
            assert(boot_calls == (failure == 2 ? 0 : 1));
            assert(have_bootargs == (bool)saved);
            if (saved) assert(!strcmp(bootargs, original));
            assert(!flash_calls); check_blocked();
        }
    }

    /* Stream every data byte, including bad-block/reserved addresses, read-only. */
    reset_state();
    struct recovery_nand_backup_file backup = {
        .mtd = &master, .cache_capacity = 0x40000,
        .cache = malloc(0x40000), .bytes_left = 0x20000000
    };
    u8 output[16384];
    uint64_t delivered = 0;
    int got;
    read_result = -EUCLEAN; /* Corrected ECC must not cause a repair write. */
    while ((got = recovery_read_nand_backup(&backup, (char *)output, sizeof(output))) > 0) {
        for (int i = 0; i < got; i++)
            assert(output[i] == (((delivered + i) / 0x40000) & 255));
        delivered += got;
    }
    assert(delivered == 0x20000000 && read_bytes == delivered);
    check_blocked();
    backup.bytes_left = 0x40000;
    read_result = -EBADMSG;
    assert(recovery_read_nand_backup(&backup, (char *)output, sizeof(output)) == FS_READ_EOF);
    assert(!backup.bytes_left); check_blocked();
    free(backup.cache);
    puts("PASS: startup gate, upload ownership/validation/ACK, relock on failure, fixed target, 512 MiB read-only backup");
    return 0;
}
