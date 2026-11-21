"""Turn the game's own item registries into a committed table.

    python tools/loot/gen_loot_registries.py [--dump work/so-dump] [--loc work/loc-dump]

The game lists every obtainable thing in registry ScriptableObjects, each entry
carrying the item id and a reference to its view asset (which holds the display
name and the icon sprite). This reads those registries instead of guessing a
category from an icon filename:

    InventoryItemViewDescriptions   499  inventory items
    LootObjectViewDescription       224  loose / static loot objects
    CargoViewDescriptions            81  cargo
    LootNodeViewDescriptions         70  loot nodes (bank_cash, ...)
    ContainersViewDescriptions       47  containers (safe_all, shelf, ...)
    CarViewDescriptions              57  vehicles
    WeaponViewDescriptions           46  weapons

Output: tools/loot/data/loot-registry.tsv
    registry <TAB> id <TAB> display name <TAB> sprite stem

That file is committed, so gen_loot_catalog.ps1 never needs the game install.
"""
import argparse, json, os, re, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ROOT, "data", "loot-registry.tsv")
ICON_INDEX = os.path.join(ROOT, "data", "icon_names.hpp")

REGISTRIES = {
    "InventoryItemViewDescriptions": "item",
    "LootObjectViewDescription": "lootobject",
    "CargoViewDescriptions": "cargo",
    "LootNodeViewDescriptions": "lootnode",
    "ContainersViewDescriptions": "container",
    "CarViewDescriptions": "car",
}

IMAGE_KEY = re.compile(r"(image|icon)", re.I)
NAME_KEY = "<Name>k__BackingField"


def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def path_key(ref):
    if isinstance(ref, dict):
        v = ref.get("m_PathID")
        if isinstance(v, int):
            return v
    return None


def localized(ref, text_by_id):
    if not isinstance(ref, dict):
        return None
    tref = ref.get("m_TableEntryReference") or {}
    for key in ("m_KeyId", "m_Key"):
        value = tref.get(key)
        if isinstance(value, int) and value in text_by_id:
            return text_by_id[value]
        if isinstance(value, str) and value in text_by_id:
            return text_by_id[value]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", default=os.path.join(ROOT, "work", "so-dump"))
    ap.add_argument("--loc", default=os.path.join(ROOT, "work", "loc-dump"))
    ap.add_argument("--out", default=OUT)
    args = ap.parse_args()

    dump = os.path.join(args.dump, "MonoBehaviour")
    if not os.path.isdir(dump):
        die(f"dump folder not found: {dump} - run extract_prices.py / update_icons.py first")

    # localization: table entry id -> English text
    text_by_id = {}
    for dp, _, files in os.walk(args.loc):
        for f in files:
            if not f.endswith(".json"):
                continue
            try:
                doc = load(os.path.join(dp, f))
            except (OSError, ValueError):
                continue
            for e in doc.get("m_TableData", []):
                if isinstance(e.get("m_Id"), int):
                    text_by_id[e["m_Id"]] = e.get("m_Localized")

    # pathID -> asset name, and asset name -> sprite stems from the icon index
    by_path, docs = {}, {}
    for f in os.listdir(dump):
        if not f.endswith(".json"):
            continue
        m = re.match(r"^(.*) @(-?\d+)\.json$", f)
        if not m:
            continue
        name, path_id = m.group(1), int(m.group(2))
        by_path[path_id] = name
        docs[path_id] = f

    sprite_by_source = {}
    sprite_by_id, name_by_id = {}, {}
    if os.path.isfile(ICON_INDEX):
        rx = re.compile(r'\{"((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)", '
                        r'"((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)"\}')
        for line in open(ICON_INDEX, encoding="utf-8"):
            m = rx.search(line)
            if not m:
                continue
            sprite, item_id = m.group(1), m.group(3)
            name, source, field = m.group(4), m.group(6), m.group(7)
            if IMAGE_KEY.search(field):
                sprite_by_source.setdefault(source, sprite)
            if item_id:
                sprite_by_id.setdefault(item_id.lower(), sprite)
                if name:
                    name_by_id.setdefault(item_id.lower(), name)

    rows, missing_name, missing_sprite = [], 0, 0
    for f in os.listdir(dump):
        if not f.endswith(".json"):
            continue
        try:
            doc = load(os.path.join(dump, f))
        except (OSError, ValueError):
            continue
        asset = doc.get("m_Name")
        kind = REGISTRIES.get(asset)
        if not kind:
            continue
        entries = doc.get("<List>k__BackingField") or []
        for entry in entries:
            if not isinstance(entry, dict):
                continue
            item_id = (entry.get("<Identified>k__BackingField") or {}).get("_id")
            if not item_id:
                continue
            view_path = path_key(entry.get("<View>k__BackingField"))
            view_name = by_path.get(view_path) if view_path is not None else None
            view = {}
            if view_path is not None and view_path in docs:
                try:
                    view = load(os.path.join(dump, docs[view_path]))
                except (OSError, ValueError):
                    view = {}

            name = localized(view.get(NAME_KEY), text_by_id) or name_by_id.get(item_id.lower())
            sprite = sprite_by_source.get(view_name or "") or sprite_by_id.get(item_id.lower())
            if not sprite:
                # fall back to any image reference on the view asset
                for key, value in view.items():
                    if IMAGE_KEY.search(key):
                        target = by_path.get(path_key(value))
                        if target:
                            sprite = sprite_by_source.get(target)
                            if sprite:
                                break
            if not name:
                missing_name += 1
            if not sprite:
                missing_sprite += 1
            rows.append((kind, item_id, name or "", sprite or ""))

    if not rows:
        die("no registry rows found - is the dump complete?")

    rows.sort()

    # Loot-object ids and item ids name the same thing differently: the game
    # spawns "star_h_m_2" on the ground and stores the weapon as "starh_m2".
    # Normalising to letters+digits makes that join deterministic, so a ground
    # weapon inherits the item's real sprite and display name instead of the
    # generic pickup icon. Only loot objects are rewritten; item rows win.
    def norm(value):
        return "".join(ch for ch in value.lower() if ch.isalnum())

    item_by_norm = {}
    for kind, item_id, name, sprite in rows:
        if kind == "item" and sprite:
            item_by_norm.setdefault(norm(item_id), (name, sprite))

    aliased = 0
    out_rows = []
    for kind, item_id, name, sprite in rows:
        if kind == "lootobject":
            hit = item_by_norm.get(norm(item_id))
            if hit:
                name = name or hit[0]
                sprite = hit[1]
                aliased += 1
        out_rows.append((kind, item_id, name or "", sprite or ""))

    rows = out_rows
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# Generated by gen_loot_registries.py from the game's registry assets.\n")
        fh.write("# registry\tid\tname\tsprite\n")
        for kind, item_id, name, sprite in rows:
            fh.write(f"{kind}\t{item_id}\t{name}\t{sprite}\n")

    counts = {}
    for kind, *_ in rows:
        counts[kind] = counts.get(kind, 0) + 1
    print(f"rows      {len(rows)} -> {args.out}")
    for kind in sorted(counts):
        print(f"  {kind:<11} {counts[kind]}")
    print(f"aliased   {aliased} loot-object ids inherited an item sprite")
    print(f"missing   name={missing_name} sprite={missing_sprite}")


if __name__ == "__main__":
    main()
