# Open-source replacement fonts

These seven files replace firmware shared fonts. Their filenames match the runtime's font requests; the font data comes from open-source projects, not Nintendo firmware.

The text fonts are Open Sans and Source Sans Pro. The two symbol files are the modified MM2 PC Symbols font, based on YuzuOSSExtension. Button aliases are included. Unsupported private symbols display an original outlined diamond so the game can initialize its glyph cache safely. CJK coverage is limited by these fonts and the fonts included in the user's game RomFS.

The upstream font package was obtained from OpenSmash-Ultimate/ssbu-recomp at commit `fea28c18b785372b5024ece0a2ea52e860b1fa04`, in `runtime/fonts_open`. Its provenance is recorded below; our checked hashes are in `manifest.json`.

## Files

The upstream binaries were reconstructed from C++ font arrays in the suyu repository at commit
`c0dd2ff554327657f0c6318f29b740ac7e329ea0` (https://github.com/suyu-emu/suyu-v0.0.4), folder
`src/core/file_sys/system_archive/data/`, where each font is a C++ array of the plain TrueType bytes. The script
checks the SHA-256 of each source file, converts the array back into the .ttf and gives the same bytes as here.

| File here | Source file (SHA-256 of the .cpp) | Font | Licence |
|---|---|---|---|
| nintendo_udsg-r_std_003.ttf | font_standard.cpp (0355fb64938bd66d96a326ab0681d551803281fe641078bf42cf84e729e5780c) | Open Sans Regular 1.10 | Apache-2.0 |
| nintendo_udsg-r_org_zh-cn_003.ttf | font_chinese_simplified.cpp (f91d1223e5c79509daf11318bf83e8ca5ed3626a0d2c6ee86d3b202f303bc0e5) | Open Sans Regular 1.10 | Apache-2.0 |
| nintendo_udsg-r_ext_zh-cn_003.ttf | font_extended_chinese_simplified.cpp (da47566f0dd8b5e28129b1dca7b2bb5f5b2af5c1841552742ed48cb27e9f4200) | Source Sans Pro Regular 2.021 | OFL-1.1 |
| nintendo_udjxh-db_zh-tw_003.ttf | font_chinese_traditional.cpp (bc6d49f2c471861e61e6a08c952088690c2bee52ef443a4fdc9a36af816b2394) | Open Sans Light 1.10 | Apache-2.0 |
| nintendo_udsg-r_ko_003.ttf | font_korean.cpp (2cb950e2de8ff3ff79c2bd88e22f7eaa70063a7c50abbd06698c81f74bf52a63) | Open Sans Regular 1.10 | Apache-2.0 |
| nintendo_ext_003.ttf | font_nintendo_extended.cpp (6fbbf5176ff4fd2024f9d150c889477f6b45ece4ac5e45d51286e32f8b13a6f2) | YuzuOSSExtension 1.000 | GPL-2.0-or-later |
| nintendo_ext2_003.ttf | font_nintendo_extended.cpp (same file as above) | YuzuOSSExtension 1.000 | GPL-2.0-or-later |

## Licences

- **Open Sans** - "Digitized data copyright (c) 2010-2011, Google Corporation." Licensed under the Apache License,
  Version 2.0 (the font's own name table says so): `LICENSE-Apache-2.0.txt`. Open Sans is a trademark of Google and
  may be registered in certain jurisdictions.
- **Source Sans Pro** - "Copyright 2010, 2012, 2014 Adobe Systems Incorporated (http://www.adobe.com/), with Reserved
  Font Name 'Source'." Licensed under the SIL Open Font License, Version 1.1: `LICENSE-OFL-1.1.txt`. Distributed
  unmodified (only the file name differs). Source is a trademark of Adobe Systems Incorporated in the United States
  and/or other countries.
- **YuzuOSSExtension** - made by the yuzu Emulator Project (copyright 2019); the suyu source file carries
  `SPDX-License-Identifier: GPL-2.0-or-later`: `LICENSE-GPL-2.0-or-later.txt`. Its source is the source file named
  above.

The licence texts: Apache-2.0 and GPL-2.0-or-later as published in suyu's `LICENSES/` folder at the same commit;
the OFL text as published by Adobe with Source Sans (`adobe-fonts/source-sans`, `LICENSE.md`), headed by the
copyright notice of the font file shipped here.

## Editable symbol source

`symbols-source.ttx` contains the editable upstream YuzuOSSExtension font tables. `tools/build_open_symbols.py` adds button-code aliases and the original diamond fallback, then renames the family to MM2 PC Symbols. Both the derivative and its source retain GPL-2.0-or-later. Install FontTools to rebuild; FontTools is not needed to play.
