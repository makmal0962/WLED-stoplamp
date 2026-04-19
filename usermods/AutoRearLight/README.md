# AutoRearLight

A WLED usermod for motorcycle rear lighting control.  
Drives turn signals, hazard detection, brake flash, and tail/idle state switching — all as a real-time overlay on top of any WLED effect.

| | |
|---|---|
| **Author** | MikaTsuki |
| **Created** | 29 March 2026 |
| **Last Updated** | 19 April 2026 |
| **Usermod ID** | 200 |
| **WLED** | 0.15+ |
| **Platforms** | ESP8266 · ESP32 · RP2040 |

---

## Features

- Turn signal wipe animation — left/right arrows reveal and hide column by column, from the outer edge inward
- Hazard detection — simultaneous left+right signals are classified as hazard (hard blink, no wipe)
- Brake flash — full-strip red flash, independent of turn signal state
- Tail/Idle state — headlamp input switches between two presets and caps global brightness to 50% when active
- 1D and 2D support — works on both linear strips and LED matrices
- Custom pixel art patterns — load your own turn/hazard bitmaps from a LittleFS text file
- Fully configurable — all colors, timing, pin assignments, and 1D lengths exposed in WLED UI

---

## Installation

### 1. Add files

Place the usermod folder inside WLED's `usermods/` directory:

```
WLED/
└── usermods/
    └── AutoRearLight/
        ├── usermod_autoRearLight.cpp
        ├── library.json
        └── README.md
```

### 2. Enable in `platformio_override.ini`

Add `AutoRearLight` to `custom_usermods` in your build environment:

```ini
[env:d1_mini]
extends = env:d1_mini
custom_usermods = ${env:d1_mini.custom_usermods} AutoRearLight
```

For ESP32:

```ini
[env:esp32dev]
extends = env:esp32dev
custom_usermods = ${env:esp32dev.custom_usermods} AutoRearLight
```

### 3. Build and flash

```
pio run -e d1_mini --target upload
```

---

## Hardware

Four digital inputs are read directly from the motorcycle's relay outputs.  
All inputs are **active-HIGH**. Use a voltage divider or optocoupler if your signals exceed 3.3V.

| Signal | Description |
|--------|-------------|
| Headlamp | HIGH when headlamp / ignition is on |
| Brake | HIGH when brake lever or pedal is pressed |
| Left Turn | HIGH during left blink phase |
| Right Turn | HIGH during right blink phase |

### Default Pin Assignments

| Platform | Headlamp | Brake | Left | Right |
|----------|----------|-------|------|-------|
| ESP32 | 18 | 19 | 21 | 22 |
| RP2040 | 10 | 11 | 12 | 13 |
| ESP8266 | 14 | 5 | 12 | 13 |

All pins are overridable in **WLED UI → Config → Usermods → AutoRearLight → Hardware Pins**.

---

## Configuration

All settings are available under **WLED UI → Config → Usermods → AutoRearLight**.

### Overlay Settings

| Key | Default | Description |
|-----|---------|-------------|
| Overlay Brightness | 255 | Master brightness for all overlay colors. Automatically capped at 128 (50%) in Tail state. |
| Keep Turns Animation When Signals Blink OFF | false | If true, turn overlay stays visible during the blink-off phase |
| Exit Style, 0: Rev, 1: Fwd, 2: OFF | 1 | Wipe-out animation: `0` = reverse shrink inward, `1` = forward push outward, `2` = hard blank |
| Background R/G/B | 40/0/0 | Background color behind overlay when any signal is active (default: dark red) |
| Turn R/G/B | 255/165/0 | Turn signal color (default: amber) |
| Hazard R/G/B | 255/165/0 | Hazard color (default: amber) |

### 1D Strip Settings

Only applies when `matrixHeight == 1` (standard linear strip).

| Key | Default | Description |
|-----|---------|-------------|
| Turn Signal Length (pixels) | 8 | Pixels lit per side for turn signal wipe |
| Hazard Length (pixels) | 8 | Pixels lit for hazard blink, centered on the strip |

> Values are clamped to total strip length automatically.

