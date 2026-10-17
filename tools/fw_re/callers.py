import sys,struct,subprocess
from capstone import *
from capstone.arm import *
D=open(sys.argv[1],'rb').read()
BASE=0x02000000
lo,hi=0x02010000,0x021f0000
tg={int(k,16)&~1:v for k,v in (x.split('=') for x in sys.argv[2].split(','))}
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB|CS_MODE_MCLASS)
def u32(a):
    o=a-BASE
    return struct.unpack_from('<I',D,o)[0] if 0<=o<=len(D)-4 else None
def cstr(a):
    o=a-BASE
    if 0<=o<len(D):
        e=D.find(b'\0',o,o+120); s=D[o:e]
        if len(s)>=3 and all(32<=c<127 for c in s): return s.decode()
for o in range(lo-BASE,hi-BASE,2):
    h1,h2=struct.unpack_from('<HH',D,o)
    if (h1&0xf800)==0xf000 and (h2&0xd000)==0xd000:
        S_=(h1>>10)&1; imm10=h1&0x3ff; J1=(h2>>13)&1; J2=(h2>>11)&1; imm11=h2&0x7ff
        I1=1-(J1^S_); I2=1-(J2^S_)
        off=(S_<<24)|(I1<<23)|(I2<<22)|(imm10<<12)|(imm11<<1)
        if S_: off-=1<<25
        t=BASE+o+4+off
        if t in tg:
            a=BASE+o
            print(f'===== {a:#x} -> {tg[t]}')
            # disasm preceding 40 bytes (try a few alignments)
            for back in (40,38):
                ins=list(md.disasm(D[o-back:o+4],a-back))
                if ins and ins[-1].address==a: break
            for i in ins[-14:]:
                c=''
                if i.mnemonic=='ldr' and '[pc' in i.op_str:
                    try:
                        disp=int(i.op_str.split('#')[-1].rstrip(']'),16)
                        v=u32(((i.address+4)&~3)+disp); s=cstr(v) if v else None
                        c=f' ; ={v:#x}'+(f' "{s}"' if s else '')
                    except Exception: pass
                print(f'  {i.address:08x}: {i.mnemonic:7s} {i.op_str}{c}')
