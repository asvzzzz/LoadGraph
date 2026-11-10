#!/usr/bin/env python3
"""Read a .ico file and emit a C header with the bytes as a constexpr array."""
import sys, os

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.ico> <output.h>", file=sys.stderr)
        sys.exit(1)
    in_path, out_path = sys.argv[1], sys.argv[2]
    data = open(in_path, "rb").read()
    if len(data) == 0:
        print(f"Empty input: {in_path}", file=sys.stderr)
        sys.exit(1)
    # Ensure output directory exists
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        f.write("// Auto-generated from loadgraph.ico — do not edit.\n")
        f.write(f"// Size: {len(data)} bytes\n\n")
        f.write("static const unsigned char kLoadGraphIcoBytes[] = {\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            hex_line = ", ".join(f"0x{b:02X}" for b in chunk)
            f.write(f"    {hex_line},\n")
        f.write("};\n\n")
        f.write(f"static constexpr size_t kLoadGraphIcoSize = {len(data)};\n")
    print(f"Generated {out_path} ({len(data)} bytes)")

if __name__ == "__main__":
    main()
