import sys,struct,re
from capstone import *
from capstone.arm import *
D=open(sys.argv[1],'rb').read()
BASE=0x02000000
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB|CS_MODE_MCLASS)
md.detail=True
def cstr(a):
    o=a-BASE
    if 0<=o<len(D):
        e=D.find(b'\0',o,o+200)
        s=D[o:e] if e>0 else b''
        if len(s)>=3 and all(32<=c<127 or c in (9,10,13) for c in s):
            return s.decode()
    return None
def u32(a):
    o=a-BASE
    if 0<=o<=len(D)-4: return struct.unpack_from('<I',D,o)[0]
    return None
names={}
try:
    for l in open(sys.argv[2]):
        a,n=l.split(); names[int(a,16)]=n
except Exception: pass
def dis(start,end):
    pos=start
    while pos<end:
      last=pos
      for ins in md.disasm(D[pos-BASE:end-BASE],pos):
        last=ins.address+ins.size
        cmt=''
        if ins.id in (ARM_INS_LDR,) and len(ins.operands)==2 and ins.operands[1].type==ARM_OP_MEM and ins.operands[1].mem.base==ARM_REG_PC:
            la=((ins.address+4)&~3)+ins.operands[1].mem.disp
            v=u32(la)
            if v is not None:
                s=cstr(v)
                cmt=f' ; ={v:#x}'+(f' "{s}"' if s else '')+(f' <{names[v&~1]}>' if (v&~1) in names else '')
        elif ins.id in (ARM_INS_BL,ARM_INS_B) and ins.operands[0].type==ARM_OP_IMM:
            t=ins.operands[0].imm
            if t in names: cmt=f' ; <{names[t]}>'
        elif ins.id==ARM_INS_ADR:
            v=ins.operands[1].imm if len(ins.operands)>1 else None
        print(f'{ins.address:08x}: {ins.bytes.hex():12s} {ins.mnemonic:8s} {ins.op_str}{cmt}')
      if last<end:
        print(f'{last:08x}: .short {D[last-BASE:last-BASE+2].hex()}')
      pos=last+2
a=int(sys.argv[3],16); b=int(sys.argv[4],16)
dis(a,b)
