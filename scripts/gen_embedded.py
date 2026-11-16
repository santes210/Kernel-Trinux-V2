#!/usr/bin/env python3
"""Regenera los headers con el fuente embebido que el kernel escribe en /root.

Los archivos examples/*.c se compilan DENTRO de Trinux con `tcc`, pero el
kernel los lleva embebidos como arrays para que estén disponibles en el primer
arranque (ver kernel/kernel.c).  Antes estos headers se generaban a mano con
`xxd -i` y se desincronizaban en silencio cada vez que se editaba el ejemplo.

Uso:  python3 scripts/gen_embedded.py            (regenera)
      python3 scripts/gen_embedded.py --check    (solo verifica, exit 1 si difieren)
"""
import sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
PAIRS = [
    ("examples/tce.c",      "kernel/tce_src.h"),
    ("examples/mini_test.c","kernel/mini_test_src.h"),
    ("examples/slow.c",     "kernel/slow_src.h"),
]

def render(name: str, data: bytes) -> str:
    lines = [
        f"/* Generado por scripts/gen_embedded.py desde examples/{name}.c — NO EDITAR A MANO. */",
        "#ifndef KERNEL_EMBEDDED_" + name.upper() + "_H",
        "#define KERNEL_EMBEDDED_" + name.upper() + "_H",
        "",
        f"static const unsigned char {name}_src[] = {{",
    ]
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i:i+12])
        lines.append("    " + chunk + ",")
    lines += ["};", "", f"static const unsigned int {name}_src_size = {len(data)};", "",
              "#endif", ""]
    return "\n".join(lines)

def main():
    check = "--check" in sys.argv
    bad = 0
    for src, dst in PAIRS:
        name = pathlib.Path(src).stem
        data = (ROOT / src).read_bytes()
        text = render(name, data)
        target = ROOT / dst
        current = target.read_text() if target.exists() else ""
        if check:
            ok = current == text
            print(f"{'OK  ' if ok else 'DIFF'} {dst}")
            bad += 0 if ok else 1
        else:
            target.write_text(text)
            print(f"wrote {dst} ({len(data)} bytes)")
    if check and bad:
        print(f"\n{bad} header(s) out of date — ejecuta: python3 scripts/gen_embedded.py")
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
