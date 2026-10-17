import sys,re
data=open(sys.argv[1],'rb').read()
end=0x31b000
out=bytearray()
for o in range(0,end - end%34,34):
    out+=data[o:o+32]
open(sys.argv[2],'wb').write(out)
# strings
with open(sys.argv[3],'w',encoding='utf-8') as f:
    for m in re.finditer(rb'[\x20-\x7e\t]{5,}',out):
        f.write(f"{m.start():07x} {m.group().decode()}\n")
with open(sys.argv[4],'w',encoding='utf-8') as f:
    for m in re.finditer(rb'[\x20-\x7e\t]{5,}',data[end:]):
        f.write(f"{m.start()+end:07x} {m.group().decode()}\n")
