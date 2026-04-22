#!/usr/bin/env python3
# Convert dc-texdump .bin files to .png previews.
# Requires the venv at /tmp/texdec (texture2ddecoder + Pillow).
#
# Data is still GCN-tiled where the game stored it tiled, so BC block
# positions are scrambled and the preview will look spatially wrong —
# coarse colour patches still survive enough to recognise a photo from
# noise.
#
# Usage:
#   /tmp/texdec/bin/python tools/texdump_to_png.py <pattern-or-file>...
#
# Examples:
#   /tmp/texdec/bin/python tools/texdump_to_png.py \
#       /mnt/data/distrobox/gaming/.local/share/shadPS4/texdump/s001169_*.bin
#   /tmp/texdec/bin/python tools/texdump_to_png.py \
#       /mnt/data/distrobox/gaming/.local/share/shadPS4/texdump/*a0*.bin

import os
import re
import sys
from pathlib import Path

import texture2ddecoder as t2d
from PIL import Image


OUT_DIR = Path('/mnt/data/distrobox/gaming/.local/share/shadPS4/texdump_png')
OUT_DIR.mkdir(parents=True, exist_ok=True)

PAT = re.compile(r"s(\d+)_a([0-9a-f]+)_(\d+)x(\d+)_(\w+)_tm(\d+)\.bin$")


def decode(data, w, h, fmt):
    if fmt in ('Bc7UnormBlock', 'Bc7SrgbBlock'):
        rgba = t2d.decode_bc7(data, w, h)
    elif fmt in ('Bc1RgbaSrgbBlock', 'Bc1RgbaUnormBlock'):
        rgba = t2d.decode_bc1(data, w, h)
    elif fmt in ('Bc3SrgbBlock', 'Bc3UnormBlock'):
        rgba = t2d.decode_bc3(data, w, h)
    elif fmt in ('Bc4UnormBlock', 'Bc4SnormBlock'):
        rgba = t2d.decode_bc4(data, w, h)
    elif fmt in ('Bc5UnormBlock', 'Bc5SnormBlock'):
        rgba = t2d.decode_bc5(data, w, h)
    elif fmt in ('R8G8B8A8Srgb', 'R8G8B8A8Unorm', 'R8G8B8A8Snorm'):
        need = w * h * 4
        if len(data) < need:
            return None
        rgba = data[:need]
    else:
        return None
    # texture2ddecoder returns BGRA; swap
    img = Image.frombytes('RGBA', (w, h), rgba, 'raw', 'BGRA')
    return img


def convert(path):
    m = PAT.search(path.name)
    if not m:
        return None
    _s, _a, w, h, fmt, _tm = m.groups()
    w, h = int(w), int(h)
    data = path.read_bytes()
    try:
        img = decode(data, w, h, fmt)
    except Exception as e:
        print(f"  [skip] {path.name}: {e}")
        return None
    if img is None:
        return None
    out = OUT_DIR / (path.stem + '.png')
    img.save(out)
    return out


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(2)

    paths = []
    for a in args:
        if os.path.isdir(a):
            paths.extend(Path(a).glob('*.bin'))
        elif '*' in a or '?' in a:
            from glob import glob
            paths.extend(Path(p) for p in glob(a))
        else:
            paths.append(Path(a))

    done = 0
    for p in sorted(paths):
        r = convert(p)
        if r:
            print(r)
            done += 1
    print(f"\nConverted {done} / {len(paths)} files.")
    print(f"Output dir: {OUT_DIR}")


if __name__ == '__main__':
    main()
