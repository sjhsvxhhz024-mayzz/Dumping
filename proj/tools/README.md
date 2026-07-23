# il2cpp v39 (Unity 6) metadata tools

Toolkit for decrypting and analyzing the protected `global-metadata.dat` of this
Unity 6 / il2cpp v39 build. All facts below were recovered deterministically by
disassembling the metadata loader inside `libil2cpp.so` — not by guessing.

## Protection summary

| Property            | Value                                              |
|---------------------|----------------------------------------------------|
| XOR key             | `0xA5C3F19D`                                        |
| Encrypted region    | header bytes `0x08 .. 0x17C`                         |
| Header format       | 31 **triples** `(offset, size, count)`, 12 bytes each |
| Section order       | **non-standard** (e.g. `images` = triple #14, not #20) |
| magic / version     | `0xFAB11BAF` / `39` (plaintext, not encrypted)      |

The non-standard triple layout is why no public Il2CppDumper fork parses the file:
they all assume classic `(offset, size)` pairs in the standard section order.

## Files

- `decrypt_metadata.py` — decrypts the header (`raw ^ 0xA5C3F19D` over `0x08..0x17C`).
  Body after `0x17C` is left untouched (plaintext).
  ```
  python3 decrypt_metadata.py global-metadata.dat global-metadata.dec.dat
  ```
- `extract_header_layout.py` — re-derives the header layout from a `libil2cpp.so`
  by disassembling the loader. Use this to re-confirm the layout on future builds.
  ```
  python3 extract_header_layout.py libil2cpp.so
  ```
- `v39_header_layout.md` — full recovered field map with offsets/values/anchors.

## Verified anchors (cross-checked from engine code)

- `images`   : off `0x0175E050`, count `188`
- `assemblies`: off `0x0175FAC0`, count `188`  (images == assemblies)
- `typeDefinitions`: count `225735`
- `string` (identifiers): off `0xDC0EC` -> `"Assembly-CSharp"`

## Next step (fork)

To produce `dump.cs`, fork `il2cpp-dumper-rs`: override the header read to consume
31 triples in this build's section order, and disable the variable-width index
heuristic (this build uses fixed 4-byte indices). Feed BOTH the decrypted metadata
and `libil2cpp.so`.

## Requirements

```
pip install capstone pyelftools
```
