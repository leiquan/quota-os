#!/bin/bash
# LVGL guest. Upstream tree: third_party/lvgl (v9.3.0, see PIN.txt).
# Writes build/quotaos-lvgl.bin. After a QEMU check, that file is installed
# as prebuilt/quotaos.bin and copied to prebuilt/quotaos-lvgl.bin.
# Does not touch prebuilt/quotaos-serial.bin, quotaos-jsc.bin, or history.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
CC=${CC:-aarch64-linux-gnu-gcc}
CFLAGS=(-std=gnu11 -Wall -Wextra -Werror -O2 -ffreestanding -fno-stack-protector
    -fno-builtin -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
    -fno-asynchronous-unwind-tables -I include -I build)
mkdir -p build
short=$(git rev-parse --short=7 HEAD)
printf '#define QUOTAOS_COMMIT "%s"\n' "$short" > build/commit.h
echo "banner parent $short"
srcs=(
    boot/startup.S boot/vectors.S
    kernel/main.c kernel/uart.c kernel/printf.c kernel/string.c
    kernel/dtb.c kernel/gic.c kernel/timer.c kernel/resource.c
    kernel/sched.c kernel/demo.c kernel/decl.c kernel/ipc.c kernel/dev.c
    kernel/color.c kernel/e4.c kernel/fb.c kernel/input.c kernel/lvgl_boot.c
    kernel/webkit.c kernel/wkalloc.c kernel/cxxrt.cpp kernel/cxxtest.cpp
)
objs=()
for f in "${srcs[@]}"; do
    obj="build/${f%.*}.o"
    mkdir -p "$(dirname "$obj")"
    if [[ $f == *.cpp ]]; then
        aarch64-linux-gnu-g++ -std=c++20 -Wall -Wextra -Werror -O2 -ffreestanding \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector \
            -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align \
            -fno-asynchronous-unwind-tables -I include -I build -c -o "$obj" "$f"
    else
        $CC "${CFLAGS[@]}" -c -o "$obj" "$f"
    fi
    objs+=("$obj")
done
LVFLAGS=(-std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-builtin
    -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
    -fno-asynchronous-unwind-tables -Wno-unused-parameter -w
    -DLV_CONF_INCLUDE_SIMPLE -I third_party/lvgl -I port/quotaos -I include)
mkdir -p build/lvgl build/port
while IFS= read -r src; do
    rel=${src#third_party/lvgl/}
    obj="build/lvgl/${rel%.c}.o"
    mkdir -p "$(dirname "$obj")"
    $CC "${LVFLAGS[@]}" -c -o "$obj" "$src"
    objs+=("$obj")
done < <(find third_party/lvgl/src/libs/bin_decoder -name '*.c'; find third_party/lvgl/src \
    -path 'third_party/lvgl/src/libs' -prune -o \
    -path 'third_party/lvgl/src/drivers' -prune -o \
    -path 'third_party/lvgl/src/draw/vg_lite' -prune -o \
    -path 'third_party/lvgl/src/draw/nema_gfx' -prune -o \
    -path 'third_party/lvgl/src/draw/nxp' -prune -o \
    -path 'third_party/lvgl/src/draw/sdl' -prune -o \
    -path 'third_party/lvgl/src/draw/opengles' -prune -o \
    -path 'third_party/lvgl/src/draw/dma2d' -prune -o \
    -path 'third_party/lvgl/src/draw/renesas' -prune -o \
    -name '*.c' -print | sort)
$CC "${LVFLAGS[@]}" -c -o build/port/lvgl_demo.o port/quotaos/lvgl_demo.c
objs+=(build/port/lvgl_demo.o)
$CC -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none \
    -T boot/linker.ld -o build/quotaos-lvgl.elf "${objs[@]}" -lgcc
aarch64-linux-gnu-objcopy -O binary build/quotaos-lvgl.elf build/quotaos-lvgl.bin
echo "linked $(wc -c < build/quotaos-lvgl.bin) bytes"
aarch64-linux-gnu-nm build/quotaos-lvgl.elf | rg "lvgl_demo_run|__image_end" || true
