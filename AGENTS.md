# AGENTS.md — Waveshare ESP32-C6-Touch-LCD-1.47

Working notes for agents/humans developing against this board. Written after a
full bring-up (display + LVGL + capacitive touch verified on hardware). Facts are
marked **[verified]** (observed on this unit or read out of the vendor source) or
**[unverified]** (believed, not tested here). Do not silently promote the latter.

This file is self-contained: it is meant to be copied into any project that talks
to this board, not read in place.

---

## 1. What the board is

ESP32-C6FH8 dev board: 8 MB flash, 172x320 IPS panel, capacitive touch,
6-axis IMU, TF slot, LiPo charger.

| Peripheral | Chip / part | Interface | Pins |
|---|---|---|---|
| MCU | ESP32-C6FH8, RISC-V, 1x HP core @ 160 MHz | — | — |
| LCD | JD9853, 172x320, RGB565 | SPI (4-wire) | SCLK 1, MOSI 2, CS 14, DC 15, RST 22, BL 23 |
| Touch | AXS5106L, I2C addr **0x63** | I2C + INT/RST | SDA 18, SCL 19, RST 20, INT 21 |
| IMU | QMI8658A | I2C (shared) + INT | INT1 5, INT2 6 |
| TF card | — | SPI (shares SCLK/MOSI with LCD) | SCLK 1, MOSI 2, MISO 3, CS 4 |
| Battery ADC | `BAT_ADC`, R21 200K pull-up / R22 100K pull-down | ADC | GPIO 0 |
| USB | native USB-Serial-JTAG | USB FS | D- 12, D+ 13 |
| UART0 | `ESP_TXD`/`ESP_RXD` — **header only, not on USB-C** | UART | TX 16, RX 17 |
| Buttons | RESET, BOOT | — | `ESP_RST`, GPIO 8 |

Notes:

- `GPIO1`/`GPIO2` are shared between LCD and TF — keep the unused device's CS inactive.
- `GPIO18`/`GPIO19` are the shared onboard I2C bus (touch + IMU). Mind address
  conflicts and bus capacitance if you hang more devices off it.
- `GPIO8` is the BOOT **strapping** pin. If firmware drives it, you may be unable
  to enter download mode — the vendor workaround is to short GPIO8 to 3V3 and retry.
- `GPIO12`/`GPIO13` must not be used as GPIO while USB is in use.
- `VBAT` must never be tied to `3V3` or `VBUS`. Battery voltage = `VADC * 3`.

Docs: <https://docs.waveshare.com/ESP32-C6-Touch-LCD-1.47>
Schematic: <https://files.waveshare.com/wiki/ESP32-C6-Touch-LCD-1.47/ESP32-C6-Touch-LCD-1.47-Schematic.pdf>
Vendor demo (all examples, BSP, Arduino ports, datasheets):
<https://files.waveshare.com/wiki/ESP32-C6-Touch-LCD-1.47/ESP32-C6-Touch-LCD-1.47-Demo.zip>

---

## 2. Toolchain

Board needs **ESP-IDF >= 5.5.0** (vendor docs). Verified working with **v5.5.1**.

```bash
# one-time, if not already present
git clone -b v5.5.1 --recursive --depth 1 --shallow-submodules \
    https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32c6

# every shell
. ~/esp/esp-idf/export.sh
```

- Host Python 3.14 works fine with IDF 5.5.1 **[verified]**.
- Toolchain lands in `~/.espressif/tools/riscv32-esp-elf/`; the Python venv in
  `~/.espressif/python_env/idf5.5_py3.14_env/`.
- The vendor's `dependencies.lock` records `idf: 5.3.2`, contradicting their own
  docs. 5.5.1 is the tested combination; do not pin to 5.3.2 to "match" the lock.

Build / flash / monitor:

```bash
idf.py set-target esp32c6     # first time only
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Rebuilding the same source does **not** give a byte-identical `.bin` **[verified]**:
exactly the compile-time string, `esp_app_desc_t.app_elf_sha256` and the trailing
image digest differ (~70 bytes). Same size, same config, same code. Don't chase
that as a reproducibility bug.

---

## 3. Console and the single cable

`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` is set in `sdkconfig.defaults`. This is
deliberate: the USB-C connector is wired to the native USB-Serial-JTAG
peripheral, while UART0 is exposed only on the 22-pin header. Routing the console
to USB-Serial-JTAG means one cable does flash **and** monitor.

- Device enumerates as `303a:1001` "Espressif USB JTAG/serial debug unit".
- Port is `/dev/ttyACM0`, owned `root:uucp` — the user needs to be in `uucp`.
- `idf.py monitor` performs the reset; without `--no-reset` you get the boot log.

### Trap: charge-only USB cables

Symptom: board appears to power up (LEDs on), but `lsusb` shows no `303a:xxxx`,
there is no `/dev/ttyACM0`, and the kernel logs **no** USB event at all.

The ESP32-C6 brings up USB-Serial-JTAG from ROM, so a healthy board on a
data-capable cable enumerates *before any firmware runs*. Zero bus activity
therefore means VBUS reached the board but D+/D- did not — the cable, not the
board or the firmware. Diagnose without root:

```bash
lsusb | grep 303a                                  # absent?
paste -d: <(cat /sys/bus/usb/devices/*/idVendor) \
          <(cat /sys/bus/usb/devices/*/idProduct)  # live kernel state
journalctl -k --since "-5min" | grep -i usb        # no event == no enumeration
```

---

## 4. Reusing this BSP in a new project

The fastest correct path is to copy this whole project as a known-good template
(it builds and runs as-is), then strip what you don't want. `BSP_SRC` is the path
of this project.

```bash
BSP_SRC=<path to this project>
NEW=<path to new project>

mkdir -p "$NEW"
cp -r "$BSP_SRC"/{components,main} "$BSP_SRC"/CMakeLists.txt \
      "$BSP_SRC"/{partitions.csv,sdkconfig.defaults,dependencies.lock,AGENTS.md} "$NEW"/

