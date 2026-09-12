/* SPDX-License-Identifier: GPL-2.0+ */
/* Host adapters: real libfdt parsing and OpenSSL/zlib hash verification. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <libfdt.h>
#include <openssl/evp.h>
#include <zlib.h>
typedef uint8_t u8;
#define FIT_IMAGES_PATH "/images"
#define FIT_HASH_NODENAME "hash"
#define FIT_KERNEL_PROP "kernel"
#define FIT_FDT_PROP "fdt"
#define IH_PHASE_NONE 0
#define IH_TYPE_KERNEL 2
#define IH_ARCH_ARM64 22
#define IH_OS_LINUX 5
#define IH_COMP_NONE 0
#define fit_get_name fdt_get_name

static int fit_image_get_data(const void *fit, int node, const void **data, size_t *size) {
    int len;
    const fdt32_t *pos = fdt_getprop(fit, node, "data-position", &len);
    size_t offset = 0;
    if (!pos) {
        pos = fdt_getprop(fit, node, "data-offset", &len);
        offset = (fdt_totalsize(fit) + 3U) & ~3U;
    }
    if (pos) {
        if (len != 4) return -EINVAL;
        offset += fdt32_to_cpu(*pos);
        pos = fdt_getprop(fit, node, "data-size", &len);
        if (!pos || len != 4) return -EINVAL;
        *size = fdt32_to_cpu(*pos);
        *data = (const char *)fit + offset;
        return 0;
    }
    *data = fdt_getprop(fit, node, "data", &len);
    if (!*data || len < 0) return -EINVAL;
    *size = len; return 0;
}
static int fit_conf_get_node(const void *fit, const char *name) {
    int node = fdt_path_offset(fit, "/configurations");
    if (node < 0) return node;
    if (!name) name = fdt_getprop(fit, node, "default", NULL);
    return name ? fdt_subnode_offset(fit, node, name) : -EINVAL;
}
static int fit_conf_get_prop_node(const void *fit, int conf, const char *prop, int phase) {
    const char *name = fdt_getprop(fit, conf, prop, NULL);
    int images = fdt_path_offset(fit, "/images");
    return name && images >= 0 ? fdt_subnode_offset(fit, images, name) : -EINVAL;
}
static int get_enum(const void *fit, int node, const char *prop, const char *want, u8 value, u8 *out) {
    const char *str = fdt_getprop(fit, node, prop, NULL);
    if (!str || strcmp(str, want)) return -EINVAL;
    *out = value; return 0;
}
#define ENUM_HELPER(fn, prop, str, value) \
static int fn(const void *fit, int node, u8 *out) { return get_enum(fit, node, prop, str, value, out); }
ENUM_HELPER(fit_image_get_type, "type", "kernel", IH_TYPE_KERNEL)
ENUM_HELPER(fit_image_get_arch, "arch", "arm64", IH_ARCH_ARM64)
ENUM_HELPER(fit_image_get_os, "os", "linux", IH_OS_LINUX)
ENUM_HELPER(fit_image_get_comp, "compression", "none", IH_COMP_NONE)
static int fit_all_image_verify(const void *fit) {
    int images = fdt_path_offset(fit, "/images"), node, hash;
    fdt_for_each_subnode(node, fit, images) {
        const void *data; size_t size;
        if (fit_image_get_data(fit, node, &data, &size)) return 0;
        fdt_for_each_subnode(hash, fit, node) {
            if (strncmp(fdt_get_name(fit, hash, NULL), "hash", 4)) continue;
            const char *algo = fdt_getprop(fit, hash, "algo", NULL);
            int want_len;
            const void *want = fdt_getprop(fit, hash, "value", &want_len);
            unsigned char digest[EVP_MAX_MD_SIZE]; unsigned len;
            if (!algo || !want) return 0;
            if (!strcmp(algo, "crc32")) {
                fdt32_t crc = cpu_to_fdt32(crc32(0, data, size));
                memcpy(digest, &crc, 4); len = 4;
            } else {
                const EVP_MD *md = EVP_get_digestbyname(algo);
                if (!md || !EVP_Digest(data, size, digest, &len, md, NULL)) return 0;
            }
            if (len != (unsigned)want_len || memcmp(digest, want, len)) return 0;
        }
    }
    return 1;
}

/* INSERT PRODUCTION CODE */

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 2;
    fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
    void *data = malloc(size);
    if (!data || fread(data, 1, size, file) != (size_t)size) return 2;
    fclose(file);
    int ret = recovery_validate_q1000k_fit(data, size);
    if (ret) fprintf(stderr, "FIT rejected: %d\n", ret);
    free(data);
    return ret ? 1 : 0;
}
