#!/usr/bin/env python3
# Reconstructed IL2CPP resolver for the protected Oxide build (v39, ARM64).
import struct

BINPATH='/data/libil2cpp.so'
METPATH='/data/global-metadata.dat'
B=open(BINPATH,'rb').read()
M=open(METPATH,'rb').read()

def bu16(o): return struct.unpack_from('<H',B,o)[0]
def bu32(o): return struct.unpack_from('<I',B,o)[0]
def bu64(o): return struct.unpack_from('<Q',B,o)[0]
def bs64(o): return struct.unpack_from('<q',B,o)[0]
def mu32(o): return struct.unpack_from('<I',M,o)[0]
def ms32(o): return struct.unpack_from('<i',M,o)[0]

assert B[:4]==b'\x7fELF' and B[4]==2, 'not ELF64'
e_phoff=bu64(0x20); e_phentsize=bu16(0x36); e_phnum=bu16(0x38)
segs=[]
dyn_off=None
for i in range(e_phnum):
    o=e_phoff+i*e_phentsize
    p_type=bu32(o); p_offset=bu64(o+8); p_vaddr=bu64(o+16); p_filesz=bu64(o+32)
    if p_type==1: segs.append((p_vaddr,p_offset,p_filesz))
    elif p_type==2: dyn_off=p_offset
segs.sort()

def va2fo(va):
    for v,o,fs in segs:
        if v<=va<v+fs: return o+(va-v)
    return None

DT_RELA=7; DT_RELASZ=8; DT_RELAENT=9
rela_va=None; rela_sz=0; rela_ent=24
if dyn_off is not None:
    o=dyn_off
    while True:
        tag=bs64(o); val=bu64(o+8); o+=16
        if tag==0: break
        if tag==DT_RELA: rela_va=val
        elif tag==DT_RELASZ: rela_sz=val
        elif tag==DT_RELAENT: rela_ent=val
if rela_va is None:
    rela_fo=0x31778; rela_sz=0x1c19280
else:
    rela_fo=va2fo(rela_va)

R_AARCH64_RELATIVE=1027
reloc={}
cnt=rela_sz//rela_ent
for k in range(cnt):
    base=rela_fo+k*rela_ent
    r_off=bu64(base); r_info=bu64(base+8); r_add=bs64(base+16)
    if (r_info & 0xffffffff)==R_AARCH64_RELATIVE:
        reloc[r_off]=r_add

def ptr(va):
    if va in reloc: return reloc[va]
    fo=va2fo(va)
    return bu64(fo) if fo is not None else 0

STR=0x000DC0EC
TYPE_OFF=0x01512204
TYPE_CNT=29366
TYPE_REC=82
TYPES_VA=0xb63eb48
TYPES_CNT=104482

def cstr(rel):
    if rel<0: return ''
    o=STR+rel; e=M.find(b'\x00',o)
    return M[o:e].decode('utf-8','replace')

def type_ptr(idx):
    if idx<0 or idx>=TYPES_CNT: return None
    return reloc.get(TYPES_VA+idx*8)

def _td(i): return TYPE_OFF+i*TYPE_REC
def _nameIdx(i): return mu32(_td(i))
def _nsIdx(i): return mu32(_td(i)+4)
def _declType(i): return ms32(_td(i)+0x0C)

def _typedef_of_typeindex(tidx):
    va=type_ptr(tidx)
    if va is None: return -1
    fo=va2fo(va)
    if fo is None: return -1
    return bu64(fo)&0xffffffff

def tdname(i):
    if i<0 or i>=TYPE_CNT: return '?'
    parts=[cstr(_nameIdx(i))]
    d=_declType(i); g=0; top=i
    while d>=0 and g<12:
        di=_typedef_of_typeindex(d)
        if di<0 or di>=TYPE_CNT: break
        parts.append(cstr(_nameIdx(di))); top=di; d=_declType(di); g+=1
    ns=cstr(_nsIdx(top))
    name='.'.join(reversed(parts))
    name='.'.join(seg.split('`')[0] for seg in name.split('.'))
    return (ns+'.'+name) if ns else name

PRIM={1:'void',2:'bool',3:'char',4:'sbyte',5:'byte',6:'short',7:'ushort',8:'int',9:'uint',10:'long',11:'ulong',0x0C:'float',0x0D:'double',0x0E:'string',0x18:'IntPtr',0x19:'UIntPtr',0x1C:'object'}

def _type_tag(fo): return (bu32(fo+8)>>16)&0xff
def _type_data_va(va,fo):
    if va in reloc: return reloc[va]
    return bu64(fo)

GPL='TUVWXYZABCDEFGH'

def read_type(va, depth=0):
    if va is None: return '?'
    fo=va2fo(va)
    if fo is None: return '?'
    tp=_type_tag(fo)
    if tp in PRIM: return PRIM[tp]
    if tp==0x11 or tp==0x12:
        data=bu64(fo)
        return tdname(data&0xffffffff)
    if tp==0x0F:
        return read_type(_type_data_va(va,fo),depth+1)+'*'
    if tp==0x1D:
        return read_type(_type_data_va(va,fo),depth+1)+'[]'
    if tp==0x14:
        at=_type_data_va(va,fo); afo=va2fo(at)
        etype=ptr(at)
        rank=bu32(afo+8) if afo is not None else 1
        return read_type(etype,depth+1)+'['+','*(max(rank,1)-1)+']'
    if tp==0x13 or tp==0x1E:
        data=bu64(fo)&0xff
        return GPL[data% len(GPL)]
    if tp==0x15:
        if depth>6: return '?'
        gc=_type_data_va(va,fo); gfo=va2fo(gc)
        if gfo is None: return '?'
        gtype=ptr(gc)
        base=read_type(gtype,depth+1).split('<')[0]
        inst=ptr(gc+8)
        args=_read_generic_inst(inst,depth)
        if args is None: return base
        return base+'<'+', '.join(args)+'>'
    if tp==0x1B:
        return 'IntPtr'
    return 'object'

def _read_generic_inst(inst_va, depth):
    if not inst_va: return None
    ifo=va2fo(inst_va)
    if ifo is None: return None
    argc=bu64(ifo)&0xffffffff
    argv=ptr(inst_va+8)
    if not argv or argc==0 or argc>64: return None
    out=[]
    for k in range(argc):
        out.append(read_type(ptr(argv+k*8),depth+1))
    return out

ENUM=PRIM
