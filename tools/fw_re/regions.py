import sys
data=open(sys.argv[1],'rb').read()
def crc(d):
    c=0xffff
    for b in d:
        c^=b<<8
        for _ in range(8):
            c=((c<<1)^0x8005)&0xffff if c&0x8000 else (c<<1)&0xffff
    return c
# check per 4K sector: fraction of 34-byte blocks valid (aligned from offset 0 in 34-blocks; partitions might start at 34*k aligned to 4K? use physical alignment)
N=len(data)
SEC=0x1000
res=[]
for s in range(0,N,SEC):
    sec=data[s:s+SEC]
    if sec==b'\xff'*SEC: res.append((s,'E')); continue
    if sec==b'\x00'*SEC: res.append((s,'Z')); continue
    # blocks aligned to 34 relative to 0
    first=((s+33)//34)*34
    ok=tot=0
    o=first
    while o+34<=s+SEC:
        b=data[o:o+34]
        if b!=b'\xff'*34:
            tot+=1
            if crc(b[:32])==int.from_bytes(b[32:],'big'): ok+=1
        o+=34
    res.append((s,'C' if tot and ok/tot>0.9 else ('?' if tot else 'E')))
# compress runs
cur=None;start=0
for s,t in res+[(N,'END')]:
    if t!=cur:
        if cur is not None: print(f"{start:08x}-{s:08x} {cur} ({(s-start)//1024}K)")
        cur=t;start=s
