"""Extract item prices from the shipped game data into item-prices.tsv.

    python tools/loot/extract_prices.py --game "path\\to\\Rob Run"

The game ships EasyAntiCheat. This script is deliberately file-only:

  * it refuses to run while the game, its launcher or EasyAntiCheat is running;
  * it never opens a process, never injects, never touches game memory;
  * it never starts the game and never writes inside the install folder;
  * it reads two things off disk - the Addressables bundle
    scriptableobjects_assets_all.bundle, and (through AssetStudioModCLI) the
    .NET metadata inside that bundle, which is ordinary serialised data.

That is the same read-only path tools/loot/update_icons.py already uses.
If you would rather not run it at all, keep item-prices.tsv empty: the ESP then
shows no price for items that carry no in-game cash amount.

Steps:
  1. locate ROB RUN_Data\\StreamingAssets\\aa\\StandaloneWindows64
  2. dump scriptableobjects_assets_all.bundle as JSON with AssetStudioModCLI
  3. walk every dumped object for price-like numeric fields and for the item id
     that owns them
  4. write item-prices.tsv and a report of which field names were found

Re-run gen_loot_catalog.ps1 afterwards to bake the values into the catalog.
"""
import argparse, json, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.abspath(__file__))
ASMOD = os.path.join(ROOT, ".asmod", "AssetStudioModCLI.exe")
OUT_TSV = os.path.join(ROOT, "data", "item-prices.tsv")
DUMP_DIR = os.path.join(ROOT, "work", "so-dump")
REPORT = os.path.join(ROOT, "work", "price-report.txt")

GAME_PROCESSES = ("rob run.exe", "start_protected_game.exe", "easyanticheat.exe")
PRICE_KEYS = re.compile(r"price|cost|worth|sell", re.I)
ID_KEYS = ("<Identified>k__BackingField", "<Id>k__BackingField", "_id", "Id", "_identifier")
# Higher wins when one item exposes several price-ish fields.
PRIORITY = ("sellprice", "sale_price", "sell", "price", "cost", "worth", "value")


def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def game_running():
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True,
                             text=True, errors="replace").stdout.lower()
    except OSError:
        return []
    return [p for p in GAME_PROCESSES if f'"{p}"' in out]


def pick_id(node):
    for key in ID_KEYS:
        v = node.get(key)
        if isinstance(v, dict) and isinstance(v.get("_id"), str) and v["_id"]:
            return v["_id"]
        if isinstance(v, str) and v:
            return v
    return None


def walk(node, owner, found):
    """Collect (id, field, value) for numeric price-ish fields."""
    if isinstance(node, list):
        for item in node:
            walk(item, owner, found)
        return
    if not isinstance(node, dict):
        return

    mine = pick_id(node) or owner
    for key, value in node.items():
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            continue
        if PRICE_KEYS.search(key):
            found.append((mine, key, float(value)))

    for value in node.values():
        if isinstance(value, (dict, list)):
            walk(value, mine, found)