cd "$NEW" && . ~/esp/esp-idf/export.sh
idf.py set-target esp32c6      # regenerates sdkconfig from sdkconfig.defaults
idf.py build
```

You now have a project that flashes and runs. `main/main.c` shows the verified
init order, and `main/ui_animation.c` is a working LVGL screen to crib from —
delete or replace it and point `main/CMakeLists.txt` at your own source.

If you only want the drivers, copy the components and write `main/` yourself:

| Component | Contents |
|---|---|
| `esp_lcd_jd9853/` | JD9853 SPI panel driver (`esp_lcd_new_panel_jd9853`) |
| `esp_lcd_touch_axs5106/` | AXS5106L I2C touch driver |
| `esp_bsp/` | `bsp_display` / `bsp_touch` / `bsp_i2c` / `bsp_spi` init glue |

`main/CMakeLists.txt` for a new app:

```cmake
idf_component_register(
    SRCS "main.c" "your_ui.c"
    INCLUDE_DIRS "."
    REQUIRES "esp_bsp" "esp_lcd" "driver" "nvs_flash"
             "espressif__esp_lvgl_port" "lvgl__lvgl")
```

Note the managed-component names are namespaced (`espressif__esp_lvgl_port`,
`lvgl__lvgl`) — that is the spelling `REQUIRES` expects. `main/` gets an implicit
dependency on everything, but being explicit is cheaper to debug.

The `esp_bsp` `CMakeLists.txt` here trims to the four modules used:

```cmake
idf_component_register(
    SRCS "bsp_display.c" "bsp_touch.c" "bsp_i2c.c" "bsp_spi.c"
    INCLUDE_DIRS "."
    REQUIRES "esp_lcd" "driver" "esp_lcd_jd9853" "esp_lcd_touch_axs5106")
```

The vendor's version instead uses `file(GLOB_RECURSE ...)` and additionally
requires `esp_adc fatfs lwip esp_wifi`. Restore those requires if you add back
`bsp_sdcard.c`, `bsp_qmi8658.c`, `bsp_battery.c`, `bsp_wifi.c` — those files are
**not** in this project; re-download the demo zip (`ESP-IDF/01_factory/components/esp_bsp/`)
to get them.

`main/idf_component.yml` — pin exactly, these are the versions validated here:

```yaml
dependencies:
  idf: ">=5.5.0"
  lvgl/lvgl: "^9.0.0"
  espressif/esp_lcd_touch: "1.1.2"
  espressif/esp_lvgl_port: "2.5.0"
```

Copy the project's `dependencies.lock` too for reproducible resolution.
`esp_lvgl_port` 2.5.0 ships separate `src/lvgl8/` and `src/lvgl9/` trees and
declares `lvgl: ">=8,<10"`, and this project is now on **LVGL 9.5.0**: the face it
renders is that. Everything below about the *board* still holds — the JD9853, the
panel mapping, the touch driver — but where a note says "LVGL 8", it is describing
the version this project *left*, and §6 and §11 say what replaced it.

### sdkconfig must-haves

```ini
CONFIG_IDF_TARGET="esp32c6"
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y          # C6 default is 2 MB < the 6 MB app partition
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
# (LVGL 9 has no CONFIG_LV_COLOR_16_SWAP: the byte order is set on the display in
#  main.c — flags.swap_bytes; see §6)
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_MEM_SIZE_KILOBYTES=64           # vendor demo uses 48
CONFIG_LV_FONT_MONTSERRAT_12=y            # enable every size you reference
CONFIG_LV_USE_ASSERT_NULL=y
CONFIG_LV_USE_ASSERT_MALLOC=y
```

`partitions.csv`:

```
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 6M,
```

With `CONFIG_LV_CONF_SKIP=y` (LVGL's own Kconfig, the vendor default and this
project's setting), LVGL is configured **only** through `CONFIG_LV_*` items —
there is no `lv_conf.h` in an ESP-IDF LVGL build.

---

## 5. Display bring-up

Init order that works (see `main/main.c`):

1. `bsp_i2c_init()` → returns `i2c_master_bus_handle_t`
2. `bsp_spi_init()` → initialises `SPI2_HOST` (SCLK 1 / MOSI 2 / MISO 3, shared
   with the TF slot)
3. `bsp_display_init(&io, &panel, 172 * draw_buf_lines)` — installs SPI panel IO
   at `EXAMPLE_LCD_PIXEL_CLOCK_HZ` (80 MHz), creates the JD9853 panel, resets,
   inits, `invert_color(true)`, `mirror(false, false)`, `disp_on_off(true)`
4. `bsp_touch_init(&touch, i2c_bus, hres, vres, rotation)`
5. `lvgl_port_init()` → `lvgl_port_add_disp()` → `lvgl_port_add_touch()`
6. `bsp_display_brightness_init()` then `bsp_display_set_brightness(0..100)`

Two things to know:

- **`bsp_display_init` does not set the GRAM gap** — the vendor comments the call
  out. You must call it yourself or the image is offset:
  `esp_lcd_panel_set_gap(panel, 34, 0)` for rotations 0/180 **[verified]**.
  Setting it before `lvgl_port_add_disp` (vendor order) or immediately after both
  work — it only has to precede the first flush.
- **Backlight starts at 0** (LEDC duty 0 at init). A good pattern is to set
  brightness only after the first frame has been flushed, so the user never sees
  uninitialised GRAM: create the UI under the lock, `vTaskDelay(~200 ms)`, then
  `bsp_display_set_brightness(100)`.

Rotation (from the vendor `03_lvgl_example/main/main.c`); `hres`/`vres` swap at
90/270. **[unverified — only rotation 0 was tested here]**

| rotation | `swap_xy` | `mirror_x` | `mirror_y` | gap | hres x vres |
|---|---|---|---|---|---|
| 0 | false | false | false | (34, 0) | 172 x 320 |
| 90 | true | true | false | (0, 34) | 320 x 172 |
| 180 | false | true | true | (34, 0) | 172 x 320 |
| 270 | true | false | true | (0, 34) | 320 x 172 |

The `swap_xy`/`mirror_*` values must match between `esp_lcd_panel_mirror()` (the
panel) and `lvgl_port_display_cfg_t.rotation` (LVGL). The rotation enum in
`main/main.c` of the vendor demo encodes both.

---

## 6. LVGL port specifics (LVGL 9)

Locking — always wrap LVGL calls made from outside the LVGL task:

```c
if (lvgl_port_lock(0)) {     /* 0 == portMAX_DELAY, waits forever */
    ui_animation_create();
    lvgl_port_unlock();
}
```

**[verified]** `lvgl_port_lock()` is a *recursive* mutex; the timeout unit is ms.

Port config used here:

```c
const lvgl_port_cfg_t port_cfg = {
    .task_priority = 4, .task_stack = 1024 * 8, .task_affinity = -1,
    .task_max_sleep_ms = 500, .timer_period_ms = 5,
};
```

Display config — `buffer_size` is in **pixels**, not bytes:

```c
const lvgl_port_display_cfg_t disp_cfg = {
    .io_handle = io, .panel_handle = panel,
    .buffer_size = 172 * 50,      /* 17.2 KB/buffer at RGB565 */
    .double_buffer = 1,
    .hres = 172, .vres = 320, .monochrome = false,
    .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
    .flags = { .buff_dma = true },
};
```

- **The panel's byte order is `flags.swap_bytes = 1`** in the display config in
  `main.c` (it is `#if`-guarded for `LVGL_VERSION_MAJOR >= 9` in
  `esp_lvgl_port_disp.h`). LVGL 8 took it from `CONFIG_LV_COLOR_16_SWAP=y` instead,
  and setting `swap_bytes` under LVGL 8 does not compile. Getting this wrong inverts
  every colour on the panel, so it is the first thing to check after a version bump.
