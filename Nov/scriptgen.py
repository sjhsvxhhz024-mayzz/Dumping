#!/usr/bin/env python3
# script.json in Il2CppDumper style: {"ScriptMethod":[{Address,Name,Signature}]}
import resolver as R, json, struct
M=R.M
def mu16(o): return struct.unpack_from('<H',M,o)[0]
def mu32(o): return struct.unpack_from('<I',M,o)[0]
def ms32(o): return struct.unpack_from('<i',M,o)[0]

IMG_OFF=0x175E050; IMG_CNT=188; IMG_REC=36
METH_OFF=0x0047507C; METH_REC=32
PARAM_OFF=16361464; PARAM_REC=12
TYPE_OFF=R.TYPE_OFF; TYPE_REC=R.TYPE_REC; TYPE_CNT=R.TYPE_CNT

imgs=[]
for i in range(IMG_CNT):
    b=IMG_OFF+i*IMG_REC
    imgs.append({'name':R.cstr(mu32(b)),'ts':mu16(b+8),'tc':mu16(b+10)})
_ti2img=[None]*TYPE_CNT
for im in imgs:
    for ti in range(im['ts'],min(im['ts']+im['tc'],TYPE_CNT)):
        _ti2img[ti]=im['name']

cr=json.load(open('/data/codereg.json'))
modmap={m['name']:m for m in cr['modules'] if m}
def method_addr(rid,imgname):
    m=modmap.get(imgname)
    if not m or not m['mpp']: return None
    if rid<1 or rid>m['mpc']: return None
    return R.reloc.get(m['mpp']+(rid-1)*8)

def T(i): return TYPE_OFF+i*TYPE_REC

out=[]
for i in range(TYPE_CNT):
    ms_=mu32(T(i)+0x1E); mc=mu16(T(i)+0x3A)
    tname=R.tdname(i); img=_ti2img[i]
    for k in range(mc):
        mb=METH_OFF+(ms_+k)*METH_REC
        mname=R.cstr(mu32(mb)); ret_idx=mu32(mb+6)
        pstart=mu32(mb+0x0E); token=mu32(mb+0x14); pcount=mu16(mb+0x1E)
        rid=token&0xffffff; addr=method_addr(rid,img)
        if not addr: continue
        ret=R.read_type(R.type_ptr(ret_idx)); params=[]
        for pk in range(pcount):
            pb=PARAM_OFF+(pstart+pk)*PARAM_REC
            params.append('%s %s'%(R.read_type(R.type_ptr(mu32(pb+8))),R.cstr(mu32(pb))))
        out.append({'Address':addr,'Name':'%s$$%s'%(tname,mname),
                    'Signature':'%s %s(%s)'%(ret,mname,', '.join(params))})

json.dump({'ScriptMethod':out},open('/data/script.json','w'),ensure_ascii=False)
print('ScriptMethod',len(out))
