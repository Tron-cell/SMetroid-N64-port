#!/bin/bash
# Builds the lockstep harness from a source tree.
#   ref    : x86, plain C (the original PC semantics = the oracle)
#   refcpp : x86, C++ with the LeInt wrapper types (checks wrapper semantics)
#   be.elf : big-endian MIPS VR4300 (o64, like libdragon), C++ with wrappers
# usage: build.sh <src_dir> <out_dir> [frames] [dump_frame]
set -e
S=$(realpath $1); O=$2; FR=${3:-3000}; DF=${4:--1}; SC=${5:-0}; R=$(dirname $(realpath $0))
mkdir -p $O/obj $O/objx
GAME=$(ls $S/*.c); SNES=$(ls $S/snes/*.c)
COMMON="-DN64 -DRIG -w -fno-strict-aliasing -iquote $S"
CXX="-x c++ -std=gnu++20 -fpermissive -fno-exceptions -fno-rtti -fno-threadsafe-statics"
par() { while [ $(jobs -r | wc -l) -ge 4 ]; do sleep 0.05; done; }
# x86 reference (C)
gcc -O1 $COMMON -o $O/ref $R/rig_main.c $GAME $SNES -lm &
# x86 C++ + wrappers
objs=""
for f in $R/rig_main.c $GAME; do o=$O/objx/$(basename ${f%.c}).o; par; g++ -O1 -g $COMMON $CXX -DSM_LE_TYPES -c $f -o $o & objs="$objs $o"; done
for f in $SNES; do o=$O/objx/snes_$(basename ${f%.c}).o; par; g++ -O1 -g $COMMON $CXX -c $f -o $o & objs="$objs $o"; done
wait
g++ -o $O/refcpp $objs -lm
# big-endian MIPS
X=/opt/libdragon/bin/mips64-elf
CF="-march=vr4300 -mtune=vr4300 -mabi=o64 -O2 -g $BE_EXTRA $COMMON -DRIG_BE -DRIG_FRAMES=$FR -DRIG_DUMP_FRAME=$DF -DRIG_SCRIPT=$SC"
objs=""
for f in $R/rig_main.c $GAME; do o=$O/obj/$(basename ${f%.c}).o; par; $X-gcc $CF $CXX -DSM_LE_TYPES -c $f -o $o & objs="$objs $o"; done
for f in $SNES; do o=$O/obj/snes_$(basename ${f%.c}).o; par; $X-gcc $CF $CXX -c $f -o $o & objs="$objs $o"; done
wait
$X-gcc -march=vr4300 -mabi=o64 -nostartfiles -T $R/link.ld -o $O/be.elf $R/crt0.o $R/sys.o $objs -lc -lm -lgcc
echo built
