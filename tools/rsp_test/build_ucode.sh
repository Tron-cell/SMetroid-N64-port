#!/bin/bash
# build_ucode.sh foo.S -> foo.text.bin foo.data.bin (+ foo.elf)
X=/opt/libdragon/bin/mips64-elf; N=${1%.S}
$X-gcc -march=mips1 -mabi=32 -Wa,--fatal-warnings -I/opt/libdragon/mips64-elf/include -L/opt/libdragon/mips64-elf/lib \
  -nostartfiles -Wl,-Trsp.ld -Wl,--gc-sections -o $N.elf $1 || exit 1
$X-objcopy -O binary -j .text $N.elf $N.text.bin && $X-objcopy -O binary -j .data $N.elf $N.data.bin
echo "$N: text $(stat -c%s $N.text.bin) bytes ($(( $(stat -c%s $N.text.bin) / 4 )) instr), data $(stat -c%s $N.data.bin) bytes"
