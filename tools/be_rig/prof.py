# Aggregate block-start instruction counts into functions using nm.
import struct, subprocess, sys, bisect
elf = sys.argv[1]
h = open('prof.bin','rb').read(); n = len(h)//8
cnt = struct.unpack('<%dQ' % n, h)
syms = []
for l in subprocess.run(['/opt/libdragon/bin/mips64-elf-nm','-n','-C',elf],capture_output=True,text=True).stdout.splitlines():
    p = l.split(' ',2)
    if len(p)==3 and p[1] in 'tTWw': syms.append((int(p[0],16), p[2]))
addrs=[a for a,_ in syms]
agg={}; tot=0
for i,c in enumerate(cnt):
    if not c: continue
    a=0x80000000+4*i; k=bisect.bisect_right(addrs,a)-1
    name=syms[k][1] if k>=0 else '?'
    agg[name]=agg.get(name,0)+c; tot+=c
print('total %.1fM instr' % (tot/1e6))
for name,c in sorted(agg.items(), key=lambda x:-x[1])[:int(sys.argv[2]) if len(sys.argv)>2 else 30]:
    print('%6.2f%%  %s' % (100*c/tot, name[:110]))