def rank(field):
    key = field.lower()
    for i, want in enumerate(PRIORITY):
        if want in key:
            return i
    return len(PRIORITY)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True, help="Rob Run install folder")
    ap.add_argument("--asmod", default=ASMOD)
    ap.add_argument("--dump-dir", default=DUMP_DIR)
    ap.add_argument("--out", default=OUT_TSV)
    ap.add_argument("--keep-dump", action="store_true")
    args = ap.parse_args()

    running = game_running()
    if running:
        die(f"close the game first (running: {', '.join(running)}).")
    if not os.path.isfile(args.asmod):
        die(f"AssetStudioModCLI not found at {args.asmod} - run tools/loot/update_icons.py once to fetch it.")

    data = os.path.join(args.game, "ROB RUN_Data")
    bundles = os.path.join(data, "StreamingAssets", "aa", "StandaloneWindows64")
    if not os.path.isdir(bundles):
        die(f"bundle folder not found: {bundles}")
    bundle = os.path.join(bundles, "scriptableobjects_assets_all.bundle")
    if not os.path.isfile(bundle):
        die(f"bundle not found: {bundle}")
    # Read-only guard: the only file this script ever opens is that bundle, and
    # only through AssetStudioModCLI. Nothing from the install is executed.
    if os.path.dirname(os.path.abspath(bundle)) != os.path.abspath(bundles):
        die("refusing to read outside the Addressables bundle folder")

    if os.path.isdir(args.dump_dir):
        for name in os.listdir(args.dump_dir):
            p = os.path.join(args.dump_dir, name)
            if os.path.isfile(p):
                os.remove(p)
    os.makedirs(args.dump_dir, exist_ok=True)

    log(f"dumping {os.path.basename(bundle)} ({os.path.getsize(bundle) / (1024 * 1024):.1f} MB)")
    proc = subprocess.run([args.asmod, bundle, "-m", "jsonDump", "-t", "monoBehaviour",
                           "-g", "type", "-f", "assetName_pathID", "-o", args.dump_dir,
                           "--log-level", "warning"], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        tail = "\n".join(l for l in (proc.stdout + proc.stderr).splitlines() if "%]" not in l)[-1500:]
        die(f"AssetStudioModCLI failed:\n{tail}")

    files = [f for f in os.listdir(args.dump_dir) if f.lower().endswith(".json")]
    if not files:
        die("the dump produced no JSON files - the bundle layout changed.")
    log(f"scanning {len(files)} objects")

    found = []
    for name in files:
        try:
            with open(os.path.join(args.dump_dir, name), encoding="utf-8") as fh:
                walk(json.load(fh), None, found)
        except (OSError, ValueError):
            continue

    if not found:
        die("no price-like fields found - send tools/loot/work/price-report.txt so the fields can be mapped.")

    by_field = {}
    for _, field, _value in found:
        by_field[field] = by_field.get(field, 0) + 1

    best = {}
    for item_id, field, value in found:
        if not item_id:
            continue
        key = item_id.lower()
        cand = (rank(field), -value, field, value)
        if key not in best or cand < best[key]:
            best[key] = cand

    rows = [(k, int(best[k][3])) for k in sorted(best) if best[k][3] > 0]

    os.makedirs(os.path.dirname(REPORT), exist_ok=True)
    with open(REPORT, "w", encoding="utf-8") as fh:
        fh.write(f"objects scanned   {len(files)}\n")
        fh.write(f"price fields      {len(found)}\n")
        fh.write(f"ids with a price  {len(rows)}\n\n")
        fh.write("field name histogram (field<TAB>count):\n")
        for field, count in sorted(by_field.items(), key=lambda kv: -kv[1]):
            fh.write(f"  {field}\t{count}\n")
        fh.write("\nsample rows:\n")
        for item_id, value in rows[:40]:
            fh.write(f"  {item_id}\t{value}\n")

    if not rows:
        # Never wipe a hand-filled table with an empty result.
        print(f"prices    none found in this build; {args.out} left untouched")
        print(f"report    {REPORT}")
        print("  The shipped data has no numeric item prices: item type descriptions carry")
        print("  display stats only, and cargo's SellPrice is a localized UI label.")
        print("  Fill item-prices.tsv from a market list, or leave it empty - the ESP then")
        print("  shows a price only for entities that carry an in-game cash amount.")
        cleanup(args)
        return

    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# id\tvalue\t(optional rarity: Legendary/Epic/Rare/Uncommon/Common)\n")
        fh.write("# Written by extract_prices.py - re-run gen_loot_catalog.ps1 after editing.\n")
        for item_id, value in rows:
            fh.write(f"{item_id}\t{value}\n")

    print(f"prices    {len(rows)} ids -> {args.out}")
    print(f"fields    {len(by_field)} distinct price-like names")
    for field, count in sorted(by_field.items(), key=lambda kv: -kv[1])[:8]:
        print(f"  {field}: {count}")
    print(f"report    {REPORT}")
    cleanup(args)


def cleanup(args):
    if args.keep_dump:
        return
    for name in os.listdir(args.dump_dir):
        try:
            os.remove(os.path.join(args.dump_dir, name))
        except OSError:
            pass


if __name__ == "__main__":
    main()
