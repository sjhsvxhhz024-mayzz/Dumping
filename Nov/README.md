# Nov — dumper & offline resolvers

Scripts that turn `libil2cpp.so` + decrypted `global-metadata.dat` into the
headers consumed by `proj/`.  All scripts are pure Python 3 stdlib unless
otherwise noted.

## Files

- `resolver.py` — ELF + relocation parser, feeds every other script.
- `codereg.py` — finds `Il2CppCodeRegistration` and 183 CodeGenModules.
- `dumpgen.py` — emits per-image/per-method RVA tables.
- `il2cppgen.py` — field-offset generator.
- `scriptgen.py` — companion `script.json`-style output for the C++ side.
- `runtime_probe.py` — Frida script that logs live `Il2CppClass*` addresses
  from a running process (fallback for values not resolvable offline).
- `typeinfo_offline.py` — **offline** TypeInfo slot RVA resolver.  Scans
  `libil2cpp.so` .text for the codegen'd
  `MOVZ W0,#<TDI>` → `BL GetTypeInfoFromTypeDefinitionIndex` → `ADRP`/`STR`
  pattern and prints a drop-in patch for `proj/include/oxide_offsets.h`.
  No runtime log required. Refs #2.

## Typical flow

```
# 1. decrypt metadata (proj/tools/decrypt_metadata.py)
# 2. dump everything static
python3 Nov/dumpgen.py libil2cpp.so global-metadata.decrypted.dat
python3 Nov/codereg.py libil2cpp.so
python3 Nov/il2cppgen.py libil2cpp.so global-metadata.decrypted.dat
# 3. resolve TypeInfo slots offline
python3 Nov/typeinfo_offline.py libil2cpp.so
#    (paste output into proj/include/oxide_offsets.h)
# 4. only if a slot is unresolved — fall back to runtime_probe.py
```
