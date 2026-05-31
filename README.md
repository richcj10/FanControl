# FanControl

ESP32-based 5-channel PWM fan controller with a browser dashboard, MQTT integration, and closed-loop RPM control — built on the **EMC2305** fan controller IC and the **WT32-ETH01** (wired Ethernet) board.

---

## Features

- **5-channel fan control** via EMC2305 over I2C
- **Direct PWM mode** — set duty cycle 0–100% per fan
- **Closed-loop RPM mode** — EMC2305 hardware PID holds a target RPM
- **Ramp-rate control** — limits PWM slew rate to protect fan bearings
- **Live web dashboard** — dark-theme UI served from LittleFS over Ethernet
  - Animated SVG RPM gauges (colour shifts cyan → orange → red as RPM climbs)
  - Per-fan PWM bar, mode toggle, alarm badges
  - MQTT credentials panel
  - Per-fan RPM alarm threshold configuration
- **MQTT publisher** — fan status JSON every 5 s; alarm state changes published immediately with retained flag (Home Assistant ready)
- **NVS-persisted config** — MQTT host/port/credentials and alarm thresholds survive reboots
- **Doxygen-documented** source

---

## Hardware

| Component | Detail |
|-----------|--------|
| MCU board | WT32-ETH01 (ESP32 + LAN8720 PHY) |
| Fan controller | EMC2305 (5-channel PWM + tach) |
| Interface | I2C — SDA GPIO33, SCL GPIO32 |
| EMC2305 I2C address | 0x2C |
| Ethernet clock | 50 MHz oscillator → GPIO0 (`ETH_CLOCK_GPIO0_IN`) |
| Ethernet PHY address | 1 |
| Fans | 4-wire PWM, up to 5 channels |

### Wiring

```
WT32-ETH01          EMC2305
──────────          ───────
GPIO33  ──── SDA
GPIO32  ──── SCL
3.3V    ──── VCC
GND     ──── GND
```

---

## Software Stack

| Layer | Library |
|-------|---------|
| Web server | mathieucarbou/ESPAsyncWebServer |
| TCP async | mathieucarbou/AsyncTCP |
| MQTT | knolleary/PubSubClient |
| JSON | bblanchon/ArduinoJson v7 |
| Filesystem | LittleFS (ESP32 Arduino core) |
| Config store | ESP32 Preferences (NVS) |

---

## Project Structure

```
FanControl/
├── platformio.ini          # PlatformIO build config (board, libs, LittleFS)
├── src/
│   ├── main.cpp            # Setup, ETH events, main loop
│   ├── emc2305.h/.cpp      # EMC2305 I2C driver
│   ├── config.h/.cpp       # AppConfig + FanState structs, NVS persistence
│   ├── web_server.h/.cpp   # AsyncWebServer routes + REST API
│   └── mqtt_handler.h/.cpp # PubSubClient MQTT publisher
└── data/                   # LittleFS web UI assets
    ├── index.html
    ├── style.css
    └── app.js
```

---

## Build & Flash

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- USB connection to the WT32-ETH01

### Steps

```bash
# 1. Clone
git clone https://github.com/richcj10/FanControl.git
cd FanControl

# 2. Build and flash firmware
pio run --target upload

# 3. Upload web UI assets to LittleFS
pio run --target uploadfs
```

> **Note:** The filesystem upload (`uploadfs`) must be done at least once. If the web UI shows a blank page, re-run the uploadfs step.

---

## REST API

All endpoints return `application/json`.

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/status` | Current IP, RPM, PWM, mode, and alarm flags for all 5 fans |
| `GET` | `/api/config` | MQTT settings and per-fan alarm thresholds |
| `POST` | `/api/fan/pwm` | Set PWM — body: `{"fan":1, "pwm":128}` |
| `POST` | `/api/fan/mode` | Set mode — body: `{"fan":1, "mode":"pwm"}` or `"rpm"` |
| `POST` | `/api/fan/target` | Set RPM target — body: `{"fan":1, "rpm":1200}` |
| `POST` | `/api/config/mqtt` | Save MQTT credentials to NVS |
| `POST` | `/api/config/alarms` | Save per-fan RPM alarm thresholds to NVS |

---

## MQTT Topics

All topics are prefixed with the configured **topic prefix** (default: `fancontrol`).

| Topic | Payload | Retained |
|-------|---------|----------|
| `fancontrol/fanN/status` | JSON: rpm, pwm, closedLoop, alarm flags | No |
| `fancontrol/fanN/alarm/stall` | `1` or `0` | Yes |
| `fancontrol/fanN/alarm/spin_fail` | `1` or `0` | Yes |
| `fancontrol/fanN/alarm/drive_fail` | `1` or `0` | Yes |

### Example status payload

```json
{
  "rpm": 1450,
  "pwm": 180,
  "closedLoop": false,
  "stall": false,
  "spinFail": false,
  "driveFail": false,
  "rpmLow": false,
  "rpmHigh": false
}
```

### Home Assistant MQTT sensor example

```yaml
mqtt:
  sensor:
    - name: "Fan 1 RPM"
      state_topic: "fancontrol/fan1/status"
      value_template: "{{ value_json.rpm }}"
      unit_of_measurement: "RPM"

  binary_sensor:
    - name: "Fan 1 Stall"
      state_topic: "fancontrol/fan1/alarm/stall"
      payload_on: "1"
      payload_off: "0"
```

---

## Configuration

On first boot, factory defaults are applied and saved to NVS:

| Setting | Default |
|---------|---------|
| MQTT Host | `192.168.1.1` |
| MQTT Port | `1883` |
| MQTT User | *(empty)* |
| MQTT Password | *(empty)* |
| Topic Prefix | `fancontrol` |
| Alarm thresholds | disabled (0) |

All settings are configurable via the web dashboard at `http://<device-ip>/`.

---

## Generating Docs

```bash
doxygen -g          # create default Doxyfile
# Set INPUT = src and EXTRACT_ALL = YES in Doxyfile
doxygen             # outputs HTML docs to docs/html/
```
