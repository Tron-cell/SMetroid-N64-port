#!/bin/bash
# Capture + C reference renderer vs original renderer, x86, both scripts. Also checks emulation is unchanged.
S=/home/claude/n64/sm_n64/src; ROM=/home/claude/n64/sm_n64/filesystem/sm.smc; cd $(dirname $0)
gcc -O2 -DN64 -DRIG -DRIG_PPUCHECK -w -fno-strict-aliasing -iquote $S -iquote . -o pref rig_main.c $(ls $S/*.c $S/snes/*.c) -lm || exit 1
for sc in 1 2; do
  fr=$([ $sc = 1 ] && echo 20000 || echo 30000)
  ./pref $ROM $fr -1 /dev/null $sc > p_s$sc.txt
  d=$(diff <(grep ^F baseline/s$sc.txt) <(grep ^F p_s$sc.txt) | grep '^>' | head -1)
  echo "script $sc: emulation $([ -z "$d" ] && echo unchanged || echo "CHANGED at ${d:2:6}")"
  grep -m5 BADLINE p_s$sc.txt; grep "PPUCHECK\|WHY" p_s$sc.txt
done
