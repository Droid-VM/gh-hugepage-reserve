#!/bin/bash
# In-DDK-container build for one GKI tag. Mirrors the old rig: copy the unity
# root + parts/ + abi/ into a build dir and make external modules against the
# tag's GKI tree with its clang. Args: "<src.c>:<modname>" [more...].
set -e
CLANG_DIR=$(ls -d /opt/ddk/clang/clang-*)
KMI=$(ls /opt/ddk/kdir/)
KDIR="/opt/ddk/kdir/${KMI}"
TAG="$KMI"
export PATH="${CLANG_DIR}/bin:${PATH}"
BUILD_DIR="/tmp/build"
mkdir -p "$BUILD_DIR"

# unity build inputs: parts/ and abi/ must sit next to the module .c so its
# #include "parts/..." and #include "abi/kapi_abi.gen.h" resolve.
[ -d /src/parts ] && { mkdir -p "$BUILD_DIR/parts"; cp /src/parts/* "$BUILD_DIR/parts/"; }
[ -d /src/abi ]   && { mkdir -p "$BUILD_DIR/abi";   cp /src/abi/*   "$BUILD_DIR/abi/" 2>/dev/null || true; }
# regenerate the ABI header from the TSV so .ko and the userspace preflight agree
if command -v awk >/dev/null 2>&1 && [ -f /src/abi/gen_kapi.awk ]; then
	awk -f /src/abi/gen_kapi.awk /src/abi/kapi_abi.tsv > "$BUILD_DIR/abi/kapi_abi.gen.h"
fi

MAKEFILE=""
for ENTRY in "$@"; do
	SRC="${ENTRY%%:*}"; MOD="${ENTRY##*:}"
	MAKEFILE="${MAKEFILE}obj-m += ${MOD}.o
"
	cp "/src/${SRC}" "${BUILD_DIR}/${MOD}.c"
done
# unity build => 'public' cross-part api is file-scope; silence its warnings.
printf 'ccflags-y += -Wno-missing-prototypes -Wno-unused-function\n%s' "$MAKEFILE" \
	> "${BUILD_DIR}/Makefile"

FAIL=0
make -C "${KDIR}" -j "$(nproc)" M="${BUILD_DIR}" ARCH=arm64 LLVM=1 LLVM_IAS=1 modules || FAIL=1
for ENTRY in "$@"; do
	MOD="${ENTRY##*:}"
	if [ -f "${BUILD_DIR}/${MOD}.ko" ]; then
		llvm-strip -d "${BUILD_DIR}/${MOD}.ko"
		cp "${BUILD_DIR}/${MOD}.ko" "/out_${MOD}/${TAG}.ko"
		echo "OK: ${MOD}@${TAG}"
	else
		echo "FAIL: ${MOD}@${TAG}"
	fi
done
exit $FAIL
