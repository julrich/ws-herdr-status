# ws-herdr-status

A desk companion that shows what the coding agents on this machine are doing. An
ESP32-C6 board with a 172x320 panel sits on the desk, joins WiFi and draws the
live state of the agents running in [herdr](https://herdr.dev) as
a small kawaii face with an agent list under it. The panel is portrait, always:
172x320, the face above the list.

No cable to the PC is needed after flashing: the device polls a tiny HTTP bridge
running on the PC.

```
herdr ──(unix socket, JSON)──> bridge/herdr_status_bridge.py ──(HTTP GET /state)──> device
  agent status                  0.0.0.0:8787, stdlib only                    WiFi poller + LVGL UI
```

| Path | What it is |
|---|---|
| `bridge/herdr_status_bridge.py` | PC side. Reads herdr over its Unix socket, serves `GET /state` and `GET /stats` on the LAN. Python 3 stdlib only, no dependencies. |
| `main/` | Firmware: WiFi station, HTTP poller and the LVGL companion. |
| `components/` | Board drivers (JD9853 panel, AXS5106L touch, BSP glue) plus two fixes to the vendor touch driver, see *Notes*. |
| `tools/ui_host_test/` | Renders the UI on the host and asserts on pixels — the only way to check the artwork without eyes on the panel. |
| `AGENTS.md` | The board's bring-up notes and this project's invariants. Read §11 before changing display or touch code. |

## What you need

- Waveshare ESP32-C6-Touch-LCD-1.47 (ESP32-C6FH8, 172x320 IPS, capacitive touch, QMI8658A IMU).
- ESP-IDF **5.5.1** (the version this was built and flashed with), exported in the shell.
- `herdr` running on the PC.
- A **2.4 GHz** WiFi network; the C6 has no 5 GHz radio.

## Quick start

**1. The bridge (on the PC).** Check it can see herdr, then install it as a user
service so it survives reboots:

```bash
make bridge-once          # prints one /state document; exits non-zero if herdr is unreachable
make bridge-service-install
make bridge-service-status
curl -sS localhost:8787/state | jq .
```

`/state` looks like this — `gen` increments on every poll, `stale` turns true if
herdr has not answered for 5 s:

```json
{"v":1,"gen":17285,"stale":false,
 "agents":[{"id":"w5:p1","kind":"omp","label":"PoC","status":"working","focus":true}]}
```

`/stats` carries the per-agent session numbers the stats view draws (tokens in
and out, calls, messages, age, and what the session has cost), keyed and ordered
like the `/state` agents. Money travels as integer **micro-dollars** — `1_000_000`
is $1 — so the device can print two decimals with integer arithmetic:

```json
{"v":1,"gen":17285,"stale":false,"sessions":2,
 "totals":{"in":41235,"out":9004,"calls":311,"messages":264,"age_s":1840,"cost_micro":12745891},
 "agents":[{"id":"w5:p1","in":25120,"out":6103,"calls":188,"messages":160,"age_s":1840,
            "cost_micro":4564666,"model":"opus"}]}
```

**2. The firmware (on the device).** Set the WiFi credentials and the bridge
address once, then build and flash:

```bash
. ~/esp/esp-idf/export.sh
idf.py menuconfig        # "Herdr status companion": WiFi SSID / password / bridge host
make fw-build fw-flash fw-monitor
```

The credentials live in `sdkconfig`, which is **gitignored** — they must never
be added to `sdkconfig.defaults`.

## Using it

- **Tap the top half** (the face) to force an immediate poll; the mood's flourish
  plays as feedback.
- **Tap the bottom half** (the list) to page the agent list when more agents are
  running than fit on the screen.
- **Hold a finger down** (~1 s) anywhere to switch between the mood view and the
  stats view. The stats view opens on the sessions page — tokens in and out, calls,
  messages and the money spent, with a row per agent — and a tap on the list's half
  pages to the link and device figures.
- **Double tap the top half** does the same switch, but prefer the hold: it needs
  both taps inside LVGL's own time and movement limits, which a thumb on a 172 px
  panel manages only sometimes.

The panel reports a single touch, so that is the whole vocabulary: a tap in each
half, a hold anywhere, and a double tap on the face's half for what the hold does.
Swipes were tried first and dropped — a drag that LVGL reads as a gesture also
suppresses the click, so a swipe that fell short did nothing at all (AGENTS.md §11).
- The face tells you the aggregate mood at a glance; the headline and list tell
  you which agents are responsible:

| Mood | Face | Means |
|---|---|---|
| BLOCKED | amber, `!` pulsing, sweat drop | at least one agent needs you |
| WORKING | blue, spinning ring | an agent is working |
| DONE | green, confetti burst | an agent finished |
| IDLE | pale, blushing | agents online, nothing happening |
| SLEEP | dark, `z`s drifting | no agents running |
| OFFLINE | grey, dimmed list | the bridge or the network is unreachable |

## Configuration

Set with `idf.py menuconfig` → *Herdr status companion*:

| Option | Default | Meaning |
|---|---|---|
| `HERDR_WIFI_SSID` / `HERDR_WIFI_PASSWORD` | empty | Station credentials. Empty SSID = the firmware runs but never joins. |
| `HERDR_BRIDGE_HOST` / `HERDR_BRIDGE_PORT` | `192.168.2.203` / `8787` | Where the bridge listens. |
| `HERDR_POLL_PERIOD_MS` | `1000` | Sleep between polls; the cadence is this plus one HTTP round trip. |
| `HERDR_UI_MAX_AGENTS` | `4` | Rows shown (1-6). Extra agents surface as `+N` in the summary. |

## Tests

```bash
make ui-test              # 24 pixel scenarios: every mood, the burst, the idle
                          # animations, decorations, both orientations, and the
                          # gestures driven through LVGL's own pointer events
```

It runs on the host, needs no hardware, and exits non-zero on failure.
`make ui-test` writes `tools/ui_host_test/frame_*.ppm` (gitignored) so the art can
be eyeballed — the mood face included, since the harness builds the real kawaii
component.

## What healthy looks like

```
I wifi: connected, ip=192.168.2.222
I herdr: GET http://192.168.2.203:8787/state -> 200 (228 bytes)
I herdr: gen=1235 online=1 agents=2 blocked=0 working=1 done=0 idle=1
I ui: mood=working online=1 agents=2 overflow=0
```

Two failure modes worth knowing (both degrade rather than break):

- **No `I2C read error!` lines from the touch driver** over minutes is the bus
  health signal. If the touch controller stops acknowledging, the driver logs it
  and the UI keeps running; a power cycle clears it.

If nothing appears at all, check the cable is data-capable and the console
(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`) reaches `/dev/ttyACM0` — see AGENTS.md §3.

## Notes

The firmware carries two fixes to the vendor touch driver, both in
`components/esp_lcd_touch_axs5106/` and both described in AGENTS.md §7:

- the register read stays the vendor's two transactions (`i2c_master_transmit`
  then `i2c_master_receive`) because this controller NACKs the repeated-start
  form — measured, see AGENTS.md §7 — but the transmit result is now checked, so a
  blind receive never follows an unacknowledged address phase. (The QMI8658A on the
  same bus is what that used to knock out; this project no longer uses it.)
- the controller is read when INT says a report is ready, and also while the last
  report still says a finger is down. Both halves are load-bearing, and both were
  measured the hard way: reading on every poll is NACKed every time at idle (the
  reset watchdog then fires and the controller does not recover), while reading
  only on the asserting edge never notices a release — the finger-down sample
  stays in the handle and LVGL holds a press that never ends, so tapping does
  nothing at all;
- the read is 8 bytes, one finger, rather than the vendor's 14: both answer, and
  this panel is single-touch, so there is nothing beyond finger 1 to fetch.

The panel is **single-touch**, and the way the second point fails is in AGENTS.md
§7: a 14-byte (two-finger) read of the touch block is NACKed mid-transfer while an
8-byte one succeeds. Anything that needs two fingers is not available here.

Deeper notes — the panel's GRAM gap, the ASCII-only fonts, the LVGL port's
behaviour, and the fact that this project is portrait for good — live in
`AGENTS.md` §11. Read it before touching display or touch code.

## Contributing

New development happens on a branch, reviewed as a pull request; the maintainer
merges. `main` is expected to build and to pass `make ui-test`.
