#!/usr/bin/env python3
# Wrap raw texdump .bin files as .dds so they can be previewed in an image
# viewer. Data is still GCN-tiled where the game stored it tiled, so the
# preview will be garbled in a *structured* way; recognisable colour /
# silhouette often still survive the swizzle. Good enough to triage.
#
# Usage:
#   tools/texdump_to_dds.py <dumpdir> <outdir>
# Defaults:
#   dumpdir = ~/.local/share/shadPS4/texdump
#   outdir  = ~/.local/share/shadPS4/texdump_dds

import os
import re
import struct
import sys
from pathlib import Path

# DDS header constants
DDS_MAGIC = b'DDS '
DDS_HEADER_FLAGS_TEXTURE = 0x1007  # CAPS | HEIGHT | WIDTH | PIXELFORMAT
DDS_HEADER_FLAGS_LINEARSIZE = 0x80000
DDS_HEADER_FLAGS_MIPMAP = 0x20000
DDPF_FOURCC = 0x4
DDSCAPS_TEXTURE = 0x1000

FMT_MAP = {
    # BC formats -> DX10 header with DXGI format
    'Bc1RgbaSrgbBlock':  ('DX10', 72),   # DXGI_FORMAT_BC1_UNORM_SRGB
    'Bc1RgbaUnormBlock': ('DX10', 71),   # DXGI_FORMAT_BC1_UNORM
    'Bc3SrgbBlock':      ('DX10', 78),   # DXGI_FORMAT_BC3_UNORM_SRGB
    'Bc3UnormBlock':     ('DX10', 77),
    'Bc4UnormBlock':     ('DX10', 80),
    'Bc4SnormBlock':     ('DX10', 81),
    'Bc5UnormBlock':     ('DX10', 83),
    'Bc5SnormBlock':     ('DX10', 84),
    'Bc7UnormBlock':     ('DX10', 98),
    'Bc7SrgbBlock':      ('DX10', 99),
    'R8G8B8A8Srgb':      ('DX10', 29),   # DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
    'R8G8B8A8Unorm':     ('DX10', 28),
    'R8G8B8A8Snorm':     ('DX10', 31),
}

def block_size(fmt):
    if fmt.startswith('Bc1') or fmt.startswith('Bc4'):
        return 8
    if fmt.startswith('Bc'):
        return 16
    return 4  # uncompressed 32-bit

def is_block_compressed(fmt):
    return fmt.startswith('Bc')

def dds_header(w, h, fmt, data_len):
    dxgi = FMT_MAP[fmt][1]
    bc = is_block_compressed(fmt)
    bs = block_size(fmt)
    # Compute pitch or linear size
    if bc:
        blocks_w = max(1, (w + 3) // 4)
        blocks_h = max(1, (h + 3) // 4)
        linear_size = blocks_w * blocks_h * bs
        flags = DDS_HEADER_FLAGS_TEXTURE | DDS_HEADER_FLAGS_LINEARSIZE
    else:
        linear_size = w * bs
        flags = DDS_HEADER_FLAGS_TEXTURE

    # DDS_HEADER (124 bytes = 31 u32s):
    # 7 main + 11 reserved + 8 pixelformat + 5 caps/reserved2 = 31
    header = struct.pack(
        '<31I',
        124, flags, h, w, linear_size, 0, 0,            # 7
        *([0]*11),                                       # 11
        32, DDPF_FOURCC,
        int.from_bytes(b'DX10', 'little'),
        0, 0, 0, 0, 0,                                   # 8 pixelformat
        DDSCAPS_TEXTURE, 0, 0, 0, 0,                     # 5 caps+reserved2
    )
    # DDS_HEADER_DXT10 (20 bytes)
    header10 = struct.pack(
        '<I I I I I',
        dxgi,   # dxgiFormat
        3,      # resourceDimension = TEXTURE2D
        0,      # miscFlag
        1,      # arraySize
        0,      # miscFlags2
    )
    return DDS_MAGIC + header + header10


PAT = re.compile(r"s(\d+)_a([0-9a-f]+)_(\d+)x(\d+)_(\w+)_tm(\d+)\.bin")

def convert(src_path, out_dir):
    name = src_path.name
    m = PAT.match(name)
    if not m:
        return None
    _submit, _addr, w, h, fmt, _tm = m.groups()
    w, h = int(w), int(h)
    if fmt not in FMT_MAP:
        return None
    data = src_path.read_bytes()
    hdr = dds_header(w, h, fmt, len(data))
    out_name = name.replace('.bin', '.dds')
    out_path = out_dir / out_name
    out_path.write_bytes(hdr + data)
    return out_path


def main():
    home = Path.home()
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else home / '.local/share/shadPS4/texdump'
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else home / '.local/share/shadPS4/texdump_dds'
    if not src.is_dir():
        # distrobox share path
        alt = Path('/mnt/data/distrobox/gaming/.local/share/shadPS4/texdump')
        if alt.is_dir():
            src = alt
            dst = Path('/mnt/data/distrobox/gaming/.local/share/shadPS4/texdump_dds')
    dst.mkdir(parents=True, exist_ok=True)

    n_ok = 0
    n_skip = 0
    for p in sorted(src.glob('*.bin')):
        try:
            r = convert(p, dst)
            if r is None:
                n_skip += 1
            else:
                n_ok += 1
        except Exception as e:
            print(f"[err] {p.name}: {e}", file=sys.stderr)
            n_skip += 1
    print(f"Converted {n_ok} files, skipped {n_skip}. Output: {dst}")


if __name__ == '__main__':
    main()
