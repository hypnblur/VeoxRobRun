"""Cross-check catalog sprite names against what is actually embedded.

    python tools/loot/check_icons.py

Three things must agree for an icon to appear:
  1. the catalog row names a sprite,
  2. that sprite was staged into OrbitResources/icons (rc/RCDATA source),
  3. the runtime lookup sanitises the name to the same identifier.

This script reports every catalog row that fails 1 or 2, and every staged PNG
that no catalog row asks for. It reads files only.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CATALOG = os.path.join(ROOT, "VeoxDMA", "Config", "LootFilterData.h")
STAGED = os.path.join(ROOT, "OrbitResources", "icons")
SOURCE = os.path.join(HERE, "icons")


def sanitize(name):
    """Mirror IconSprites::SanitizeName: uppercase, [A-Za-z0-9_] only."""
    return re.sub(r"[^A-Za-z0-9_]", "", name).upper()


def staged_names():
    """Resource names the DLL actually carries: parsed from icons.rc.

    The staged PNGs are renamed to ic_NNNN.png, so the filenames say nothing
    about sprite names - the rc entries are the source of truth.
    """
    names = {}
    rc = os.path.join(ROOT, "OrbitResources", "icons.rc")
    if not os.path.isfile(rc):
        return names
    for line in open(rc, encoding="utf-8", errors="replace"):
        m = re.match(r"^([A-Za-z0-9_]+)\s+RCDATA\s+\"([^\"]+)\"", line.strip())
        if m:
            names[m.group(1)] = m.group(2)
    return names


def main():
    if not os.path.isfile(CATALOG):
        raise SystemExit("catalog not found: " + CATALOG)

    rows = []
    for line in open(CATALOG, encoding="utf-8"):
        m = re.match(r'\s*\{ "([^"]+)", "((?:[^"\\]|\\.)*)", "([^"]*)"', line)
        if m:
            rows.append((m.group(1), m.group(2), m.group(3)))

    staged = staged_names()
    print(f"catalog rows      {len(rows)}")
    print(f"embedded names    {len(staged)}")
    print(f"art source folder {'present' if os.path.isdir(SOURCE) else 'MISSING'}")

    with_sprite = [r for r in rows if r[2]]
    missing = [r for r in with_sprite if sanitize(r[2]) not in staged]
    empty = [r for r in rows if not r[2]]

    print(f"rows with a sprite {len(with_sprite)}")
    print(f"  sprite not embedded: {len(missing)}")
    for row in missing[:20]:
        print(f"    {row[0]:<24} wants {row[2]}")

    print(f"rows without a sprite {len(empty)}")
    for row in empty[:20]:
        print(f"    {row[0]:<24} {row[1]}")

    wanted = {sanitize(r[2]) for r in with_sprite}
    orphans = sorted(set(staged) - wanted)
    print(f"staged but unreferenced {len(orphans)}")
    for name in orphans[:20]:
        print(f"    {staged[name]}")


if __name__ == "__main__":
    main()