### Preset IDs

| Key | Default | Description |
|-----|---------|-------------|
| Idle Preset | 1 | WLED preset applied when headlamp is OFF |
| Tail Preset | 2 | WLED preset applied when headlamp is ON |

### Timing

| Key | Default | Description |
|-----|---------|-------------|
| Pins Debounce (ms) | 50 | Debounce window for all input pins |
| Signal Return Delay (ms) | 500 | Delay after signals go LOW before clearing signal state |
| Hazard Detection (ms) | 50 | Window to classify simultaneous left+right as hazard |
| Brake Flash Speed (ms) | 25 | Brake flash toggle interval |
| Wipe Speed (ms) | 10 | Wipe animation step interval (ms per column/pixel) |

### Pattern File

| Key | Default | Description |
|-----|---------|-------------|
| Pattern File | /autoRearLight.txt | Path to custom pattern file on LittleFS |

---

## Custom Pattern File

Upload a `.txt` file via WLED's file manager (`/edit`) to replace the built-in arrow patterns.

### Format

```
# left
00001010
00010100
00101000
01010000
10100000
01010000
00101000
00010100

# right
01010000
00101000
00010100
00001010
00000101
00001010
00010100
00101000

# hazard
0000000110000000
0000000110000000
0000000110000000
0000000110000000
0000000000000000
0000000110000000
0000000110000000
0000000000000000
```

**Rules:**
- Section headers must contain `left`, `right`, or `hazard` (case-insensitive), prefixed with `#`
- `1` = pixel on, anything else = off
- Optional dimension lines (`8x8`, `16x8`) are skipped automatically
- Max 32 rows × 64 columns per pattern
- File is loaded at startup and reloaded only when the filename changes in config

> If the file is missing or unreadable, the usermod falls back to the built-in PROGMEM patterns silently.

---

## Behavior Reference

### State Machine

```
Headlamp LOW  →  IDLE  →  Full brightness, Idle preset applied
Headlamp HIGH →  TAIL  →  Brightness capped at 50%, Tail preset applied
```

### Signal Classification

On rising edge of either turn signal, a `hazardDetectMs` window opens:

```
left && right  →  SIG_HAZARD  →  hard blink, no wipe
left only      →  SIG_LEFT    →  wipe from right edge inward
right only     →  SIG_RIGHT   →  wipe from left edge inward
neither        →  SIG_NONE    →  overlay inactive
```

Hazard-off clears only after both signals stay LOW longer than `hazardDetectMs`,  
to handle relay contacts that drop at slightly different times.

### Wipe Exit Styles

| Mode | Behavior |
|------|----------|
| 0 — Reverse | Pattern shrinks inward from the outer edge back to center |
| 1 — Forward | Pattern continues and pushes outward, disappearing off the far edge |
| 2 — Hard blank | Pattern disappears immediately |

### Priority

```
Brake flash  >  Turn/Hazard overlay  >  Background fill  >  Base WLED effect
```

---

## Live Debug (Info Tab)

| Field | Values |
|-------|--------|
| State | `Idle`/`Tail`, `HEAD`, `BRAKE`, signal state (`None`/`Left`/`Right`/`Hazard`) |
| Wipe | Current column/pixel, animation state (`Idle`/`In`/`Out`) |
| Matrix | Width × Height (Height = 1 means 1D mode is active) |
| Patterns | Loaded file path, or `defaults` if using built-in patterns |

---

## Known Behavior Notes

- **Centering with odd sizes:** When `(matrixWidth − patternW)` or `(total − hazardLen)` is odd, integer division shifts the pattern one pixel left/up. Not visible in practice.
- **Signal change mid-animation:** If signal type changes while a wipe is running, the animation resets cleanly to a fresh wipe-in.
- **Brake + turn simultaneously:** Brake flash takes full priority over turn overlay.
- **1D hazard:** Hard blink only, no wipe. Use `Hazard Length (pixels)` to control size.
- **Brightness cap:** The 50% cap in Tail state applies to both the global `bri` value and all overlay colors via `overlayBrightness`. If `overlayBrightness` is already below 128, it is not raised.
