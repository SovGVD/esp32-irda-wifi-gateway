#!/usr/bin/env python3
"""Generate synthetic logo-size PNG/JPEG/GIF/WebP fixtures; requires Pillow."""
import io
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
out = ["// Synthetic codec fixtures generated with Pillow, matching common logo sizes.\n",
       "struct CodecFixture {const char *name,*bytes;size_t size;};\nstatic const CodecFixture codec_fixtures[]={\n"]
cases = [("palette PNG", "PNG", (272, 92), False), ("RGBA PNG", "PNG", (400, 160), False),
         ("JPEG", "JPEG", (272, 92), False), ("GIF", "GIF", (272, 92), False),
         ("lossy WebP", "WEBP", (128, 64), False), ("lossless WebP", "WEBP", (64, 32), True)]
for name, fmt, size, lossless in cases:
    image = Image.new("RGBA", size, (255, 255, 255, 0))
    draw = ImageDraw.Draw(image)
    draw.rectangle((2, 2, size[0] // 2, size[1] - 2), fill=(230, 40, 10, 255))
    draw.rectangle((size[0] // 2, 2, size[0] - 2, size[1] - 2), fill=(40, 60, 200, 128))
    if name == "palette PNG":
        image = image.quantize()
    if fmt == "JPEG":
        image = image.convert("RGB")
    file = io.BytesIO()
    image.save(file, format=fmt, lossless=lossless)
    data = file.getvalue()
    encoded = "".join(f"\\{byte:03o}" for byte in data)
    out.append('{"' + name + '","' + encoded + '",' + str(len(data)) + '},\n')
out.append("};\n")
(root / "test/host/data/xiino_codec_fixtures.h").write_text("".join(out))