- There is no `color_format` field under LVGL 8 either (also `#if`-guarded).

Measured performance **[verified]**: 33 fps sustained with a 172x50
double-buffered draw buffer, which is exactly the cap imposed by
`LV_DISP_DEF_REFR_PERIOD = 30 ms` (1000/30 = 33.3). The panel is not the
bottleneck at this geometry. To go faster, lower the refresh period first.

`lvgl_port_task_wake()` returns `ESP_ERR_NOT_SUPPORTED` under LVGL 8 — don't
build on it.

---

## 7. Touch

- `bsp_touch_init(&touch, i2c_bus, xmax, ymax, rotation)` adds the device at 0x63
  (400 kHz) and creates the driver. `mirror`/`swap` flags are set per rotation in
  the vendor code; rotation 0 uses `swap_xy=0, mirror_x=1, mirror_y=0` **[verified working]**.

### The vendor touch driver is sloppy — don't trust it to report faults

**[verified by source inspection]**

- `touch_axs5106_init()` is literally `return ESP_OK;` — no chip-ID read, no
  validation. **A clean boot therefore proves nothing about the touch panel.**
- `touch_axs5106_i2c_read()` does `i2c_master_transmit(...)` then
  `i2c_master_receive(...)` as two transactions, discarding the transmit result —
  a failed register-address write is masked. **Do not "fix" this into a single
  `i2c_master_transmit_receive()`**: measured on this unit, the AXS5106 answers
  the two-transaction form and *NACKs* the repeated-start form (see below). What
  the transmit result check buys is narrower than that — skip the blind receive
  when the address phase was not acknowledged.
- `touch_axs5106_i2c_write()` had its entire body commented out and **fell off
  the end of a non-void function**. It was never called, so it was inert, but it
  emitted an `-Wunused-function` warning at build time. In *this* project's copy
  it is deleted, and `touch_axs5106_i2c_read()` now uses a single
  `i2c_master_transmit_receive()` instead of a discarded transmit followed by a
  blind receive — that sequence used to wedge the QMI8658A sharing the bus
  (§11). The vendor's original is otherwise unchanged.

### Reading the controller: neither extreme works

Two failures are measured on this unit, and the design sits between them.

**Reading on every poll** — which the vendor's own *IDF* driver does — is refused
every single time. At idle, every read NACKs at the port's 50 ms rate, the
fail-streak reset below fires every ~8 attempts, and the controller does not
recover: the log becomes a wall of `I2C transaction unexpected nack detected` and
`I2C read error!`. This controller answers only when it has a report to give, and
INT is how it says so — which is also why the vendor's *Arduino* driver for this
panel is interrupt-driven.

**Reading only on the asserting edge** (the first attempt at following that
Arduino driver) loses the release, and it fails **silently**. A release that is
not announced as another report leaves the last finger-down sample in `tp->data`,
and nothing else clears it: `esp_lcd_touch_read_data()` only forwards to the
driver, while `esp_lcd_touch_get_coordinates()` *consumes* the sample by zeroing
`tp->data.points`. LVGL then holds one pointer press that never comes up — **no
click, no long press, no swipe** — with a healthy driver, a clean boot log and a
clean health signal. That is exactly how this presented: a dead-looking panel.

**And the announcement is a *pulse*.** Even a gate that reads on the asserting edge
only sees the pulses its poll happens to land on: polling the level from the port's
50 ms indev timer, **one touch in eight** reached the driver — and the one that did
worked perfectly end to end (`finger down (73,144)` → `ui: click face` → the
refresh), which is what made a missing report look like a gesture-recognition bug.
The edge was configured all along (`GPIO_INTR_NEGEDGE`, active-low), but nothing had
registered a handler, so the interrupt fired into nothing.

The driver now registers `axs5106_isr()` through
`esp_lcd_touch_register_interrupt_callback()` — the library installs the ISR
service, enables the pin and routes the edge — and `read_data()` reads when *any* of
three things says so:

