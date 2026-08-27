#!/bin/bash
set -e
TOP_DIR="$(cd "$(dirname "$0")" && pwd)"
MOD_NAME=gh_hugepage_reserve
# Older KMIs (5.10 / 5.15) cannot build the real module (it needs 6.1+ mm
# internals); they get a functionless placeholder instead (see placeholder.c),
# so the Magisk module still installs and loads on those kernels. The build loop
# tries the real module first and falls back to the placeholder per tag, so this
# list can carry every KMI without splitting it into "real" vs "placeholder".
TAGS=(
	android12-5.10
	android13-5.10
	android13-5.15
	android14-5.15
	android14-6.1
	android15-6.6
	android16-6.12
)

SUCCESS=()
FAILED=()
TOTAL=0

if [ "${USE_CLASSFUN}" == true ]; then
	IMAGE="10.44.0.51/ghcr/ddk-min"
else
	IMAGE="ghcr.io/ylarod/ddk-min"
fi

rm -f "${TOP_DIR}/package/ko/"*/*.ko

# Regenerate the ABI single-source header from the TSV so the .ko (compiled in)
# and the userspace preflight (abi/kapi_check) share one definition. See abi/.
if command -v awk >/dev/null 2>&1 && [ -f "${TOP_DIR}/abi/gen_kapi.awk" ]; then
	awk -f "${TOP_DIR}/abi/gen_kapi.awk" "${TOP_DIR}/abi/kapi_abi.tsv" \
		> "${TOP_DIR}/abi/kapi_abi.gen.h"
	echo "generated abi/kapi_abi.gen.h"
fi

OUT_DIR="${TOP_DIR}/package/ko/${MOD_NAME}"
mkdir -p "$OUT_DIR"

# Build one tag from a given source (real module or placeholder). docker_exec.sh
# copies /src/<src> to build/<MOD_NAME>.c, so both produce a gh_hugepage_reserve.ko.
build_one() {  # <tag> <src.c>
	docker run --rm -v "${TOP_DIR}:/src:ro" -v "${OUT_DIR}:/out_${MOD_NAME}" \
		"${IMAGE}:${1}" sh /src/docker_exec.sh "${2}:${MOD_NAME}" || true
	[ -f "${OUT_DIR}/${1}.ko" ]
}

for TAG in "${TAGS[@]}"; do
	echo "============================================"
	echo "  Building ${MOD_NAME} for ${TAG}"
	echo "============================================"
	TOTAL=$((TOTAL + 1))
	rm -f "${OUT_DIR}/${TAG}.ko"
	if build_one "$TAG" gh_hugepage_reserve.c; then
		SUCCESS+=("real@${TAG}")
		echo "  -> OK (real): ${TAG}"
	else
		echo "  real module did not build on ${TAG}; falling back to placeholder"
		if build_one "$TAG" placeholder.c; then
			SUCCESS+=("placeholder@${TAG}")
			echo "  -> OK (placeholder): ${TAG}"
		else
			FAILED+=("${TAG}")
			echo "  -> FAILED (both real and placeholder): ${TAG}"
		fi
	fi
	echo ""
done

echo "============================================"
echo "  Build Summary"
echo "============================================"
echo "Success (${#SUCCESS[@]}/${TOTAL}):"
for t in "${SUCCESS[@]}"; do echo "  ✓ $t"; done
if [ ${#FAILED[@]} -gt 0 ]; then
	echo "Failed (${#FAILED[@]}/${TOTAL}):"
	for t in "${FAILED[@]}"; do echo "  ✗ $t"; done
	exit 1
fi
echo "All builds succeeded."

# Device binary (kapi_check) is NOT committed: build it the same way CI does
# (QEMU arm64 container) so the local zip is complete. load.sh runs it against
# /sys/kernel/btf/vmlinux for the ABI + MIGRATE_CMA preflight.
# Best-effort - without arm64 docker support the zip ships without it
# (load.sh fail-opens: module loads, CMA reservoir stays off).
if docker run --rm --platform linux/arm64 -v "${TOP_DIR}:/src" debian:bookworm \
	bash -c 'apt-get update -qq >/dev/null && \
		apt-get install -y -qq gcc libbpf-dev libelf-dev zlib1g-dev libzstd-dev >/dev/null && \
		gcc -O2 -Wall -I/src/abi /src/abi/kapi_check.c -lbpf -lelf -lz -lzstd -static -o /src/package/kapi_check'; then
	echo "built package/kapi_check (aarch64 static)"
else
	echo "WARNING: kapi_check not rebuilt (arm64 docker unavailable)"
fi

pushd package
rm -f "${TOP_DIR}/gh-hugepage-reserve.zip"
zip -r "${TOP_DIR}/gh-hugepage-reserve.zip" .
popd
ls -lh "${TOP_DIR}/gh-hugepage-reserve.zip"
