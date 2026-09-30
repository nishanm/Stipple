# Changelog

Notable changes per release. The release notes on GitHub are generated from
this file, so what is written here is what people read when they download a
build.

Versions are `MAJOR.MINOR.PATCH`. While on `0.x` every release is a
prerelease: the interfaces move, and nothing here installs onto a stock
device without a capture of that device first.

## Unreleased

### Added

- **A glucose app.** Six faces for a continuous glucose monitor reading - a
  hero value with direction, with delta, with three hours of history, a
  bedside clock-and-value, a full-panel graph, and an explicit no-data face -
  reproduced byte for byte from the `nightscout-pixbar` reference renderer's
  golden corpus, and held to it by the test suite. See
  `docs/development/glucose-app.md`.
- **The glucose app reads Nightscout itself.** Set the site's URL and API
  secret under the app's settings and the device polls `entries.json` once a
  minute, ages the reading honestly when the server stops answering, shows the
  no-data face once it is twenty minutes old, and holds off for five minutes
  (doubling to thirty) after a refused credential. The device keeps only the
  secret's SHA-1 - the form Nightscout checks - and never returns it.
- **The glucose display is a mode.** While a Nightscout source is set and the
  app is pinned (the default), it stays on screen and the knob moves between
  its faces instead of between apps - one detent, one face, the choice kept
  across reboots. The middle button still goes back to the clock, and the
  carousel returns to glucose on its own. A face changed while the reading is
  stale is named on the readout, since the no-data face would hide the change.
- **Fetches can carry a header and ask for a larger body.** `IHttpClient::begin`
  takes an `HttpRequest` with one optional header and a per-request body cap;
  the kilobyte default is unchanged for scripts. The simulator now truncates at
  the cap like the device does.

### Fixed

- **Two things fetching no longer fight over the one HTTP client.** The script
  fetcher waits while another request is in flight instead of failing it, and
  deleting a script no longer resets a fetch it did not start.
- **An expired adjustment readout is painted over.** Over a face that does not
  redraw on its own it stayed on the panel until the next minute.

- **`https` works.** Stipple carries its own TLS, so a script can fetch from
  an API that requires it. The certificate chain is verified against trusted
  roots, the certificate must name the host asked for, and it must be valid
  *now* — none of which can be switched off. A device whose clock has not
  synchronised yet refuses with `clock not set` rather than skipping the
  validity check, because a device sitting at 1970 would reject every
  certificate ever issued.

### Fixed

- **A release is now one button.** Run the Release workflow from the Actions
  tab and it works out the next version, writes it into the files that have to
  agree, names the changelog's `## Unreleased` section after it, runs the full
  gate set, packages, commits the bump, tags it and publishes. Nothing is
  written until everything has passed, so a failed run leaves `main` exactly as
  it was.

### Changed

- **TLS is BearSSL, not the device's OpenSSL.** The TC002's OpenSSL turned out
  to be an OpenWrt build from 2018 with every TLS protocol version compiled
  out — a crypto library with a stub SSL layer, which answered
  `NO_PROTOCOLS_AVAILABLE` for every protocol floor including none at all. So
  there was nothing on the platform to borrow. BearSSL is MIT, allocates
  nothing of its own, and cost about 130 KB.
- Trusted roots ship as a file that can be replaced without reflashing, rather
  than a table compiled into the firmware. A device needing a rebuild to trust
  a new CA is one that stops working on a date nobody scheduled.

## 0.2.3 — Scripts that can hear, speak and ask

Berry scripts get the speaker, the microphone, the broker and the network.
0.2.1 and 0.2.2 were bumped in the tree but never released, so everything
below is what changed since 0.2.0.

### Added

- **The speaker.** `tone()`, `sound()`, `audio_known()` and `volume()`. Four
  sounds per call, refilled each frame — the panel would keep rendering
  happily while the speaker worked through a minute of queued beeps, which is
  a device nobody can use and nothing on screen to say why. There is no
  `set_volume`: the level is whatever its owner chose, and an app turning it
  up in the night is not a feature.
