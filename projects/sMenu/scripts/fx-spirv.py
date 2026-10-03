#!/usr/bin/env python3
"""SPIR-V for the menu's GPU effects, for the Vulkan build (make VULKAN=1).

The effects are GLSL ES string literals in the C++ sources, written for the
OpenGL path (Gfx.cpp's raw GL section). This pulls every `k<Name>Vs` /
`k<Name>Fs` raw literal out of the given sources, rewrites it as Vulkan GLSL,
compiles it with glslangValidator and writes one header of SPIR-V words.

Each program is keyed by an FNV-1a hash of its two source strings, which the
menu computes at run time from the same literals - so an edited shader that
was not rebuilt is simply not found, and that effect keeps its fallback.

Rewriting: uniforms go into one std140 block of vec4s (`#define`s keep the
names; a float lives in .x), varyings get locations by name, and gl_FragColor
and texture2D take their 450 forms. Vertex uniforms are set 1 and fragment
uniforms set 3, samplers set 2 - SDL_GPU's SPIR-V layout.

`Card` is special: SDL's GPU renderer runs it as a custom fragment shader
behind its own vertex shader, whose outputs are the vertex colour (location
0) and the texture coordinate (location 1) - so the shader's `vP` is read
from the texture coordinate, which the menu fills with the pixel position.

Usage: fx-spirv.py OUT.h SOURCE...
"""
import re, subprocess, sys, tempfile, os

# Must match kVsPrelude / kFsPrelude in Gfx.cpp.
VS_PRELUDE_UNIFORMS = [('float', 'uFlip'), ('float', 'uTime')]
FS_PRELUDE_UNIFORMS = [('float', 'uTime')]
VS_PRELUDE_CODE = '''const float TAU = 6.2831853;
vec4 Clip(vec2 p) { return vec4(p.x / 640.0 - 1.0, (1.0 - p.y / 360.0) * uFlip, 0.0, 1.0); }
float Hash(float v) { return fract(sin(v) * 43758.5453); }
vec2 Corner() { return vec2(mod(aV.w, 2.0), floor(aV.w / 2.0)); }
'''
SWIZZLE = {'float': '.x', 'vec2': '.xy', 'vec3': '.xyz', 'vec4': ''}


def fnv1a(data, h=0x811c9dc5):
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def literals(paths):
    progs = {}
    for p in paths:
        src = open(p, encoding='utf-8').read()
        for m in re.finditer(r'\bk(\w+?)(Vs|Fs)\s*=\s*R"\((.*?)\)"', src, re.S):
            progs.setdefault(m.group(1), {})[m.group(2)] = m.group(3)
    return {k: v for k, v in progs.items() if 'Fs' in v}


def uniforms(body):
    out = []
    for m in re.finditer(r'^\s*uniform\s+(float|vec2|vec3|vec4)\s+([^;]+);', body, re.M):
        out += [(m.group(1), n.strip()) for n in m.group(2).split(',')]
    return out


def strip_decls(body):
    body = re.sub(r'^\s*uniform\s+(float|vec2|vec3|vec4)\s+[^;]+;[^\n]*\n', '', body, flags=re.M)
    return body.replace('gl_FragColor', 'fx_FragColor').replace('texture2D(', 'texture(')


def varyings(body):
    out = []
    for m in re.finditer(r'^\s*varying\s+(\w+)\s+([^;]+);', body, re.M):
        out += [(m.group(1), n.strip()) for n in m.group(2).split(',')]
    return out


def block(table, set_):
    lines = ['layout(std140, set = %d, binding = 0) uniform FxU { vec4 v[%d]; } fxu;' % (set_, max(1, len(table)))]
    lines += ['#define %s (fxu.v[%d]%s)' % (n, i, SWIZZLE[t]) for i, (t, n) in enumerate(table)]
    return '\n'.join(lines) + '\n'


def compile_glsl(text, stage):
    with tempfile.TemporaryDirectory() as d:
        src, spv = os.path.join(d, 'fx.' + stage), os.path.join(d, 'fx.spv')
        open(src, 'w').write(text)
        r = subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.0', '-S', stage,
                            '-o', spv, src], capture_output=True, text=True)
        if r.returncode:
            numbered = '\n'.join('%3d %s' % (i + 1, l) for i, l in enumerate(text.split('\n')))
            sys.exit('fx-spirv: %s shader failed:\n%s\n%s' % (stage, r.stdout, numbered))
        data = open(spv, 'rb').read()
    return [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]


