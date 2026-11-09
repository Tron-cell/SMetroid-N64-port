# Map RSP PC histogram to labels (objdump of the ucode ELF).
import subprocess, sys, re, collections
elf = sys.argv[1]
dis = subprocess.run(['/opt/libdragon/bin/mips64-elf-objdump', '-d', elf], capture_output=True, text=True).stdout
lab = {}; cur = '?'
for l in dis.splitlines():
    m = re.match(r'^([0-9a-f]+) <([^>]+)>:', l)
    if m: cur = m.group(2); continue
    m = re.match(r'^\s*([0-9a-f]+):', l)
    if m: lab[int(m.group(1), 16) & 0xfff] = cur
agg = collections.Counter(); tot = 0
for l in open('pc_hist.txt'):
    a, c = l.split(); a = int(a, 16) & 0xfff; c = int(c)
    agg[lab.get(a, '?')] += c; tot += c
for k, v in agg.most_common(25): print('%6.2f%%  %s' % (100 * v / tot, k))
