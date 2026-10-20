#!/usr/bin/env python3
"""Build redistributable text-based controller hints from licensed DejaVu Sans.

Requires fontTools only when regenerating these committed assets.
"""
from pathlib import Path
from fontTools import subset
from fontTools.ttLib import TTFont
import argparse
import shutil

p = argparse.ArgumentParser()
p.add_argument("--font", default="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
p.add_argument("--license", default="/usr/share/doc/fonts-dejavu-core/copyright")
args = p.parse_args()
root = Path(__file__).resolve().parents[1] / "resources" / "font"
font = TTFont(args.font)
mapping = {
    0xE0E0: "A", 0xE0E1: "B", 0xE0E2: "X", 0xE0E3: "Y",
    0xE0E4: "L", 0xE0E5: "R", 0xE0E6: "L", 0xE0E7: "R",
    0xE0EF: "+", 0xE0F0: "-", 0xE104: "L", 0xE105: "R",
    0xE0ED: "←", 0xE0EB: "↑", 0xE0EE: "→", 0xE0EC: "↓", 0xE152: "?",
}
cmap = font.getBestCmap()
for table in font["cmap"].tables:
    if table.isUnicode():
        for code, char in mapping.items():
            table.cmap[code] = cmap[ord(char)]
sub = subset.Subsetter()
sub.populate(unicodes=list(mapping))
sub.subset(font)
font["name"].setName("Netease Controller Hints", 1, 3, 1, 0x409)
font["name"].setName("Netease Controller Hints", 4, 3, 1, 0x409)
font["name"].setName("NeteaseControllerHints", 6, 3, 1, 0x409)
font.save(root / "switch_icons.ttf")
shutil.copyfile(args.font, root / "switch_font.ttf")
shutil.copyfile(args.license, root / "DEJAVU-LICENSE.txt")
