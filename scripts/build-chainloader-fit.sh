#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0+
# Build the Q1000K RAM-bootable FIT, without accessing the device.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
package_only=false
output=

usage() {
	cat <<'EOF'
Usage: scripts/build-chainloader-fit.sh [--package-only] [OUTPUT.itb]

Build q1000k_defconfig and package u-boot.bin with the Q1000K U-Boot DTB.
The default output is q1000k-chainload-uboot.itb in the source directory.
A relative OUTPUT path is relative to the caller's working directory.

Options:
  --package-only  Package an existing build after checking its configuration.
  -h, --help      Show this help.

Environment:
  CROSS_COMPILE  Compiler prefix (default: aarch64-linux-gnu-).
  JOBS           Parallel build jobs (default: online CPU count).
  STAGING_DIR    Set this when using an OpenWrt cross compiler.

Example:
  CROSS_COMPILE=aarch64-linux-gnu- ./scripts/build-chainloader-fit.sh
EOF
}

die() {
	printf 'build-chainloader-fit: %s\n' "$*" >&2
	exit 1
}

while (($#)); do
	case "$1" in
		--package-only) package_only=true ;;
		-h|--help) usage; exit 0 ;;
		-*) die "Unknown option: $1" ;;
		*)
			[[ -z "$output" ]] || die 'Only one output path is allowed.'
			output=$1
			;;
	esac
	shift
done
output=${output:-"$repo/q1000k-chainload-uboot.itb"}
[[ "$output" == *.itb ]] || die 'The output filename must end in .itb.'
[[ ! -d "$output" ]] || die 'The output path is a directory.'

if ! "$package_only"; then
	cross_compile=${CROSS_COMPILE:-aarch64-linux-gnu-}
	jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
	[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die 'JOBS must be a positive integer.'
	command -v "${cross_compile}gcc" >/dev/null ||
		die "Compiler not found: ${cross_compile}gcc; set CROSS_COMPILE."
	make -C "$repo" CROSS_COMPILE="$cross_compile" q1000k_defconfig
	make -C "$repo" -j"$jobs" CROSS_COMPILE="$cross_compile"
fi

config="$repo/.config"
[[ -f "$config" ]] || die 'Missing .config; build q1000k_defconfig first.'
for setting in \
	CONFIG_ARM64=y \
	CONFIG_TARGET_AN7581=y \
	'CONFIG_DEFAULT_DEVICE_TREE="airoha/q1000k"' \
	CONFIG_OF_SEPARATE=y \
	CONFIG_ENV_IS_NOWHERE=y \
	CONFIG_HTTPD_RECOVERY=y \
	CONFIG_Q1000K_INSTALLER=y \
	CONFIG_AIROHA_SNFI_NAND_WRITE_GUARD=y; do
	grep -Fxq "$setting" "$config" || die "Required configuration: $setting"
done
grep -Eq '^CONFIG_TEXT_BASE=0x81[eE]00000$' "$config" ||
	die 'CONFIG_TEXT_BASE must match the FIT load/entry address 0x81e00000.'

payload="$repo/u-boot.bin"
dtb="$repo/dts/upstream/src/arm64/airoha/q1000k.dtb"
template="$repo/board/airoha/an7581/q1000k-chainloader.its"
for input in "$payload" "$dtb" "$template"; do
	[[ -s "$input" ]] || die "Missing or empty input: $input"
done
for tool in tools/mkimage tools/dumpimage scripts/dtc/dtc; do
	[[ -x "$repo/$tool" ]] || die "Missing build tool: $repo/$tool"
done

mkdir -p -- "$(dirname -- "$output")"
output_dir=$(cd -- "$(dirname -- "$output")" && pwd)
output="$output_dir/$(basename -- "$output")"
tmpdir=$(mktemp -d "$output_dir/.q1000k-fit.XXXXXX")
trap 'rm -rf -- "$tmpdir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Keep /incbin/ paths relative to the ITS so caller paths need no substitution.
cp -- "$payload" "$tmpdir/u-boot.bin"
cp -- "$dtb" "$tmpdir/q1000k.dtb"
cp -- "$template" "$tmpdir/chainloader.its"
PATH="$repo/scripts/dtc:$PATH" "$repo/tools/mkimage" \
	-f "$tmpdir/chainloader.its" "$tmpdir/chainloader.itb"

size=$(wc -c < "$tmpdir/chainloader.itb")
((size <= 0x100000)) || die "FIT is $size bytes; the limit is 1048576 (1 MiB)."

# Extraction checks ensure the FIT contains the raw payload and matching DTB.
"$repo/tools/dumpimage" -T flat_dt -p 0 -o "$tmpdir/kernel.bin" \
	"$tmpdir/chainloader.itb" >/dev/null
"$repo/tools/dumpimage" -T flat_dt -p 1 -o "$tmpdir/fdt.dtb" \
	"$tmpdir/chainloader.itb" >/dev/null
cmp -- "$tmpdir/u-boot.bin" "$tmpdir/kernel.bin"
cmp -- "$tmpdir/q1000k.dtb" "$tmpdir/fdt.dtb"

# Replace the previous artifact only after packaging and verification succeed.
mv -- "$tmpdir/chainloader.itb" "$output"
printf '\nBuilt %s (%s bytes)\n' "$output" "$size"
sha256sum -- "$output"
