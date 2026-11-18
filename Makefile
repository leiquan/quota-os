# 一条命令：make
# 产物：build/quotaos.elf / build/quotaos.bin，并复制到 prebuilt/
# 构建可逐字节复现：逐个文件编译到 build/，不留临时文件名、构建路径和 build-id。
CROSS ?= aarch64-linux-gnu-
CC := $(CROSS)gcc
CXX := $(CROSS)g++

CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O2 \
	-ffreestanding -fno-stack-protector -fno-builtin -fno-pic -fno-pie \
	-mgeneral-regs-only -mstrict-align -fno-asynchronous-unwind-tables \
	-ffile-prefix-map=$(CURDIR)/= -I include -I build
LDFLAGS := -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none

# 启动横幅里的提交号：构建时的 git rev-parse --short=7 HEAD。
# 每个版本的提交连同 prebuilt/ 一起提交，所以横幅显示的是这个版本所基于的提交（它的父提交）。
# 映像里不放时间戳。
SRC_COMMIT := $(shell git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)

SRCS := boot/startup.S boot/vectors.S \
	kernel/main.c kernel/uart.c kernel/printf.c kernel/string.c \
	kernel/dtb.c kernel/gic.c kernel/timer.c kernel/resource.c kernel/mmu.c \
	kernel/sched.c kernel/demo.c kernel/decl.c kernel/ipc.c kernel/dev.c kernel/color.c kernel/e4.c \
	kernel/fb.c kernel/input.c kernel/lvgl_boot.c kernel/webkit.c kernel/wkalloc.c \
	kernel/proc.c kernel/isolate.c \
	kernel/cxxrt.cpp kernel/cxxtest.cpp
OBJS := $(patsubst %,build/%.o,$(basename $(SRCS)))

CXXFLAGS := -std=c++20 -Wall -Wextra -Werror -O2 \
	-ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-stack-protector -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align \
	-fno-asynchronous-unwind-tables -ffile-prefix-map=$(CURDIR)/= \
	-I include -I build

# WebKit tag WebKit-7625.2.8.18.3, commit 4d967f4ca7bd. Not part of `make`.
WEBKIT_SRC ?= /tmp/WebKit

.PHONY: all clean run run-small webkit-osalloc-check webkit-jsc-flags-check

# prebuilt/quotaos.bin is the known-good serial drop-in. `make` does not copy over it.
all: build/quotaos.elf build/quotaos.bin

# 提交号变了才重写这个文件，main.c 依赖它。
build/commit.h: FORCE
	@mkdir -p build
	@echo '#define QUOTAOS_COMMIT "$(SRC_COMMIT)"' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

build/kernel/main.o: build/commit.h

build/user/paint.elf: user/paint.S user/user.ld
	@mkdir -p build/user
	$(CC) -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -T user/user.ld -o $@ user/paint.S

build/user/paint.bin: build/user/paint.elf
	$(CROSS)objcopy -O binary $< $@

build/kernel/proc.o: build/user/paint.bin

.PHONY: FORCE
FORCE:

build/%.o: %.c include/quotaos.h Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: %.S Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: %.cpp include/quotaos.h Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Typecheck the real WTF::OSAllocator methods. Does not link them in:
# the header needs libstdc++, and this kernel does not.
webkit-osalloc-check:
	$(CXX) -std=c++23 -O2 -Wall -Wextra -Werror -Wno-expansion-to-defined \
		-fno-exceptions -fno-rtti \
		-fno-threadsafe-statics -DRELEASE_WITHOUT_OPTIMIZATIONS \
		-DBASSERT_ENABLED=0 -DNDEBUG \
		-I $(WEBKIT_SRC)/Source/WTF -I $(WEBKIT_SRC)/Source/bmalloc \
		-c port/quotaos/OSAllocatorQuotaOS.cpp -o build/OSAllocatorQuotaOS.o

webkit-jsc-flags-check:
	$(CXX) -std=c++20 -O2 -Wall -Wextra -Werror -Wno-expansion-to-defined \
		-I $(WEBKIT_SRC)/Source/WTF \
		-c port/quotaos/jsc_interpreter_only.cpp -o build/jsc_interpreter_only.o

build/quotaos.elf: $(OBJS) boot/linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -T boot/linker.ld -o $@ $(OBJS) -lgcc

build/quotaos.bin: build/quotaos.elf
	$(CROSS)objcopy -O binary $< $@

prebuilt/quotaos.elf: build/quotaos.elf
	@mkdir -p prebuilt
	cp $< $@

prebuilt/quotaos.bin: build/quotaos.bin
	@mkdir -p prebuilt
	cp $< $@

# 用原始映像启动，QEMU 才会按 Linux 引导协议把设备树指针放进 x0。
run: build/quotaos.bin
	qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 128M \
		-nographic -kernel build/quotaos.bin

run-small: build/quotaos.bin
	qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 64M \
		-nographic -kernel build/quotaos.bin

clean:
	rm -rf build
