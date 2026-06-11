# Raspberry-Pi-Pico-W-Zambretti-Weather-Station


An advanced, internet-connected weather station built on the Raspberry Pi Pico W. The system fetches indoor and outdoor environmental data from a ThingSpeak channel, retrieves real-time wind direction from the Open-Meteo API, and displays live measurements alongside a mathematically optimized 1-hour meteorological forecast on an SSD1306 OLED display.

---

## Hardware Setup & Wiring

The system uses an **SSD1306 128x64 OLED display** communicating via the **I2C protocol**. By default, the Arduino wire library on the Raspberry Pi Pico W initializes the default `I2C0` bus.

Connect your OLED display to the Pico W using the following pinout configuration:

| OLED Pin | Pico W Pin Name | Pico W Physical Pin | Description |
| :--- | :--- | :--- | :--- |
| **VCC** | 3V3(OUT) | Pin 36 | 3.3V Power Supply |
| **GND** | GND | Pin 38 (or any GND) | Ground |
| **SDA** | GP4 | Pin 6 | I2C Data Line |
| **SCL** | GP5 | Pin 7 | I2C Clock Line |

---

## Key Improvements over Standard Implementations

Compared to basic implementations, this software introduces several robust architectural upgrades:

* **Real-time 1-Minute Intervals:** The blocking hourly delay has been eliminated. The system cycles through memory every minute, ensuring sudden weather fronts are captured instantly.
* **Fencepost (Off-by-One) Error Correction:** History arrays are expanded to 61 and 11 elements respectively. This guarantees that the delta between the first and last array indexes represents exactly 60 minutes and 10 minutes of elapsed time.
* **Moving Average Noise Filtering:** Uses a 3-minute rolling average for barometric pressure to eliminate sensor noise and sudden micro-fluctuations caused by wind gusts.
* **Dynamic Trend Indicators:** Arrows (`^`, `v`, `-`) utilize isolated, meteorologically realistic thresholds ($0.10\text{ hPa}$ for pressure, $0.20\text{ °C}$ for temperature) over a precise 10-minute window.
* **Modernized JSON Parsing:** Upgraded to the official ArduinoJson v7 standard, replacing deprecated dynamic documents with memory-safe, auto-scaling `JsonDocument` architecture.

---

## Mathematical Calculations & Forecast Logic

The core prediction algorithm runs on three distinct rolling time horizons using localized physics adjustments.

### 1. 3-Minute Moving Average (Noise Reduction)
Every minute, the raw pressure sensor data is passed through a low-pass arithmetic filter:
`pressureMA[60] = (pressureHistory[60] + pressureHistory[59] + pressureHistory[58]) / 3`

### 2. 5-Minute Barometric Crash (Emergency Storm Warning)
The system continuously monitors the short-term velocity of pressure drops:
`shortTrend = pressureMA[60] - pressureMA[55]`
* **Trigger:** If `shortTrend <= -0.7 hPa`, a rapid decompression event is recognized (typical of severe thunderstorms or supercells). The system immediately overrides the standard forecast to trigger a flashing **STORM WARNING**.

### 3. 1-Hour Forecast Algorithm
The baseline pressure trend is calculated across a 1-hour window:
`raw_trend = pressureMA[60] - pressureMA[0]`

The `raw_trend` is then modified by real-time external environmental parameters:
1. **Wind Direction Modifier (`wind_mod`):** Southerly winds (S, SW, SE) intrinsically bring low-pressure front systems in the region, whereas Easterly/Western flows are more stable.
   * Southerly flow: `trend = raw_trend - 0.8 hPa`
   * Lateral flow: `trend = raw_trend - 0.4 hPa`
2. **Seasonal Offset Factor (`seasonalFactor`):** Due to the thermal differences of air mass density, summer pressure drops are shallow but intense, while winter shifts are heavy and broad. The algorithm applies a `-0.3 hPa` offset in summer (increasing sensitivity) and `+0.3 hPa` in winter.

#### Decision Matrix
Based on the finalized corrected `trend` and absolute pressure (`p`), the system assigns the forecast text:
* `trend <= -1.5 + seasonalFactor` $\rightarrow$ **STORMY RAIN** (if $p < 1005\text{ hPa}$) or **RAIN/WEATHER**
* `trend <= -0.6` $\rightarrow$ **BAD WEATHER**
* `trend >= 1.2 + seasonalFactor` $\rightarrow$ **SUNNY/CLEAR**
* `trend >= 0.5` $\rightarrow$ **SLOW IMPROV.**
* *Stagnant trends fallback to absolute values:*
  * $p \geq 1020\text{ hPa}$ $\rightarrow$ **STABLE SUNNY**
  * $p \geq 1013\text{ hPa}$ $\rightarrow$ **SUNNY/DRY** (Summer) / **CLOUDY/DRY** (Winter)
  * $p \geq 1005\text{ hPa}$ $\rightarrow$ **CLOUDY/STAB.**
  * $p < 1005\text{ hPa}$ $\rightarrow$ **LOW/CLOUDY**

---

## Execution Flow & Lifecycle

The program bypasses blocking `delay()` loops in the main cycle, utilizing asynchronous `millis()` time-stamping to handle multiple hardware events simultaneously:
