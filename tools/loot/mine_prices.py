"""Locate the item price functions inside GameAssembly.dll.

    python tools/loot/mine_prices.py [--dump work/il2cpp-dump] [--binary path\\GameAssembly.dll]

Prices are not stored in any asset: InventoryItemDescription carries
BasePriceFunc (0x78) and SellPriceFunc (0x88) as Func<int>, built in the static
constructor of InventoryItemDescriptions. So the numbers live in IL2CPP method
bodies, and this script reads them straight out of the shipped binary - pure
file access, the game is never touched.

Stage 1 (this script, default): map every InventoryItemDescriptions method to a
file offset and classify the small ones that look like `return <constant>`.
Stage 2 (--emit): write tools/loot/data/sell-prices.tsv for the constants found.
"""
import argparse, json, os, struct, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DUMP = os.path.join(ROOT, "work", "il2cpp-dump")
DEFAULT_BINARY = os.path.join(DEFAULT_DUMP, "DummyDll", "GameAssembly.dll")


class PE:
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.data = fh.read()
        if self.data[:2] != b"MZ":
            sys.exit("not a PE file")
        pe_off = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe_off:pe_off + 4] != b"PE\0\0":
            sys.exit("missing PE signature")
        coff = pe_off + 4
        nrva = struct.unpack_from("<H", self.data, coff + 2)[0]
        opt_size = struct.unpack_from("<H", self.data, coff + 16)[0]
        opt = coff + 20
        magic = struct.unpack_from("<H", self.data, opt)[0]
        self.image_base = struct.unpack_from("<Q", self.data, opt + 24)[0] if magic == 0x20B \
            else struct.unpack_from("<I", self.data, opt + 28)[0]
        self.sections = []
        sec = opt + opt_size
        for i in range(nrva):
            base = sec + i * 40
            name = self.data[base:base + 8].rstrip(b"\0").decode("latin-1")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", self.data, base + 8)
            self.sections.append((name, vaddr, vsize, rawptr, rawsize))

    def offset(self, rva):
        for _name, vaddr, vsize, rawptr, rawsize in self.sections:
            span = max(vsize, rawsize)
            if vaddr <= rva < vaddr + span:
                delta = rva - vaddr
                if delta >= rawsize:
                    return None
                return rawptr + delta
        return None

    def read(self, rva, size):
        off = self.offset(rva)
        if off is None or off + size > len(self.data):
            return b""
        return self.data[off:off + size]


def constant_of(blob):
    """Byte patterns an IL2CPP `return <int literal>;` compiles to."""
    if len(blob) >= 5 and blob[0] == 0xB8:                      # mov eax, imm32
        value = struct.unpack_from("<i", blob, 1)[0]
        if len(blob) >= 6 and blob[5] == 0xC3:
            return value, "mov eax,imm32;ret"
        return value, "mov eax,imm32"
    if len(blob) >= 7 and blob[:3] == b"\x48\xc7\xc0":          # mov rax, imm32
        value = struct.unpack_from("<i", blob, 3)[0]
        if len(blob) >= 8 and blob[7] == 0xC3:
            return value, "mov rax,imm32;ret"
        return value, "mov rax,imm32"
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", default=DEFAULT_DUMP)
    ap.add_argument("--binary", default=DEFAULT_BINARY)
    ap.add_argument("--emit", action="store_true", help="write the mined table")
    args = ap.parse_args()

    if not os.path.isfile(args.binary):
        sys.exit(f"binary not found: {args.binary}")
    pe = PE(args.binary)
    print(f"image base 0x{pe.image_base:X}, sections: {len(pe.sections)}, size {len(pe.data)/1e6:.0f} MB")

    script = os.path.join(args.dump, "script.json")
    if not os.path.isfile(script):
        sys.exit(f"script.json not found: {script}")
    doc = json.load(open(script, encoding="utf-8"))
    methods = doc["ScriptMethod"]

    target = "InventoryItemDescriptions"
    cands = [m for m in methods if target in m.get("Name", "")]
    print(f"methods in {target}: {len(cands)}")

    ints = [m for m in cands if m.get("Signature", "").lstrip().startswith("int32_t")]
    print(f"  of those returning int32: {len(ints)}")

    found, shapes = [], {}
    for m in ints:
        addr = m["Address"]
        rva = addr - pe.image_base if addr >= pe.image_base else addr
        blob = pe.read(rva, 16)
        if not blob:
            continue
        value, shape = constant_of(blob)
        shapes[shape] = shapes.get(shape, 0) + 1
        if value is not None and 0 < value < 100_000_000:
            found.append((m["Name"].split("$$")[-1], value, addr))

    print("byte-shape histogram:")
    for shape, count in sorted(shapes.items(), key=lambda kv: -kv[1]):
        print(f"  {shape}: {count}")
    print(f"constants recovered: {len(found)}")
    for name, value, addr in found[:15]:
        print(f"  {name:<32} {value}")

    if args.emit and found:
        out = os.path.join(ROOT, "data", "sell-prices.tsv")
        with open(out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# id\tvalue\n")
            for name, value, _addr in found:
                fh.write(f"{name}\t{value}\n")
        print(f"written {len(found)} rows -> {out}")


if __name__ == "__main__":
    main()
