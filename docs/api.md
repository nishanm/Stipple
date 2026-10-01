# The Stipple API

One surface, `/api/v1/*`, served by the device itself. No cloud, no account,
no broker in the middle unless you want one.

**Reference:** [`openapi.yaml`](openapi.yaml) — 21 routes, 33 operations,
checked against the router in CI so it cannot quietly drift.

```bash
# Render it as browsable docs, no install:
npx @redocly/cli preview-docs docs/openapi.yaml
```

## Five minutes

```bash
DEV=192.168.1.42

curl $DEV/api/v1/device                       # everything, in one call
curl $DEV/api/v1/health                       # is it alive
curl $DEV/api/v1/apps                         # what is in the carousel

# Say something
curl -X POST $DEV/api/v1/notifications \
     -H 'Content-Type: application/json' \
     -d '{"text":"tea is ready","durationSeconds":10}'

# ...with an icon, by the id you uploaded it under
curl -X POST $DEV/api/v1/notifications \
     -H 'Content-Type: application/json' \
     -d '{"text":"new mail","icon":"mail"}'

# A custom app
curl -X PUT $DEV/api/v1/apps/weather \
     -H 'Content-Type: application/json' \
     -d '{
           "name": "Weather",
           "durationSeconds": 8,
           "scene": {"elements":[
             {"type":"text","text":"12°C","x":2,"y":4,"color":"#00c8ff"}
           ]}
         }'

curl -X POST $DEV/api/v1/apps/weather/activate   # show it now
```

## How it is shaped

**The device does not fetch anything.** Apps are pushed to it. There is no
polling, no webhook, no cloud — if you want a weather app, something on your
side reads the weather and `PUT`s a scene. That is a deliberate trade: it
keeps the device simple and offline, and it means recurring data needs a
cron job or a Home Assistant automation rather than a setting.

**Scenes are declarative.** You describe what should be on the panel, not how
to draw it. Elements: `pixel`, `line`, `rectangle`, `text`, `icon`, `bitmap`,
`sprite`, `progress`, `graph`, `animation`, `group`.

**Capabilities are explicit.** `battery.known` is `false` on a device that
cannot measure one — which is a different thing from `0%`, and clients must
tell them apart. The same holds for the radio, the microphone and audio. A
missing capability is reported, never faked as an empty success.

**Errors all have one shape**, so a client needs one branch:

```json
{ "error": { "code": "not_found", "message": "no app with that id" } }
```

## Authentication

Off by default. A device that demanded a password before it would show a
clock would be a worse first five minutes than the risk it removes.

When you set one, HTTP Basic covers the API and the web UI alike — they are
the same server, and two schemes would be two things to get wrong.

```bash
curl -u admin:secret $DEV/api/v1/device
```

It is sent in the clear over your network and stored in the clear on the
device. It keeps the rest of the LAN out; it is not protection from somebody
holding the device.

**Locked out?** Hold **−** and **+** together for five seconds. That clears
the password and nothing else — apps and settings are kept, because somebody
locked out of a clock wants their configuration to still be there when they
get back in.

## The routes

| | |
|---|---|
| `GET /device` | Everything in one call — the first call to make |
| `GET /health` | Liveness, uptime, whether the clock is trustworthy |
| `GET /version` | Firmware, API and config schema versions |
| `GET /diagnostics` | Render, input and carousel counters |
| `GET /logs` | The ring buffer |
| `GET /display/frame` | The frame currently on the panel |
| `POST /input` | Inject a button press |
| `POST /glucose/alarm/test` | Play a glucose alarm melody now: `{"melody":"<rtttl>"}` or `{"alarm":"urgentLow"}`; `409` while a real alarm sounds |
| `GET POST /sound` | What it can play, and play one |
| `GET POST /apps` | List, add |
| `GET PUT PATCH DELETE /apps/{id}` | One app |
| `POST /apps/{id}/activate` | Show it now |
| `GET POST DELETE /notifications` | List, raise, clear |
| `DELETE /notifications/{id}` | Dismiss one |
| `GET POST DELETE /assets` | Icons |
| `GET DELETE /assets/{id}` | One icon |
| `GET PATCH /settings` | Everything configurable |
| `GET /network` | What it sees and what it is on |
| `POST /network/scan` | Ask the radio to look |
| `POST /network/join` | Join a network |
| `GET POST DELETE /system/firmware` | Update, and roll back |
| `POST /system/reset` | Configuration back to defaults |
| `POST /system/reboot` | Restart |

### Sounds are asked for, not assumed

`GET /api/v1/sound` lists what this device can play, with how long each one
lasts:

```json
{"sounds": [{"name": "chime", "durationMillis": 280}], "silentName": "none"}
```

Ask rather than hard-code. The firmware is the only thing that knows its own
catalogue, and the web UI's own dropdown was for a while offering three of the
five sounds that existed, which is what writing a list down twice does.

`POST` takes one of three shapes:

```json
{"sound": "chime"}
{"frequencyHz": 880, "durationMillis": 200}
{"stop": true}
```

A name the device does not have is `422`, not a substituted beep - if you
asked for something specific and got a `204`, that is what played. An inline
tone is bounded to 50-8000 Hz and five seconds, which is what a script gets
too; the network has no business reaching further into the speaker than the
device's own code does.

On a device with no speaker both verbs answer `404`. That is absence, not a
malformed request - `capabilities.audio` in `GET /device` says the same thing
before you ask.

### Updating is two steps, not one

`POST /system/firmware` writes the new library and stops. It does not reload
anything, and the device goes on running the version it was already running
until it restarts.

That matters because the obvious fields do not say so. `version` is the
version of the *process answering the request*, never the file just uploaded,
and `installedBytes` reports the same number whether the override is waiting
for a restart or already loaded — after a restart it is both. Read
`restartPending` instead: it is true when an install or rollback has happened
since the process started, which is exactly the case where what is on disk is
not what is running. It lives in memory, so a restart clears it.

Anything under `/api/` that is not `/api/v1/` answers `404` saying so
explicitly. There is no compatibility layer for other projects' APIs and none
is planned; if you want one it belongs outside the firmware, as a translating
proxy.

## Over MQTT

The same router answers both, so the two surfaces cannot drift — an MQTT
command is turned into the same request object an HTTP call produces. Off by
default; the namespace is `stipple/{deviceId}/...`.

See [mqtt.md](mqtt.md).

## Limits worth knowing

This runs on a device with 36 MB of RAM, so nothing is unbounded:

- **Request bodies are capped.** Ordinary requests have a small ceiling;
  firmware and asset uploads have their own, larger one. Over it is `413`.
- **The notification queue is bounded.** Full gets you `429`, not a device
  that slowly eats itself.
- **Icons have a byte budget**, not a count. Sixty-four static 8×8 glyphs and
  eight eight-frame animations cost the same RAM, and a per-icon limit would
  either forbid the first or permit far too much of the second.
- **Apps have a maximum count**, and ordering is explicit — never inferred
  from filesystem or map iteration, so it is stable across reboots.

## Secrets

The MQTT password and the access password are **write-only**. The device will
take them and will not give them back, to anyone — including a settings
backup, which is why restoring one asks you to type the MQTT password again.
