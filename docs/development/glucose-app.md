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

Stage 1 was faces only; Stage 2 added the Nightscout source and its settings;
Stage 3 made the display a mode the knob works inside. Stage 4 added the
speaker alarm, configurable like nightscout-clock's. The brightness schedule
and the rest of the settings page are later stages, each scoped on its own.

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

## Stage 2: the source

`apps::glucose::NightscoutSource` (`GlucoseSource.h`) polls
`<url>/api/v1/entries.json?count=38&find[type]=sgv` - three hours of
five-minute readings plus two for the ones Dexcom Share repeats, the same
request the reference makes - once a period, and rebuilds the reading from the
samples after every fetch and on every minute. The parsing is a pure function,
`parseEntries`, so it is tested without a client; the reference's own test
fixture is used verbatim, so both implementations are held to one answer.

### Decisions

**The device stores the secret's SHA-1, never the secret.** Nightscout checks
the `api-secret` header against the SHA-1 of the site secret, so the digest is
the credential and the plaintext has no reason to exist on the device. The API
hashes what it is given on receipt (`glucose.apiSecret`, write-only) and keeps
`apiSecretSha1`; GET reports `apiSecretSet` and nothing else, diagnostics
report neither the credential nor the URL. `core/Sha1.cpp` exists for this one
job and is not a security primitive for anything new. Hashing happens on the
device because the panel serves plain HTTP, where a browser has no
`crypto.subtle`.

**No compiled defaults.** This repository is public; a LAN address or a secret
in the source would be published with it. Both arrive through the API.

**One HTTP client, shared.** The platform offers a single `IHttpClient`, and
`ScriptFetcher` used to assume it owned it - `start()` would `begin()` on a
busy client, fail, and `reset()` whoever's request was in flight, and
`forget()` reset the client whenever a script was deleted. Both now check.
The rule for anything that fetches: begin only when the client is `Idle`,
poll your own request, `reset()` after you collect it, and treat `Idle` while
you believed you were running as "somebody reset it" rather than a failure.

**Requests may carry a header and ask for a larger body.** `HttpRequest` adds
one header as a name/value pair (checked as a token and for control
characters, not scanned for line breaks) and a per-request body cap. The
kilobyte default stands for scripts; the source asks for 32 KiB because real
entries carry a dozen or more fields. The simulator truncates at the cap too,
so the emulator cannot pass on a body the panel would never see. Diagnostics
report `lastBodyBytes` so the cap can be revisited with a measurement.

**Failures keep the samples.** A refused credential (401/403) holds the source
off for five minutes, doubling to thirty, so a bad secret cannot ask somebody's
server once a minute for ever. Everything else - no route, a timeout, a 500, a
body that is not JSON, an empty list - is tried again next period. Either way
the last samples stay and the reading is rebuilt with the new time, so it ages
into stale rather than freezing on a number that is no longer true.

**Nothing before the wall clock is set.** The device boots at 1970. A reading's
age computed from that would be a lie in the safe-looking direction, so the
source waits for SNTP and shows the no-data face until then.

**Stale shows the no-data face.** Whatever face is chosen, a reading older
than twenty minutes is drawn as the explicit no-data face (the reference's
rule): a grey `---` on the hero face reads as a value that is merely dim.

## Stage 3: the knob owns the faces

A glucose display that the carousel rotates away from is a demo. While a source
is configured and `glucose.pinned` is on (the default), the host pins the app
with the carousel's own `pin()` whenever the carousel is showing it, and a knob
detent steps the face instead of the app.

### Decisions

**This is the controls rule applied, not bent.** DESIGN.md: "Turn the knob to
move between things ... The mode changes what a control applies *to*, never
what it means." In the glucose mode the things are faces. Press, hold, − / +
and the middle button keep exactly their meanings.

**No mode is a trap.** The pin does not chase the user. `Back` (the middle
button) still activates the clock, which clears the pin; the carousel then
rotates through everything and re-pins glucose only when it is showing it
again. If the clock cannot be activated - disabled, or not installed - `Back`
unpins and moves to the next app instead, so there is always a way out. The
API's `activate` behaves the same way.

**One detent, one face.** The mapper's acceleration is ignored, as it is for
apps, and `NoData` is never in the cycle: it is what a stale reading is drawn
as, not a choice.

**Feedback while stale.** A stale reading always draws the no-data face, so a
detent would change the setting invisibly. When that is the case the change is
named on the adjustment readout (`FACE` / `DELTA`), the same two-line overlay
− / + use for a level. Fixing that exposed a general bug: nothing repainted the
panel when a readout expired, which the clock's per-second redraw had hidden.

**Persisted, but not per detent.** The face is saved once the knob has been
still for two seconds - one flash write per decision. The API's PATCH saves
itself, so a knob change is also carried by any later PATCH.

## Stage 4: the alarm

The panel sounds an alarm through its own speaker. The owner asked for it to
be "configurable to the level of" the nightscout-clock firmware on their three
TC001 clocks, so the settings are that firmware's, nearly field for field, under
`glucose.alarms`:

| | |
|---|---|
| `urgentLow`, `low`, `high` | `enabled`, `mgdl` (30-399), `snoozeMinutes` (5, 10, 15, 30, 60, 120, or 0 = until it leaves the range), `windows`, `melody` |
| `noData` | the same, with `minutes` (20, 30, 45, 60) instead of `mgdl` |
| `repeatSeconds` | 60, 120, 300 |
| `intensive` | repeat two seconds after the melody ends |
| `volumePercent` | 20-100, the alarm's own level |

Defaults are the clocks' (55 / 70 / 280; snooze 15 / 30 / 60; their three
melodies, verbatim), except that urgent low is **on**: spec A1 requires it, and
it can only sound on a fresh reading, which needs a configured source.

### Decisions

**The clocks' model, kept to the letter where the owner will notice.** Ranges
are theirs: urgent low is `[1, U]`, low is `[U+1, L-1]` whether or not urgent
low is enabled, high is `H` and up. Alert windows are theirs: `{days, from,
to}` with `days` as `tm_wday` digits, sounding only inside a window, a window
whose end is before its start running past midnight, the days being the days it
starts on, and an empty list meaning any time. Melodies are RTTTL in the
dialect their settings page accepts, and the page offers their presets, so a
melody copied from a clock sounds the same here. A stale reading silences the
glucose alarms, as on the clocks.

**Three deliberate differences.** A lesser snooze never silences a worse
alarm: on the clocks a snoozed low that drops into urgent low inherits the low's
snooze, here urgent low sounds at once (the reverse - urgent low snoozed,
recovering into low - stays quiet until the snooze ends). Snooze counts from
the press, not from the last time it sounded. High has no 401 cap, because the
source accepts readings up to 1000.

**No data is an alarm here, off by default.** The clocks have none, and a stale
reading silences everything - so a Nightscout that stops answering during a low
ends the alarm. `noData` is the net under that: it counts from the newest
sample, or from when fetching became possible if none ever came. The owner
chose to add it and to leave it off.

**The alarm's volume is absolute.** `IAudioOutput::playMelody(melody,
level)` ignores `setVolume`; − and + never make an alarm quieter, and there is
no 0 - to silence an alarm, disable it. Volume is never swapped temporarily
through `setVolume`, which would race with PATCH and − / +.

**Melodies are timed by samples, not by the loop.** `audio::MelodyGenerator`
is pulled by the adapter like `ToneGenerator` and changes note on exact sample
counts. Stepping notes from the main loop would start each one up to a loop
iteration late - half a note on the 34 ms notes of the "urgent" preset. A
melody is at most 64 notes and ten seconds, and one made only of rests is
refused: it would be a silent alarm.

**Nothing cuts it off.** A tone replaces whatever the speaker is playing, and
the clock ticks once a second whenever Back lands on it. While a melody plays,
the TC002 adapter refuses tones and named sounds and `stop()` leaves the melody
alone; notifications, the clock tick, the volume beep and scripts all reach the
speaker through `audio::SharedSpeaker`, which refuses them for the melody's
length. A test melody from the API takes the same hold, and the API refuses a
test while a real alarm sounds.

**It takes the screen.** On every sound - new, escalated or repeated - the host
leaves settings, dismisses the splash, activates glucose and pins it;
`glucoseHoldWanted()` includes "an alarm is sounding" so the pin survives
`pinned: false`. Back still leaves (no trap) and the next repeat comes back.
While it sounds the reading is drawn over a notification and on a panel that
is switched off - drawn, never switched on - with a brightness floor of 16.

**The knob press snoozes, from anywhere.** Taken from the raw press-down, after
the rescue and setup gestures and before the splash, settings and the mapper,
so it does not depend on what is on screen or how the buttons are bound. The
release is swallowed, so the press does nothing else and a held knob never
opens settings. `SNOOZE / 15 MIN` is shown for 2.5 s. With no alarm sounding,
the press does what it always did. Snooze is never persisted - a reboot
re-alarms, the safe direction - and never writes flash, so a fsync stall cannot
gap the next melody.

**Settings never fall back to silence.** PATCH validates `glucose.alarms`
strictly and whole (wrong type, range or order: `422`, nothing changed). A
stored alarm that fails today's rules loads as its default; a melody that will
not compile plays the built-in default and is counted in diagnostics
(`alarm.melodyFallbacks`). The config parser's token budget doubled to 2048 for
four alarms of eight windows each, and its tokens moved to the heap.

**Diagnostics say what happened.** `alarm.state` (quiet / sounding / snoozed),
`kind`, `snoozedSeconds`, `lastPlayAgeSeconds`, `plays`, `playFailures`,
`melodyFallbacks`, `speaker`. No speaker is logged as a warning at boot and
shown as a banner on the settings page.

### Known limits

No hysteresis: a reading that flaps across a threshold re-arms each time (the
clocks do the same). Any knob press snoozes. A main-loop stall longer than the
driver's ~48 ms buffer gaps a melody. Safe mode has no source and so no alarm.
Loudness at 100 % is `ToneGenerator`'s 9000/32767 peak, the same as a 100 %
button beep - whether that wakes someone across a bedroom is for the owner's
ears on the device.

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
