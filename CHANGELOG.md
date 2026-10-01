# Changelog

Notable changes per release. The release notes on GitHub are generated from
this file, so what is written here is what people read when they download a
build.

Versions are `MAJOR.MINOR.PATCH`. While on `0.x` every release is a
prerelease: the interfaces move, and nothing here installs onto a stock
device without a capture of that device first.

## 0.2.9

### Added

- **Ten sounds instead of five, and they mean something.** `success`,
  `failure`, `notify`, `alarm` and `startup` join the original five, and all
  of them are now short sequences of notes rather than single beeps. The
  shapes carry the meaning: rising for good, falling for bad, repeated for
  urgent. `success` and `failure` are distinguishable without looking at the
  panel, which is the entire point of a device making a noise.

  `chime` and `alert` were single notes and are now the two- and three-note
  shapes their names always implied — `alert` in particular managed to be less
  alarming than `chime`, because one long note reads as a drone. `beep`,
  `tick` and `tock` are unchanged note for note.

- **`GET POST /api/v1/sound`, and `cmd/sound` over MQTT.** Until now nothing
  but a notification could make the device make a noise. `POST` plays a named
  sound, an inline tone, or stops whatever is playing; `GET` lists what this
  device can play, because the firmware is the only thing that knows.

  An unknown name is `422`, never a substituted beep. If you asked for
  something specific and got a `204`, that is what played. A device with no
  speaker answers `404` on both verbs — absence, not a malformed request.

- **A "Play it" button beside the notification sound.** Choosing a sound from
  a dropdown and then waiting for a notification to discover what you chose is
  not a choice, it is a guess.

### Changed

- **AWTRIX NG is credited for the scripting interface.** The shape of a script,
  the builtin names, `store.get` / `store.set` and the `# @config` header are
  its design, and the public docs said only "compatibility" where they should
  have said credit — the decision to match that interface was recorded
  internally but never written down where anyone could read it. Now stated in
  `THIRD_PARTY_NOTICES.md` and `docs/scripting.md`, both linking the AWTRIX NG
  scripting guide. The rule it was always under is unchanged: reimplemented
  from published documentation, no AWTRIX source read or used.

- **The sound catalogue moved into core.** It used to be an if-chain inside
  `Tc002Audio`, which meant the simulator carried a second and different list,
  and the simulator is what people write scripts against. `sound('trumpet')`
  worked on a desk and did nothing on a clock, returning true in both places.
  Both adapters now resolve through one table in `audio/Sound.h`, so they
  cannot disagree, and a sound's timing can be tested without a speaker in the
  room.

  What a chime sounds like was never a fact about SigmaStar hardware.

- **`.containerignore`**, because the cross-toolchain build context was 1.4 GB
  of build output and Visual Studio index for a Containerfile that copies
  nothing in. Worse than slow: `.vs/` holds files the IDE keeps open, and
  podman failed outright trying to read one, so the cross build could not run
  while the editor was.

- The web UI's sound dropdown is filled from the device instead of from
  markup. It had been listing three of the five sounds the firmware had, which
  is what writing a catalogue down twice does.

- **`firmware/tools/player_probe`**, which asks whether the TC002 can play an
  audio *file* through the vendor's own player. It settles that
  `libzkmedia.so` exposes a `PlayerFactory` — so the "C++ class of unknown
  size" objection that had parked this is answered — and that `EMediaType 0`
  is audio.

  It does not yet get a sound out. `play()` returns the same thing for a real
  MP3, a real WAV and a path that does not exist, which means it never opened
  any of them; the likely cause is that STIPPLE itself holds `/dev/mi_ao`.
  Proving that needs the panel released, and that test was deliberately not
  run — see the findings doc.

### Fixed

- **A sound ending mid-frame replayed a fragment of the previous frame.**
  `Tc002Audio` handed the driver a full frame whether or not the sound had
  filled it, leaving stale samples in the tail. One note ending was quiet
  enough to miss; a sequence has a boundary between every pair of notes, so
  this had to be fixed before the sounds above could exist.

