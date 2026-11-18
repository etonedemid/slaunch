#!/usr/bin/env python3
"""Packs an Xbox 360 avatar export's animations for the 3D Mii.

    avatar-anims.py <AvatarExtract dir> <out anims.bin>

Input: the folder an avatar exporter wrote (avatar.gltf + avatar.bin +
face_keyframes.txt). The animations belong to Microsoft: this only converts
your own export for your own console. Copy the output to
sdmc:/slaunch/mii/anims.bin; it is never shipped with sLaunch.

Output, little endian:
  "SLMA" u32 version=1 u16 joints u16 anims  u16 xbox_joint[joints]
  per anim: char name[32] u16 frames u16 face_keys  f32 fps
            frames x (f32 root_dx, root_dy, root_dz)      root offset from bind
            frames x joints x i16[4] rotation quaternion (x y z w) * 32767
            face_keys x (u16 frame, u8 mouth, u8 eyebrow, u8 eye)  + pad to 4
Rotations are local, relative to a bind pose whose rotations are all identity,
so they apply to any skeleton with the same joint directions.
"""
import json, math, struct, sys, os

# The joints the Mii body uses, by Xbox index (see MiiBody.hpp).
JOINTS = [0, 1, 5, 14, 19,            # pelvis, spine, chest, neck, head
          12, 20, 25, 33,             # left clavicle, shoulder, elbow, hand
          16, 22, 28, 36,             # right
          2, 6, 11, 21,               # left hip, knee, ankle, toe
          3, 8, 15, 23]               # right
FPS = 30.0


def accessor(g, blob, i):
    a = g['accessors'][i]
    v = g['bufferViews'][a['bufferView']]
    n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
    fmt = {5126: 'f', 5121: 'B', 5123: 'H', 5125: 'I'}[a['componentType']]
    size = struct.calcsize(fmt) * n
    off = v.get('byteOffset', 0) + a.get('byteOffset', 0)
    stride = v.get('byteStride', size)
    return [struct.unpack_from('<' + fmt * n, blob, off + stride * k) for k in range(a['count'])]


def slerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b, d = [-x for x in b], -d
    if d > 0.9995:
        r = [x + (y - x) * t for x, y in zip(a, b)]
    else:
        th = math.acos(d)
        s = math.sin(th)
        wa, wb = math.sin((1 - t) * th) / s, math.sin(t * th) / s
        r = [wa * x + wb * y for x, y in zip(a, b)]
    n = math.sqrt(sum(x * x for x in r)) or 1
    return [x / n for x in r]


def sample(times, values, t, rot):
    if t <= times[0]:
        return list(values[0])
    if t >= times[-1]:
        return list(values[-1])
    lo, hi = 0, len(times) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if times[mid] <= t:
            lo = mid
        else:
            hi = mid
    f = (t - times[lo]) / (times[hi] - times[lo])
    if rot:
        return slerp(values[lo], values[hi], f)
    return [x + (y - x) * f for x, y in zip(values[lo], values[hi])]


def main():
    src, out = sys.argv[1], sys.argv[2]
    g = json.load(open(os.path.join(src, 'avatar.gltf')))
    blob = open(os.path.join(src, 'avatar.bin'), 'rb').read()
    node_of_joint = {int(n['name'][1:]): i for i, n in enumerate(g['nodes']) if n.get('name', '').startswith('j')}

    faces = {}
    fk = os.path.join(src, 'face_keyframes.txt')
    if os.path.exists(fk):
        for line in open(fk):
            p = line.split()
            if len(p) == 5:
                faces.setdefault(p[0], []).append((int(p[1]), int(p[2]), int(p[3]), int(p[4])))

    anims = []
    for a in g['animations']:
        tracks = {}
        dur = 0.0
        for ch in a['channels']:
            s = a['samplers'][ch['sampler']]
            times = [t[0] for t in accessor(g, blob, s['input'])]
            vals = accessor(g, blob, s['output'])
            tracks[(ch['target']['node'], ch['target']['path'])] = (times, vals)
            dur = max(dur, times[-1])
        frames = max(1, int(round(dur * FPS)) + 1)
        root, rots = [], []
        for f in range(frames):
            t = f / FPS
            # The exporter writes the root's translation as an offset from its bind position.
            tr = tracks.get((node_of_joint[0], 'translation'))
            root.append(sample(*tr, t, False) if tr else [0.0, 0.0, 0.0])
            for j in JOINTS:
                r = tracks.get((node_of_joint[j], 'rotation'))
                q = sample(*r, t, True) if r else [0, 0, 0, 1]
                rots.append([max(-32767, min(32767, int(round(c * 32767)))) for c in q])
        # Face key frames are in the source's 30 fps frames already.
        anims.append((a['name'], frames, root, rots, faces.get(a['name'], [])))

    with open(out, 'wb') as f:
        f.write(b'SLMA' + struct.pack('<IHH', 1, len(JOINTS), len(anims)))
        f.write(struct.pack('<%dH' % len(JOINTS), *JOINTS))
        for name, frames, root, rots, fkeys in anims:
            f.write(name.encode()[:31].ljust(32, b'\0'))
            f.write(struct.pack('<HHf', frames, len(fkeys), FPS))
            for p in root:
                f.write(struct.pack('<3f', *p))
            for q in rots:
                f.write(struct.pack('<4h', *q))
            for fr, mouth, brow, eye in fkeys:
                f.write(struct.pack('<HBBB', fr, mouth, brow, eye))
            pad = (-(len(fkeys) * 5)) % 4
            f.write(b'\0' * pad)
    print('%d anims, %d joints, %d bytes -> %s' % (len(anims), len(JOINTS), os.path.getsize(out), out))


if __name__ == '__main__':
    main()