| trigger | covers |
|---|---|
| the ISR flag | the normal case: every announced report, pulsed or held |
| the INT level, on a fresh assertion | a missed edge, and a line held rather than pulsed |
| the last report said a finger is down | the release, when it is not announced as a report |

If that last read finds nothing pending, the finger has stopped reporting, which *is*
the release — so the driver clears the sample and logs `finger up (no report
pending)`. It also logs the down transition, `finger down (x,y)`, with the raw
coordinates, which is how the panel's mapping is checked without eyes on the screen.
Init logs `report interrupt registered on GPIO N`, so the running image says which
mechanism it has, and a read that was announced but got no data warns
(`announced a report but did not hand it over`, at most once per 5 s) — that silence
is what made a missed touch indistinguishable from a touch that never happened.

Idle panels still cost nothing on the bus.

### The read shape, measured

A probe that tried all four shapes over a single touch settled this, and it was
worth the tap: the controller answers **two transactions** (`i2c_master_transmit`
for the register address, then `i2c_master_receive` for the data, STOP between
them) and **NACKs** a read joined to the address phase by a repeated start
(`i2c_master_transmit_receive` → `ESP_ERR_INVALID_STATE`). Both lengths worked, so
the length was never the problem:

```
read two-step 14 -> ESP_OK                 block 00 01 80 5d 00 c5 40 10
read two-step  8 -> ESP_OK                 block 00 01 80 5d 00 c5 40 10
read one-call 14 -> ESP_ERR_INVALID_STATE  (NACK)
read one-call  8 -> ESP_ERR_INVALID_STATE  (NACK)
```

That block decodes through the driver's own arithmetic to one finger at
x=93, y=197. This also explains the asymmetric symptom that cost a session: the
QMI8658A on the same bus reads happily through `i2c_master_transmit_receive`, so
the touch path looked like the odd one out rather than the repeated start being
the wrong shape for this part.

The read is **8 bytes from 0x01** (count + finger 1, registers 0x01..0x08) rather
than the vendor's 14 — both answer, and this panel is single-touch, so there is
nothing beyond finger 1 to fetch.

Useful consequence: because the port reads the driver on its own schedule
(30 ms) and logs `I2C read error!` at ERROR level on failure, **a session with
zero such lines is a live I2C health signal** — the controller is ACKing at 0x63
and returning data. Absence of errors over minutes is decent evidence the bus and
controller are alive. It does not prove the panel reports coordinates; only a
physical press does.

### One finger, not two

The driver caches the second point (`touchpad_x[1]`/`y[1]`) but on this panel it
is unreachable. **Measured** (see "The read shape, measured" above): the touch block answers both
an 14-byte and an 8-byte read, and reports `points = 1` for one finger. The
project's touch failures were a read-shape problem, not a length or a window
problem. The driver reads **8 bytes** (count + finger 1, `0x01`..`0x08`), which is
all a single-touch panel has to give.

Nothing in the vendor docs, the demo or the product page describes this panel as
multi-touch. Treat it as single-touch: one finger, and LVGL's own click /
long-press / gesture events are the whole input vocabulary (§11).

`bsp_touch_init` sets `x_max = min(xmax, ymax)` and `y_max = max(xmax, ymax)`
regardless of rotation. That is correct for portrait, and pairs with
`swap_xy=1` in landscape — but it is a hard-coded assumption that looks wrong for
other layouts. **[unverified for rotations other than 0]**

---

## 8. Traps that cost time here

1. **`bsp_display_init` omits the GRAM gap.** No gap ⇒ shifted image. See §5.
2. **Plain `lv_obj` is not a blank rectangle.** LVGL's default theme applies a
   *card* style to `lv_obj_class`: `bg_opa = COVER`, a 2 px border, `pad_all`,
   radius, and a text colour. Any decorative object must explicitly override
   `bg_opa`, `border_width` and `pad_all` or you get an unintended box.
3. **Click-transparency is opt-in.** Widgets swallow clicks by default. To have a
   click land on the screen, the child objects need
   `lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE)` (and `LV_OBJ_FLAG_SCROLLABLE`),
   and the screen needs `LV_OBJ_FLAG_CLICKABLE` plus an `LV_EVENT_CLICKED` handler.
   Verified by pixel-probing: taps over a decorative widget and over bare
   background both reached the screen.
4. **Arcs, not `lv_obj` backgrounds, are right for sweeps.** An `lv_arc` whose
   `LV_PART_MAIN` arc width is 0 draws no track (`lv_draw_arc` returns early on
   `width == 0`), so you can put a sweep on top of a plain bordered circle. Note
   `lv_arc_set_bg_angles(0, 360)` *does* mean a full circle — the guards are
   `> 360`, not `>= 360`. A `rounded` indicator grows by `width/2` at each end.
5. **The FPS log mode is LVGL 9 only.** `LV_USE_PERF_MONITOR_LOG_MODE` prints to the
   log instead of drawing an overlay; LVGL 8.4 had `LV_USE_PERF_MONITOR` but no log
   mode (`lv_refr_get_fps_avg()` came with the on-screen label).
6. **The harness must hold a scripted press across an indev read.** LVGL 9 gives
   every indev its own read timer (measured at ~60 ms here) instead of reading at
   each `lv_timer_handler()` as v8 did, so a press held for a single 30 ms tick is
   seen only by luck: `click_at()` holds for three ticks and then releases. The same
   change is why the flush's pixels are native RGB565 while `lv_color_t` is RGB888 —
   `widen565()` in `tools/ui_host_test/host_main.c` does that widening for both the
   flush and the expectations, so the two can never disagree.
7. **`UI_LOGI` prints on the host.** With no ESP-IDF it goes to stderr with the tag,
   so a failing scenario can say *why* a click did nothing — which is how the two
   LVGL 9 click bugs above were found.
