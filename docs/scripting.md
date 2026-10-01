# Scripting

Stipple runs [Berry](https://github.com/berry-lang/berry) — a small, dynamically
typed language designed for microcontrollers. A script is an app: it appears in
the carousel, it draws on the panel, and it can take the button.

Scripts are written in the device's own web page, under **Scripts**, or pushed
over `/api/v1/scripts`. There are worked examples in
[`scripts/`](../scripts/), listed at
[the script shop](https://galadril.github.io/Stipple/shop/).

## The shape of a script

A script is a class with a `draw()` method, and the file ends by returning an
instance of it:

```berry
class App
  def draw()
    clear(rgb(0, 0, 0))
    text(2, 1, "hello", rgb(0, 190, 255))
  end
end

return App()
```

`draw()` is called once per frame while the app is on screen. `init()`, if you
write one, runs once when the script is saved.

Forgetting the `return` is the commonest first mistake, and the device says so
in those words rather than reporting a generic failure.

## What a script can do

### The panel

The panel is 52 × 16. Coordinates outside it are clipped, not an error.

| Call | Does |
|---|---|
| `width()`, `height()` | `52` and `16`. Use these rather than the numbers, so a script survives a different panel. |
| `clear(colour)` | Fill everything. |
| `pixel(x, y, colour)` | One pixel. |
| `line(x1, y1, x2, y2, colour)` | |
| `rect(x, y, w, h, colour)` | Outline. |
| `rect_fill(x, y, w, h, colour)` | Solid. |
| `text(x, y, string, colour)` | Draws, and returns the width it used. |
| `text_width(string)` | The width without drawing — for right-aligning. |
| `rgb(r, g, b)` | A colour. Each channel 0–255. |

The font is 5 × 7 and advances six pixels per character, so a line holds eight.
**Text that runs past pixel 51 is clipped silently.** Nothing warns you, the
panel just shows less than you wrote, so measure with `text_width()` rather
than centring by eye — this caught three of the six example scripts.

### Time

```berry
if time_known()
  text(1, 0, string.format("%02d:%02d", hour(), minute()), rgb(255, 255, 255))
else
  text(8, 5, "no time", rgb(180, 60, 60))
end
```

| Call | Does |
|---|---|
| `time_known()` | Whether the device has a wall clock it believes in. |
| `hour()`, `minute()`, `second()` | Local time. |
| `day()`, `month()`, `year()` | Local date. |
| `weekday()` | 0 is Sunday. |
| `now_ms()` | Milliseconds since the device started. Never goes backwards. |
| `elapsed_ms()` | Milliseconds since **this app** came on screen. |

### Which clock

Two, and using the wrong one is the commonest way to write a script that
looks fine and then stops.

`now_ms()` is the device's clock. Use it to **throttle** — to do something
every so often:

```berry
var t = now_ms()
if t - self.last >= 250
  self.last = t
  # ... move something
end
```

That pattern needs a clock that keeps counting while your app is off screen.
The carousel moves on and comes back; if the clock restarted, `self.last`
would hold a number from the future and the comparison would stay false for
as long as the previous showing lasted. The app would simply stop moving.
This is not hypothetical — it is what the aquarium did before `now_ms()` was
fixed.

`elapsed_ms()` is time since **your app appeared**. Use it for an animation
that should begin at the beginning each time somebody sees it, rather than
joining part-way through.

**Check `time_known()` first.** Before the device has synchronised its clock it
does not have a time, and the fields read zero. A script that draws `00:00`
there has invented it, and somebody looking at the panel has no way to tell an
invented midnight from a real one. Absence has to be visible rather than
dressed up as a plausible zero, and the tests hold published scripts to it.

### Battery

| Call | Does |
|---|---|
| `battery_known()` | Whether this device can measure one at all. |
| `battery()` | Percent. |
| `charging()` | |

Same rule, same reason: a device with no battery and a device with a flat one
both report zero, and only `battery_known()` tells them apart.

### The speaker

```berry
if audio_known()
  tone(880, 120)
end
```

| Call | Does |
|---|---|
| `audio_known()` | Whether this device has a speaker at all. |
| `tone(hz, ms)` | Queues a note. Returns whether it started. |
| `sound(name)` | Queues a built-in sound. Returns whether the name is one. |
| `volume()` | 0-255, the level somebody set. Read only. |

Both `tone` and `sound` return immediately - they queue, they do not wait, and
a script that bleeps does not cost a frame.

The built-in sounds are `beep`, `chime`, `alert`, `tick`, `tock`, `success`,
`failure`, `notify`, `alarm` and `startup`. They are short sequences of notes
rather than single beeps, and the shapes mean something: rising for good,
falling for bad, repeated for urgent. So `success` and `failure` are
distinguishable without looking at the panel, which is the entire point of a
device making a noise.

`GET /api/v1/sound` is the authoritative list, because the firmware is the
only thing that knows what it can play. An unknown name returns false rather
than playing a beep instead - if `sound()` says false, nothing happened.

Three limits, all deliberate. **Four sounds per call**, because the panel would
keep rendering happily while the speaker worked through a minute of backlog,
which is a device nobody can use and nothing on screen to say why. The budget
refills every frame, so the next one gets its own four. **Tones are clamped**
to 1-20000 Hz and 5 seconds. And **there is no `set_volume`** - the volume is
whatever its owner chose, and an app turning it up in the night is not a
feature. `volume()` is offered so a script can show the level or go quiet when
it is zero, not so it can change it.

On a device with no speaker `audio_known()` is false and `tone()` returns
false. Same rule as the clock and the battery: silence you chose and silence
the hardware cannot break are different things, and a script is told which it
has.

### The broker

```berry
def draw()
  mqtt_watch("home/solar/power")
  var w = mqtt_get("home/solar/power")
  if w == nil
    text(0, 0, "--", rgb(90, 90, 90))
  else
    text(0, 0, w, rgb(0, 190, 255))
  end
end
```

| Call | Does |
|---|---|
| `mqtt_known()` | Whether the device is connected to a broker right now. |
| `mqtt_watch(filter)` | Ask for a topic. Wildcards allowed. Returns whether it is being watched. |
| `mqtt_get(filter)` | The last payload seen, or `nil` if nothing has arrived. |
| `mqtt_age_ms(filter)` | How long ago that arrived. Negative when nothing has. |
| `mqtt_publish(leaf, payload [, retain])` | Publish under this script's own topic. Returns whether it went. |

**`mqtt_watch` is called from `draw()`, every frame.** There is no "the broker
connected" callback for a script to hook, so `draw()` is the only place a watch
can be asked for - and watching the same filter again costs nothing and does
not consume a second slot. Six filters per script.

**`mqtt_get` returns `nil`, not `""`, when nothing has arrived.** A sensor that
published an empty payload and a sensor that has said nothing since the device
booted are different states, and a script that cannot tell them apart will draw
one as the other. Check for `nil` before using the value.

**`mqtt_age_ms` is how you catch a corpse.** A retained message from a sensor
whose battery died three weeks ago arrives the instant the device connects and
looks exactly like a live reading. Age is the only thing that tells them apart,
so grey out anything older than you would trust.

#### Publishing

`mqtt_publish("state", "on")` lands on
`stipple/{deviceId}/script/{scriptId}/state` - a script writes only under its
own subtree. It cannot forge a status message, answer a command on the device's
behalf, or overwrite another script's output. A leaf that is empty, starts or
ends with `/`, contains `#`, `+` or `..`, or is over 128 bytes is refused.

Two publishes per frame. Higher would let a script loop over `mqtt_publish` and
flood a whole home automation from inside the network it lives on - and it
would look like the broker misbehaving rather than like a script anybody would
think to suspect.

Retain is off unless you ask. A retained message outlives the device that sent
it, and a script's last reading sitting on somebody's broker for ever is rarely
what was wanted.

Reading is unrestricted and writing is not, which is deliberate: the broker
belongs to whoever installed the script, and showing what is already on it is
the entire point.

### Settings somebody can fill in

A script that needs a channel ID, a username or a city should not make people
edit Berry to set it. Declare the field in a comment at the top and the
device's web page renders it:

```berry
# @config chan text "Channel ID" default="UCpGLAL..." maxlen=32 help="The UC... part of the URL, not the @handle"
# @config views boolean "Show total views instead" default=false

def draw()
  var id = store.get("chan", "UCpGLAL...")
end
```

**You read it with the `store.get` you were already using.** The declaration
names the same key, so there is no second API to learn and no second place
for the value to live - and a script running on a firmware too old to know
about `@config` still works, because `store.get` falls back on its own.

| Part | |
|---|---|
| `key` | Lowercase letters, digits, `-` and `_`. This is the store key. |
| type | `text`, `number` or `boolean`. |
| label | Quoted. Shown beside the field. |
| `default=` | What the field shows when nothing is set. |
| `help=` | A line under the field. Worth writing. |
| `maxlen=` | Text only. |
| `min=` / `max=` | Numbers only. Values outside are refused. |

Eight settings per script. The header ends at the first line that is not a
comment or blank, so a `@config` written halfway down the file is a comment
about the code there.

**A malformed declaration costs that field and nothing else.** A typo in a
settings line should not stop the script compiling, so an unknown type or a
missing label is skipped and everything else still works.

Values are stored with the type you declared, which matters more than it
looks: `store.get("views", false)` has to come back as a boolean, because a
string `"false"` is truthy in Berry and the check would silently always pass.

### The network

```berry
def draw()
  http_follow("http://192.168.1.10:8123/api/now", 300)
  var body = http_get("http://192.168.1.10:8123/api/now")
  if body == nil
    text(0, 0, "--", rgb(90, 90, 90))
  else
    text(0, 0, body, rgb(0, 190, 255))
  end
end
```

| Call | Does |
|---|---|
| `http_known()` | Whether this device can fetch anything right now. |
| `http_follow(url [, seconds])` | Put a URL on this script's list. Returns whether it is on it. |
| `http_get(url)` | The body of the last successful fetch, or `nil`. |
| `http_status(url)` | 200, 404, 500. Zero before the first answer. |
| `http_age_ms(url)` | How long ago that body arrived. Negative when none has. |
| `http_error(url)` | Why the last attempt failed, or `nil`. |

**There is deliberately no call that fetches and returns.** It would have to
block the thread that draws the panel, and a thirty-second connect timeout
would be thirty seconds of frozen display. So a script says what it wants and
how often, the device fetches it on its own schedule, and the script draws
whatever arrived last.

That is not a consolation prize. When an API goes down, this keeps the last
reading on screen with a visible age instead of hanging - which is what you
would have had to build on top of a blocking call anyway.

`http_follow` is called from `draw()`, every frame, for the same reason
`mqtt_watch` is: there is nowhere else to call it from. Calling it again is
free.

#### What it will and will not do

**Two URLs per script**, and **one request in flight across the whole
device**. A panel 52 pixels wide is not a dashboard, and a pool of sockets
would be sixteen timeouts to get right in exchange for fetching two things at
once instead of one.

**Thirty seconds is the floor**, five minutes the default, six hours the
ceiling. Asking for less than thirty gets thirty. Somebody's free API does not
want a request a frame from every one of these devices, and the script author
is not the person who would find out. Shortening the interval does not pull
the next fetch forward, so calling `http_follow` with a smaller number every
frame is not a way around it.

**A failure backs off for two minutes**, longer than any interval. An endpoint
refusing connections is not one to ask every thirty seconds.

**Only a 2xx becomes the body.** A 500 with an error page in it is not data.
The status changes, the old body stays, and the age keeps counting - so a
script can draw the last good reading and grey it out rather than putting
somebody's stack trace on the panel.

**A kilobyte of body, and the first kilobyte.** Anything a 52-pixel panel can
show is near the front of the document.

**`https` works, and is never downgraded.** Stipple carries its own TLS
(BearSSL), because the device's OpenSSL turned out to be an OpenWrt build with
every protocol version compiled out. Three things are checked before a byte of
your request goes out, and none of them can be switched off:

- the certificate chain verifies against the trusted roots;
- the certificate names the host you asked for;
- the certificate is valid *now*.

That last one needs a clock. A device that has not synchronised yet sits at
1970, which would reject every certificate ever issued - so it refuses with
`clock not set` rather than skipping the check. Wait for the clock.

Failures are named rather than generic: `certificate expired`,
`issuer not trusted`, `wrong host on cert`. TLS 1.2 only, which is what
BearSSL 0.6 speaks and what every API worth fetching still accepts.

### The microphone

```berry
if mic_known()
  var loud = mic_level()   # 0 to 32767
end
```

| Call | Does |
|---|---|
| `mic_known()` | Whether this device can hear. |
| `mic_level()` | Amplitude, 0 to 32767. Zero on a device that cannot hear. |

**One amplitude, about twenty times a second. Not a spectrum.** The TC002
reports a single level over its MCU link and nothing more, so there is no
`band()` here and there will not be one - a script drawing eight columns
labelled 60Hz to 16kHz would be making seven of them up, and it would look
convincing. Plot the level against time instead; a beat becomes a shape you
can follow, which fake bands never are.

**Raw, not normalised.** What counts as loud depends on the room and nothing
below this can know that. A visualiser that wants to fill the panel keeps its
own recent maximum and scales to it - that is auto-gain, and it belongs where
the history is. `neon-bars.be` in the shop does exactly this.

`mic_known()` is false on a device with no microphone, and `mic_level()` is
then zero - which is also what a silent room reads. Check the first before
believing the second, or a deaf device draws a flatline that looks like a bug.

### The button

```berry
def on_button(name)
  # ...
end
```

If your script has an `on_button`, it receives the action press — `name` is
`"select"` — instead of the carousel pausing. If it has none, the press does
what it always does: a script that swallowed the only button would be an app
you could not pause.

That is one control, which is enough for Flappy and not enough for Tetris.

### Taking the controls

```berry
# @input exclusive
```

Declare that in the header and your script gets everything except the way out:

| Control | `name` |
|---|---|
| − button | `minus` |
| + button | `plus` |
| knob press | `select` |
| knob, counter-clockwise | `left` |
| knob, clockwise | `right` |

**The middle button is never yours, and neither is a held knob.** The middle
button always goes back, and holding the knob always reaches settings, in
every app and every mode. That is what makes it safe to hand over the rest —
there is no script you can write that a person cannot walk away from. Pressing
middle leaves your app and returns to the clock.

Holding − or + still changes brightness, for the same reason: somebody
squinting at a panel they cannot read should not have to quit a game first.

One detent is one `left` or `right`, however fast the knob is turned. The
acceleration that makes brightness pleasant would make a paddle unplayable.

Two things worth knowing:

- **It is opt-in because it has to be.** Without the directive, nothing
  changes — every script written before this keeps the single press it was
  written against.
- **Declare it and forget `on_button` and nothing breaks.** The presses fall
  through to their ordinary jobs rather than vanishing into an app with five
  dead controls.

A press from a phone, a broker or a thumb are indistinguishable to your
script, because they all arrive through the same mapper. `POST /api/v1/input`
and the MQTT `cmd/input` topic take `minus`, `plus`, `left` and `right` under
those same names, and the knob press as `press` — the one place the two
vocabularies differ, because the API names switches and your script is told
what the press *meant*.

The device serves a ready-made controller at `/gamepad.html`. Open it on a
phone on the same network and you have a working gamepad, with the arrow keys
and space bar wired up too if there is a keyboard to hand.

### Modules

`string`, `json` and `math` are available with `import`. Everything else is
not — see the sandbox below.

## The sandbox

A script arrives over the network from whoever can reach the device, and it
runs on the thread that draws the panel. So:

- **No filesystem.** `open()` exists as a name because Berry's builtin table
  always has it, and calling it raises. There is nothing on this device a
  script should be reading; the interesting files are the Wi-Fi credentials and
  the firmware.
- **No dynamic loading**, no bytecode loader. Scripts arrive as source and are
  compiled here, where they can be rejected.
- **No `os`, `sys`, `debug`, `introspect` or `solidify`.** `debug` in
  particular would let a script remove its own instruction budget.
- **An instruction budget per frame.** A script that loops for ever loses its
  frame, not the panel. The device keeps rendering and the carousel keeps
  moving.
- **A failed script is disabled, not retried.** One that threw thirty times a
  second would fill the log and starve everything else of time. Save it again
  to re-enable it.

Each script gets its own interpreter, so one cannot reach another's state. That
costs about 4 KB each, measured rather than estimated.

Scripts are capped at 16 KB of source, and the library at 16 scripts.

## When it goes wrong

Saving a script that does not compile **succeeds**. The source is stored and
the error is attached to it, because an editor holds work in progress and a
device that refused to save until the code compiled would be one you could not
edit on.

The editor shows the compiler's message with its line number. On the panel a
broken script shows `SCRIPT ERROR` rather than going black — a black panel is
indistinguishable from a script that drew nothing, from a crashed device, and
from a dead row of LEDs.

## Credit and compatibility

**The scripting interface is AWTRIX NG's design.** The shape of a script, the
builtin names, `store.get` / `store.set` and the `# @config` header come from
[the AWTRIX NG scripting guide](https://blueforcer.github.io/awtrix-ng/guides/scripting/),
and Blueforcer is owed the credit for them. None of it is Berry's — Berry is
just the language underneath. Matching it was deliberate, because a script
already written against it running here unchanged is worth more than an
interface of our own.

That is a reimplementation from the documented interface. No AWTRIX source was
read or used, and none will be: the project studies other products as a
reference for behaviour and never as a source of code. Stipple is not
affiliated with or endorsed by AWTRIX or AWTRIX NG.

Scripts written for a TC001 will need their layout redone regardless. That
panel is 32 × 8 — a quarter of the area — and a layout squeezed into it usually
wants rethinking rather than scaling.
