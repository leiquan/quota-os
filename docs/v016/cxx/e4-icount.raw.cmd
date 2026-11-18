timeout_s: 120
kill: SIGKILL
mode: icount
kernel: prebuilt/quotaos.bin
command:
qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 128M -nographic -nic none -kernel prebuilt/quotaos.bin -append e4 -icount shift=4,sleep=off
