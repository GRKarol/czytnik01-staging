#!/usr/bin/env python3
"""Write <dir>/index.json listing the .fnt files in <dir> with their sizes.

The web flasher's card step (src/flasher/card-setup.ts) reads it to know
which fonts to copy onto the reader's card. Run by deploy.yml after
firmware/tools/generate_font_pack.sh fills the folder.

Usage: python tools/write_font_index.py public/fonts
"""
import json
import sys
from pathlib import Path

folder = Path(sys.argv[1])
files = sorted(folder.glob("*.fnt"))
if not files:
    sys.exit(f"no .fnt files in {folder}")
index = {"files": [{"name": f.name, "size": f.stat().st_size} for f in files]}
(folder / "index.json").write_text(json.dumps(index, indent=1) + "\n", encoding="utf-8")
print(f"{len(files)} fonts, {sum(f.stat().st_size for f in files)} bytes")
