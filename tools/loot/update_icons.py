"""Re-extract every Rob Run icon and regenerate icon_names.hpp in one go.

    python tools/loot/update_icons.py [--game "path\\to\\Rob Run"]

Run it with the game CLOSED. It only reads the game's files on disk (no process
access, no injection), so EasyAntiCheat is never involved.

Steps:
  1. fetch AssetStudioModCLI (pinned, SHA-256 checked) into tools/loot/.asmod/ if missing
  2. find every Addressables bundle that contains sprites
  3. export all sprites as PNG -> icons/ (replaced only if the export succeeds)
  4. JSON-dump item data + English string tables
  5. join sprite -> item id -> English name and write icon_names.hpp
  6. print which icons were added/removed since the last run
"""
import argparse, collections, concurrent.futures as cf, hashlib, io, json, os, re
import shutil, subprocess, sys, tempfile, time, urllib.request, zipfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GAME = None
ICONS_DIR = os.path.join(ROOT, "icons")
HEADER = os.path.join(ROOT, "data", "icon_names.hpp")

ASMOD_DIR = os.path.join(ROOT, ".asmod")
ASMOD_EXE = os.path.join(ASMOD_DIR, "AssetStudioModCLI.exe")
ASMOD_URL = ("https://github.com/aelurum/AssetStudioMod/releases/download/v0.20.0/"
             "AssetStudioModCLI_net10_win64.zip")
ASMOD_SHA256 = "0CAFF1E9D381083324F65241F55171768CD08EFFE626B0125B63497CD2510CC7"

GAME_PROCESSES = ("rob run.exe", "start_protected_game.exe", "easyanticheat.exe")
# bundles that never hold UI sprites (meshes, audio, text, scenes) - skipped in the scan
SKIP_BUNDLES = re.compile(r"^(character-|wwise|localization|[0-9a-f]{32}_)|standalone|_scenes_")


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def die(msg):
    print(f"\nERROR: {msg}", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------- environment

def game_running():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                         errors="replace").stdout.lower()
    return [p for p in GAME_PROCESSES if f'"{p}"' in out]


def ensure_asmod():
    if os.path.isfile(ASMOD_EXE):
        return
    log(f"downloading AssetStudioModCLI -> {ASMOD_DIR}")
    try:
        req = urllib.request.Request(ASMOD_URL, headers={"User-Agent": "icon-update-script"})
        with urllib.request.urlopen(req, timeout=120) as r:
            data = r.read()
    except OSError as e:
        # e.g. MSYS2/embedded Pythons without a CA bundle: use Windows' curl (system cert store)
        curl = shutil.which("curl.exe") or os.path.join(os.environ.get("SystemRoot", r"C:\Windows"),
                                                        "System32", "curl.exe")
        log(f"  python download failed ({e.__class__.__name__}); retrying with {curl}")
        p = subprocess.run([curl, "-fsSL", "-A", "icon-update-script", ASMOD_URL], capture_output=True)
        if p.returncode != 0:
            die(f"could not download AssetStudioMod: {p.stderr.decode(errors='replace').strip()}")
        data = p.stdout
    digest = hashlib.sha256(data).hexdigest().upper()
    if digest != ASMOD_SHA256:
        die(f"AssetStudioMod zip hash mismatch ({digest}); refusing to use it.")
    os.makedirs(ASMOD_DIR, exist_ok=True)
    zipfile.ZipFile(io.BytesIO(data)).extractall(ASMOD_DIR)
    if not os.path.isfile(ASMOD_EXE):
        die("AssetStudioModCLI.exe not found in the downloaded zip.")


