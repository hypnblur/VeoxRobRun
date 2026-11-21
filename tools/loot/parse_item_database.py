"""Parse the saved ROB RUN item database page into a committed table.

    python tools/loot/parse_item_database.py ["ROB RUN Item Database.html"]

The page lists every item as a table row carrying the game id, the display
name, the market price, the rarity and the wiki's category:

    <tr class="loot-row" data-id="alcohol_12" ...
        ...<strong>Krystal Authority Bottle</strong>
        <span class="loot-price">23,960 $</span>
        <span class="loot-rarity">...Legendary</span>
        <span class="loot-category">Valuables</span>

Output: tools/loot/data/item-database.tsv
    id <TAB> name <TAB> category <TAB> rarity <TAB> price

This is the only price source the project has: the shipped game data carries
no numeric item prices, so the catalog takes them from here.
"""
import html
import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
DEFAULT_HTML = os.path.join(ROOT, "work", "ROB RUN Item Database.html")
OUT = os.path.join(ROOT, "data", "item-database.tsv")

ROW = re.compile(r'<tr class="loot-row"[^>]*data-id="([^"]+)"(.*?)</tr>', re.S)
NAME = re.compile(r'<strong>(.*?)</strong>', re.S)
PRICE = re.compile(r'class="loot-price">([^<]*)<')
RARITY = re.compile(r'class="loot-rarity"[^>]*>.*?</i>(.*?)</span>', re.S)
CATEGORY = re.compile(r'class="loot-category">([^<]*)<')

RARITIES = ("Legendary", "Epic", "Rare", "Uncommon", "Common", "Exotic")


def text(value):
    value = html.unescape(value or "")
    return re.sub(r"\s+", " ", value).strip()


def parse_price(value):
    digits = re.sub(r"[^0-9]", "", value or "")
    return int(digits) if digits else 0


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_HTML
    if not os.path.isfile(path):
        sys.exit(f"ERROR: {path} not found (save the item page as HTML first)")

    with open(path, encoding="utf-8", errors="replace") as fh:
        page = fh.read()

    rows, skipped = [], 0
    for match in ROW.finditer(page):
        item_id = match.group(1).strip()
        body = match.group(2)

        name = NAME.search(body)
        price = PRICE.search(body)
        rarity = RARITY.search(body)
        category = CATEGORY.search(body)

        if not item_id or not name or not category:
            skipped += 1
            continue

        rarity_text = text(rarity.group(1)) if rarity else ""
        category_text = text(category.group(1))
        # The page ships one template row ("${esc(item.category)}") that is not
        # an item; drop anything that still looks like markup.
        if "$" in category_text or "{" in category_text:
            skipped += 1
            continue
        rows.append((
            item_id,
            text(name.group(1)),
            category_text,
            rarity_text if rarity_text in RARITIES else "",
            parse_price(price.group(1) if price else ""),
        ))

    if not rows:
        sys.exit("ERROR: no loot rows parsed - did the page structure change?")

    seen, unique = set(), []
    for row in rows:
        if row[0] in seen:
            continue
        seen.add(row[0])
        unique.append(row)
    unique.sort(key=lambda r: (r[2].lower(), r[1].lower()))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# Parsed from the ROB RUN item database page by parse_item_database.py.\n")
        fh.write("# id\tname\tcategory\trarity\tprice\n")
        for row in unique:
            fh.write("\t".join(str(v) for v in row) + "\n")

    categories = {}
    for row in unique:
        categories[row[2]] = categories.get(row[2], 0) + 1
    priced = sum(1 for r in unique if r[4] > 0)
    print(f"items      {len(unique)} -> {OUT}")
    print(f"priced     {priced}")
    print(f"categories {len(categories)}")
    for name, count in sorted(categories.items(), key=lambda kv: -kv[1]):
        print(f"  {name:<22} {count}")
    if skipped:
        print(f"skipped    {skipped} malformed rows")


if __name__ == "__main__":
    main()
