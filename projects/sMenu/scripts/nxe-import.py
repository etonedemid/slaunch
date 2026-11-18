#!/usr/bin/env python3
"""Makes an NXE skin from your own Xbox 360 dashboard: dash.xex -> skin folder.

The 2008-2011 dashboard keeps its pictures in XUI packages inside dash.xex,
which is encrypted with the retail key and LZX-compressed. This unpacks it
(scripts/lzxd.c, built with cc on first use), finds the packages and copies
the pictures the NXE layout uses into a skin folder:

  background.jpg  the backdrop        floor.png        the stage's lit edge
  tile.jpg        a panel's face      icon_games.png   the Game Library's icon
  icon_music.png, icon_photos.png, icon_settings.png
  panel_games/videos/music/photos.jpg  the libraries' faces   icon_videos.png
  shadow.png      a panel's shadow    avatar_shadow.png  under the account picture
  button_a/b/x/y.png                  the legend's buttons

Nothing from the dashboard ships with sLaunch; use a dashboard you own.

Usage: nxe-import.py dash.xex OUTDIR      (OUTDIR e.g. <SD>/slaunch/skins/nxe/dashboard)
Needs pycryptodome (pip install pycryptodome) and a C compiler.
"""
import os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
RETAIL = bytes.fromhex('20B185A59D28FDC340583FBB0896BF91')
WANT = {   # skin file <- the dashboard's picture
    'background.jpg': 'defaultbackground.jpg', 'floor.png': 'aura_horizon.png',
    'tile.jpg': 'blankgreen.jpg', 'icon_games.png': 'icon_gamelib.png',
    'icon_music.png': 'icon_musiclib.png', 'icon_photos.png': 'icon_picturelib.png',
    'icon_settings.png': 'icon_settings.png',
    'panel_videos.jpg': 'videos.jpg', 'icon_videos.png': 'icon_videolib.png', 'panel_games.jpg': 'games.jpg',
    'panel_music.jpg': 'music.jpg', 'panel_photos.jpg': 'photos.jpg',
    'shadow.png': 'panelshadow.png', 'avatar_shadow.png': 'avatarshadow.png',
    'button_a.png': 'legend_a.png', 'button_b.png': 'legend_b.png',
    'button_x.png': 'legend_x.png', 'button_y.png': 'legend_y.png',
}


def unpack(d):
    from Crypto.Cipher import AES
    if d[:4] != b'XEX2':
        sys.exit('nxe-import: not an XEX2 file')
    _, pe_off, _, sec_off, nopt = struct.unpack('>IIIII', d[4:24])
    opts = dict(struct.unpack('>II', d[24 + 8 * i:32 + 8 * i]) for i in range(nopt))
    image_size = struct.unpack('>I', d[sec_off + 4:sec_off + 8])[0]
    o = opts[0x3ff]
    _, enc, comp = struct.unpack('>IHH', d[o:o + 8])
    data = d[pe_off:]
    if enc == 1:
        key = AES.new(RETAIL, AES.MODE_ECB).decrypt(d[sec_off + 0x150:sec_off + 0x160])
        data = AES.new(key, AES.MODE_CBC, b'\0' * 16).decrypt(data[:len(data) // 16 * 16])
    if comp == 1:   # basic: runs of data, then zeros
        out, p = bytearray(), 0
        for i in range(8, struct.unpack('>I', d[o:o + 4])[0], 8):
            n, z = struct.unpack('>II', d[o + i:o + i + 8])
            out += data[p:p + n] + b'\0' * z
            p += n
        return bytes(out)
    if comp != 2:
        sys.exit('nxe-import: unknown compression %d' % comp)
    window, size = struct.unpack('>II', d[o + 8:o + 16])
    stream, p = bytearray(), 0
    while size:
        blk = data[p:p + size]
        p += size
        size, q = struct.unpack('>I', blk[:4])[0], 24
        while q + 2 <= len(blk):
            n = struct.unpack('>H', blk[q:q + 2])[0]
            q += 2
            if not n:
                break
            stream += blk[q:q + n]
            q += n
    tool = os.path.join(tempfile.gettempdir(), 'slaunch-lzxd')
    if not os.path.exists(tool):
        subprocess.run(['cc', '-O2', '-o', tool, os.path.join(HERE, 'lzxd.c')], check=True)
    r = subprocess.run([tool, str(window.bit_length() - 1), str(image_size)], input=bytes(stream),
                       capture_output=True)
    if r.returncode or r.stdout[:2] != b'MZ':
        sys.exit('nxe-import: decompression failed ' + r.stderr.decode())
    return r.stdout


def files(image):
    """name (lower case, no folder) -> bytes, across every package in the image."""
    out = {}
    for m in re.finditer(b'XUIZ', image):
        b = m.start()
        try:
            ver = struct.unpack('>I', image[b + 4:b + 8])[0]
            cnt = struct.unpack('>H', image[b + 0x14:b + 0x16])[0]
            p, ents = b + 0x16, []
            for _ in range(cnt):
                size, off = struct.unpack('>II', image[p:p + 8])
                n = image[p + 8]
                p += 9
                nb = 2 * n if ver == 1 else n
                name = image[p:p + nb].decode('utf-16-be' if ver == 1 else 'latin1')
                p += nb
                ents.append((name, off, size))
        except (struct.error, UnicodeDecodeError):
            continue
        for name, off, size in ents:
            out.setdefault(name.replace('\\', '/').split('/')[-1].lower(), image[p + off:p + off + size])
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    image = unpack(open(sys.argv[1], 'rb').read())
    have = files(image)
    os.makedirs(sys.argv[2], exist_ok=True)
    made = 0
    for dst, src in WANT.items():
        if src in have:
            open(os.path.join(sys.argv[2], dst), 'wb').write(have[src])
            made += 1
    ini = os.path.join(sys.argv[2], 'skin.ini')
    if not os.path.exists(ini):
        open(ini, 'w').write('; made by nxe-import.py from your dash.xex - edit freely, see skins/README.txt\n')
    print('nxe-import: %d of %d pictures -> %s' % (made, len(WANT), sys.argv[2]))


if __name__ == '__main__':
    main()
