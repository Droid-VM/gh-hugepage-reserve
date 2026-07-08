#!/bin/bash
set -e
TOP_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULES=(
	"gh_hugepage_reserve.c:gh_hugepage_reserve"
)
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

for TAG in "${TAGS[@]}"; do
	echo "============================================"
	echo "  Building all modules for ${TAG}"
	echo "============================================"

	DOCKER_VOLS=("-v" "${TOP_DIR}:/src:ro")
	MOD_ARGS=()
	for MOD_ENTRY in "${MODULES[@]}"; do
		MOD_NAME="${MOD_ENTRY##*:}"
		OUT_DIR="${TOP_DIR}/package/ko/${MOD_NAME}"
		mkdir -p "$OUT_DIR"
		DOCKER_VOLS+=("-v" "${OUT_DIR}:/out_${MOD_NAME}")
		MOD_ARGS+=("$MOD_ENTRY")
	done

	if docker run --rm "${DOCKER_VOLS[@]}" "${IMAGE}:${TAG}" \
		sh /src/docker_exec.sh "${MOD_ARGS[@]}"; then
		for MOD_ENTRY in "${MODULES[@]}"; do
			MOD_NAME="${MOD_ENTRY##*:}"
			TOTAL=$((TOTAL + 1))
			SUCCESS+=("${MOD_NAME}@${TAG}")
			echo "  -> OK: ${MOD_NAME}@${TAG}"
		done
	else
		for MOD_ENTRY in "${MODULES[@]}"; do
			MOD_NAME="${MOD_ENTRY##*:}"
			OUT_DIR="${TOP_DIR}/package/ko/${MOD_NAME}"
			TOTAL=$((TOTAL + 1))
			if [ -f "${OUT_DIR}/${TAG}.ko" ]; then
				SUCCESS+=("${MOD_NAME}@${TAG}")
				echo "  -> OK: ${MOD_NAME}@${TAG}"
			else
				FAILED+=("${MOD_NAME}@${TAG}")
				echo "  -> FAILED: ${MOD_NAME}@${TAG}"
			fi
		done
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

# Device binaries (kapi_check + balloon) are NOT committed: build them the
# same way CI does (QEMU arm64 container) so the local zip is complete.
# Best-effort - without arm64 docker support the zip ships without them
# (post-fs-data fail-opens: module loads, CMA reservoir stays off).
if docker run --rm --platform linux/arm64 -v "${TOP_DIR}:/src" debian:bookworm \
	bash -c 'apt-get update -qq >/dev/null && \
		apt-get install -y -qq gcc libbpf-dev libelf-dev zlib1g-dev libzstd-dev >/dev/null && \
		gcc -O2 -Wall -I/src/abi /src/abi/kapi_check.c -lbpf -lelf -lz -lzstd -static -o /src/package/kapi_check && \
		gcc -O2 -Wall -static -o /src/package/balloon /src/tools/balloon.c'; then
	echo "built package/kapi_check + package/balloon (aarch64 static)"
else
	echo "WARNING: device binaries not rebuilt (arm64 docker unavailable)"
fi

pushd package
rm -f "${TOP_DIR}/gh-hugepage-reserve.zip"
zip -r "${TOP_DIR}/gh-hugepage-reserve.zip" .
popd
ls -lh "${TOP_DIR}/gh-hugepage-reserve.zip"
