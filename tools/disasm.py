import pefile,capstone,sys,re
pe=pefile.PE('data/Persona.exe'); base=pe.OPTIONAL_HEADER.ImageBase
d=pe.get_memory_mapped_image()
imp={i.address:(e.dll.decode()+'!'+i.name.decode()) for e in pe.DIRECTORY_ENTRY_IMPORT for i in e.imports if i.name}
def s(v):
    r=v-base
    if 0<=r<len(d):
        m=re.match(rb'[\x20-\x7e]{4,}',d[r:r+80])
        if m: return '"'+m.group().decode()+'"'
    return ''
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
a=int(sys.argv[1],16); n=int(sys.argv[2],0)
for i in md.disasm(d[a-base:a-base+n],a):
    ex=''
    for h in re.findall(r'0x[0-9a-f]+',i.op_str):
        v=int(h,16)
        if v in imp: ex+=' ; '+imp[v]
        elif s(v): ex+=' ; '+s(v)
    print(hex(i.address),i.mnemonic,i.op_str,ex)
