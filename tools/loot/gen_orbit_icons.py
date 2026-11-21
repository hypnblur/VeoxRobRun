"""Embed every loot-filter icon into OrbitResources.dll.

    python tools/loot/gen_orbit_icons.py [--icons-root ...] [--size 64]

Reads the generated loot catalog (VeoxDMA/Config/LootFilterData.h) for the set
of sprites the UI can actually show, resolves each one through the exported
sprite index (VeoxDMA/Esp/IconIndex.h), downscales it to a square icon and
rewrites the resource script, so the DLL carries the whole icon set and the
overlay never touches the filesystem for an icon.

Outputs (all regenerated, never appended):
    OrbitResources/icons/ic_NNNN.png   downscaled copies, one per sprite
    OrbitResources/icons.rc            string-named RCDATA entries
    OrbitResources/resource.h          ORBIT_ICON_COUNT

The exported game sprites live under tools/loot/icons, which is git-ignored and
produced by tools/loot/update_icons.py. Re-run that first after a game
update, then this.
"""
import argparse, os, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CATALOG = os.path.join(ROOT, "VeoxDMA", "Config", "LootFilterData.h")
INDEX = os.path.join(ROOT, "VeoxDMA", "Esp", "IconIndex.h")
PROJECT = os.path.join(ROOT, "OrbitResources")


def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def unescape_cpp(value):
    """Decode the C++ string literal body the catalog stores (octal escapes are
    raw UTF-8 bytes), so the resource name matches what the runtime asks for."""
    out = bytearray()
    i = 0
    while i < len(value):
        ch = value[i]
        if ch == "\\" and i + 1 < len(value):
            octal = value[i + 1:i + 4]
            if len(octal) == 3 and all(c in "01234567" for c in octal):
                out.append(int(octal, 8))
                i += 4
                continue
            out += value[i + 1].encode("latin-1", "ignore")
            i += 2
            continue
        out += ch.encode("utf-8")
        i += 1
    return out.decode("utf-8", "replace")


def parse_catalog_sprites(path):
    rx = re.compile(r'^\s*\{\s*"((?:[^"\\]|\\.)*)",\s*"(?:[^"\\]|\\.)*",\s*"((?:[^"\\]|\\.)*)"')
    sprites = []
    seen = set()
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = rx.match(line)
            if not m:
                continue
            sprite = unescape_cpp(m.group(2))
            if not sprite:
                continue
            key = sprite.lower()          # Windows resource names are case-insensitive
            if key in seen:
                continue
            seen.add(key)
            sprites.append(sprite)
    return sprites


def parse_index_paths(path):
    rx = re.compile(r'\{"((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)"')
    paths = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = rx.search(line)
            if not m:
                continue
            sprite, rel = m.group(1), m.group(2)
            if rel and sprite.lower() not in paths:
                paths[sprite.lower()] = rel
    return paths


_SAFE = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")


def resource_name(sprite):
    """Map a sprite stem to an rc-safe resource name.

    rc.exe keeps the quotes of a quoted name as part of the resource name and
    cannot parse a name containing a space at all, so the rc uses bare
    identifiers and this rule (applied identically by IconSprites) folds every
    other character to '_'. RC also uppercases resource names.
    """
    if not sprite:
        return None
    if any(ord(c) > 127 for c in sprite):
        return None                      # the resource script is ASCII
    return "".join(c if c in _SAFE else "_" for c in sprite).upper()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--icons-root", default=os.path.join(HERE, "icons"))
    ap.add_argument("--catalog", default=CATALOG)
    ap.add_argument("--index", default=INDEX)
    ap.add_argument("--size", type=int, default=0,
                    help="downscale to a square icon of this edge; 0 (default) keeps the exported sprite as-is")
    ap.add_argument("--keep-old", action="store_true", help="keep PNGs already in OrbitResources/icons")
    args = ap.parse_args()

    from PIL import Image

    if not os.path.isdir(args.icons_root):
        die(f"icon export folder not found: {args.icons_root}")
    sprites = parse_catalog_sprites(args.catalog)
    if len(sprites) < 50:
        die(f"catalog parse produced {len(sprites)} sprites - wrong file?")
    paths = parse_index_paths(args.index)

    # Vehicles report their exact model+paint id as the entity label, so the
    # overlay asks for that sprite directly. Embed every vehicle paint, not
    # just the one row per model family that the catalog lists.
    known = {s.lower() for s in sprites}
    for sprite, rel in sorted(paths.items()):
        if "/Vehicles/" in rel and sprite not in known:
            known.add(sprite)
            sprites.append(sprite)

    out_dir = os.path.join(PROJECT, "icons")
    if os.path.isdir(out_dir) and not args.keep_old:
        shutil.rmtree(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    entries = []
    skipped = []
    used = set()
    total_out = 0
    for sprite in sprites:
        name = resource_name(sprite)
        rel = paths.get(sprite.lower())
        if not name or not rel:
            skipped.append((sprite, "no index path" if rel is None else "unsafe name"))
            continue
        if name.lower() in used:
            skipped.append((sprite, "name collides after sanitising"))
            continue
        src = os.path.join(args.icons_root, *rel.split("/"))
        if not os.path.isfile(src):
            skipped.append((sprite, "file missing"))
            continue

        idx = len(entries)
        dst_name = f"ic_{idx:04d}.png"
        dst = os.path.join(out_dir, dst_name)
        try:
            if args.size > 0:
                with Image.open(src) as im:
                    im = im.convert("RGBA")
                    im.thumbnail((args.size, args.size), Image.LANCZOS)
                    canvas = Image.new("RGBA", (args.size, args.size), (0, 0, 0, 0))
                    canvas.paste(im, ((args.size - im.width) // 2, (args.size - im.height) // 2))
                    canvas.save(dst, "PNG", optimize=True)
            else:
                # Full resolution: copy the exported bytes untouched so the icon
                # keeps whatever detail the game shipped.
                shutil.copyfile(src, dst)
        except Exception as exc:                      # noqa: BLE001 - one bad sprite must not stop the run
            skipped.append((sprite, f"decode failed: {exc.__class__.__name__}"))
            continue
        total_out += os.path.getsize(dst)
        used.add(name.lower())
        entries.append((name, dst_name))

    if not entries:
        die("no icons produced")

    rc = [
        "// Generated by tools/loot/gen_orbit_icons.py - do not edit by hand.",
        f"// {len(entries)} icons, {args.size}x{args.size} RGBA, served by name from OrbitResources.dll.",
        '#include "resource.h"',
        "",
    ]
    for name, fname in entries:
        rc.append(f'{name} RCDATA "icons\\\\{fname}"')
    rc_path = os.path.join(PROJECT, "icons.rc")
    with open(rc_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(rc) + "\n")

    res_h = [
        "// Generated by tools/loot/gen_orbit_icons.py - do not edit by hand.",
        "#pragma once",
        "",
        f"#define ORBIT_ICON_COUNT {len(entries)}",
        "",
    ]
    with open(os.path.join(PROJECT, "resource.h"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(res_h))

    size_label = f"{args.size}x{args.size}" if args.size > 0 else "full resolution"
    print(f"icons     {len(entries)} at {size_label}, {total_out / (1024 * 1024):.2f} MB on disk")
    print(f"catalog   {len(sprites)} sprites referenced")
    if skipped:
        print(f"skipped   {len(skipped)}")
        for sprite, why in skipped[:15]:
            print(f"  {sprite}: {why}")
        if len(skipped) > 15:
            print(f"  ... and {len(skipped) - 15} more")


if __name__ == "__main__":
    main()
