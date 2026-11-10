sLaunch layout skins
====================

The Blades layout can draw from a folder of your own images instead of its
built-in look. sLaunch ships no skin art: this folder starts empty.

  sdmc:/slaunch/skins/blades/<skin name>/

A skin made by the import below is selected as it is made. Any other is
chosen by its folder's name: skin_blades=<skin name> in settings.txt under
sdmc:/slaunch/config. Delete a skin's folder to go back to the built-in look.
Every file below is optional; anything missing is drawn the built-in way.
Images are .png (with transparency) or .jpg. The screen is 1280x720.

Blades
------
  blade.png            The face of a blade, any size. Pinned to the screen
                       (stretched to 1280x720) and cut along the curve, so
                       the blade's colour is multiplied in: draw it light.
  blade_<cat>.png      One category's own face, drawn as is (no tint).
  spine.png            One whole blade as the left-hand stack shows it,
                       drawn straight (sLaunch bends it): the full height of
                       the screen, its leading edge on the right, its name
                       tab on the left. No name on it. Draw it in light
                       metal: the open blade's is tinted its colour.
  spine_right.png      The same for the right-hand stack (the mirror image).
                       Without it spine.png is used on both sides.
  light.png            What lies over an open blade's flat colour: glow,
                       shadow, bands, as see-through white and black.
  light_wide.png       The same without the bands, for the screens that
                       fill the display with one blade (About, Files...).
  spine_<cat>.png      One category's own spine, drawn as is.
  panel.png            The list panel (nine-slice, see below).
  highlight.png        The bar behind the selected list entry (nine-slice).
  detail.png           The panel beside the list (nine-slice; panel.png if
                       missing).
  gamercard.png        The gamer card above the list, 500x102 on screen
                       (nine-slice).

  <cat> is one of: settings media user network games homebrew

Everywhere
----------
  button_a.png button_b.png button_x.png button_y.png
                       The button glyphs in the hint line, 22x22 on screen.
  font.ttf             Used for every label in the layout.
  sounds/              Sound effects, the same file names as
                       sdmc:/slaunch/sounds (move.wav, confirm.wav, ...).
                       Without it the Metro set (sounds/metro) is used.
  skin.ini             Colours and borders, all optional:

    ; category colours, RRGGBB or RRGGBBAA
    settings = 7c8492
    media    = 1e70cc
    user     = 9258be
    network  = ec8c12
    games    = 68b220
    homebrew = 1a9c94
    ; text
    text               = ffffff   ; titles
    list_text          = ffffff   ; list entries
    list_text_selected = 17270a   ; the selected one
    ; nine-slice borders, in pixels of the image
    panel_border     = 16
    highlight_border = 12
    gamercard_border = 12

Nine-slice
----------
A nine-slice image keeps its corners at their size and stretches the edges
and middle, so one small framed image fits a box of any size. The border is
how many pixels in from each edge the corners reach in the image.

From your own dashboard
-----------------------
Copy the resource packages (res.xzp, shrdres.xzp) from a dashboard you have
dumped from your own console into

  sdmc:/slaunch/skins/import/blades/

then switch to the Blades layout (restart the menu if it is already on).
sLaunch builds a skin called "dashboard" from them (button glyphs, list
panel, highlight), selects it, and renames the packages to .xzp.done so they
are not read again. Sounds and fonts are not converted:
add them to the skin folder yourself.

Only use art you made yourself or have the right to use.
