import sys,struct
from capstone import *
from capstone.arm import *
D=open(sys.argv[1],'rb').read()
BASE=0x02000000
targets=[int(x,16)&~1 for x in sys.argv[3:]]
lo,hi=[int(x,16) for x in sys.argv[2].split('-')]
# brute: decode BL/B.W at every halfword
for o in range(lo-BASE,hi-BASE,2):
    h1,h2=struct.unpack_from('<HH',D,o)
    if (h1&0xf800)==0xf000 and (h2&0x9000)==0x9000 or ((h1&0xf800)==0xf000 and (h2&0xd001)==0xc000):
        S=(h1>>10)&1; imm10=h1&0x3ff; J1=(h2>>13)&1; J2=(h2>>11)&1; imm11=h2&0x7ff
        I1=1-(J1^S); I2=1-(J2^S)
        off=(S<<24)|(I1<<23)|(I2<<22)|(imm10<<12)|(imm11<<1)
        if S: off-=1<<25
        t=BASE+o+4+off
        if (h2&0x1000)==0: t&=~3  # BLX
        if t in targets: print(hex(BASE+o), 'BL' if h2&0x4000 else 'B.W', '->',hex(t))
# also literal pointers
for o in range(lo-BASE,hi-BASE-3,4):
    v=struct.unpack_from('<I',D,o)[0]
    if (v&~1) in targets and v&1: print(hex(BASE+o),'PTR',hex(v))
