# MQTT

Optional, and off by default. A Stipple device is fully usable with no broker at
all, and fully usable from MQTT with no HTTP client (blueprint §20). It will
never connect to a broker nobody configured.

## Topics

`{baseTopic}/{deviceId}/...`, where `baseTopic` defaults to `stipple` and
`deviceId` is derived from the device name — lower-cased, with anything that is
not a letter or digit folded to `-`. "Kitchen Clock" becomes `kitchen-clock`.

| Topic | Direction | Retained | Meaning |
|---|---|---|---|
| `availability` | out | yes | `online` / `offline` |
| `status` | out | yes | device state, JSON |
| `button` | out | no | a control was used |
| `cmd/...` | in | — | commands, below |
| `result/...` | out | no | the answer to a command |

Availability is backed by a **last will**, so a device that loses power is marked
offline by the broker rather than lying `online` until a keepalive expires.

Results are published under `result/` rather than inside `cmd/` on purpose: the
device subscribes to `cmd/#`, so replying there would echo every answer back to
itself.

## Commands

Each command is one call into the same `/api/v1/*` handler that HTTP uses. The
payload is the HTTP request body, and the reply carries the HTTP status.

| Topic | Becomes |
|---|---|
| `cmd/notify` | `POST /api/v1/notifications` |
| `cmd/settings` | `PATCH /api/v1/settings` |
| `cmd/apps` | `POST /api/v1/apps` |
| `cmd/apps/{id}` | `PATCH /api/v1/apps/{id}`, or `DELETE` if the payload is empty |
| `cmd/apps/{id}/activate` | `POST /api/v1/apps/{id}/activate` |
| `cmd/input` | `POST /api/v1/input` |
| `cmd/sound` | `POST /api/v1/sound` |
| `cmd/reboot` | `POST /api/v1/system/reboot` |

An empty payload on `cmd/apps/{id}` deletes, because publishing an empty retained
message is how MQTT conventionally says "this is gone".

```bash
mosquitto_pub -t 'stipple/kitchen-clock/cmd/notify' -m '{"text":"Dinner"}'
mosquitto_pub -t 'stipple/kitchen-clock/cmd/settings' -m '{"display":{"power":false}}'
mosquitto_pub -t 'stipple/kitchen-clock/cmd/input' -m '{"control":"plus"}'
mosquitto_pub -t 'stipple/kitchen-clock/cmd/sound' -m '{"sound":"alert"}'
```

**Anything HTTP refuses, MQTT refuses identically** — the routing happens before
any handler, so validation, limits and error shapes cannot drift between the two.
A command MQTT could express but HTTP could not would be a bug.

Unknown commands are *reported*, never guessed at: the counter in
`/api/v1/diagnostics` goes up and a line lands in the log. A typo'd topic is
otherwise indistinguishable from a broken device.

## Reconnection

Exponential backoff from 1 s, doubling, capped at 60 s. The cap matters more than
the rate: without one, a device that was offline overnight would take hours to
notice the broker came back.

On reconnect it republishes availability and status, both retained, so a
subscriber that missed the outage still ends up with the truth.

## Credentials

The broker password is stored — a device has to reconnect unattended — and that
is the **only** place it appears.

- `GET /api/v1/settings` returns `passwordSet: true|false`, never the value, and
  no masked placeholder that a client might helpfully save back.
- `PATCH` accepts it. An empty string clears it; that is the only way to remove
  one through the API.
- It never reaches the log, diagnostics, the status topic or any other published
  payload. `MqttService.TheBrokerPasswordNeverLeavesTheDevice` and the emulator's
  `verify` harness both assert this against every message the device sends.

TLS is a **request, not a guarantee**: `mqtt.tls` asks the transport for it, and
an adapter that cannot provide it must refuse to connect rather than quietly
sending credentials in the clear. `Tc002MqttClient` does exactly that — setting
`mqtt.tls` on hardware fails the connection today. Falling back to plaintext
would put the broker password on the wire of a network somebody believed was
protected, which is worse than not connecting.

That is a gap rather than an impossibility. The question used to be whether the
device could do TLS at all; it is now answered. The TC002's own OpenSSL cannot
— it is an OpenWrt build with every protocol version compiled out — so Stipple
carries BearSSL, which is what makes `https` work for scripts. Pointing the
MQTT transport at the same TLS is work that has not been done, not a wall.

## Home Assistant

Turn on `mqtt.discovery` and the device publishes Home Assistant discovery
documents under `homeassistant/...`, retained, so the entities survive a Home
Assistant restart rather than vanishing until the device next says something.

Five entities, all carrying the same device block so they group under one
device instead of scattering across the dashboard:

| Entity | Component | What it is |
|---|---|---|
| Panel | `light` | On, off and brightness |
| Volume | `number` | 0–100 |
| Battery | `sensor` | Percentage, absent on a device that cannot report one |
| Signal | `sensor` | RSSI |
| App | `sensor` | Which app is on screen |

The panel uses the **template** schema rather than the default. The default
would send `ON` to a command topic, and this device speaks a settings patch — a
template lets Home Assistant emit exactly the JSON that already works, so
discovery adds no second control path to keep in step with the first.

Turning the setting off withdraws the entities by publishing an empty payload
to each config topic, which is how MQTT says "this is gone". Leaving them
orphaned in somebody's dashboard would be worse than never publishing them.

## What is not built

- **MQTT over TLS.** The device refuses `mqtt.tls` rather than downgrading,
  and now says so: `capabilities.mqttTls` is false on `/api/v1/device` and the
  web page disables the switch and explains it, rather than leaving a refusal
  that looks exactly like an unreachable broker.

  What it would take is worth writing down, because it is not simply "call the
  TLS we already have". That TLS is driven by BearSSL's `br_sslio`, which
  blocks until it has what it needs — fine for the fetch worker, which has a
  thread to block. This transport is polled from the render loop and must
  never wait, so it needs BearSSL's engine API instead. And the transport
  takes a numeric address today, because a statically linked binary cannot
  use `getaddrinfo`; certificates are issued for names, so TLS also needs the
  resolver the HTTP client carries. Two pieces, neither of them large on its
  own, neither of them a wall.
- **QoS 2.** Deliberately not offered. It costs a four-way handshake and
  per-message state on a device with an unmeasured RAM budget, to solve a problem
  this product does not have: a duplicated "show a notification" is a much
  smaller harm than a stalled session.
