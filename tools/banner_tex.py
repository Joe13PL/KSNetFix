#!/usr/bin/env python3
# Converts the SteamNet lobby banner (640x128 PNG) to the game's texture format:
#   python3 tools/banner_tex.py src/res/steamnet_banner.png src/res/steamnet_banner.tex
# The game shows Banners\BannerDef.tex (Interface.wd) above the lobby chat; KSNetFix serves this
# file in its place. Format: "TEX\0", version 2, 0x80000000 (several textures), count 5, then
# five 128x128 tiles left to right, each "TEX\0", 2, flags, 0x8888, 0, width, height and 4 bytes
# per pixel in R, G, B, A order; flags 0x03000002 opaque, 0x03000022 with transparency (as the
# game's dialog frames). Needs Pillow.
import struct
import sys

from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
im = Image.open(src).convert('RGBA')
if im.size != (640, 128):
    sys.exit('the banner must be 640x128, not %dx%d' % im.size)
flags = 0x03000022 if im.getchannel('A').getextrema()[0] < 255 else 0x03000002
out = bytearray(b'TEX\0' + struct.pack('<III', 2, 0x80000000, 5))
for i in range(5):
    tile = im.crop((i * 128, 0, i * 128 + 128, 128))
    out += b'TEX\0' + struct.pack('<IIHHII', 2, flags, 0x8888, 0, 128, 128) + tile.tobytes()
open(dst, 'wb').write(out)
print('%s: %d bytes, %s' % (dst, len(out), 'transparent' if flags & 0x20 else 'opaque'))