def asmod(*args, check=True):
    p = subprocess.run([ASMOD_EXE, *map(str, args)], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    out = p.stdout + p.stderr
    if check and p.returncode != 0:
        tail = "\n".join(l for l in out.splitlines() if "%]" not in l)[-2000:]
        die(f"AssetStudioModCLI failed ({' '.join(map(str, args[:3]))}):\n{tail}")
    return out


def stage(files, dest):
    """Hard-link (same drive) or copy files into dest - AssetStudio loads one folder."""
    os.makedirs(dest, exist_ok=True)
    for f in files:
        target = os.path.join(dest, os.path.basename(f))
        try:
            os.link(f, target)
        except OSError:
            shutil.copy2(f, target)


# ---------------------------------------------------------------- extraction

def find_sprite_bundles(bundle_dir):
    bundles = sorted(f for f in os.listdir(bundle_dir) if f.endswith(".bundle"))
    # UI atlases ship as _4k/_qhd/_hd copies holding the same sprites; the 4K copy (or the
    # only copy) is enough, and skipping the rest saves loading ~40 bundles.
    picked = [f for f in bundles if f.startswith("ui") and not re.search(r"_(hd|qhd)(\.spriteatlasv2)?\.bundle$", f)]
    candidates = [f for f in bundles if not f.startswith("ui") and not SKIP_BUNDLES.search(f)]
    log(f"scanning {len(candidates)} non-UI bundles for sprites ({len(picked)} UI bundles included)")

    def count(name):
        out = asmod(os.path.join(bundle_dir, name), "-m", "info", check=False)
        m = re.search(r"# Sprite: (\d+)", out)
        return name, int(m.group(1)) if m else 0

    with cf.ThreadPoolExecutor(max_workers=min(6, os.cpu_count() or 2)) as ex:
        for name, n in ex.map(count, candidates):
            if n:
                log(f"  {n:5} sprites  {name}")
                picked.append(name)
    return [os.path.join(bundle_dir, f) for f in picked]


def export_bundles(bundle_dir, work):
    stage(find_sprite_bundles(bundle_dir), os.path.join(work, "bundles"))
    out = os.path.join(work, "icons")
    log("exporting sprites from bundles (all atlases loaded together so references resolve)")
    # Export as "Name @PathID.png" so every file is tied to its exact sprite, then rename
    # deterministically. (AssetStudio's own clash suffixes depend on export order, so with
    # duplicate names like Handcuffs/Handcuffs or Weapon/weapon the plain name could land on
    # either sprite from run to run.)
    asmod(os.path.join(work, "bundles"), "-m", "export", "-t", "sprite", "-g", "container",
          "-f", "assetName_pathID", "-o", out, "--image-format", "png",
          "--export-asset-list", "xml", "--log-level", "warning")
    log("bundle export finished")

    groups = collections.defaultdict(list)
    for root, _, files in os.walk(out):
        for f in files:
            m = re.match(r"^(.*) @(-?\d+)\.png$", f)
            if m:  # Windows paths are case-insensitive, so group "Weapon" with "weapon"
                groups[(root, m.group(1).lower())].append((int(m.group(2)), m.group(1), f))
    pid_path = {}
    for (root, _), items in groups.items():
        for n, (pid, name, f) in enumerate(sorted(items)):
            new = f"{name}.png" if n == 0 else f"{name} ({n + 1}).png"
            os.rename(os.path.join(root, f), os.path.join(root, new))
            pid_path[pid] = os.path.relpath(os.path.join(root, new), out).replace("\\", "/")
    return out, pid_path


def export_core(game_data, work, icons_out):
    """The player's own asset files repeat the bundle sprites (without folder paths) plus a
    handful of their own. List them in raw mode (no image decoding, ~1s), then convert only
    the ones the bundle export doesn't already have."""
    core = [os.path.join(game_data, f) for f in os.listdir(game_data)
            if re.match(r"^(globalgamemanagers|level\d|resources\.assets|sharedassets\d)", f)]
    stage(core, os.path.join(work, "core"))
    listing = os.path.join(work, "core_list")
    asmod(os.path.join(work, "core"), "-m", "exportRaw", "-t", "sprite", "-g", "none", "-o", listing,
          "--export-asset-list", "xml", "--log-level", "warning")
    core_names = {a.findtext("Name") for a in ET.parse(os.path.join(listing, "assets.xml")).getroot().iter("Asset")}
    known = {os.path.splitext(f)[0].lower() for _, _, fs in os.walk(icons_out) for f in fs if f.endswith(".png")}
    missing = sorted(n for n in core_names if n and n.lower() not in known)
    log(f"core asset files: {len(core_names)} sprites, {len(missing)} not in the bundles")
    dest = os.path.join(icons_out, "_core_ui")
    for i in range(0, len(missing), 200):  # keep each command line well under Windows' limit
        rx = "^(" + "|".join(re.escape(n) for n in missing[i:i + 200]) + ")$"
        asmod(os.path.join(work, "core"), "-m", "export", "-t", "sprite", "-g", "none", "-o", dest,
              "--image-format", "png", "--filter-with-regex", "--filter-by-name", rx, "--log-level", "warning")


def dump_data(bundle_dir, work):
    dump = os.path.join(work, "dump")
    sources = {
        "scriptableobjects": "scriptableobjects_assets_all.bundle",
        "localization-shared": "localization-assets-shared_assets_all.bundle",
        "localization-en": next((f for f in os.listdir(bundle_dir)
                                 if f.startswith("localization-string-tables-english")), None),
    }
    for key, fn in sources.items():
        if not fn or not os.path.isfile(os.path.join(bundle_dir, fn)):
            die(f"expected bundle for '{key}' not found in {bundle_dir} - the game layout changed.")
        log(f"dumping {fn}")
        asmod(os.path.join(bundle_dir, fn), "-m", "jsonDump", "-t", "monoBehaviour", "-g", "type",
              "-f", "assetName_pathID", "-o", os.path.join(dump, key), "--log-level", "warning")
    return dump


# ---------------------------------------------------------------- name mapping

TAG = re.compile(r"<[^<>]{1,40}>")
NAME_FIELDS = ["<Name>k__BackingField", "<CarName>k__BackingField", "<NameString>k__BackingField",
               "<Title>k__BackingField", "<Header>k__BackingField", "_name", "_title", "_header",
               "<ShortName>k__BackingField", "<MiniTooltipTitle>k__BackingField", "_localizedString"]
ID_FIELDS = ["<Identified>k__BackingField", "<Id>k__BackingField", "_id", "Id", "_identifier"]


def json_files(folder):
    for r, _, fs in os.walk(folder):
        for f in fs:
            if f.endswith(".json"):
                with open(os.path.join(r, f), encoding="utf-8") as fh:
                    yield json.load(fh)


def build_rows(dump, icons_out, pid_path):
    # localization: key id -> English text (and key name -> text)
    key_names, text_by_id = {}, {}
    for key in ("localization-shared", "localization-en"):
        for d in json_files(os.path.join(dump, key)):
            for e in d.get("m_Entries", []):
                key_names[e["m_Id"]] = e["m_Key"]
            for e in d.get("m_TableData", []):
                text_by_id[e["m_Id"]] = e["m_Localized"]
    text_by_key = {key_names[i]: t for i, t in text_by_id.items() if i in key_names}

    def loc(v):
        if isinstance(v, dict) and isinstance(v.get("_localizedString"), dict):
            v = v["_localizedString"]
        if not isinstance(v, dict) or "m_TableEntryReference" not in v:
            return None
        ref = v["m_TableEntryReference"]
        t = text_by_id.get(ref.get("m_KeyId")) or text_by_key.get(ref.get("m_Key") or "")
        return re.sub(r"\s+", " ", TAG.sub("", t)).strip() if t else None

    # sprites: PathID -> (name, path relative to icons/)
    sprites = {}
    for a in ET.parse(os.path.join(icons_out, "assets.xml")).getroot().iter("Asset"):
        if a.findtext("Type") != "Sprite":
            continue
        name = a.findtext("Name")
        pid = int(a.findtext("PathID"))
        sprites[pid] = (name, pid_path.get(pid, ""))
    by_name = {}
    for s in sprites.values():
        by_name.setdefault(s[0], s)

    def pick_id(d):
        for k in ID_FIELDS:
            v = d.get(k)
            if isinstance(v, dict) and isinstance(v.get("_id"), str) and v["_id"]:
                return v["_id"]
            if isinstance(v, str) and v:
                return v
        return None

    def pick_name(d):
        for k in NAME_FIELDS:
            t = loc(d.get(k))
            if t:
                return t
        return None

    def sprite_of(v):
        if isinstance(v, dict):
            pid = v.get("m_PathID")
            if isinstance(pid, int) and pid in sprites:
                return sprites[pid]
            sub = v.get("m_SubObjectName")
            if isinstance(sub, str) and sub in by_name:
                return by_name[sub]
        return None

    records = []    # (sprite, path, id, name, source, field)
    id_index = {}   # lower(id) -> (id, name, source); entries with a name win
    skin_base = {}  # skin id -> ("name", car name) | ("id", weapon model id)

    def walk(node, ctx, asset):
        if isinstance(node, list):
            for x in node:
                walk(x, ctx, asset)
            return
        if not isinstance(node, dict):
            return
        my_id, my_name = pick_id(node), pick_name(node)
        if my_id and my_id not in skin_base:
            car = loc(node.get("<CarName>k__BackingField"))
            if car:
                skin_base[my_id] = ("name", car)
            weps = node.get("_weapons")
            if isinstance(weps, list) and weps and isinstance(weps[0], dict) and weps[0].get("Id"):
                skin_base[my_id] = ("id", weps[0]["Id"])
        if my_id:
            prev = id_index.get(my_id.lower())
            if prev is None or (my_name and not prev[1]):
                id_index[my_id.lower()] = (my_id, my_name or "", asset)
        here = (my_id or ctx[0], my_name or ctx[1])
        for k, v in node.items():
            for i, x in enumerate(v if isinstance(v, list) else [v]):
                s = sprite_of(x)
                if s:
                    field = k.replace("k__BackingField", "").strip("<>_") + (f"[{i}]" if isinstance(v, list) else "")
                    records.append((s[0], s[1], here[0] or "", here[1] or "", asset, field))
        for v in node.values():
            if isinstance(v, list) or (isinstance(v, dict) and "m_PathID" not in v):
                walk(v, here, asset)

    for d in json_files(os.path.join(dump, "scriptableobjects")):
        walk(d, (None, pick_name(d)), d.get("m_Name", ""))

    def base_of(item_id):
        kind, val = skin_base.get(item_id, ("", ""))
        if kind == "name":
            return val
        if kind == "id":
            hit = id_index.get(val.lower()) or id_index.get(re.sub(r"_m_\d+$", "", val).lower())
            return hit[1] if hit else ""
        return ""

    # id-only references: borrow the name from wherever that id is defined
    records = [r if r[3] or not r[2] else (*r[:3], id_index.get(r[2].lower(), ("", "", ""))[1], *r[4:])
               for r in records]
    # sprites loaded by Addressable GUID: the sprite name is the item id
    referenced = {r[0] for r in records}
    for name, path in sprites.values():
        hit = id_index.get(name.lower())
        if hit and name not in referenced:
            records.append((name, path, hit[0], hit[1], hit[2], "id==sprite"))
            referenced.add(name)

    seen, rows = set(), []
    for r in sorted(records, key=lambda r: (r[0].lower(), r[2] == "", r[3] == "", r[2], r[5])):
        if (r[0], r[2], r[5], r[4]) not in seen:
            seen.add((r[0], r[2], r[5], r[4]))
            rows.append(r)
    for name, path in sorted(sprites.values(), key=lambda s: s[0].lower()):
        if name not in referenced:
            rows.append((name, path, "", "", "", ""))
            referenced.add(name)
    rows.sort(key=lambda r: (r[0].lower(), r[3] == "", r[2]))
    rows = [(r[0], r[1], r[2], r[3], base_of(r[2]) if r[2] else "", r[4], r[5]) for r in rows]
    return rows, len(text_by_id), len(sprites)


# ---------------------------------------------------------------- C++ output

def cstr(s):
    out = []
    for b in s.encode("utf-8"):
        c = chr(b)
        out.append("\\\\" if c == "\\" else '\\"' if c == '"' else c if 0x20 <= b < 0x7F else "\\%03o" % b)
    return '"' + "".join(out) + '"'


def write_header(rows, unity_version):
    named = sum(1 for r in rows if r[3])
    lines = [
        f"// Generated by tools/loot/update_icons.py from Rob Run game data (Unity {unity_version}).",
        "// Do not edit by hand - re-run the script after a game update.",
        "// Maps every exported icon (icons/...) to the item id and English display name",
        "// that reference it. One sprite can appear several times when multiple items",
        "// or fields use it; sprites no item data references have empty id/name.",
        f"// Entries: {len(rows)}  with display name: {named}  distinct sprites: {len({r[0] for r in rows})}",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <string_view>",
        "",
        "namespace robrun::icons {",
        "",
        "struct IconEntry {",
        "    std::string_view sprite;    // sprite asset name (PNG file stem)",
        "    std::string_view path;      // path relative to icons/ (empty if not exported)",
        "    std::string_view id;        // game item id, e.g. \"gemstone_3\"",
        "    std::string_view name;      // English display name",
        "    std::string_view base;      // for car/weapon skins: the car or weapon's name",
        "    std::string_view source;    // ScriptableObject the reference came from",
        "    std::string_view field;     // field on that object holding the sprite",
        "};",
        "",
        f"inline constexpr std::array<IconEntry, {len(rows)}> kIcons{{{{",
        *("    {" + ", ".join(cstr(x) for x in r) + "}," for r in rows),
        "}};",
        "",
        "// First entry for a sprite name, preferring one with a display name.",
        "constexpr const IconEntry* find_by_sprite(std::string_view sprite) noexcept {",
        "    for (const auto& e : kIcons)",
        "        if (e.sprite == sprite) return &e;",
        "    return nullptr;",
        "}",
        "",
        "// First entry for a game item id.",
        "constexpr const IconEntry* find_by_id(std::string_view id) noexcept {",
        "    for (const auto& e : kIcons)",
        "        if (!id.empty() && e.id == id) return &e;",
        "    return nullptr;",
        "}",
        "",
        "// English display name for a sprite, or empty if none is known.",
        "constexpr std::string_view display_name(std::string_view sprite) noexcept {",
        "    const IconEntry* e = find_by_sprite(sprite);",
        "    return e ? e->name : std::string_view{};",
        "}",
        "",
        "}  // namespace robrun::icons",
        "",
    ]
    tmp = HEADER + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    os.replace(tmp, HEADER)


def uncstr(s):
    """Inverse of cstr() for the body of a literal."""
    out, i = bytearray(), 0
    while i < len(s):
        if s[i] == "\\" and re.match(r"[0-7]{3}", s[i + 1:i + 4]):
            out.append(int(s[i + 1:i + 4], 8)); i += 4
        elif s[i] == "\\":
            out += s[i + 1].encode(); i += 2
        else:
            out += s[i].encode(); i += 1
    return out.decode("utf-8", errors="replace")


def header_sprites():
    if not os.path.isfile(HEADER):
        return {}
    rows = {}
    with open(HEADER, encoding="utf-8") as f:
        for line in f:
            m = re.match(r'\s+\{"((?:[^"\\]|\\.)*)", "(?:[^"\\]|\\.)*", "(?:[^"\\]|\\.)*", "((?:[^"\\]|\\.)*)"', line)
            if m:
                names = rows.setdefault(uncstr(m.group(1)), set())
                if m.group(2):
                    names.add(uncstr(m.group(2)))
    return rows


def syntax_check():
    gxx = shutil.which("g++") or shutil.which("clang++")
    if not gxx:
        return "skipped (no g++/clang++ on PATH)"
    p = subprocess.run([gxx, "-std=c++17", "-fsyntax-only", "-x", "c++", HEADER],
                       capture_output=True, text=True)
    return "ok" if p.returncode == 0 else "FAILED:\n" + p.stderr[-1500:]


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=DEFAULT_GAME, help="Rob Run install folder")
    ap.add_argument("--keep-work", action="store_true", help="keep the temporary work folder")
    args = ap.parse_args()
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="replace")  # item names can include non-Latin text

    if not args.game:
        args.game = input("Enter Rob Run install folder: ").strip().strip('"')
    if not args.game:
        die("no game path provided. Pass --game or type it when prompted.")

    game_data = os.path.join(args.game, "ROB RUN_Data")
    bundle_dir = os.path.join(game_data, "StreamingAssets", "aa", "StandaloneWindows64")
    if not os.path.isdir(bundle_dir):
        die(f"bundle folder not found: {bundle_dir}")
    running = game_running()
    if running:
        die(f"close the game first (running: {', '.join(running)}).")

    started = time.time()
    ensure_asmod()
    work = tempfile.mkdtemp(prefix="robrun_icons_")
    log(f"work folder: {work}")
    try:
        # The data dumps don't depend on the sprite export, so they run alongside it.
        # (Running two image exports at once doesn't help: each already uses every core.)
        with cf.ThreadPoolExecutor(max_workers=1) as ex:
            dump_job = ex.submit(dump_data, bundle_dir, work)
            icons_out, pid_path = export_bundles(bundle_dir, work)
            export_core(game_data, work, icons_out)
            dump = dump_job.result()
        log("mapping sprites to item names")
        rows, n_strings, n_sprites = build_rows(dump, icons_out, pid_path)
        if n_sprites < 100 or n_strings < 100:
            die(f"suspiciously little data (sprites={n_sprites}, strings={n_strings}); leaving icons/ untouched.")

        before = header_sprites()
        old = ICONS_DIR + ".old"
        if os.path.isdir(old):
            shutil.rmtree(old)
        if os.path.isdir(ICONS_DIR):
            os.rename(ICONS_DIR, old)
        shutil.move(icons_out, ICONS_DIR)
        if os.path.isdir(old):
            shutil.rmtree(old)

        info = asmod(os.path.join(bundle_dir, "localization-locales_assets_all.bundle"), "-m", "info", check=False)
        ver = re.search(r"# (\d{4,}\.\d+\.\d+\w+)", info)
        write_header(rows, ver.group(1) if ver else "unknown")
    finally:
        if args.keep_work:
            log(f"kept work folder: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    after = {}
    for r in rows:
        names = after.setdefault(r[0], set())
        if r[3]:
            names.add(r[3])
    added = sorted(set(after) - set(before)) if before else []
    removed = sorted(set(before) - set(after)) if before else []
    renamed = sorted(s for s in set(after) & set(before) if before[s] != after[s])
    fmt = lambda names: " / ".join(sorted(names)) or "-"
    pngs = sum(1 for _, _, fs in os.walk(ICONS_DIR) for f in fs if f.endswith(".png"))

    print()
    log(f"done in {time.time() - started:.0f}s")
    print(f"  icons/          {pngs} PNGs")
    print(f"  icon_names.hpp  {len(rows)} entries, {len({r[0] for r in rows})} sprites, "
          f"{len({r[0] for r in rows if r[3]})} named")
    print(f"  syntax check    {syntax_check()}")
    if before:
        print(f"  changes         +{len(added)} new, -{len(removed)} removed, {len(renamed)} renamed")
        for label, items in (("new", added), ("removed", removed), ("renamed", renamed)):
            for s in items[:40]:
                extra = f"  ({fmt(before[s])} -> {fmt(after[s])})" if label == "renamed" else \
                        f"  {fmt(after.get(s) or before.get(s) or set())}"
                print(f"    {label:8} {s}{extra}")
            if len(items) > 40:
                print(f"    ... and {len(items) - 40} more {label}")


if __name__ == "__main__":
    main()
