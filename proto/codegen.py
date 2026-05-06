#!/usr/bin/env python3
"""
proto/codegen.py — Read proto/schema.json, emit:
  - proto/generated/types.ts                              (TypeScript types)
  - firmware/components/proto/include/proto_gen.h        (C structs + enums)
  - firmware/components/proto/src/proto_gen.c            (snprintf serializers)

Run from repo root: python3 proto/codegen.py

Field types supported:
  Primitives: u8, u16, u32, u64, i32, bool, string
  References: any name in `enums` or `structs`

CI gate (TODO): in phase 3 add a check that fails the build if a field
mentioned in schema.json is not covered by an emitted serializer.
"""

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# (ts_type, c_type, printf_fmt)  — printf_fmt is None for bool (handled specially)
PRIMITIVES = {
    "u8":  ("number", "uint8_t",  "%u"),
    "u16": ("number", "uint16_t", "%u"),
    "u32": ("number", "uint32_t", "%u"),
    "u64": ("number", "uint64_t", "%llu"),
    "i32": ("number", "int32_t",  "%d"),
    "bool":   ("boolean", "bool",        None),
    "string": ("string",  "const char *", '\\"%s\\"'),
}


def fail(msg):
    print(f"codegen: ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def load_schema():
    path = REPO / "proto" / "schema.json"
    if not path.exists():
        fail(f"missing {path}")
    with open(path) as f:
        return json.load(f)


# ---- TypeScript ----

def ts_type_for(ftype, schema):
    if ftype in PRIMITIVES:
        return PRIMITIVES[ftype][0]
    if ftype in schema.get("enums", {}):
        return ftype
    if ftype in schema.get("structs", {}):
        return ftype
    fail(f"unknown type: {ftype}")


def gen_ts(schema):
    out = []
    out.append("// AUTO-GENERATED from proto/schema.json — do not edit by hand.\n")
    out.append("// Run `python3 proto/codegen.py` from the repo root to regenerate.\n\n")

    for ename, evals in schema.get("enums", {}).items():
        out.append(f"export type {ename} =\n")
        for i, v in enumerate(evals):
            sep = ";" if i == len(evals) - 1 else ""
            out.append(f'  | "{v}"{sep}\n')
        out.append("\n")

    for sname, sdef in schema.get("structs", {}).items():
        out.append(f"export interface {sname} {{\n")
        for fname, ftype in sdef["fields"].items():
            out.append(f"  {fname}: {ts_type_for(ftype, schema)};\n")
        out.append("}\n\n")

    for mname, mdef in schema.get("messages", {}).items():
        if "doc" in mdef:
            out.append(f"/** {mdef['doc']} */\n")
        out.append(f"export interface {mname} {{\n")
        for fname, ftype in mdef["fields"].items():
            out.append(f"  {fname}: {ts_type_for(ftype, schema)};\n")
        out.append("}\n\n")

    out.append("// Discriminated union of all server→client messages.\n")
    msgs = list(schema.get("messages", {}).keys())
    out.append("export type GrasshopperMessage =\n")
    for i, m in enumerate(msgs):
        sep = ";" if i == len(msgs) - 1 else ""
        out.append(f"  | ({m} & {{ type: \"{m.lower()}\" }}){sep}\n")
    out.append("\n")

    return "".join(out)


# ---- C header ----

def c_type_for(ftype, schema):
    if ftype in PRIMITIVES:
        return PRIMITIVES[ftype][1]
    if ftype in schema.get("enums", {}):
        return f"{ftype}_t"
    if ftype in schema.get("structs", {}):
        return f"{ftype}_t"
    fail(f"unknown type: {ftype}")


def gen_h(schema):
    out = []
    out.append("// AUTO-GENERATED from proto/schema.json — do not edit by hand.\n")
    out.append("// Run `python3 proto/codegen.py` from the repo root to regenerate.\n\n")
    out.append("#pragma once\n\n")
    out.append("#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n\n")
    out.append("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")

    for ename, evals in schema.get("enums", {}).items():
        out.append(f"typedef enum {{\n")
        for v in evals:
            out.append(f"    {ename.upper()}_{v},\n")
        out.append(f"}} {ename}_t;\n\n")
        out.append(f"const char *{ename}_str({ename}_t v);\n\n")

    for sname, sdef in schema.get("structs", {}).items():
        out.append(f"typedef struct {{\n")
        for fname, ftype in sdef["fields"].items():
            out.append(f"    {c_type_for(ftype, schema)} {fname};\n")
        out.append(f"}} {sname}_t;\n\n")

    for mname, mdef in schema.get("messages", {}).items():
        out.append(f"typedef struct {{\n")
        for fname, ftype in mdef["fields"].items():
            out.append(f"    {c_type_for(ftype, schema)} {fname};\n")
        out.append(f"}} {mname}_t;\n\n")
        out.append(
            f"size_t {mname}_to_json(char *buf, size_t bufsz, const {mname}_t *m);\n\n"
        )

    out.append("#ifdef __cplusplus\n}\n#endif\n")
    return "".join(out)


# ---- C serializer ----

def emit_field(out_fmt, out_args, struct_path, fname, ftype, schema, is_first):
    """Append a JSON field's format chunk + args to out_fmt / out_args.

    struct_path: e.g. 'm->' or 'm->wifi.'
    """
    sep = "" if is_first else ","
    base = ftype
    if base == "bool":
        out_fmt.append(f'{sep}\\"{fname}\\":%s')
        out_args.append(f'({struct_path}{fname} ? "true" : "false")')
    elif base == "string":
        out_fmt.append(f'{sep}\\"{fname}\\":\\"%s\\"')
        out_args.append(f"{struct_path}{fname}")
    elif base in PRIMITIVES:
        fmt = PRIMITIVES[base][2]
        out_fmt.append(f'{sep}\\"{fname}\\":{fmt}')
        if base == "u64":
            out_args.append(f"(unsigned long long){struct_path}{fname}")
        else:
            out_args.append(f"{struct_path}{fname}")
    elif base in schema.get("enums", {}):
        out_fmt.append(f'{sep}\\"{fname}\\":\\"%s\\"')
        out_args.append(f"{base}_str({struct_path}{fname})")
    elif base in schema.get("structs", {}):
        out_fmt.append(f'{sep}\\"{fname}\\":{{')
        sdef = schema["structs"][base]
        first = True
        for sf, st in sdef["fields"].items():
            emit_field(out_fmt, out_args, f"{struct_path}{fname}.", sf, st, schema, first)
            first = False
        out_fmt.append("}")
    else:
        fail(f"unknown type while serializing: {ftype}")


def gen_c(schema):
    out = []
    out.append("// AUTO-GENERATED from proto/schema.json — do not edit by hand.\n\n")
    out.append('#include "proto_gen.h"\n#include <stdio.h>\n\n')

    for ename, evals in schema.get("enums", {}).items():
        out.append(f"const char *{ename}_str({ename}_t v) {{\n")
        out.append("    switch (v) {\n")
        for v in evals:
            out.append(f'        case {ename.upper()}_{v}: return "{v}";\n')
        out.append('        default: return "?";\n')
        out.append("    }\n")
        out.append("}\n\n")

    for mname, mdef in schema.get("messages", {}).items():
        out.append(
            f"size_t {mname}_to_json(char *buf, size_t bufsz, const {mname}_t *m) {{\n"
        )
        fmt_parts = ['"{']
        args = []
        first = True
        for fname, ftype in mdef["fields"].items():
            emit_field(fmt_parts, args, "m->", fname, ftype, schema, first)
            first = False
        fmt_parts.append('}"')
        fmt_str = "".join(fmt_parts)
        args_str = ",\n        ".join(args) if args else ""
        out.append(f"    return snprintf(buf, bufsz,\n        {fmt_str},\n        {args_str});\n")
        out.append("}\n\n")

    return "".join(out)


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    print(f"wrote {path.relative_to(REPO)}")


def main():
    schema = load_schema()
    write(REPO / "proto" / "generated" / "types.ts", gen_ts(schema))
    write(REPO / "firmware" / "components" / "proto" / "include" / "proto_gen.h", gen_h(schema))
    write(REPO / "firmware" / "components" / "proto" / "src" / "proto_gen.c", gen_c(schema))


if __name__ == "__main__":
    main()
