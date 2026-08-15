# Minimal icon pack

Icons by **MeepCat55** (https://github.com/meepcat55), contributed as
[pull request #3](https://github.com/etonedemid/slaunch/pull/3) and shipped here
as a selectable icon pack rather than as a replacement for the built-in set, so
both styles are available from Theming > Appearance > Icon pack.

Drawn in Inkscape; the source vectors are in `source-svgs/` if you want to
re-export them at a different size. The PNGs are 256x256 RGBA and the menu
rescales them to 64x64 on load.

## Later additions (not by MeepCat55)

`settings.png`, `music.png`, `games.png` and `homebrewmenu.png` were added
afterwards to complete the set, and are **not** MeepCat55's work. They follow the
same convention as the originals - a black circular plate of radius 110 centred
in a 256x256 RGBA canvas, white artwork on top, transparent outside the plate -
but they were generated rather than drawn in Inkscape, so they have no entry in
`source-svgs/`.

The original `settings.png` in this pack was a wifi glyph (its source vector is
still `source-svgs/settings.svg`, saved by Inkscape under the name `wifi.svg`),
so it was showing a wifi symbol against the Settings entry. It has been kept as
`wifi.png` and replaced with a gear.
