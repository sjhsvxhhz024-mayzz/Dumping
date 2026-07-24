#!/usr/bin/env python3
# Rebuild code registration -> per-module method pointer tables.
# Anchored on the verified Assembly-CSharp array slot, expanded both directions.
import resolver as R, json

ASM_SLOT=0xb88a920   # verified: reloc[ASM_SLOT] -> Assembly-CSharp module struct

def bcstr(va):
    fo=R.va2fo(va)
    if fo is None: return None
    e=R.B.find(b'\x00',fo)
    try: return R.B[fo:e].decode('utf-8')
    except: return None

def module_at(slot_va):
    """Return module dict if slot points to a valid Il2CppCodeGenModule, else None."""
    modptr=R.reloc.get(slot_va)
    if not modptr: return None
    nv=R.reloc.get(modptr+0)
    if not nv: return None
    nm=bcstr(nv)
    if not nm or not nm.endswith('.dll') or not (3<=len(nm)<=100): return None
    fo=R.va2fo(modptr+8)
    if fo is None: return None
    mpc=R.bu32(fo)
    mpp=R.reloc.get(modptr+16)
    if mpc>500000: return None
    return {'name':nm,'base':modptr,'mpc':mpc,'mpp':mpp}

# expand backward
lo=0
k=1
while k<2000:
    if module_at(ASM_SLOT-k*8): lo=k; k+=1
    else: break
# expand forward
hi=0
k=1
while k<2000:
    if module_at(ASM_SLOT+k*8): hi=k; k+=1
    else: break

start=ASM_SLOT-lo*8
total=lo+hi+1
mods=[]
for i in range(total):
    mods.append(module_at(start+i*8))

import os as _os
_out=_os.environ.get('OX_CODEREG','/data/codereg.json')
json.dump({'ARRAY_VA':start,'CNT':total,'modules':mods},open(_out,'w'))
print('array start',hex(start),'total',total)
print('first 6:',[m['name'] for m in mods[:6]])
print('last 4:',[m['name'] for m in mods[-4:]])
for m in mods:
    if m['name'].startswith('Assembly-CSharp'):
        print('  ',m['name'],'mpc',m['mpc'],'mpp',hex(m['mpp']) if m['mpp'] else None)
for want in ('mscorlib.dll','UnityEngine.CoreModule.dll'):
    hit=[m for m in mods if m['name']==want]
    print(' ',want,'present' if hit else 'MISSING')
