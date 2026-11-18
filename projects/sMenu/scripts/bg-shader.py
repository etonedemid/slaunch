#!/usr/bin/env python3
"""Builds background shader files for the console: name.frag -> name.dksh.

A background shader is a GLSL ES fragment shader body, as the menu's own
effects are written (see sdmc:/slaunch/shaders/README.txt). The simulator and
GL builds compile name.frag themselves; deko3d cannot compile on the console,
so this compiles it with devkitPro's uam, laid out exactly as the menu's
built-in BgFile program (Gfx.cpp) expects, and the menu loads the .dksh.

Usage: bg-shader.py [-o OUTDIR] FILE.frag...
"""
import importlib.util, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('fxspirv', os.path.join(HERE, 'fx-spirv.py'))
fx = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fx)
fx.DKSH = True

GFX = os.path.join(HERE, '..', 'source', 'sl', 'menu', 'gfx', 'Gfx.cpp')


def main():
    args, out_dir = sys.argv[1:], None
    if len(args) >= 2 and args[0] == '-o':
        out_dir, args = args[1], args[2:]
    if not args:
        sys.exit(__doc__)
    prog = fx.literals([GFX]).get('BgFile')
    if not prog:
        sys.exit('bg-shader: BgFile program not found in Gfx.cpp')
    vs, fs = prog['Vs'], prog['Fs']
    head = fs[:fs.index('void main')]             # the declarations a file may use
    table = []
    for u in fx.VS_PRELUDE_UNIFORMS + fx.uniforms(vs) + fx.uniforms(fs):
        if u[1] not in [n for _, n in table]:
            table.append(u)
    for path in args:
        body = open(path, encoding='utf-8').read()
        if re.search(r'^\s*(uniform|varying)\b', body, re.M):
            sys.exit('bg-shader: %s: declare no uniforms or varyings, the menu supplies them' % path)
        src = head + body
        frag = ('#version 450\nlayout(location = 0) out vec4 fx_FragColor;\n' + fx.block(table, 3) +
                re.sub(r'^\s*varying\s+vec2\s+vP\s*;', 'layout(location = 0) in vec2 vP;',
                       fx.strip_decls(src), flags=re.M))
        words = fx.compile_glsl(frag, 'frag')
        name = os.path.splitext(os.path.basename(path))[0] + '.dksh'
        dst = os.path.join(out_dir or os.path.dirname(os.path.abspath(path)), name)
        with open(dst, 'wb') as f:
            f.write(b''.join(w.to_bytes(4, 'little') for w in words))
        print('bg-shader: %s -> %s' % (path, dst))


if __name__ == '__main__':
    main()
