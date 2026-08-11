# sLaunch bundled fonts

The fonts in the table below are redistributable open-licensed fonts.

At runtime these are installed to `sdmc:/slaunch/fonts/` and selectable from the
Fonts screen. Users can add their own `.ttf` / `.otf` / `.ttc` files to that
folder too.

| File | Family | Author | License |
|------|--------|--------|---------|
| PressStart2P-Regular.ttf   | Press Start 2P     | CodeMan38                     | SIL OFL 1.1 |
| VT323-Regular.ttf          | VT323              | Peter Hull                    | SIL OFL 1.1 |
| Lobster-Regular.ttf        | Lobster            | Impallari Type                | SIL OFL 1.1 |
| RedactedScript-Regular.ttf | Redacted Script    | Christian Naths               | SIL OFL 1.1 |
| NotoSansSymbols2-Regular.ttf | Noto Sans Symbols 2 | Google (Noto Project)      | SIL OFL 1.1 |
| NotoSansCJK-Regular.ttc    | Noto Sans CJK      | Google / Adobe (Noto Project) | SIL OFL 1.1 |

Full license text: `LICENSE-OFL.txt`.

## About the CJK font

`NotoSansCJK-Regular.ttc` (19 MB) is a TrueType collection covering Japanese,
Korean, Simplified Chinese and Traditional Chinese in one file; the menu opens
face 0, which carries the whole repertoire. It is here so a user can pick a CJK
font by hand, e.g. to read game names in a script their system language does not
select.

It is not needed for the menu's own text. sLaunch already picks the console's
matching shared font (Nintendo ships Korean and both Chinese fonts alongside the
Standard one) from the system language, so a Korean or Chinese console renders
correctly with nothing installed. See `Gfx::Init`.
