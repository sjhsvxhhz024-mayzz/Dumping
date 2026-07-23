#!/usr/bin/env python3
# il2cpp.h: per-type <Mang>_Fields / <Mang>_o structs with C field types + offsets.
import resolver as R, struct, re
M=R.M
def mu16(o): return struct.unpack_from('<H',M,o)[0]
def mu32(o): return struct.unpack_from('<I',M,o)[0]
def ms32(o): return struct.unpack_from('<i',M,o)[0]

FLD_OFF=18882964; FLD_REC=12
FIELDOFF_VA=0xb980808
TYPE_OFF=R.TYPE_OFF; TYPE_REC=R.TYPE_REC; TYPE_CNT=R.TYPE_CNT

def T(i): return TYPE_OFF+i*TYPE_REC
def mang(name):
    return re.sub(r'[^0-9A-Za-z_]','_',name)

CTAG={1:'void',2:'bool',3:'uint16_t',7:'uint16_t',4:'int8_t',5:'uint8_t',6:'int16_t',
      8:'int32_t',9:'uint32_t',10:'int64_t',11:'uint64_t',0x0C:'float',0x0D:'double',
      0x18:'intptr_t',0x19:'uintptr_t',0x0E:'String_o*',0x1C:'Il2CppObject_o*'}
def ctype(type_idx):
    va=R.type_ptr(type_idx)
    if va is None: return 'void*'
    fo=R.va2fo(va)
    if fo is None: return 'void*'
    tp=R._type_tag(fo)
    if tp in CTAG: return CTAG[tp]
    if tp==0x11:  # valuetype by value
        return mang(R.tdname(R.bu64(fo)&0xffffffff))+'_o'
    if tp==0x12:  # class ptr
        return mang(R.tdname(R.bu64(fo)&0xffffffff))+'_o*'
    return 'void*'

out=['// Reconstructed IL2CPP field header (functional, not byte-identical to Il2CppDumper).',
     'typedef struct Il2CppObject_o Il2CppObject_o;','typedef struct String_o String_o;','']
for i in range(TYPE_CNT):
    name=R.tdname(i); mg=mang(name)
    fc=mu16(T(i)+0x3E); fs=mu32(T(i)+0x1A)
    foff_arr=R.reloc.get(FIELDOFF_VA+i*8); afo=R.va2fo(foff_arr) if foff_arr else None
    lines=['struct %s_Fields {'%mg]
    any_inst=False
    for k in range(fc):
        fb=FLD_OFF+(fs+k)*FLD_REC
        fname=R.cstr(mu32(fb)); fti=mu32(fb+4)
        attrs=R.bu32(R.va2fo(R.type_ptr(fti))+8)&0xffff if R.type_ptr(fti) else 0
        if attrs&0x10 or attrs&0x40: continue  # skip static/const
        off=0
        if afo is not None:
            ov=struct.unpack_from('<I',R.B,afo+k*4)[0]; off=0 if ov==0xffffffff else ov
        fn=re.sub(r'[^0-9A-Za-z_]','_',fname)
        lines.append('\t%s %s; // 0x%X'%(ctype(fti),fn,off))
        any_inst=True
    lines.append('};')
    lines.append('struct %s_o { %s_Fields fields; };'%(mg,mg))
    if any_inst: out+=lines

open('/data/il2cpp.h','w').write('\n'.join(out)+'\n')
print('done il2cpp.h types',TYPE_CNT)
