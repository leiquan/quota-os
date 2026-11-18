#!/bin/bash
# Link upstream JavaScriptCore CLoop into a Quota OS image.
# Does not copy the result over prebuilt/.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
CXX=aarch64-linux-gnu-g++
CC=aarch64-linux-gnu-gcc
KFLAGS=(-std=gnu11 -Wall -Wextra -Werror -O2 -ffreestanding -fno-stack-protector
    -fno-builtin -fno-pic -fno-pie -ftls-model=local-exec -mgeneral-regs-only -mstrict-align
    -fno-asynchronous-unwind-tables -I include -I build)
mkdir -p build/kernel build
short=$(git rev-parse --short=7 HEAD)
printf '#define QUOTAOS_COMMIT "%s"\n' "$short" > build/commit.h
echo "banner parent $short"
for f in kernel/main.c kernel/resource.c kernel/color.c kernel/fb.c kernel/webkit.c kernel/wkalloc.c kernel/sched.c kernel/uart.c kernel/jsc_sys.c kernel/jsc_icu_stubs.c kernel/mmu.c; do
    $CC "${KFLAGS[@]}" -c -o "build/${f%.c}.o" "$f"
done
INCS=$(cat /tmp/jsc-build/incs.line)
$CXX -std=c++23 -Os -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector \
    -ftls-model=local-exec -ffunction-sections -fdata-sections -Wno-expansion-to-defined -w \
    -DENABLE_C_LOOP=1 -DENABLE_JIT=0 -DENABLE_YARR_JIT=0 -DENABLE_WEBASSEMBLY=0 \
    -DGIGACAGE_ENABLED=0 -DUSE_SYSTEM_MALLOC=1 \
    -I /tmp/jsc-build/include -I /tmp/jsc-build/gen \
    $INCS -idirafter /usr/include \
    -c port/quotaos/jsc_guest.cpp -o build/jsc_guest.o
$CXX -std=c++23 -Os -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector \
    -ffunction-sections -fdata-sections -w \
    -c port/quotaos/jsc_link_stubs.cpp -o build/jsc_link_stubs.o

python3 - << 'PY'
import os
drop={"WTF_wtf_generic_MemoryFootprintGeneric.cpp.o","WTF_wtf_generic_MemoryPressureHandlerGeneric.cpp.o","bmalloc_bmalloc_mbmalloc.cpp.o"}
root="/tmp/jsc-build/obj-sm"
lines=[]
for n in sorted(os.listdir(root)):
    if n.endswith(".o") and n not in drop:
        lines.append(root+"/"+n)
open("/tmp/jsc-build/objs-guest.rsp","w").write("\n".join(lines)+"\n")
print("jsc inputs", len(lines), "system malloc, no libpas")
PY

"$ROOT/tools/build-webcore-color.sh"
mapfile -t KERN < <(find build/boot build/kernel -name '*.o' | sort)
ld.lld -m aarch64linux -T boot/linker.ld --gc-sections \
    --no-undefined \
    -o build/quotaos-jsc.elf \
    "${KERN[@]}" \
    build/jsc_guest.o \
    build/jsc_link_stubs.o \
    @build/webcore-color.rsp \
    /tmp/jsc-build/std/tree.o \
    /tmp/jsc-build/std/hashtable_c++0x.o \
    @/tmp/jsc-build/objs-guest.rsp \
    /usr/aarch64-linux-gnu/lib/libm-2.39.a \
    /usr/lib/gcc-cross/aarch64-linux-gnu/13/libatomic.a \
    /usr/lib/gcc-cross/aarch64-linux-gnu/13/libgcc.a \
    > /tmp/jsc-build/lld-guest.log 2>&1 || {
        echo LINK_FAIL
        rg "^ld.lld:" /tmp/jsc-build/lld-guest.log | head -30
        exit 1
    }
echo LINK_OK
aarch64-linux-gnu-size build/quotaos-jsc.elf
echo "undef $(aarch64-linux-gnu-nm -u build/quotaos-jsc.elf | wc -l)"
aarch64-linux-gnu-objcopy -O binary build/quotaos-jsc.elf build/quotaos-jsc.bin
ls -l build/quotaos-jsc.bin
