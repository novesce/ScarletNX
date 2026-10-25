# The shared fonts' Noto stand-ins

The seven Switch shared fonts are the console firmware's and cannot be shipped. When a game folder has no
`fonts/` of its own, `src/nx/hle/services/pl.c` hands out the fonts in this folder instead: one Noto
Sans subset per script, and yuzu's controller button glyphs for `FontNintendoExtended`, which
`cmake/shared_fonts.cmake` takes from Eden's source. `cmake/shared_fonts.cmake` compiles these four
files into the runtime, and `pl_font.c` gives every one of them blank glyphs for the console's
private-use icon block (U+E000 to U+E1FF), so a layout that names an icon gets a glyph rather than a
null one.

| File | Slot | Covers |
| --- | --- | --- |
| `NotoSansJP-Subset.ttf` | `FontStandard` | ASCII, Latin-1, punctuation, JIS X 0208 (kana and kanji) |
| `NotoSansSC-Subset.ttf` | `FontChineseSimplified`, `FontExtendedChineseSimplified` | ASCII, Latin-1, punctuation, GB 2312 |
| `NotoSansTC-Subset.ttf` | `FontChineseTraditional` | ASCII, Latin-1, punctuation, Big5 |
| `NotoSansKR-Subset.ttf` | `FontKorean` | ASCII, Latin-1, punctuation, KS X 1001 (Hangul and Hanja) |

The console's shared font memory is 0x1100000 bytes and the SDK maps exactly that much (Eden's
`pl_u.cpp` has the same constant), so the stand-ins have to fit: the four total about 11 MB. The
full Noto Sans JP/SC/TC/KR variable fonts do not fit, and their pan-CJK sibling is CFF, which the SDK's
font parser did not load in a test (the run ended in the same boot crash); the subsets are TrueType
`glyf` at the Regular weight, the script's standard character set, with hinting dropped.

## Regenerating

`tools/subset_fonts.py` makes these from Google Fonts' Noto Sans JP, SC, TC and KR at commit
`e7db6ca603fd4862010383ff17ffd6cc4ffac1c0` (`ofl/notosansjp`, `ofl/notosanssc`, `ofl/notosanstc`,
`ofl/notosanskr`, `NotoSansXX[wght].ttf`). It needs `fontTools` and network access, neither of which
the build needs:

```sh
python3 tools/subset_fonts.py --out dist/fonts
```

It instantiates each variable font at weight 400, keeps ASCII, Latin-1, common punctuation and the
script's standard set, drops hinting and the variation tables, and writes the four files above. The
sources are under the SIL Open Font License 1.1; the text is `dist/licenses/Noto/OFL.txt`.
