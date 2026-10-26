"""Build the GPL-2.0-or-later PC symbol font from its included editable source."""
# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import hashlib
import json
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen

folder = Path(__file__).resolve().parents[1] / 'assets/fonts_open'
font = TTFont(recalcTimestamp=False)
font.importXML(folder / 'symbols-source.ttx')
# Original geometric fallback: an outlined diamond, visibly distinct from game icons.
pen = TTGlyphPen(None)
for points in (((64, 384), (448, 768), (832, 384), (448, 0)),
               ((448, 100), (732, 384), (448, 668), (164, 384))):
    pen.moveTo(points[0])
    for point in points[1:]:
        pen.lineTo(point)
    pen.closePath()
order = font.getGlyphOrder()[:]
font['glyf']['pcFallback'] = pen.glyph()
font['hmtx'].metrics['pcFallback'] = (896, 64)
font.setGlyphOrder(order + ['pcFallback'])
aliases = {0xe042: 0xe0a0, 0xe04c: 0xe0a0, 0xe043: 0xe0a1, 0xe04d: 0xe0a1,
           0xe04e: 0xe0a2, 0xe04f: 0xe0a3, 0xe052: 0xe0a6, 0xe083: 0xe0a6,
           0xe053: 0xe0a7, 0xe084: 0xe0a7, 0xe054: 0xe0a8, 0xe085: 0xe0a8,
           0xe055: 0xe0a9, 0xe086: 0xe0a9, 0xe045: 0xe0b3, 0xe046: 0xe0b4,
           0xe0b5: 0xe0b3, 0xe0f1: 0xe0b3, 0xe0b6: 0xe0b4, 0xe0f2: 0xe0b4}
for table in font['cmap'].tables:
    if table.isUnicode():
        for code, target in aliases.items():
            table.cmap[code] = table.cmap[target]
        for code in range(0xe000, 0xf900):
            table.cmap.setdefault(code, 'pcFallback')
for ident, text in ((1, 'MM2 PC Symbols'), (2, 'Regular'), (3, 'MM2 PC Symbols 1.0'),
                    (4, 'MM2 PC Symbols'), (6, 'MM2PCSymbols')):
    for platform, encoding, language in ((3, 1, 0x409), (1, 0, 0)):
        font['name'].setName(text, ident, platform, encoding, language)
for name in ('nintendo_ext_003.ttf', 'nintendo_ext2_003.ttf'):
    font.save(folder / name)
manifest = json.loads((folder / 'manifest.json').read_text())
manifest['symbol_modification'] = 'MM2 PC Symbols: aliases and original diamond fallback; tools/build_open_symbols.py'
for path in folder.iterdir():
    if path.is_file() and path.name != 'manifest.json':
        manifest['files'][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
(folder / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
assert all(font.getBestCmap().get(code) for code in range(0xe000, 0xf900))
print('Built PC symbol fonts with complete private-use fallback coverage.')
