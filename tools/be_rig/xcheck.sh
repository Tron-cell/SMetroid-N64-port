#!/bin/bash
# Fast equivalence check: x86 C build of the current source vs the verified baseline fingerprints.
S=/home/claude/n64/sm_n64/src; ROM=/home/claude/n64/sm_n64/filesystem/sm.smc; cd $(dirname $0)
gcc -O2 -DN64 -DRIG -w -fno-strict-aliasing -iquote $S -iquote . -o xref rig_main.c $(ls $S/*.c $S/snes/*.c) -lm || exit 1
ok=1
for sc in 1 2; do
  fr=$([ $sc = 1 ] && echo 20000 || echo 30000)
  ./xref $ROM $fr -1 /dev/null $sc | grep ^F > x_s$sc.txt
  d=$(diff <(grep ^F baseline/s$sc.txt) x_s$sc.txt | grep '^>' | head -1)
  if [ -n "$d" ]; then echo "script $sc: DIFF  base: $(grep "^${d:2:6}" baseline/s$sc.txt)"; echo "                 now: ${d:2}"; ok=0; else echo "script $sc: $fr frames identical"; fi
done
[ $ok = 1 ]