## 0.2.8

### Added

- **Eleven more scripts, and most of them are playable.** Block Drop, Neon
  Breakout, Star Defender, Moon Lander, Night Crossing, Sky Runner and Canyon
  Flyer are games with scoring and a game-over you can restart; Light Painter,
  Ripple Pond, Spiro Comet and Sunset Drive are things to look at.

  These are what `# @input exclusive` was for. 0.2.7 added the directive and
  one game that used it; this is the library catching up, now that a script
  can have a left and a right.

### Changed

- **The controller page is laid out like the case.** The knob is on the left
  and the three buttons are on the right in the order − M +, which is where
  they are on the hardware, and the knob is a dial you turn with a thumb
  rather than a pair of arrow pads. Turning it emits one detent per 18° and
  the mark tracks your finger, so a rally feels like the control it is
  imitating.

  **M moved out of the header and into its own place in the row.** It used to
  be a small pill labelled "Back", kept away from the play area; it is the one
  control a script can never take, so the page now teaches where it actually
  is. A link to the settings page replaces it as the way out of the browser.

  Holding an arrow key now repeats, because holding an arrow is how you turn a
  knob you have no thumb on. Buttons still ignore autorepeat — a second `down`
  with no `up` between would restart a hold the firmware was already
  measuring.

### Fixed

- **0.2.7 said a USB gamepad works. It does not, on this hardware.** That
  release reported that `usbhid` and `hid-generic` are bound and the EHCI root
  hub is live, and concluded a wired pad "needs no firmware code at all". The
  first half is true and the conclusion did not follow: a DualSense plugged
  into a real unit does not enumerate on either connector. The root hub stays
  empty, `usb_det` never leaves 0, and forcing a host-role re-init with the
  pad attached changes nothing.

  Two reasons, both measured. The device tree carries no VBUS property for the
  kernel to drive, so the port most likely does not power what is on it. And
  `soc:Sstar-ehci-1` is the only USB host on the SoC — no OHCI, no UHCI, no
  XHCI — so full-speed enumeration depends on this EHCI's PHY in a way nothing
  confirms. A powered hub is the next thing to try.

  There is also only one USB data port behind the two connectors on the case.

- **Three sysfs files that act when they are read**, recorded because reading
  them is the obvious first move. `usb_host`, `usb_device` and `usb_null` sit
  beside `otg_role` under `soc:usbotg`, and catting the directory to see what
  it holds walks the port into null role and leaves it with no bus. It is
  recoverable — re-reading `usb_host` restores it — but on a unit reached over
  USB rather than Wi-Fi it would not be. `tooling/probe/probe.py` reads
  `otg_role` and `usb_det` only, and now says why, so nobody widens it to a
  glob.

## 0.2.7

### Added

- **Scripts can take the controls.** A script declaring `# @input exclusive`
  in its header receives the − and + buttons, the knob press and both knob
  detents, instead of the single action press it used to get. Games were the
  reason: three of the published scripts play themselves — Auto Pong's own
  summary is *"two computer players rally forever"* — not as a design choice
  but because one button is not a game.

  **The middle button is never handed over, and neither is a held knob.**
  Middle always goes back and a held knob always reaches settings, in every
  app, so there is no script you can write that a person cannot walk away
  from. That is what makes giving away the rest safe. Holding − or + still
  changes brightness for the same reason.

  Opt-in, so every script written before this keeps exactly the behaviour it
  was written against.

- **Pong**, the playable counterpart to Auto Pong. You take the left paddle;
  the clock takes the right.

- **A controller page at `/gamepad.html`**, linked from the top of the web
  UI. Open it on a phone on the same network and you have a gamepad, with
  arrow keys and the space bar wired up if there is a keyboard to hand. It
  posts to `/api/v1/input`, which has accepted every control with separate
  press and release phases since the API existed — the page is thumb-sized
  targets over plumbing that was already there.

- **`cmd/input` over MQTT**, so an automation can press a button. It maps to
  the same `POST /api/v1/input` the browser uses.

### Changed