8. **Bundled fonts are ASCII-only.** `lv_font_montserrat_*` cover 0x20–0x7F. A
   `·`/`°`/emoji in a label renders as a missing glyph. Measure text before
   trusting it to fit — advances are in `lv_font_montserrat_N.c` (`.adv_w` in
   1/16 px, glyph id = `codepoint - 31`).
9. **`idf_monitor` compares the device's ELF SHA256 against ELFs in the cwd.**
   Running `monitor` from the wrong directory produces a bogus
   "Checksum mismatch between flashed and built applications". Launch it from the
   project whose binary you actually flashed.
10. **Benign boot warning**, ignore it:
   `W JD9853: The 3Ah command has been used and will be overwritten by external
   initialization sequence` — the vendor init table sets 3Ah twice.
11. **Flashing erases the factory firmware.** It is recoverable from the demo zip
   (`ESP-IDF/01_factory`).

---

## 9. Verification techniques that work without eyes on the panel

You often cannot see the display. Two independent halves give real coverage.

### 9a. Render the UI on the host (content correctness)

Works when your UI file depends on **LVGL only** — no `esp_*` headers. That is
worth preserving as a design constraint.

1. Copy `lv_conf_template.h` → `lv_conf.h`, enable the `#if 1` block, set
   `LV_COLOR_DEPTH 16` (match the device), `LV_MEM_SIZE`, and enable the fonts you use.
2. Point the assert handler at a function that `abort()`s, so failures are loud
   instead of hanging (`LV_ASSERT_HANDLER_INCLUDE` takes a single header name).
3. Compile all of `lvgl/src/**/*.c` plus **your unmodified UI source** with
   `-DLV_CONF_INCLUDE_SIMPLE -I<dir with lv_conf.h> -I<lvgl root>`
   (~194 TUs, parallelise it; ~1 min).
4. Register a display whose flush callback blits into an RGB888 framebuffer,
   register a pointer indev whose `read_cb` returns scripted press/release
   positions, then loop `lv_tick_inc(30); lv_timer_handler();` while advancing a
   virtual clock.
5. Dump frames (`PPM` → PNG) and assert on **pixels**, not vibes: sample the
   animated element's centre colour to prove state changes, and take a radial
   profile to prove arcs sit on their tracks.

This caught nothing critical here, but it is what converted "looks plausible" into
"palette advanced exactly twice from two taps, sweep measured 103–106° against a
predicted 105.4°".

### 9b. Probe the device (rendering + link health)

