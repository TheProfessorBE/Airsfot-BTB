# Bomb The Bus — Airsoft Game Mode

Three iBeacon props are placed on a bus. The attacking team plants all three;
the bomb arms and counts down. The defending team must hold two defusal buttons
simultaneously for a set duration to defuse it before detonation.

---

## Devices

| Device | Sketch | Role |
|---|---|---|
| Central ESP32 | `CentralESP32/CentralESP32.ino` | Game logic, LEDs, audio, button inputs |
| Defusal Device ESP32 | `DefusalDevice/DefusalDevice.ino` | LCD status display, single hold-button (optional, BLE-based) |
| Prop 1–3 | iBeacon / separate ESP32 | Advertise as `BTB01`, `BTB02`, `BTB03` |

The central unit is the only one with audio and LEDs. The defusal device is optional — the two pushbuttons wired directly to the central are the primary defusal mechanism.

---

## Central ESP32 — Wiring

### DFPlayer Mini

```
ESP32 (WROOM)   GPIO    DFPlayer Mini
──────────────────────────────────────
D17             17   ───[1kΩ]──  RX
D16             16   ──────────  TX
D25             25   ──────────  BUSY
GND                  ──────────  GND
5V                   ──────────  VCC

Speaker output:
  SPK1 ──┬── Speaker (+)
  SPK2 ──┴── Speaker (−)   8Ω / up to 3W

Line output (optional, to external amp):
  Mono:   DAC_R ──[1kΩ]── Amp input (+)
          GND   ────────── Amp input (−)

  Stereo: DAC_R ──[1kΩ]── Line R
          DAC_L ──[1kΩ]── Line L
          GND   ────────── Line GND

  GND can be taken from any GND pin on the DFPlayer, ESP32, or
  shared power supply — they are all on the same rail.
  The amp must share this same ground; a separate ground will cause hum.

  All audio clips in this project are mono — DAC_R and DAC_L
  carry the same signal, so the mono connection loses nothing.
```

> The 1 kΩ resistor on DFPlayer RX is recommended to protect the UART input.
> The 1 kΩ resistors on DAC_R/L are optional but protect against impedance mismatches.

### WS2812B LED Strip

```
ESP32 (WROOM)   GPIO    LED Strip
──────────────────────────────────
D13             13   ──[300Ω]──  DIN (data in)
5V                   ──────────  +5V
GND                  ──────────  GND
```

> For strips longer than ~30 LEDs, power the strip directly from the 5V supply
> rather than through the ESP32. Add a 300–500 Ω resistor in series on the data
> line, and a 100–1000 µF capacitor across +5V and GND at the strip connector.

### Defusal Buttons (×2)

```
ESP32 (WROOM)   GPIO    Button A        Button B
──────────────────────────────────────────────────
D26             26   ──  Pin 1          —
D27             27   ──  —              Pin 1
GND                  ──  Pin 2          Pin 2
```

Internal pull-ups are enabled in firmware — no external resistors needed.
Both buttons must be held simultaneously for the defuse hold duration.

### Full Pin Summary

| Label | GPIO | Function |
|---|---|---|
| D13 | 13 | WS2812B data via 300 Ω |
| D16 | 16 | DFPlayer TX → ESP32 RX (Serial1) |
| D17 | 17 | DFPlayer RX ← ESP32 TX (Serial1) via 1 kΩ |
| D25 | 25 | DFPlayer BUSY (LOW while playing) |
| D26 | 26 | Defusal button A (active LOW) |
| D27 | 27 | Defusal button B (active LOW) |

---

## Defusal Device ESP32 — Wiring

This is an optional secondary device. It scans for the central's BLE advertisement
and shows the current game state on an I2C LCD.

### I2C LCD (20×4 or 16×4, PCF8574 controller)

```
ESP32 (WROOM)   GPIO    LCD Module
────────────────────────────────────
D21             21   ──  SDA
D22             22   ──  SCL
3.3V                 ──  VCC   (or 5V — check your module)
GND                  ──  GND
```

Default I2C address: `0x27`. If the display stays blank, try `0x3F`
(set `LCD_ADDR` in the sketch).

### Defusal Button

```
ESP32 (WROOM)   GPIO    Button
────────────────────────────────
D0 (BOOT)       0    ──  Pin 1   (or any free GPIO)
GND                  ──  Pin 2
```

