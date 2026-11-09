#!/bin/bash
# Rebuild, run x86 reference + big-endian MIPS, report first divergence or crash location.
# usage: iterate.sh <frames> <script>
FR=${1:-3000}; SC=${2:-0}; S=/home/claude/n64/sm_n64/src; ROM=/home/claude/n64/sm_n64/filesystem/sm.smc
cd $(dirname $0); T=s${SC}_${FR}
O=out_$T; ./build.sh $S $O $FR -1 $SC >build_$T.log 2>&1 || { grep -E "error" build_$T.log | head; exit 1; }
./$O/ref $ROM $FR -1 /dev/null $SC > ref_$T.txt
timeout 7200 ./${HOST:-host} $O/be.elf $ROM > be_$T.txt; rc=$?
n=$(grep -c ^F be_$T.txt)
first=$(diff <(grep ^F ref_$T.txt | head -$n) <(grep ^F be_$T.txt) | grep '^>' | head -1)
echo "rc=$rc frames_run=$n of $FR (script $SC)"
if [ -n "$first" ]; then
  f=$(echo "$first" | cut -c3-8); echo "FIRST DIFF at frame $f"; grep "^F$f" ref_$T.txt; echo "$first" | cut -c3-
else echo "all $n frames match"; fi
if grep -q "CPU EXCEPTION" be_$T.txt; then
  grep "CPU EXCEPTION\|recent" be_$T.txt
  for a in $(grep "recent blocks" be_$T.txt | cut -d: -f2 | awk '{print $1, $2, $3}'); do
    /opt/libdragon/bin/mips64-elf-addr2line -f -i -C -e $O/be.elf 0x$a | paste - - | head -2; done
fi
