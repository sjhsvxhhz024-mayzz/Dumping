#!/usr/bin/env python3
# dump.cs in Il2CppDumper format, with generic expansion + real RVAs.
import resolver as R, json, struct
M=R.M
def mu16(o): return struct.unpack_from('<H',M,o)[0]
def mu32(o): return struct.unpack_from('<I',M,o)[0]
def ms32(o): return struct.unpack_from('<i',M,o)[0]

IMG_OFF=0x175E050; IMG_CNT=188; IMG_REC=36
METH_OFF=0x0047507C; METH_REC=32
FLD_OFF=18882964; FLD_REC=12
PARAM_OFF=16361464; PARAM_REC=12
PROP_OFF=4041880; PROP_REC=20
FIELDOFF_VA=0xb980808
TYPE_OFF=R.TYPE_OFF; TYPE_REC=R.TYPE_REC; TYPE_CNT=R.TYPE_CNT

imgs=[]
for i in range(IMG_CNT):
    b=IMG_OFF+i*IMG_REC
    imgs.append({'name':R.cstr(mu32(b)),'ts':mu16(b+8),'tc':mu16(b+10)})

cr=json.load(open('/data/codereg.json'))
modmap={m['name']:m for m in cr['modules'] if m}

def method_addr(rid,imgname):
    m=modmap.get(imgname)
    if not m or not m['mpp']: return None
    if rid<1 or rid>m['mpc']: return None
    return R.reloc.get(m['mpp']+(rid-1)*8)

def T(i): return TYPE_OFF+i*TYPE_REC
def t_name(i): return R.cstr(mu32(T(i)))
def t_ns(i): return R.cstr(mu32(T(i)+4))
def t_decl(i): return ms32(T(i)+0x0C)
def t_parent(i): return ms32(T(i)+0x10)
def t_flags(i): return mu32(T(i)+0x16)
def t_fieldStart(i): return mu32(T(i)+0x1A)
def t_methodStart(i): return mu32(T(i)+0x1E)
def t_propStart(i): return mu32(T(i)+0x26)
def t_methodCount(i): return mu16(T(i)+0x3A)
def t_propCount(i): return mu16(T(i)+0x3C)
def t_fieldCount(i): return mu16(T(i)+0x3E)

GP='TUVWXYZABCDEFGH'
def gen_suffix(raw):
    if '`' in raw:
        try: n=int(raw.split('`')[1].split('.')[0])
        except: n=1
        if n<1 or n>32: n=1
        names=[GP[k] if k<len(GP) else 'T%d'%k for k in range(n)]
        return '<'+', '.join(names)+'>'
    return ''

def simple_name(i):
    raw=t_name(i)
    return raw.split('`')[0]+gen_suffix(raw)

def decl_prefix(i):
    parts=[]
    d=t_decl(i); g=0
    while d>=0 and g<12:
        di=R._typedef_of_typeindex(d)
        if di<0 or di>=TYPE_CNT: break
        parts.append(simple_name(di)); d=t_decl(di); g+=1
    return '.'.join(reversed(parts))

def type_attrs(type_idx):
    va=R.type_ptr(type_idx)
    if va is None: return 0
    fo=R.va2fo(va)
    if fo is None: return 0
    return R.bu32(fo+8)&0xffff

FACC={1:'private',2:'private protected',3:'internal',4:'protected',5:'protected internal',6:'public'}
def field_mods(attrs):
    parts=[FACC.get(attrs&7,'private')]
    if attrs&0x10: parts.append('static')
    if attrs&0x40: parts.append('const')
    elif attrs&0x20: parts.append('readonly')
    return ' '.join(parts)

MACC={1:'private',2:'private protected',3:'internal',4:'protected',5:'protected internal',6:'public'}
STATIC=0x10; FINAL=0x20; VIRTUAL=0x40; ABSTRACT=0x400; NEWSLOT=0x100
def method_mods(flags):
    parts=[MACC.get(flags&7,'private')]
    if flags&STATIC: parts.append('static')
    if flags&ABSTRACT:
        parts.append('abstract')
        if (flags&NEWSLOT)==0 and (flags&VIRTUAL): parts.append('override')
    elif flags&FINAL:
        if (flags&VIRTUAL) and (flags&NEWSLOT)==0: parts+=['sealed','override']
    elif flags&VIRTUAL:
        parts.append('virtual' if (flags&NEWSLOT) else 'override')
    return ' '.join(parts)

TVIS={0:'',1:'public',2:'public',3:'private',4:'protected',5:'internal',6:'private protected',7:'protected internal'}
def kind_of(i):
    fl=t_flags(i)
    if fl&0x20: return 'interface'
    if t_fieldCount(i)>0 and R.cstr(mu32(FLD_OFF+t_fieldStart(i)*FLD_REC))=='value__':
        return 'enum'
    p=t_parent(i)
    if p>=0 and R.read_type(R.type_ptr(p))=='System.ValueType': return 'struct'
    return 'class'

