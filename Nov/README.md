

Полностью ручная реконструкция дампа из `libil2cpp.so` + расшифрованной
`global-metadata.dat` (magic 0xFAB11BAF, version 39, header XOR-key 0xA5C3F19D,
зашифрованный диапазон [0x08,0x17C)). Сетевого доступа не использовалось.

## Файлы
- `dump.cs`      — дамп в формате Il2CppDumper (29 366 типов, 188 образов).
- `script.json`  — {"ScriptMethod":[{Address,Name,Signature}]}, 190 128 методов
                    с реальными адресами (десятичные VA, как у Il2CppDumper).
- `il2cpp.h`     — заголовок с per-type структурами `<T>_Fields` / `<T>_o`
                    и смещениями полей (функциональный, не байт-в-байт).
- `resolver.py`  — ядро: парсинг ELF (LOAD/PT_DYNAMIC/DT_RELA), таблица
                    релокаций R_AARCH64_RELATIVE (1 225 654), va<->offset,
                    tdname, read_type (с раскрытием дженериков GENERICINST).
- `codereg.py` / `codereg.json` — Il2CppCodeGenModule[] (183 модуля): base,
                    methodPointerCount (mpc), methodPointers VA (mpp).
- `dumpgen.py`   — генератор dump.cs.
- `scriptgen.py` — генератор script.json.
- `il2cppgen.py` — генератор il2cpp.h.

## Как перезапустить
```
cp libil2cpp.so global-metadata.dat /data/     # decrypted metadata
python3 -B dumpgen.py && python3 -B scriptgen.py && python3 -B il2cppgen.py
```

## Как разрешаются адреса методов (проверено дизассемблером)
`rid = token & 0xFFFFFF`; модуль = codeGenModule с именем образа объявляющего
типа; `addr = reloc[module.mpp + (rid-1)*8]`.
Проверка: `Joystick.get_Health` rid 16888 -> 0x5B82C4C (точное совпадение с
дизассемблером: ldr s0,[x0,#0x30] / ret), `set_Health` -> 0x5B82C54.




## Статус `|-RVA:` (адреса инстанциаций дженерик-методов)
Это требует восстановления таблиц Il2CppMetadataRegistration.methodSpecs +
genericMethodTable + Il2CppCodeRegistration.genericMethodPointers. На данной
защищённой/обфусцированной сборке их надёжная привязка не гарантирована
(предупреждал: дольше и рискованно). В этом архиве основной дамп полный и
корректный без них; при подтверждённой привязке эти строки добавляются
отдельным проходом.