- **The microphone.** `mic_known()` and `mic_level()`, and deliberately
  nothing else. The TC002 reports one amplitude about twenty times a second,
  so a `band()` here would be inventing the number it returned. Raw rather
  than normalised, because what counts as loud depends on the room.
- **The broker.** `mqtt_known()`, `mqtt_watch()`, `mqtt_get()`,
  `mqtt_age_ms()` and `mqtt_publish()`. A script publishes only under
  `stipple/{deviceId}/script/{its own id}/`, so it cannot forge a status
  message or overwrite another script's output; reading is unrestricted,
  because the broker is yours and showing what is already on it is the point.
- **The network.** `http_follow()`, `http_get()`, `http_status()`,
  `http_age_ms()` and `http_error()`. There is deliberately no call that
  fetches and returns — it would block the thread drawing the panel — so a
  script says what it wants and how often and draws whatever last arrived.
  Thirty seconds is the floor whatever a script asks, one request is in
  flight across the whole device, and a failure backs off for two minutes.
- **Eleven new scripts**, all compiled and run by the test suite before
  publication: Aurora, Dutch Trains, Fireworks, Game of Life, Hootie, Kitchen
  Timer, Meteor Shower, Metronome, Neon Bars, Plasma, Power Meter, Rain,
  Selenograph, Sequencer and Weather.
- **Syntax highlighting in the device's script editor**, with line numbers and
  the line a compile error names marked in the gutter. Still no editor
  library: the page is compiled into the firmware, so it is a tokeniser and a
  real textarea, which keeps the caret, selection, undo and mobile keyboards
  working.
- **A carousel rotation switch** in the web UI. The setting already existed in
  the configuration and over the API but had no control on the page that owns
  every other app setting.
- **TLS for outbound fetches**, loaded from the device's own OpenSSL at
  runtime. Certificate chain, hostname verification and a TLS 1.2 floor, with
  no way to switch any of it off. See *Known limitations* — it does not work
  on a TC002 yet, and the reason is the device's.

### Fixed

- **Names did not resolve on the device.** The TC002 ships
  `nameserver 114.114.114.114`, which is unreachable from most of the world,
  and `getaddrinfo` spends five seconds twice on it before trying the next
  entry. Every fetch to a hostname timed out. Stipple now reads `resolv.conf`
  and asks the nameservers itself with a two-second timeout, remembering
  which one answered.
- **Icons larger than 8 × 8 were rejected for the wrong reason.** Pixels
  arrive as JSON integers, so every pixel is a token, and the parser's budget
  was 512 — two 16 × 16 frames. An icon well inside every size limit the page
  advertises came back "invalid JSON: too many tokens". Icons now have their
  own token and body ceilings.
- **Custom apps pushed over the API drew black screens.** A scene could name
  its box as `x`/`y`/`w`/`h` rather than `rect`, and an icon by `id` rather
  than `icon`, and neither was accepted; a scene nothing could draw was
  stored without complaint. Both shapes now work and an undrawable scene is
  refused with 422 and a list of what was wrong.
- **Scripts lost their carousel entry on every reboot**, and a per-app
  duration could land on the wrong app.
- **Two device builds were broken in ways no host build could show**, because
  the host does not compile the TC002 adapter: a member name collision
  between the HTTP client and the HTTP server, and a misqualified version
  constant.
- The script editor's `now_ms()` returned time since the app appeared rather
  than since boot, so scripts that throttle on it stopped moving when the
  carousel came back to them.

### Changed

- **The icon store holds 192 icons in 256 KB**, up from 64 in 64 KB. An 8 × 8
  icon is 192 bytes and a 16 × 16 is 768, so doubling the size quarters how
  many fit; the count ran out long before the bytes did.
- **Shop previews warm up for two seconds before recording.** Every script
  that accumulates anything was opening on an empty version of itself, and a
  card is only three seconds long.
- The device web page no longer reads "Stipple  stipple v0.2.3" on a device
  nobody has renamed.