- `tooling/probe/probe.py` gained a Bluetooth and USB host section, and now
  lists `/res/bin`. Run against a real clock, it settles what a second
  controller would cost.

  **A USB gamepad needs no firmware code at all**: `usbhid` and
  `hid-generic` are bound, the EHCI root hub is live, and the OTG port is in
  host role, so a wired pad appears as another `/dev/input/eventN` — which
  `Tc002Input` already takes as a parameter. The catch is that the port is
  also the charge port.

  **Bluetooth is half a stack.** L2CAP, SMP and the HCI UART line discipline
  are all in the kernel, and BlueZ tools ship in `/res/bin` — but `hidp`,
  `rfcomm` and `uhid` are all absent, so a Bluetooth gamepad cannot become an
  input device by any kernel route. It would have to be decoded in the
  adapter.

  Two documentation bugs fell out of it. The research notes said `/bin` was
  "the complete contents" — it was, but `/res/bin` exists and had never been
  listed, which is where `hciattach` lives. And the first version of the
  probe's module scan used `find`, which the device does not have, so it
  reported "none" for a directory holding two modules.

## 0.2.6

### Added

- **EVCC Energy**, a live energy balance for anyone running
  [EVCC](https://evcc.io). Solar, house, car, battery and the grid as one
  picture: what is producing on one bar, what is consuming on the other, the
  two headline numbers above them and the house battery underneath. The
  action button steps through each reading in detail. Ported from the 32x8
  original, and the extra height is spent on showing everything at once
  rather than making somebody press the button eight times to find out what
  the house is doing.
- **Plane Spotter**, the nearest aircraft overhead - callsign, altitude, and
  an arrow pointing at which window to look out of. The feed is adsb.lol,
  which is free, needs no key and is fed by volunteers with receivers on
  their roofs. Anything on the ground is skipped, because near an airport
  that would be most of them and none of them are visible from a window.
- **Tetris**, which plays itself. A sixteen-pixel-tall panel is exactly the
  shape of a well, so the board takes the left and the score the right, the
  way an arcade cabinet laid it out for the same reason. It arrives
  mid-game rather than on an empty board, and it is meant to lose
  eventually - a player that never tops out would draw the same picture for
  ever.
- **Sandbox**, falling sand that pours, piles and slumps, with the button to
  shake the whole thing loose.
- **Air Quality**, the European AQI and particulates for your street from
  Open-Meteo, which needs no key. The band is named as well as numbered,
  because 43 means nothing to most people and "MOD" means something to
  everybody, and a colour scale underneath says whether that is nearly clean
  or nearly bad.
- **Split Flap**, a departure-board clock whose characters only ever turn
  forwards, so getting from Y to A means going the whole way round. That is
  most of what makes a departure board look like one.
- **Now Playing**, whatever is on the speakers, over MQTT - title, artist and
  a progress bar that only appears when both the position and the duration
  are known. Guessing a duration would draw a bar that runs out at the wrong
  time, which is worse than no bar.
- **Langton's Ant**: two rules, ten thousand steps of chaos, and then it
  builds a road and leaves.
- **Bin Day**, which bin goes out next and how long you have. Unglamorous,
  and the one that stays on screen.
- **Internet Monitor**, which answers "is the line up, and what is my public
  address" against two independent services so that one of them being down is
  not reported as the internet being down. The address is split across two
  lines rather than scrolled, because at 52 pixels wide it fits that way and
  can be read at a glance instead of over four seconds. Both service URLs are
  settings.

### Changed

- **The script library is leaner.** Every script carried a long explanatory
  block above its code, some of them forty lines, written to justify decisions
  while they were being made. That is not what a published script is for - the
  source is what people read on the device's config page and edit in a
  textarea, and a page of prose before the first line of code makes it worse.
  Down from 6684 lines to 5919 across the library, and from 19% comments to
  10%, with the metadata headers and every `@config` line untouched.
- **Scripts are credited to Stipple**, so the library reads as one collection
  rather than a pile with different names on it. The one script whose origin
  is genuinely unknown keeps saying so, because replacing that with a name
  would be a claim rather than a credit.
- **Shop previews use plausible data per feed.** Every MQTT topic and every
  HTTP URL used to get the same canned answer, which made an energy balance
  meaningless - solar, house load and battery charge all identical, so every
  segment of the bar came out the same width. A preview that cannot be wrong
  is also one that cannot be right.

### Fixed

- **A script could be killed by pressing the button, and the tests could not
  see it.** Two faults met: the shop test pressed a button named `action`
  while the firmware sends `select`, so every published script rejected the
  press on its handler's first line and the test proved nothing about the
  code underneath; and the long-running test never checked that a script was
  still alive, only that it had not leaked. A script that accumulates - a
  pile of sand, a filling well - costs more per frame the fuller it gets, and
  its most expensive frame is nowhere near the first ninety. Both are fixed,
  and the combination reproduced the failure immediately.
- **Sandbox and Tetris were too expensive per frame.** Sandbox went over the
  instruction budget once the panel filled and somebody pressed the button,
  which disables the script outright. Reading the grid directly instead of
  through helpers halved it, and Tetris - whose peak frame sat one heartbeat
  under the ceiling, which is a script that dies the first time anything else
  is slightly slower - came down by a third the same way.
- **Home Assistant discovery is now tested.** It was implemented and working
  but had no coverage, which mattered more than it sounds: discovery is the
  one output nothing reads back, so a wrong topic or a device block that
  differed between entities would have failed silently in somebody's house
  rather than on the device. Eight tests pin the properties that make it
  correct - one shared device block, identifiers that cannot collide between
  two devices on one broker, withdrawal that removes entities rather than
  orphaning them, and templates that yield nothing for a reading the device
  does not have.
- **Asking for MQTT over TLS now says why it cannot.** The device refuses the
  connection rather than downgrading, which was always right - plaintext would
  put the broker password on the wire of a network somebody believed was
  protected. But a refusal looks exactly like an unreachable broker from the
  web page, so turning the switch on simply made MQTT stop working with no
  explanation, and the reconnect policy retried it forever. The capability is
  now reported as `capabilities.mqttTls`, the switch is disabled and explains
  itself, and the refusal no longer feeds the retry loop.

## 0.2.5

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
- **The website has pages for scripting and MQTT**, rendered from
  `docs/scripting.md` and `docs/mqtt.md` rather than written a second time.
  The script library opens with the scripts now instead of three paragraphs
  explaining what Berry is.

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
- **The emulator is no longer published to the website.** It is still built
  and verified by CI, still shipped in releases, and still runs under
  `dev.ps1 serve` — it just no longer costs the Pages build an Emscripten
  toolchain on every deploy to serve a page almost nobody opened from there.

### Fixed

- **A release is now one button.** Run the Release workflow from the Actions
  tab and it works out the next version, writes it into the files that have to
  agree, names the changelog's `## Unreleased` section after it, runs the full
  gate set, packages, commits the bump, tags it and publishes. Nothing is
  written until everything has passed, so a failed run leaves `main` exactly as
  it was.
- **A release now builds the device library it ships**, instead of taking it
  from the CI run it depends on. CI builds from the commit as it stands,
  before the version bump exists, so the artifact was renamed to the new
  version while the binary inside still reported the old one — an update that
  installed correctly and then went on reporting the version it replaced. The
  release also refuses to publish a library that does not contain its own
  version string.
- **The web page said an update was running when it was not.** Installing
  writes the new library and stops there; the device goes on running what it
  was already running until it restarts. The page reported that as "Running an
  installed update (1306 KB). Version 0.2.3." — where the size described the
  file just uploaded and the version described the process still serving the
  page. Two true halves that read as one sentence saying the update had taken
  effect. It now names the running version, says the update starts on restart,
  and offers a Restart button beside the install control.

  The install handler had the right words all along and threw them away: it
  set "Installed. Restart to run it", then reloaded the state on the very next
  line, which overwrote it. The rollback handler did the same thing.

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