def main():
    out, paths = sys.argv[1], sys.argv[2:]
    entries, blobs = [], []
    for name, p in sorted(literals(paths).items()):
        vs, fs = p.get('Vs', ''), p['Fs']
        samplers = len(re.findall(r'^\s*uniform\s+sampler2D\s+\w+\s*;', fs, re.M))
        if name == 'Card':
            table = uniforms(fs) + FS_PRELUDE_UNIFORMS
            body = strip_decls(fs)
            body = re.sub(r'^\s*varying\s+vec2\s+vP\s*;', 'layout(location = 1) in vec2 vP;', body, flags=re.M)
            body = re.sub(r'^\s*uniform\s+sampler2D\s+(\w+)\s*;',
                          r'layout(set = 2, binding = 0) uniform sampler2D \1;', body, flags=re.M)
            frag = ('#version 450\nlayout(location = 0) in vec4 fx_color;\n'
                    'layout(location = 0) out vec4 fx_FragColor;\n' + block(table, 3) + body)
            vs_words, fs_words = [], compile_glsl(frag, 'frag')
        else:
            if not vs:
                continue
            table = []
            for u in VS_PRELUDE_UNIFORMS + uniforms(vs) + uniforms(fs):
                if u[1] not in [n for _, n in table]:
                    table.append(u)
            names = sorted({n for _, n in varyings(vs) + varyings(fs)})
            loc = {n: i for i, n in enumerate(names)}

            def io(body, qual):
                return re.sub(r'^\s*varying\s+(\w+)\s+([^;]+);',
                              lambda m: ' '.join('layout(location = %d) %s %s %s;' % (loc[n.strip()], qual, m.group(1), n.strip())
                                                 for n in m.group(2).split(',')),
                              body, flags=re.M)
            vert = ('#version 450\nlayout(location = 0) in vec4 aV;\n' + block(table, 1) +
                    VS_PRELUDE_CODE + io(strip_decls(vs), 'out'))
            fbody = re.sub(r'^\s*uniform\s+sampler2D\s+(\w+)\s*;',
                           r'layout(set = 2, binding = 0) uniform sampler2D \1;', strip_decls(fs), flags=re.M)
            frag = ('#version 450\nlayout(location = 0) out vec4 fx_FragColor;\n' + block(table, 3) +
                    io(fbody, 'in'))
            vs_words, fs_words = compile_glsl(vert, 'vert'), compile_glsl(frag, 'frag')
        h = fnv1a(fs.encode(), fnv1a(vs.encode()))
        blobs.append((name, vs_words, fs_words))
        entries.append((name, h, table, samplers))

    with open(out, 'w') as f:
        f.write('// generated by scripts/fx-spirv.py - do not edit\n#pragma once\n#include <cstdint>\n#include <cstddef>\n\n')
        f.write('namespace fxspv {\n')
        f.write('struct Program {\n    const char *name;\n    uint32_t hash;   // FNV-1a over the vertex then the fragment source\n'
                '    const uint32_t *vs; size_t vs_size;   // bytes; none for Card\n'
                '    const uint32_t *fs; size_t fs_size;\n'
                '    const char *const *uniforms; int n_uniforms;   // vec4 slots, in order\n'
                '    int n_samplers;\n};\n\n')
        for name, vw, fw in blobs:
            for stage, words in (('vs', vw), ('fs', fw)):
                if words:
                    f.write('inline constexpr uint32_t k%s_%s[] = {%s};\n' % (name, stage, ','.join('0x%08x' % w for w in words)))
        for name, h, table, _ in entries:
            f.write('inline constexpr const char *k%s_u[] = {%s};\n' % (name, ', '.join('"%s"' % n for _, n in table)))
        f.write('\ninline constexpr Program kPrograms[] = {\n')
        for (name, h, table, samplers), (_, vw, fw) in zip(entries, blobs):
            vs = ('k%s_vs, sizeof(k%s_vs)' % (name, name)) if vw else 'nullptr, 0'
            f.write('    { "%s", 0x%08xu, %s, k%s_fs, sizeof(k%s_fs), k%s_u, %d, %d },\n'
                    % (name, h, vs, name, name, name, len(table), samplers))
        f.write('};\n\n} // namespace fxspv\n')


if __name__ == '__main__':
    main()