- Enable `CONFIG_LV_USE_PERF_MONITOR=y` in a **throwaway build** (it draws an
  overlay, so don't ship it) and log
  `extern uint32_t lv_refr_get_fps_avg(void);` once a second. Expect
  `1000 / LV_DISP_DEF_REFR_PERIOD`; zero means nothing is being flushed.
- Watch for `I2C read error!` from `esp_lcd_touch_axs5106` — see §7.
- On-device FPS and host-rendered content together cover both halves; only the
  physical press and the human-visible image genuinely require a person.

Keep instrumentation in a **copy** of the project (separate build dir, separate
`sdkconfig`), so the product tree never carries debug code. Verify the copy
differs only in the intended files before building it.

---

## 10. Scope: what is *not* verified here

Only display + LVGL + touch were exercised. Untested on hardware:

- QMI8658A IMU (shared I2C; docs list onboard addresses 0x51 / 0x6B / 0x7E)
- TF card over SPI (must share SCK/MOSI with the LCD)
- Battery ADC on GPIO0 (`VBAT = VADC * 3`)
- Wi-Fi 6 / BLE 5
- Panel rotations other than 0
- Deep sleep / power management
- LVGL 8.4.0 any more: this project moved to 9.5.0, and §6/§11 describe what that
  changed. The port on the other LVGL is untested.

---

## 11. This repo: `ws-herdr-status` (desk companion for herdr agent status)

### How to work in this repo

- **Never commit to `main`.** Every new development starts on a branch and is
  offered for review as a pull request; the maintainer merges. Keep a PR focused
  on one change.
- Name branches by intent: `feat/<topic>`, `fix/<topic>`, `docs/<topic>`, and
  push (`git push -u origin <branch>`) so the PR can be opened against it.
- Before asking for review: it must build (`make fw-build`) and both host suites
  must pass (`make ui-test`, `make rotation-test`). Say in the PR what you
  verified on hardware and what you could not — a panel needs eyes, a touch
  needs a finger, a turn needs a hand.
- Never commit generated files: `sdkconfig` (it holds the WiFi password),
  `build/`, `managed_components/`, compiled test binaries, or the harness's
  `frame_*.ppm`. `.gitignore` covers the existing ones; ignore a new tool's
  artefacts in the same commit that adds the tool.
- `main` is expected to build and to pass both suites at all times.

Two halves, no USB link needed after flashing:

|Path|What it is|
|---|---|
|`bridge/herdr_status_bridge.py`|PC side, Python 3 **stdlib only**. Reads herdr over its Unix socket and the agent harness's session logs, serves `GET /state` and `GET /stats` on `0.0.0.0:8787`.|
|`main/`|Device firmware: WiFi station + HTTP poller + the LVGL blob companion.|
|`tools/ui_host_test/`|Host render harness for the UI (§9a). `make ui-test`.|

`make` targets: `bridge`, `bridge-once`, `bridge-service-install`,
`bridge-service-status`, `fw-build`, `fw-flash`, `fw-monitor`, `ui-test`.

### Bridge (`bridge/herdr_status_bridge.py`)

- herdr's socket protocol is **one request per connection** — the server closes
  after the first reply. The bridge opens a fresh `AF_UNIX` connection per poll;
  reusing one fails with `BrokenPipeError`.
- `/state` body: `{"v":1,"gen":<int>,"stale":<bool>,"agents":[{id,kind,label,status,focus}]}`,
  `gen` bumping on every successful poll, `stale` true >5 s after the last one.
  Labels are ASCII-sanitised here because the device's fonts are ASCII-only.
- `/stats` body: `{"v":1,"gen":<int>,"stale":<bool>,"sessions":<int>,
  "totals":{in,out,calls,messages,age_s},"agents":[{id,in,out,calls,messages,age_s,model}]}`.
  Rows are keyed **and ordered** like the `/state` agents, so the device can match them
  by id or by index; an agent herdr reports without a session path has no row.
  The numbers come from the agent harness's own session logs
  (`~/.omp/agent/sessions/<slug>/<ts>_<uuid>.jsonl`), located through the herdr
  snapshot's `agent_session` (`kind == "path"`) — herdr has no usage data of its own,
  and `~/.omp/stats.db` is only as fresh as the last `omp stats` run.
  `calls` counts tool executions, `messages` counts assistant turns, `in` is prompt
  tokens (including cache reads/writes, so `in + out == omp's own totalTokens`),
  `age_s` is seconds since that log was last written and `totals.age_s` is the
  youngest, `model` is the last model the session used (provider prefix stripped,
  15 chars for the device's 16-byte buffer). `--once --stats` prints one body.
- Only the record type, the tool-execution marker and
  `message.{model,usage.{input,output,cacheRead,cacheWrite}}` are read out of a session
  log: the records also hold the whole conversation and none of it is stored, logged or
  served. The logs are read **incrementally** (per-file offset plus the partial trailing
  line), never re-parsed, so a `/stats` request is served from a snapshot a background
  thread refreshes once a second (~0.6 ms to serve, ~0.03 ms per refresh pass warm).
- `--fixture bridge/fixtures/<mood>.json` serves a fixed agent list instead of
  polling — this is how each companion mood is driven on real hardware; a fixture only
  carries the `/state` fields, so `/stats` reports zero sessions in that mode.
- `bridge/herdr-status-bridge.service` is a **user** unit
  (`systemctl --user`); `make bridge-service-install` installs and enables it.

### Firmware invariants — do not break these

- `main/herdr_status_types.h` is the only header shared by the firmware and the
  host harness, so it must stay free of `esp_*` includes. Same for
  `main/ui_companion.c`: **LVGL-only**, which is what lets `make ui-test`
  compile it unmodified (§9a). Its log calls go through the `UI_LOGI` macro,
  which `main/CMakeLists.txt` turns into `ESP_LOGI` via `-DUI_USE_ESP_LOG=1`.
- Only the LVGL task touches LVGL. `ui_companion_create()` builds a 200 ms
  timer (`ui_tick`); the poll task publishes into a mutex-guarded
  `herdr_status_t` and never calls into LVGL.
- `herdr_poll` needs **8 KB** of stack (`HERDR_POLL_STACK`). Measured, not
  guessed: 4 KB overflowed the stack guard on the very first poll, inside
  `_malloc_r` reached from the cJSON/HTTP path.
- `wifi_sta.c` sets **`WIFI_PS_NONE`** deliberately. With `WIFI_PS_MIN_MODEM`
  against this AP (Fritz!Box, 2.4 GHz, HE) the device's TCP handshakes reached
  the PC but the replies were never ACKed — every poll died on a 3 s
  `select() timeout`. Power save buys nothing on a mains-powered desk toy.
- `CONFIG_HERDR_WIFI_SSID` / `_PASSWORD` / `_BRIDGE_HOST` / `_BRIDGE_PORT` /
  `_POLL_PERIOD_MS` / `_UI_MAX_AGENTS` live in `sdkconfig` (`main/Kconfig.projbuild`),
  which is gitignored — credentials must never land in `sdkconfig.defaults`.
- Display init order, the `esp_lcd_panel_set_gap(panel, 34, 0)` call and the
  200 ms-then-backlight sequence are inherited from the template's `main.c`
  (§5); keep them.

### Rotation and the IMU

- `main/imu_qmi8658.c` drives the QMI8658A on the shared I2C bus at **0x6B**
  (WHO_AM_I 0x05) with the vendor's register sequence, taken from the demo zip's
  `ESP-IDF/01_factory/components/esp_bsp/bsp_qmi8658.c`: `RESET <- 0xB0`, 10 ms,
  `CTRL1 <- 0x40` (auto-increment), `CTRL7 <- 0x03` (enable both), `CTRL2 <- 0x95`
  (±4 g, 250 Hz), `CTRL3 <- 0xD5` (±512 dps, 250 Hz). Accel `4/32768` g/LSB,
  gyro `512/32768` dps/LSB, data ready in `STATUS0 & 0x03`.
- **Orientation is detected with the gyro, not the accelerometer.** A device
  lying on a desk is turned by spinning it about the screen's normal axis, and
  gravity is blind to that motion; the gyro's component along the normal
  integrates into a turn angle, and ±55° commits a ±90° rotation
  (`main/rotation_logic.c`, host-tested by `make rotation-test`).
- The **normal axis is calibrated at runtime** from the accelerometer at rest
  (the axis reading ~1 g is the normal, its sign says which way the screen
  faces), so nothing here depends on how the IMU happens to be mounted. That
  calibration is what makes this board-independent — do not replace it with
  hard-coded axis constants. Measured on this unit: `axis=1 (Y), sign=-1`, i.e.
  the panel normal is the chip's -Y.
- The calibration is latched while the device sits in the attitude it boots in
  (a flat device puts ~1 g on the panel normal, hence the 0.8 g threshold). It
  stays valid as long as the device is only *turned about that normal* — which
  is the rotation gesture. If the device is later stood up or rolled onto a
  different face, the axis it integrates is no longer the screen normal and a
  turn would be misread; a reboot re-calibrates. Doing better needs a
  magnetometer or a fixed mounting datum, neither of which this board offers.
- `main/main.c: app_apply_rotation(deg)` is the only place hardware follows the
  orientation. Order matters: set the driver's `hor_res`/`ver_res` and call
  `lv_disp_drv_update()` **first** (that fires esp_lvgl_port's `drv_update_cb`,
  which resets the panel to its base mapping), then apply the panel table below,
  then the touch flags, then rebuild the UI on a fresh screen. Validated
  on-device by sweeping all four orientations (`rot_sweep`, see §9b).
- Panel (`esp_lcd_panel_swap_xy` / `_mirror` / `_set_gap`) and touch
  (`esp_lcd_touch_set_swap_xy/_mirror_x/_mirror_y`) values are the vendor's
  (§5, §7) verbatim: 0 → (no swap, no mirror, gap 34/0), 90 → (swap, mirror_x,
  gap 0/34), 180 → (no swap, mirror x+y, gap 34/0), 270 → (swap, mirror_y,
  gap 0/34); touch 0 → (0,1,0), 90 → (1,0,0), 180 → (0,0,1), 270 → (1,1,1).
- The chosen orientation is kept in NVS (`herdr`/`rot`, stored as quarter turns)
  so a power cycle comes back the way the device was left.
  `CONFIG_HERDR_IMU_ROTATE=n` pins portrait.
- **The IMU's I2C timeout must stay at the vendor's 1000 ms.** With a 100 ms
  timeout this board's IMU reads failed in bulk (measured: a handful of samples
  per minute, `ESP_ERR_TIMEOUT` cascading, while the *init* read succeeded) —
  aborting a transaction leaves the chip's read pointer mid-transfer and the
  next read inherits the mess. At 1000 ms the same code streams at the full poll
  rate with zero failures. The chip is also polled at 10 Hz, not 250: on this
  unit the link degrades under continuous traffic (growing timeout rates after a
  minute or two) whatever the timeout or bus speed, and the detector only needs
  a few samples per gesture. It stays at the touch controller's 400 kHz on the
  same bus — running the two at different clock rates is not worth the bus
  re-clocking.
- **The chip cannot be reset from firmware**: no reset pin, and its VDD/VDDIO go
  straight to 3V3 (schematic U3 pins 5 and 8, no load switch), so a wedged IMU
  needs a power cycle. The driver latches off after two failed revival attempts
  instead of hammering the bus, logs `QMI8658 stopped responding … unplug the
  board`, and the companion keeps running in portrait. Init failure is equally
  graceful: one warning, task exits, no rotation.
- The UI lays itself out for whatever screen it is given
  (`ui_layout_init()`): portrait stacks the agent list under the face at
  172x320, landscape puts the face on the left and the list on the right at
  320x172. `make ui-test` covers both (`land_*` scenarios drive the same
  resolution switch the device uses).

### Touch input, views and the stats source

- **LVGL owns the input path, and the vocabulary is positional.** The panel is
  single-touch (§7), so the port's own pointer indev is enough; `ui_companion.c`'s
  `screen_event_cb` reads `LV_EVENT_SINGLE_CLICKED`, `LV_EVENT_DOUBLE_CLICKED` and
  `LV_EVENT_LONG_PRESSED` and nothing else. LVGL 9 classifies single and double
  clicks itself, inside the long-press time, which is less code and less guesswork
  than the hand-rolled window this used to carry — and it means **a long press sends
  no click at all**, so a hold cannot also read as a tap and there is nothing to
  suppress. Note that **`lv_event_get_indev()` is NULL for these events**: the indev
  arrives as the event's *parameter* (`lv_event_get_param`), which is how the half is
  worked out. There is no input task, no gesture recogniser of our own, and no
  `lvgl_port_remove_touch` — that whole path existed only to read a second point
  the controller will not hand over.
- **Swipes were tried and removed.** They did the view switch and the paging and
  they were the least reliable input on this panel: a drag that LVGL reads as a
  gesture *also suppresses the click*, so a swipe that fell short did nothing at
  all, and one that ran on into the wrong object did the wrong thing. The screen
  is small enough to reach every corner, so input is a tap or a double tap in one
  of two halves:

  | input | action |
  |---|---|
  | tap, the face's half | refresh from the bridge + the mood's flourish |
  | tap, the list's half | forward a page (agent list, or the stats page) |
  | double tap, the face's half | the other view (mood ↔ stats) |
  | double tap, the list's half | the diagnostics overlay |
  | hold, anywhere | the overlay too |

- The halves are **equal halves of the long side**, and the artwork keeps its roles
  in both orientations (`ui_layout_init`): portrait puts the face in the top half
  and the list below it, landscape puts the face on the left and the list beside
  it. So "the face's half" is always the first half along the split axis.
- Two asymmetries are deliberate. The **face's tap is immediate** — it is the one
  that wants feedback, and its double (the view switch) is orthogonal, so both may
  happen on a double. The **list's tap is deferred by `PAGE_DELAY_TICKS`** (three of
  `ui_tick`'s 200 ms beats, ~600 ms) and cancelled by a double: paging is not
  orthogonal to the overlay, so paging first and undoing it flickers through a page
  nobody asked for. Anything delaying the *face* by that much would read as a laggy
  poke, which is why it is not uniform.
- **The page delay is counted in `ui_tick` beats, not in a one-shot `lv_timer`.**
  Under LVGL 9 a timer created from inside the event callback never fired (measured:
  the page simply never happened), and the delay has to outlast LVGL's own
  double-click classification anyway. Three beats is comfortably clear of it.
- The harness asserts the click rules by counting bridge polls: `0` after a long
  press, `1` after a tap or a double tap (a double is a tap plus its second click).
- The screen carries `LV_OBJ_FLAG_CLICKABLE`; the view containers above it stay
  `make_passive`d, so the press is delivered to the screen. `ui_companion_create`
  owns the `ui_tick` timer and deletes the previous one, so a rotation rebuild never
  leaves a timer running against freed widgets.
- The two views are two `lv_obj` containers, both children of the single screen,
  each at the origin so children keep the coordinates they were written with;
  switching is one hidden flag. Do not turn them into separate LVGL screens:
  main.c's rotation path auto-deletes the old screen, so a second screen's stored
  pointer would dangle.
- The diagnostics overlay is created on demand and deleted on the next long
  press; `ui_companion_create()` clears those pointers because they die with the
  screen.
- **The stats view is a two-column table**, sessions first: a header and three
  totals lines, then one row per agent (a state-coloured dot, the label with its
  token count, and the cost right-aligned), and the link/device page behind it. Each
  row's figure is its own right-aligned label, because this font is not monospaced
  and padding with spaces would not line anything up. The rows use a **10 px face**
  and the headers 12: at 12 the widest row ran 4 px into its cost, measured — and
  the harness now asserts the columns cannot collide (`assert_columns`), since LVGL
  does not clip a label and an over-long string simply overlaps its neighbour.
- **Money is integer micro-dollars** on the wire (`cost_micro`, 1e6 == $1) from the
  session's own `usage.cost.total`, summed per message like the token counts. The
  device rounds to cents *before* splitting dollars off, so $12.7459 reads $12.75
  rather than $12.74. Display strings are ASCII only: the bundled Montserrat faces
  stop at 0x7F, and an em-dash in the empty state rendered as an empty box until it
  was caught by looking at the frame.
- **Stats come from the agents' own session logs**, not from herdr: herdr exposes
  only the session *path* (`agent_session.kind == "path"`), while the omp jsonl
  carries `message.message.usage.{input,output,...}` and `usage.cost.total` in
  USD. The bridge reads *only* those numeric fields — the same files are full of
  conversation content, none of which may leave the PC — and parses them
  incrementally (remember (path, size), read only the appended bytes: ~0.1–1.7 ms
  against 116 ms for a re-parse of the largest session). There is no second
  harness on this machine; `π` is omp's own title glyph. `~/.omp/stats.db` is
  pre-aggregated but only as fresh as the last `omp stats` run, so it is not used.
- `main/ui_companion.c`, `main/touch_gesture.c` and `main/ui_stats.h` stay free of
  esp_* includes; the diagnostics getters declared in `ui_stats.h` are esp-side
  (`herdr_client.c`, `ui_rotation.c`, `ui_input.c`, `ui_device.c`) and the host
  harness stubs them, exactly as it stubs `herdr_client_get()`.

### What "working" looks like in the log

```
I wifi: connected, ip=192.168.2.<ip>
I herdr: GET http://192.168.2.203:8787/state -> 200 (227 bytes)
I herdr: gen=1235 online=1 agents=2 blocked=0 working=1 done=0 idle=1
I ui: mood=working online=1 agents=2 overflow=0
```

Zero `I2C read error!` lines over minutes is the touch-bus health signal (§7).
`app: companion running — free heap …` should report >100 KB on a healthy boot;
the poll cadence is `CONFIG_HERDR_POLL_PERIOD_MS` + one GET (~250 ms).

### The companion's animation model

Everything the face does is `lv_anim` + two `lv_timer`s, all in
`main/ui_companion.c`:

- **One animation per property per mood.** `ui_apply_mood()` deletes each
  animation before restarting its replacement, and the mood table
  (`mood_cfg_t`) hands out one period/amplitude per feature. The properties in
  play are y (bob), width+x (breathe), height+y (blink), x (eye glance),
  arc width (mouth talk), border opa (ring glow), angles (sweep rotation) and
  opa (alert). Before adding an animation, check it does not write a property
  another one owns for the same mood.
- **Phase matters.** The breathe and the bob share `bob_ms`; running them in
  phase hid the breath entirely, because the widening was cancelled by the
  vertical offset. The breathe is therefore delayed by `bob_ms/4`.
- **Mood change = burst.** `ui_start_burst()` fires two staggered rings
  (`s_ripple`) out of the face plus a short background tint in the new mood's
  colour. This is the only thing that redraws the whole screen, and only for
  ~1 s per state change. Ripples are drawn at `RIPPLE_OPA` 220 so they are
  always blended — never an exact palette colour — which keeps pixel probes
  (and `tools/ui_host_test`) unambiguous.
- **Both UI timers run at `UI_LOOK_TICK` (200 ms)** and the glance interval is a
  random countdown inside `ui_look_fire`, not a re-rolled timer period: the host
  harness identifies UI timers by period when it tears a screen down
  (`drop_ui_timers`), so a variable period would leak into the next scenario.
- **Decorations** (all in the mood table, so each mood gets its own set):
  eye glints (`COL_GLINT`, children of the eye — a glance carries them and the
  eye's `clip_corner` removes them as the lid closes, so no animation manages
  them), cheek blush (DONE/IDLE), a sweat drop (BLOCKED), and one 8-object
  particle pool that serves ambient motes (sparkles in WORKING, dust in SLEEP)
  and the one-shot DONE confetti. Filled decorations fade through **`bg_opa`**;
  the object-level `opa` style is only an all-or-nothing cutoff in LVGL 8.
- Three traps paid for in measurements here, worth not repeating: a fade ramped
  over a value-stepped animation never reaches full opacity (the mote triangle
  peaked at 44% of 255 — use a trapezoid with a flat top); `lv_rand(min, max)`
  with `max < min` wraps and puts objects off-screen (clamp a mote's rise to its
  spawn band); and an effect that starts from the face's centre spends its whole
  flight hidden behind the face (confetti launches from the rim instead).

`make ui-test` covers this: `alive` (breathe, talk, glance and ring glow each
have to change the pixels over 5 s), `burst` (background takes the mood colour
and returns, a ring crosses the strip below the face and is gone after),
`sleep_z` (both floating "z"s, dust below the face, and no glint on shut eyes),
`glint` (glints appear and are clipped away by blinks), `blush`, `sweat`,
`motes` and `party` (confetti on the DONE transition, gone afterwards).