### Known limitations

- **`https` does not work on a TC002.** The TLS implementation is complete and
  verifies properly, but the device's OpenSSL is 1.1.0i, built by OpenWrt in
  2018 with every TLS protocol version compiled out — a crypto library with a
  stub SSL layer. `SSL_connect` answers `NO_PROTOCOLS_AVAILABLE` for every
  protocol floor including none at all. Scripts see `openssl has no tls` from
  `http_error()`. Plain `http` to anything on your own network works. Closing
  this needs Stipple to carry its own TLS rather than borrow the device's.
- Nothing here installs onto a stock device without a capture of that device
  first, and the firmware-update endpoint still has not been exercised end to
  end.

## 0.2.0 — Scripting

Apps you write yourself, in [Berry](https://github.com/berry-lang/berry), on
the device.

### Added

- **Berry scripting.** A script is a class with a `draw()` method; it joins
  the carousel, draws on the panel, and can take the button. Written in the
  device's own web page under **Scripts**, or pushed over
  `/api/v1/scripts`. See [docs/scripting.md](docs/scripting.md).
- **A script editor in the device web UI.** Write, save and delete; the
  carousel keeps up by itself. Saving something that does not compile
  succeeds — the source is stored with the compiler's message beside it,
  because an editor holds work in progress and a device that refused to save
  until the code compiled would be one you could not edit on.
- **A script shop** at [/shop/](https://galadril.github.io/Stipple/shop/),
  built from `scripts/` in this repository. Every script published there is
  compiled and run by the test suite first: ninety frames on a real
  52 × 16 framebuffer, buttons pressed, then six hundred more checked for
  leaks.
- **Scripts can see the device.** `hour()`, `minute()`, `second()`,
  `weekday()`, `day()`, `month()`, `year()`, `battery()`, `charging()` —
  each with a companion (`time_known()`, `battery_known()`) that says whether
  the value means anything. A device that has never synchronised its clock
  does not have a time, and a script drawing `00:00` there has invented one.
- **`store.get` / `store.set`**, so a high score survives a power cut, and
  **`scroll_text()`**, which returns completed passes — the only way a script
  can know its message has been read.
- **`duration()`**, so a script that cycles through several readouts can ask
  for longer on screen than the carousel's default.
- `/api/v1/scripts` and `/api/v1/scripts/{id}`, documented in
  [openapi.yaml](docs/openapi.yaml).

### The sandbox

A script arrives over the network from whoever can reach the device and runs
on the thread that draws the panel, so:

- No filesystem, no dynamic loader, no bytecode loader. `open()` exists as a
  name — Berry's builtin table always has it — and calling it raises.
- No `os`, `sys`, `debug`, `introspect` or `solidify`. `debug` in particular
  would let a script remove its own instruction budget.
- An instruction budget per frame and per button handler. A script that loops
  for ever loses its frame; the panel keeps rendering and the carousel keeps
  moving.
- A script that fails is disabled rather than retried thirty times a second.
- One interpreter per script, so one cannot reach another's state. About 4 KB
  each, measured rather than estimated.

### Fixed

- A one-slot-per-frame leak in the script host. Sixteen bytes a frame is
  invisible in any short test and fatal after about two minutes on screen,
  once Berry's 4000-slot stack ran out.
- Vendored headers are no longer held to this project's warning flags, which
  broke the ARM build in a way no Windows build could show.
- The script shop was missing from the published site: the Pages workflow
  copies files by name and nobody had added a line. It now checks that every
  page the navigation links to actually exists.

### Changed

- The site uses the same palette as the device's own configuration page.
  Pure black read as a void rather than a surface. The LED panel itself stays
  literally black, which is what an unlit pixel is.

## 0.1.0

First tagged build. Framebuffer, canvas, font and text engine, scenes, the
app carousel, notifications, configuration, the `/api/v1` server, the device
web UI, MQTT, and the TC002 adapter — display, input, MCU, HTTP, Wi-Fi and
SNTP — plus the browser emulator.