---

## SD Card Setup

The DFPlayer reads from the `/MP3/` folder on a FAT32-formatted SD card.

- Format as **FAT32** (not exFAT — the DFPlayer does not support it)
- Copy all `.mp3` files into a folder named `MP3` in the root
- Files are named `0001.mp3` through `0059.mp3`

The audio library has 29 clips across 59 files (some clips have 3 random variants).
See `2026 - AudioLibrary/manifest.txt` for the full list.

---

## Game Flow

```
WAITING ──── all 3 props present for arming delay ──── ARMING
   ^                                                       |
   |                                                  arming delay
   |                                                       |
   |                                               ARMED (countdown)
   |                                                /          \
   |                               both buttons held        countdown expires
   |                                       |                      |
   |                                   DEFUSING              DETONATED
   |                                  /       \           (attacking team wins)
   |                          held long       released
   |                           enough          early
   |                              |               |
   |                           DEFUSED         ARMED (cancelled)
   +────────── reset (R over UART or 8×button) ─────────────────+
```

### State Descriptions

| State | LEDs | Audio |
|---|---|---|
| WAITING | Zones pulse — lit if prop present, dim if absent | Clip plays when each prop arrives |
| ARMING | All zones fade in over arming delay | — |
| ARMED | Red chase, accelerates as time runs out | Charge % at each 10% milestone; "will detonate" at 95% |
| DEFUSING | Cyan fill bar grows left to right | "Bomb is being defused" on entry; "Defusal attempt cancelled" on abort |
| DEFUSED | Solid green | "Bomb has been defused — defending team wins" |
| DETONATED | Red strobe → dim red | "Bomb has detonated — attacking team wins" |

---

## Settings Menu (UART)

Connect at **115200 baud**. Set line ending to **Newline** in your terminal.

| Key | Action |
|---|---|
| `R` | Reset game |
| `M` | Open settings menu (only works in WAITING state) |

Menu is only accessible in WAITING state to prevent accidental access during a game.
Settings are saved to ESP32 NVS flash — they survive power cycles and reflashing
(unless you erase flash before upload).

### Available Settings

| # | Setting | Default | Range |
|---|---|---|---|
| 1 | Arming delay | 5 s | 1–300 s |
| 2 | Countdown | 300 s | 10–3600 s |
| 3 | Defuse hold | 30 s | 1–300 s |
| 4 | Audio volume | 25 | 0–30 |
| 5 | LED brightness | 80 | 0–255 |
| 6 | RSSI threshold | −60 dBm | −100–0 |
| 7 | LED count | 50 | 1–512 |

Use `S` to save, `X` to exit without saving.

---

## Reset (Hardware)

Press either defusal button **8 times within 3 seconds** to reset the game from
any state. Useful for field resets without a laptop.

---

## BLE Protocol

All devices advertise continuously. The central also scans for props and the defusal device.

### Manufacturer Data Format (company ID `0xABCD`)

```
Byte 0–1: 0xCD 0xAB   company ID (little-endian)
Byte 2:   device type  0x01 = central  0x02 = defusal device
Byte 3:   state byte
```

**Central state byte** (matches `State` enum):

| Value | State |
|---|---|
| 0 | WAITING |
| 1 | ARMING |
| 2 | ARMED |
| 3 | DEFUSING |
| 4 | DEFUSED |
| 5 | DETONATED |

**Defusal device state byte:**

| Value | State |
|---|---|
| 0x00 | Idle |
| 0x01 | Button held |
| 0x02 | Defused |

### Prop Detection

Props advertise with BLE device names `BTB01`, `BTB02`, `BTB03`. The central
uses EMA-smoothed RSSI (α = 0.2) and a configurable threshold (default −60 dBm)
to determine presence. A prop is considered gone after 2 seconds without a packet.

---

## Libraries

Install via Arduino Library Manager:

| Library | Author |
|---|---|
| FastLED | Daniel Garcia |
| DFRobotDFPlayerMini | DFRobot |
| LiquidCrystal I2C | Frank de Brabander |
| ESP32 BLE Arduino | Built-in with ESP32 board package |
| Preferences | Built-in with ESP32 board package |

Board: **ESP32 Dev Module** (or ESP32 WROOM-30). Install via Boards Manager:
`https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
