#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn the shop scripts' rendered frames into animated GIFs.

    ctest / dev.ps1 test ShopScripts.WriteFramesOnRequest   (writes .rgb)
    python3 tooling/site/build-previews.py <frames-dir>

The frames come from the real engine at the real panel size, written by
firmware/tests/test_shop_scripts.cpp. Nothing here draws anything: a preview
made by a second renderer would be a picture of something that does not
exist, and the first time the two disagreed the page would be lying without
anybody noticing.

**The GIF encoder is in this file**, for the same reason the PNG encoder is
in the tree (ADR 0012): this runs in CI, which must not install anything.
GIF's LZW is the only awkward part and it is implemented here, in about
sixty lines - see `lzw`.
"""

import io
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "site" / "shop"

WIDTH = 52
HEIGHT = 16
FRAME_BYTES = WIDTH * HEIGHT * 3

# 33 ms a frame is 30 fps, which is what the device renders and what the
# frames were sampled at. GIF delays are in hundredths of a second, so the
# closest honest value is 3.
DELAY_CS = 3

# Scaled up, because 52x16 on a modern display is a postage stamp. Nearest
# neighbour, done here rather than left to the browser: `image-rendering`
# is advisory and a smoothed preview of a pixel panel misrepresents the one
# thing the product is.
SCALE = 6

# An unlit LED is not absolutely black - the same value the site's own panel
# uses, so a preview and the hero canvas show the same thing.
UNLIT = (12, 18, 22)

# The dark line between LEDs, a shade below an unlit one so the two read as
# different things.
GAP = bytes((4, 6, 8))
GAP_PX = 1


def lit(pixel):
    """An LED the firmware left black is off, not absent."""
    return bytes(UNLIT) if pixel == b"\x00\x00\x00" else pixel


def read_frames(path):
    data = path.read_bytes()
    if len(data) % FRAME_BYTES != 0:
        raise SystemExit(
            "build-previews: %s is %d bytes, not a whole number of %dx%d frames"
            % (path.name, len(data), WIDTH, HEIGHT))
    count = len(data) // FRAME_BYTES
    if count == 0:
        raise SystemExit("build-previews: %s holds no frames" % path.name)
    return [data[i * FRAME_BYTES:(i + 1) * FRAME_BYTES] for i in range(count)]


def lit_count(frame):
    # bytes(3) is three zero bytes: an unlit pixel. No escape, so nothing
    # between here and the file can mangle it.
    return sum(1 for i in range(0, len(frame), 3) if frame[i:i + 3] != bytes(3))


def open_on_content(frames):
    """Rotate past an empty opening, and no further.

    A card shows its GIF's first frame until the animation decodes, and for
    anything that scrolls or fades in that frame is blank - Selenograph's
    caption travels in from the right, so its preview opened on a black
    rectangle advertising a script that draws a moon.

    Rotated rather than truncated: every frame is still there and the loop
    still runs the whole cycle, it just starts elsewhere.

    The rule is deliberately dumb. The first version picked the *busiest*
    frame, which sounds better and is worse: Flappy's densest frame is its
    game-over screen, so the preview for a game opened on the words "score 0".
    Density is not interest. Skipping frames that are all but empty is the
    most this can know.
    """
    # Relative to the busiest frame, with an absolute floor.
    #
    # A flat 5 pixels was not enough: Selenograph's caption scrolls in a
    # character at a time, so its fourth frame has seven lit pixels and the
    # card opened on what looked like a blank panel.
    #
    # Kept low on purpose. An earlier version aimed at 40% of the busiest
    # frame and opened Flappy on its game-over screen, because text is denser
    # than gameplay - density is not interest. 15% clears "nothing is there"
    # without reaching for "the most is there".
    busiest = max(lit_count(frame) for frame in frames)
    floor = max(5, busiest * 15 // 100)

    first = 0
    while first < len(frames) - 1 and lit_count(frames[first]) < floor:
        first += 1

    if first == 0 or lit_count(frames[first]) < floor:
        return frames, 0  # nothing anywhere; the shop test is what complains
    return frames[first:] + frames[:first], first


def quantise(frames):
    """A palette of at most 256 colours, and the frames indexed against it.

    Scripts draw with a handful of colours, so the usual case is an exact
    palette and no loss at all. The fallback only matters for something doing
    per-pixel gradients.
    """
    seen = {}
    for frame in frames:
        for i in range(0, len(frame), 3):
            seen[lit(frame[i:i + 3])] = True

    # One slot is kept back for the gap between LEDs.
    exact = len(seen) <= 255
    if exact:
        palette = sorted(seen.keys())
        gap = len(palette)
        palette.append(GAP)
        index = {c: i for i, c in enumerate(palette)}
        lookup = lambda c: index[c]
    else:
        # 3-3-2. Crude, and it only runs for a script that has earned it; a
        # smarter quantiser is a dependency or a page of median-cut for
        # something no published script currently needs.
        palette = [bytes((
            ((i >> 5) & 0x7) * 255 // 7,
            ((i >> 2) & 0x7) * 255 // 7,
            (i & 0x3) * 255 // 3,
        )) for i in range(256)]
        # Index 0 is black, which is as close to the gap colour as the
        # fallback palette gets.
        gap = 0
        lookup = lambda c: (
            ((c[0] * 7 // 255) << 5) | ((c[1] * 7 // 255) << 2) | (c[2] * 3 // 255))

    indexed = []
    for frame in frames:
        rows = []
        for y in range(HEIGHT):
            row = bytearray()
            for x in range(WIDTH):
                off = (y * WIDTH + x) * 3
                row.append(lookup(lit(frame[off:off + 3])))
            rows.append(bytes(row))
        indexed.append(rows)

    while len(palette) < 2:
        palette.append(b"\x00\x00\x00")
    return palette, indexed, exact, gap


def scale_rows(rows, gap):
    """Each LED becomes a square with a dark line to its right and below.

    Without the line neighbouring pixels fuse into one block and the panel
    reads as a low-resolution picture rather than a grid of separate LEDs.
    """
    solid = SCALE - GAP_PX
    out = []
    for row in rows:
        wide = bytes(b for value in row
                     for b in ([value] * solid + [gap] * GAP_PX))
        for _ in range(solid):
            out.append(wide)
        for _ in range(GAP_PX):
            out.append(bytes([gap]) * len(wide))
    return out


def lzw(pixels, min_code_size):
    """GIF's LZW, properly.

    The first version of this emitted every pixel as a literal - a valid
    stream that compresses nothing - on the reasoning that the result would be
    about a tenth larger and not worth a compressor for files this size.

    That reasoning was wrong by a factor of fifty. These frames are a panel
    scaled six times, so every pixel is a run of six and every row repeats six
    times; it is close to the most compressible input there is. The literal
    version produced 1.6 MB per preview. This produces about 30 KB.

    Straight out of the GIF specification: build a dictionary of sequences,
    widen the code as it fills, and clear when it reaches 4096.
    """
    clear = 1 << min_code_size
    end_code = clear + 1

    out = bytearray()
    bits = 0
    nbits = 0
    width = min_code_size + 1

    def emit(code):
        nonlocal bits, nbits
        bits |= code << nbits
        nbits += width
        while nbits >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            nbits -= 8

    def fresh():
        return {bytes([i]): i for i in range(clear)}

    table = fresh()
    next_code = end_code + 1
    emit(clear)

    if not pixels:
        emit(end_code)
        if nbits:
            out.append(bits & 0xFF)
        return sub_blocks(out)

    run = bytes([pixels[0]])
    for value in pixels[1:]:
        candidate = run + bytes([value])
        if candidate in table:
            run = candidate
            continue

        emit(table[run])

        # The decoder adds the same entry at the same moment, so the two
        # tables stay in step without anything being transmitted.
        if next_code < 4096:
            table[candidate] = next_code
            next_code += 1
            if next_code > (1 << width) and width < 12:
                width += 1
        else:
            # Full. Both sides start again; the clear must go out at the old
            # width, before it is reset.
            emit(clear)
            table = fresh()
            next_code = end_code + 1
            width = min_code_size + 1

        run = bytes([value])

    emit(table[run])
    emit(end_code)

    if nbits:
        out.append(bits & 0xFF)
    return sub_blocks(out)


def sub_blocks(data):
    """GIF carries image data in runs of at most 255 bytes, length-prefixed."""
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)


def build_gif(palette, frames, gap):
    width = WIDTH * SCALE
    height = HEIGHT * SCALE

    # The table is a power of two, padded. `bits` is what the header declares.
    bits = max(1, (len(palette) - 1).bit_length())
    size = 1 << bits
    table = bytearray()
    for i in range(size):
        table += palette[i] if i < len(palette) else b"\x00\x00\x00"

    out = bytearray(b"GIF89a")
    out += struct.pack("<HH", width, height)
    out.append(0xF0 | (bits - 1))   # global table present, `bits` deep
    out.append(0)                   # background index
    out.append(0)                   # pixel aspect ratio: unspecified
    out += table

    # Loop for ever. Without this a preview plays once and then sits on its
    # last frame, which for a game is its game-over screen.
    out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"

    min_code_size = max(2, bits)
    for rows in frames:
        wide = scale_rows(rows, gap)
        pixels = b"".join(wide)

        out += b"\x21\xF9\x04"
        out.append(0x04)                       # dispose: restore to background
        out += struct.pack("<H", DELAY_CS)
        out.append(0)                          # no transparent index
        out.append(0)

        out += b"\x2C"
        out += struct.pack("<HHHH", 0, 0, width, height)
        out.append(0)                          # no local table, not interlaced
        out.append(min_code_size)
        out += lzw(pixels, min_code_size)

    out += b"\x3B"
    return bytes(out)


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: build-previews.py <frames-dir> [<output-dir>]")
    frames_dir = Path(sys.argv[1])
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else OUT
    if not frames_dir.is_dir():
        raise SystemExit("build-previews: no such directory: %s" % frames_dir)

    sources = sorted(frames_dir.glob("*.rgb"))
    if not sources:
        raise SystemExit(
            "build-previews: no .rgb files in %s - run the ShopScripts frame "
            "dump first" % frames_dir)

    out_dir.mkdir(parents=True, exist_ok=True)
    for path in sources:
        frames = read_frames(path)
        frames, rotated = open_on_content(frames)
        palette, indexed, exact, gap = quantise(frames)
        gif = build_gif(palette, indexed, gap)

        target = out_dir / (path.stem + ".gif")
        target.write_bytes(gif)
        print("build-previews: %-16s %3d frames, %3d colours%s, %6d bytes%s"
              % (target.name, len(frames), len(palette),
                 "" if exact else " (quantised)", len(gif),
                 "" if not rotated else "  (opens %d frames in)" % rotated))
    return 0


if __name__ == "__main__":
    sys.exit(main())
