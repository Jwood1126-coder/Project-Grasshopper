# proto/

Single source of truth for Grasshopper wire messages.

- **`schema.json`** — declarative schema (enums, structs, messages).
- **`codegen.py`** — Python script that emits TS types + C structs + C serializers.
- **`generated/types.ts`** — output, committed (frontend imports this).

## Workflow

```bash
# Edit schema.json
$EDITOR proto/schema.json

# Regenerate
python3 proto/codegen.py

# Outputs:
#   proto/generated/types.ts
#   firmware/components/proto/include/proto_gen.h
#   firmware/components/proto/src/proto_gen.c
```

The generated files are committed so:
- Firmware can build with no Python toolchain on the build host.
- Frontend can build with no Python toolchain.
- CI runs `python3 proto/codegen.py && git diff --exit-code` to ensure
  whoever last edited `schema.json` ran the codegen.

## Supported field types

| Schema | TypeScript | C |
|--------|------------|---|
| `u8`/`u16`/`u32` | `number` | `uint{8,16,32}_t` |
| `u64` | `number` | `uint64_t` (printed as `%llu`) |
| `i32` | `number` | `int32_t` |
| `bool` | `boolean` | `bool` |
| `string` | `string` | `const char *` |
| enum reference | string union | `<Name>_t` enum |
| struct reference | nested interface | nested struct value |

Not yet supported: arrays, optional/nullable fields, deep nesting (>1
level). Phase 3 will extend as needed.
