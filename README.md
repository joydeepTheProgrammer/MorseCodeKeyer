> ESP32 Morse Code Keyer using native ESP-IDF APIs. Send text as Morse via LED/buzzer, or decode button taps back to text in real time.

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.0+-blue.svg)](https://docs.espressif.com/projects/esp-idf/)

---

## Table of Contents

- [Features](#features)
- [Hardware](#hardware)
- [Circuit](#circuit)
- [Getting Started](#getting-started)
- [Usage](#usage)
- [How It Works](#how-it-works)
- [API Reference](#api-reference)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## Features

- **Dual-mode operation**
  - **Send**: Type text over serial — LED flashes and buzzer beeps Morse code
  - **Receive**: Tap a button — auto-decodes to text in real time
- **Adjustable speed**: 5–40 WPM via serial command
- **Thread-safe**: ISR-to-task queue + mutex protection on shared state
- **Zero Arduino dependencies**: Pure ESP-IDF C, works on ESP32 / ESP32-S3 / ESP32-C3
- **Passive buzzer support**: PWM tone generation via LEDC peripheral

---

## Hardware

| Component | GPIO | Notes |
|-----------|------|-------|
| Push Button | **GPIO 4** | One leg to pin, other to GND. Internal pull-up enabled. |
| LED | **GPIO 2** | Built-in LED on most dev boards. External LED needs 220Ω resistor. |
| Passive Buzzer | **GPIO 5** | Piezo buzzer, + to pin, – to GND. |

### Pinout Diagram

```
<img width="1536" height="1024" alt="image" src="https://github.com/user-attachments/assets/06924828-94ae-45ab-9fd6-7ba24ec0d657" />

```

> **Active buzzer?** If your buzzer has a +/– label and makes sound when 3.3V is applied directly, it's active. Replace `tone_out()` with simple `gpio_set_level(BUZZER_PIN, 1/0)` and add `BUZZER_PIN` back into `init_gpio()` as an output.

---

## Circuit

![Circuit Diagram](docs/circuit.svg)

| Wire Color | Connection |
|------------|------------|
| 🔴 Red | GPIO 4 → Button → GND |
| 🟢 Green | GPIO 2 → LED → 220Ω → GND |
| 🟡 Yellow | GPIO 5 → Buzzer (+) → Buzzer (–) → GND |
| ⬛ Black | Common GND rail |

---

## Getting Started

### Prerequisites

- [ESP-IDF v5.0+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- ESP32 development board (DevKitC, WROOM, etc.)
- USB cable
- Push button, LED, passive buzzer, 220Ω resistor

### Build & Flash

```bash
# Clone the repo
git clone https://github.com/yourusername/dit-dah.git
cd dit-dah

# Set target (ESP32, ESP32-S3, ESP32-C3, etc.)
idf.py set-target esp32

# Build
idf.py build

# Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

> **Port reference:**
> - Linux: `/dev/ttyUSB0` or `/dev/ttyACM0`
> - macOS: `/dev/tty.usbserial-*`
> - Windows: `COM3`

---

## Usage

### Serial Commands

Type commands in the serial monitor (115200 baud) and press **Enter**:

| Command | Action |
|---------|--------|
| `HELLO WORLD` | Transmit text as Morse code (LED + buzzer) |
| `wpm=15` | Set speed to 15 WPM (range: 5–40) |
| `clear` | Clear the decoded message buffer |
| `help` | Print Morse code reference chart |

### Example Session

```
╔══════════════════════════════════════╗
║     ESP32 MORSE CODE — ESP-IDF       ║
╠══════════════════════════════════════╣
║  SEND: Type text in Serial Monitor   ║
║  READ: Tap button to input Morse     ║
╚══════════════════════════════════════╝
Commands: wpm=<5-40> | clear | help

> SOS
Sending: SOS
S=... O=--- S=...  [DONE]

> wpm=20
Speed set to 20 WPM

> help
┌────── MORSE CHART ──────┐
│ A=.-      B=-...        │
│ C=-.-.    D=-..         │
│ ...
└─────────────────────────┘
```

### Button Input (Receive Mode)

Tap the button on **GPIO 4** to input Morse:

| Input | Result |
|-------|--------|
| Short tap (< 2× dot) | **Dot** (`.`) |
| Long tap (> 2× dot) | **Dash** (`-`) |
| Pause (~3× dot) | Auto-decodes letter |
| Long pause (~7× dot) | Adds word space |

```
> .-.-.
 → R | Message: R
> .-..
 → L | Message: RL
  [WORD]
Message: RL 
```

---

## How It Works

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                         ESP32                               │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐     │
│  │  UART Task  │    │ Button Task │    │   ISR       │     │
│  │   Core 0    │    │   Core 1    │    │  GPIO 4     │     │
│  │             │    │             │    │             │     │
│  │ Reads serial│    │ Decodes     │◄───│ Edge detect │     │
│  │ → send Morse│    │ Morse queue │    │ 40ms debounce    │
│  │             │    │ → text      │    │             │     │
│  └──────┬──────┘    └──────┬──────┘    └─────────────┘     │
│         │                  │                                 │
│         ▼                  ▼                                 │
│  ┌─────────────┐    ┌─────────────┐                         │
│  │  LEDC PWM   │    │   Mutex     │                         │
│  │  GPIO 5     │    │  protects   │                         │
│  │  800 Hz     │    │ decoded_msg │                         │
│  └──────┬──────┘    └─────────────┘                         │
│         │                                                    │
│         ▼                                                    │
│  ┌─────────────┐    ┌─────────────┐                         │
│  │    LED      │    │   Buzzer    │                         │
│  │   GPIO 2    │    │   GPIO 5    │                         │
│  └─────────────┘    └─────────────┘                         │
└─────────────────────────────────────────────────────────────┘
```

### Timing (Paris Standard)

Derived from `dot_ms = 1200 / WPM`:

| Element | Multiplier | @ 10 WPM | @ 20 WPM |
|---------|-----------|----------|----------|
| Dot | 1× | 120 ms | 60 ms |
| Dash | 3× | 360 ms | 180 ms |
| Element gap | 1× | 120 ms | 60 ms |
| Letter gap | 3× | 360 ms | 180 ms |
| Word gap | 7× | 840 ms | 420 ms |

### Button Debounce

Mechanical buttons bounce for 1–20 ms. The ISR timestamps every edge and the task drops events < 40 ms apart. This handles both press and release bounce without external capacitors.

---

## API Reference

### Core Functions

```c
void set_wpm(int wpm);              // Set Morse speed (5–40)
void send_morse(const char *code);  // Transmit a Morse string (".-." etc.)
void tone_out(int duration_ms);     // LED on + buzzer tone for N ms
```

### Lookup Helpers

```c
const char* char_to_morse(char c);  // 'A' → ".-", returns NULL if unknown
char morse_to_char(const char *code); // ".-" → 'A', '?' if unknown
```

### Tasks

```c
void btn_task(void *pvParameters);  // Core 1, priority 10 — decode button taps
void uart_task(void *pvParameters); // Core 0, priority 5 — serial I/O
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Buzzer silent | Wrong buzzer type or PWM config | Check if active vs passive. Active buzzers need `gpio_set_level()`, not LEDC. |
| Button double-fires | Mechanical bounce | Add 100 nF cap across button legs, or increase debounce threshold in `btn_task()` |
| Decoded text wrong | Inconsistent tapping | Practice rhythm. Start slow: `wpm=10` |
| Build error `ledc_update_duty` | ESP-IDF version mismatch | v5.0+ uses `ledc_update_duty()`. v4.x uses `ledc_set_duty_and_update()` |
| No serial output | Wrong baud rate or port | Verify 115200 baud. Check `idf.py -p PORT monitor` |
| LED doesn't light | Wrong pin or no current limit | External LEDs need 220Ω resistor. Built-in LED on GPIO 2 works without. |
| "Failed to create queue" | Out of heap memory | Reduce `BUF_SIZE` or FreeRTOS heap size in `sdkconfig` |

---

## File Structure

```
dit-dah/
├── CMakeLists.txt          # Project-level CMake
├── main/
│   ├── CMakeLists.txt      # Component-level CMake
│   └── main.c              # All application code
├── docs/
│   └── circuit.svg         # Circuit diagram
├── README.md               # This file
└── LICENSE                 # MIT License
```

---

## Roadmap

- [ ] Wi-Fi UDP socket for remote Morse transmission
- [ ] Bluetooth LE GATT service for wireless keyer
- [ ] I2C OLED display for standalone operation (no PC)
- [ ] Non-volatile storage (NVS) for saved messages
- [ ] Morse training mode with random character drills

---

## Contributing

Pull requests welcome. For major changes, open an issue first to discuss.

1. Fork the repo
2. Create a branch: `git checkout -b feature/amazing-thing`
3. Commit: `git commit -m 'Add amazing thing'`
4. Push: `git push origin feature/amazing-thing`
5. Open a Pull Request

---

