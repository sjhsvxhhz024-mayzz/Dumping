# Eclips Oxide

Two-part stack: a hand-rolled IL2CPP dumper (`Nov/`) and an Android
native menu built on Dear ImGui + Vulkan (`proj/`). The dumper produces
the metadata the menu consumes.

## Layout

```
.
├── Nov/                # IL2CPP dumper (Python)
│   ├── resolver.py     # ELF / PT_DYNAMIC / DT_RELA / R_AARCH64_RELATIVE, va<->offset
│   ├── codereg.py      # Il2CppCodeGenModule[] table (base, mpc, mpp)
│   ├── dumpgen.py      # dump.cs
│   ├── scriptgen.py    # script.json (Address / Name / Signature)
│   ├── il2cppgen.py    # il2cpp.h (<T>_Fields / <T>_o with field offsets)
│   └── README.md
│
└── proj/               # Android native menu (C++17, NDK)
    ├── Android.mk / Application.mk
    ├── src/
    │   ├── main.cpp                     # entry, memory, adaptive UI plumbing
    │   ├── eclips_oxide_menu.{h,cpp}    # menu widgets / pages
    │   ├── ImGui/                       # Dear ImGui (vendored)
    │   ├── Android_vulkan/              # Vulkan bootstrap + wrapper
    │   ├── Android_draw/                # 2D overlay draw
    │   ├── Android_touch/               # touch injection helpers
    │   ├── oxorany/                     # compile-time string obfuscation
    │   ├── LuaIntegration.cpp           # Lua 5.4 embedding
    │   └── ...
    ├── include/                         # public headers
    ├── lua/lua-5.4.7/                   # vendored Lua sources
    ├── tools/                           # decrypt_metadata.py, layout dumpers
    ├── CHANGELOG_RVA.txt
    └── OXIDE_all_rva.txt                # RVA table for the target build
```

## Kept out of Git (see `.gitignore`)

Big / regenerable inputs and outputs:

| Path | Size | Why |
|---|---|---|
| `Nov/libil2cpp.so` | ~191 MB | target-game binary, exceeds GitHub 100 MB per-file limit — bring your own |
| `Nov/global-metadata.dat` | ~26 MB | target-game metadata (decrypted, magic `0xFAB11BAF`) — bring your own |
| `Nov/dump.cs`, `Nov/script.json`, `Nov/il2cpp.h`, `Nov/codereg.json` | — | outputs of `dumpgen.py` / `scriptgen.py` / `il2cppgen.py`, regenerate |
| `proj/dump.cs` | ~38 MB | copied dumper output, regenerate |
| `proj/libs/arm64-v8a/oxideesp` | ~3.8 MB | NDK build artifact, rebuild with `ndk-build` |

Bring `libil2cpp.so` + decrypted `global-metadata.dat` in yourself, then:

```bash
cd Nov
python3 -B dumpgen.py && python3 -B scriptgen.py && python3 -B il2cppgen.py
```

## Notes

- Dumper is fully offline: no network, no third-party Il2CppDumper.
  Header decryption is done in-place with the recorded XOR key
  (`0xA5C3F19D`, range `[0x08, 0x17C)`, metadata version 39).
- Method address resolution goes
  `rid = token & 0xFFFFFF`
  → module = `codeGenModule` for the declaring type's image
  → `addr = reloc[module.mpp + (rid - 1) * 8]`.
- Menu targets Android arm64-v8a (Application.mk pins ABI + platform 29).
