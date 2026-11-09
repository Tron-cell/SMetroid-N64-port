# Per-source-line instruction profile (block-start granularity) for the hottest code.
import struct, subprocess, sys, collections
elf=sys.argv[1]; top=int(sys.argv[2]) if len(sys.argv)>2 else 40
h=open('prof.bin','rb').read(); n=len(h)//8; cnt=struct.unpack('<%dQ'%n,h)
items=[(0x80000000+4*i,c) for i,c in enumerate(cnt) if c]
tot=sum(c for _,c in items)
addrs=[hex(a) for a,_ in items]
out=subprocess.run(['/opt/libdragon/bin/mips64-elf-addr2line','-e',elf]+addrs,capture_output=True,text=True).stdout.split('\n')
agg=collections.Counter()
for (a,c),l in zip(items,out):
    agg[l.split('/')[-1].split(' ')[0]]+=c
for l,c in agg.most_common(top): print('%6.2f%%  %s'%(100*c/tot,l))
