#!/bin/bash
# Instruction counts + profile for gameplay frames 2700..3300 (attract demo), big-endian MIPS build with ROM flags.
S=/home/claude/n64/sm_n64/src; ROM=/home/claude/n64/sm_n64/filesystem/sm.smc; cd $(dirname $0)
export BE_EXTRA="-falign-functions=32 -ffunction-sections -fdata-sections -ffast-math -ftrapping-math -fno-associative-math -ftrivial-auto-var-init=uninitialized -DRIG_PROF_FROM=2700 $PERF_EXTRA"
./build.sh $S out_perf 3300 -1 1 > build_perf.log 2>&1 || { grep error build_perf.log | head; exit 1; }
PROF=1 ./host out_perf/be.elf $ROM > perf_run.txt
grep ^I perf_run.txt | awk -F'[= ]' '{ f=substr($1,2)+0; if (f>=2700) {g+=$3; a+=$5; n++; if ($3>mg) mg=$3} } END { printf "frames 2700-3299: avg game+video %.0fK, max %.0fK, audio %.0fK instr/frame\n", g/n/1000, mg/1000, a/n/1000 }'
python3 prof.py out_perf/be.elf ${1:-25} | grep -v "  main$"
