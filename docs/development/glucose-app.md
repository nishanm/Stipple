# The glucose app

Design notes for the `glucose` built-in. The upstream project keeps architecture
decisions in a private `private/adr/` that this fork does not carry, so this
page is where those decisions live for the glucose work.

## What it is

A glucose display for the TC002: one reading from a continuous glucose monitor,
drawn in one of six faces, with the value's colour carrying the glucose state
(green in range, yellow warning, red urgent, gray once stale). It is the
firmware half of a two-track project whose other half,
[`nightscout-pixbar`](https://github.com/nishanm/nightscout-pixbar), renders the
same faces on a host and pushes them to the stock Ulanzi firmware. That
renderer is the reference: the faces here are a port of its `faces.py`, and the
test suite holds them **byte-identical** to its golden corpus.

Stage 1 (this document) is faces only. The Nightscout data source, the
urgent-low alarm, knob face switching with persistence, and the settings fields
are later stages, each scoped on its own.

## Decisions

### A built-in, not a scene or a script

The scene model has no way to express a fifteen-row digit, and a Berry script
would put the pixel-exactness requirement behind an interpreter. A built-in is
the pattern the clock already uses: a free function drawing straight onto
`Canvas`, dispatched from `ApplicationHost::renderFrame()`.

### Its own glyph tables and text engine

`text::kMaxGlyphRows` is 8 and the only shipped font is 5x7. The hero digit on
a glucose face is 15 rows of Spleen 12x24, cropped to its ink. Rather than
widen the core font engine for one app, the glucose app carries its own tables
(`GlucoseFontData.h`, generated from the reference's `fonts.py`) and a small
text engine (`GlucoseFont.h`) that reproduces the reference's layout rules:
glyphs trimmed to their ink, one pixel of tracking, a blank glyph advancing
three, and a string sat so its first inked row lands on the y it asked for.
That literalness is what makes the goldens match; it is not a general text
engine and should not grow into one.

Spleen is BSD-2-Clause; the notice is in `THIRD_PARTY_NOTICES.md`.

### Dimming is `scale8`, not `stipple::scale`

The reference dims with FastLED's `scale8` (`(c * (factor + 1)) >> 8`). The
core's `scale()` divides by 255. They differ by one in places, and the golden
corpus and the night-legibility audit were judged with `scale8`, so the faces
dim their own accents with `apps::scale8` and the tests judge low brightness
with it. Panel brightness is still applied by the platform adapter as before;
this only concerns colours the faces choose themselves.

### Rounding is Python's

`faces.py` uses `round()`, which is round-half-to-even on a double, and the
corpus really does land on exact halves (a 78-minute-old point on a 16-column
graph is 6.5). The port uses `std::nearbyint` under the default rounding mode
on the same double arithmetic, and keeps `last - frac * span` as two statements
so clang's default floating-point contraction cannot fuse it into an FMA and
move a tie.

### Data kept off the heap

`glucose::Reading` is plain data with a fixed-capacity history
(`kMaxHistory = 64`, enough for three hours of five-minute readings with room
for a source that repeats readings). It lives in the host and is handed to the
renderer by const reference; nothing on the frame path allocates.

## The golden gate

`firmware/tests/testdata/glucose-<face>--<state>.rgb` are the reference's 54
golden frames (6 faces x 9 states) as raw RGB888, and
`firmware/tests/support/GlucoseGoldenInputs.h` holds the exact inputs behind
each, including every history point verbatim. Both are written by
`nightscout-pixbar/export_stipple.py`, which decodes each golden PNG and checks
its SHA-256 against the reference's `manifest.json` before writing anything, so
the fixtures cannot drift from the corpus.

To regenerate after a deliberate face change on the reference side:

```
cd nightscout-pixbar
python make_golden.py           # new corpus + manifest, committed there
python export_stipple.py <path-to-this-repo>
```

then run the suite here. Run it with `STIPPLE_STRICT_GOLDEN=1` so a missing
fixture fails rather than being created from unreviewed output.

`test_glucose.cpp` also ports the reference's `audit.py`: no face may light an
edge column unless it is a full-bleed graph, no face may say almost nothing,
at least half the ink must survive the night dim level, and the urgent and
stale states must still show something at the floor.
