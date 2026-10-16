# Xiino dependencies

`openxiino/` contains the unmodified server, converter sources, palette and built-in
HTML from [nicl83/OpenXiino](https://github.com/nicl83/OpenXiino), commit
`e9ef5062095f66f7e731c5a0f831e394a4c02380` (MIT; license included).
These Python files are reference material, not a runtime requirement. The firmware
port is `src/xiino.cpp`, `src/xiinoimage.cpp` and `src/irxiino.cpp`.

The active upstream server path is ported: GET requests, the binary response
padding, supported-tag parser, Latin-1 output, absolute/downgraded anchor links,
colour mode 9 images and built-in pages. Grayscale requests are reported on the
device-info page but, like upstream, images are still colour. The original tag
parser's supported-state behaviour and mode 9 run/lookback advancement are
preserved; this change does not repair upstream's known issues.

`stb/stb_image.h` is stb_image v2.30 from
[nothings/stb](https://github.com/nothings/stb), commit
`2c980bb59875b0d32144a71867fbdebb2f77cd20`. Its MIT/public-domain licenses are
included at the end of the header. It handles JPEG/BMP and other fallback
formats using native channels rather than always expanding to RGBA.

`pngle/` is [kikuchan/pngle](https://github.com/kikuchan/pngle), commit
`b1c68193f1d3f8642b3e0e095d457a828038e6fb` (MIT; included). PNG scanlines are
sampled directly into the target thumbnail, with alpha composited onto white.
Local changes route allocation through the bounded allocator, use ESP32-C3 ROM
miniz in firmware, and cast the high byte before a 32-bit shift. Portable host
tests use the bundled public-domain miniz with unaligned integer accesses
disabled; its feature-macro guard was adjusted to permit this override.

`libwebp/` contains the portable decoder subset and headers from
[webmproject/libwebp](https://github.com/webmproject/libwebp), commit
`4f1384dff81fb9a4764d6194d59a16ca5c830020`. COPYING, PATENTS and AUTHORS are
included. Lossy/lossless WebP with alpha uses the advanced scaling API to decode
to thumbnail size. SIMD and worker threads are disabled; scaling remains enabled.
The sole upstream C change routes WebPSafeMalloc/Calloc/Free through the shared
bounded allocator. A small local `src/webp/config.h` selects portable code.

GIF uses a local first-frame LZW decoder in `src/xiinoimage.cpp`, based on the
[GIF89a specification](https://www.w3.org/Graphics/GIF/spec-gif89a.txt). It keeps a
fixed 16 KiB dictionary and samples pixels directly into the thumbnail instead
of keeping frame history. Global/local palettes, transparency and interlace are
handled. GIF is a service mark of CompuServe Incorporated.

Static PNG/JPEG/GIF/WebP/BMP images are supported; animated GIFs use the first
frame, while animated WebP, TIFF and SVG are unsupported. Small inline data URIs
are decoded without an HTTP request, within tag/source/image limits. PNG/GIF
thumbnail sampling, WebP decoder scaling and fallback bicubic reduction can
differ from Pillow. Palette conversion uses white compositing/error diffusion;
the upstream mode 9 compression quirks remain.

ESP32-specific bounds: page input is scanned in 512-byte blocks up to a 2 MiB
raw-input budget. A bounded streaming filter discards script/style contents,
comments and unused modern attributes, collapses ordinary whitespace and keeps
preformatted whitespace. It retains no more than 48,000 compacted-source bytes;
individual tag buffering is capped at 4 KiB (larger tags retain only their name).
The converter sends HTML in blocks of at most 512 bytes, flushing before each
image request, and appends complete EBD payloads after the HTML. It uses the
same HTTP/1.0 close-delimited framing as OpenXiino, without waiting to assemble
the complete converted HTML page. Output remains capped at 48,000 bytes.
Pages exceeding retained-source, scan or output budgets keep their readable
prefix, complete embedded image payloads and a visible shortening notice.

Image limits remain 64,000 compressed
image bytes, 192 KiB shared decoder allocations, maximum source dimension 2048, and a 24 KiB
free-heap reserve during decoder allocation. Actual available heap can reject
smaller images. Unsupported/oversized decoded images use placeholders;
optional image-download failures or image/output budget overflows also leave
placeholders instead of aborting the page. A failed page download still returns
a framed error page. Origin
connections are closed before parsing/decoding or fetching the next image.
HTTPS uses the existing ESP-IDF CA bundle. Origins are asked for uncompressed
responses; a server ignoring that request is rejected.

Page downloads have a 20-second deadline across redirects/reads. Each image
download has a 3-second deadline; attempts share an 8-second elapsed budget and
an eight-image count cap. Deadlines are updated before each blocking read.
Conversion/delivery time between attempts counts towards the elapsed image gate.
Cancelled sinks abort conversion and further image fetches; socket writes have
a stall timeout. These bounds do not establish progressive drawing in Xiino or
an overall wall-clock bound on a healthy but slow IR transfer.

`src/xiino_assets.inc`, `src/xiino_palette.inc` and `src/xiino_entities.inc`
are generated by `python3 tools/generate_xiino_assets.py`. The character reference
table comes from Python's `html.entities.html5`, matching upstream's HTMLParser.

`test/host/data/xiino_fixtures.h` is generated by
`python3 test/host/generate_xiino_fixtures.py` (requires Pillow and Requests).
The reference cases cover HTML behaviour, mode 9 compression, and PNG/JPEG/GIF/BMP
image output. Normal firmware builds and host tests need no Python image libraries.
Synthetic logo-size codec fixtures are regenerated by
`python3 tools/generate_xiino_codec_fixtures.py` (Pillow). Tests cover each codec,
truncated data, allocation release, stream byte parity/order, cancelled sinks,
inline images and image deadlines. Captured Google/Wikipedia inputs stay in
`/tmp` and are optional arguments to the host test script.