def type_mods(i,kind):
    fl=t_flags(i); parts=[]
    v=TVIS.get(fl&7,'')
    if v: parts.append(v)
    if kind=='class':
        if (fl&0x80) and (fl&0x100): parts.append('static')
        elif fl&0x80: parts.append('abstract')
        elif fl&0x100: parts.append('sealed')
    return ' '.join(parts)

def base_clause(i,kind):
    if kind in ('enum','interface','struct'): return ''
    p=t_parent(i)
    if p<0: return ''
    bn=R.read_type(R.type_ptr(p))
    if bn in ('System.Object','object',''): return ''
    return ' : '+bn

_ti2img=[None]*TYPE_CNT
for im in imgs:
    for ti in range(im['ts'],min(im['ts']+im['tc'],TYPE_CNT)):
        _ti2img[ti]=im['name']
def type_image_name(i): return _ti2img[i]

def render_type(i):
    kind=kind_of(i); ns=t_ns(i); L=[]
    L.append('// Namespace: %s'%ns)
    mods=type_mods(i,kind); pre=decl_prefix(i)
    nm=(pre+'.' if pre else '')+simple_name(i)
    decl=(mods+' ' if mods else '')+kind+' '+nm+base_clause(i,kind)
    L.append('%s // TypeDefIndex: %d'%(decl,i))
    L.append('{')
    fc=t_fieldCount(i); fs=t_fieldStart(i)
    if fc>0:
        L.append('\t// Fields')
        foff_arr=R.reloc.get(FIELDOFF_VA+i*8)
        afo=R.va2fo(foff_arr) if foff_arr else None
        for k in range(fc):
            fb=FLD_OFF+(fs+k)*FLD_REC
            fname=R.cstr(mu32(fb)); fti=mu32(fb+4)
            attrs=type_attrs(fti); ftype=R.read_type(R.type_ptr(fti))
            off=0
            if afo is not None:
                ov=struct.unpack_from('<I',R.B,afo+k*4)[0]
                off=0 if ov==0xffffffff else ov
            L.append('\t%s %s %s; // 0x%X'%(field_mods(attrs),ftype,fname,off))
        L.append('')
    pc=t_propCount(i); ps=t_propStart(i); ms_=t_methodStart(i)
    if pc>0:
        L.append('\t// Properties')
        for k in range(pc):
            pb=PROP_OFF+(ps+k)*PROP_REC
            pname=R.cstr(mu32(pb)); getRel=ms32(pb+4); setRel=ms32(pb+8)
            ptype='object'; pmods='public'
            if getRel>=0:
                gmb=METH_OFF+(ms_+getRel)*METH_REC
                ptype=R.read_type(R.type_ptr(mu32(gmb+6))); pmods=method_mods(mu16(gmb+0x18))
            elif setRel>=0:
                smb=METH_OFF+(ms_+setRel)*METH_REC
                pcount=mu16(smb+0x1E); pstart=mu32(smb+0x0E)
                if pcount>0:
                    lpb=PARAM_OFF+(pstart+pcount-1)*PARAM_REC
                    ptype=R.read_type(R.type_ptr(mu32(lpb+8)))
                pmods=method_mods(mu16(smb+0x18))
            acc=''
            if getRel>=0: acc+=' get;'
            if setRel>=0: acc+=' set;'
            L.append('\t%s %s %s {%s }'%(pmods,ptype,pname,acc))
        L.append('')
    mc=t_methodCount(i)
    if mc>0:
        L.append('\t// Methods'); img=type_image_name(i)
        for k in range(mc):
            mb=METH_OFF+(ms_+k)*METH_REC
            mname=R.cstr(mu32(mb)); ret_idx=mu32(mb+6)
            pstart=mu32(mb+0x0E); token=mu32(mb+0x14); flags=mu16(mb+0x18)
            slot=mu16(mb+0x1C); pcount=mu16(mb+0x1E); rid=token&0xffffff
            addr=method_addr(rid,img)
            L.append('')
            slotstr=' Slot: %d'%slot if slot!=0xffff else ''
            if addr:
                fo=R.va2fo(addr)
                L.append('\t// RVA: 0x%X Offset: 0x%X VA: 0x%X%s'%(addr,fo if fo else 0,addr,slotstr))
            else:
                L.append('\t// RVA: -1 Offset: -1%s'%slotstr)
            ret=R.read_type(R.type_ptr(ret_idx)); params=[]
            for pk in range(pcount):
                pb=PARAM_OFF+(pstart+pk)*PARAM_REC
                params.append('%s %s'%(R.read_type(R.type_ptr(mu32(pb+8))),R.cstr(mu32(pb))))
            L.append('\t%s %s %s(%s) { }'%(method_mods(flags),ret,mname,', '.join(params)))
    L.append('}')
    return '\n'.join(L)

if __name__=='__main__':
    out=[]
    for i,im in enumerate(imgs):
        out.append('// Image %d: %s - Il2CppDumper.TypeDefinitionIndex'%(i,im['name']))
    for im in imgs:
        for ti in range(im['ts'],min(im['ts']+im['tc'],TYPE_CNT)):
            out.append(render_type(ti))
    open('/data/dump.cs','w').write('\n'.join(out)+'\n')
    print('types',TYPE_CNT)
