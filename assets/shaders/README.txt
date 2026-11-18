Background shaders
==================

Every .frag file here is a background: Theming > Themes, edit a custom
theme, and step through Background until its name comes up.

A file is the body of a GLSL ES 1.0 fragment shader. Write main() and any
helpers; do not declare uniforms or varyings, the menu supplies these:

  vec2  vP      this pixel, in 1280x720 menu pixels (0,0 = top left)
  float uTime   seconds, always increasing
  vec4  uColA   the theme's effect colour A        (rgba, 0..1)
  vec4  uColB   the theme's effect colour B
  vec4  uTop    the theme's gradient top colour
  vec4  uBot    the theme's gradient bottom colour
  vec4  uFx     the theme editor's knobs: lines, thickness, amplitude, seed
  vec4  uFx2    ...and layers, y, x, camera

Write gl_FragColor with premultiplied alpha: vec4(rgb * a, a). Alpha 1 covers
the theme's gradient; less lets it show through. The shader runs for every
pixel of every frame, so keep it light: a few sin() calls per pixel are fine,
long loops are not.

On the console
--------------
The console cannot compile GLSL, so each .frag needs a .dksh beside it, of
the same name. On a PC with devkitPro installed:

  python3 projects/sMenu/scripts/bg-shader.py myshader.frag

from a checkout of sLaunch, then copy myshader.dksh here. A file without its
.dksh (or with one built from an older .frag) shows the plain gradient.
