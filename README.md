# phf-stream-esp32 📷

Ultra-low latency, **100% bit-exact lossless** camera streaming server designed for the **AI-Thinker ESP32-CAM** (OV2640), powered by the **Pure High-Fidelity Format (PHF v1.1)**.

Streams live uncompressed RGB video from the ESP32 micro-controller to modern web browsers decoding in real-time with WebAssembly (`phf.wasm` SIMD128).

---

## 🚀 Features

- **100% Standalone & In-Chip**: Serves the entire web dashboard (HTML, CSS, JS, WASM) and the live WebSocket stream directly from the ESP32-CAM.
- **Bit-Exact Lossless Video**: Full mathematical preservation of the pixel buffer without chroma subsampling or macroblock artifacts.
- **Microcontroller-Optimized Memory Architecture**:
  - Automatically routes codec allocations to the 4MB external PSRAM (`MALLOC_CAP_SPIRAM`), preventing SRAM exhaustion.
  - Dedicated FreeRTOS task with **32 KB stack** on Core 1 for the PHF compression pipeline.
  - Lightweight main loop on Core 1 exclusively serving HTTP and WebSocket traffic.
- **Hardware Sensor Stability**:
  - 16 MHz XCLK clock rate eliminating DVP/DMA sampling scanlines.
  - Automated PWDN hardware power cycle for instant recovery across software resets.
  - Color channel correction (BGR-to-RGB conversion for OV2640 hardware decoders).
  - Sensor register calibration: AWB, AEC, AGC gain ceiling, and gamma correction.
- **Embedded WebAssembly Engine**: High-speed SIMD128 decoder (`phf.wasm`) runs client-side in the browser, rendering frames directly into an HTML5 `<canvas>`.
- **Pre-Compressed Gzip PROGMEM Assets**: Entire web stack is stored compressed in Flash memory (~39 KB total) and served with native HTTP `Content-Encoding: gzip`.
- **Live Camera Controls**: Dynamic Vertical Flip, Horizontal Mirror, Resolution toggle (QVGA 320x240 vs QQVGA 160x120), and Flash LED (GPIO 4) control.

---

## 📊 Measured Performance

Real-world benchmarks recorded on an AI-Thinker ESP32-CAM (240 MHz Xtensa LX6):

| Resolution | Frame Size (Raw RGB) | PHF Compressed Size | Encode Time (ESP32) | Client Decode Time (WASM) | Stream FPS |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **QVGA (320x240)** | 230.4 KB | ~78 - 86 KB | ~600 - 650 ms | ~1.5 - 2.5 ms | ~1.6 FPS |
| **QQVGA (160x120)** | 57.6 KB | ~20 - 23 KB | ~360 - 380 ms | ~0.5 - 0.9 ms | ~3.0 FPS |

*Note: For higher framerates (30-60 FPS at 720p/1080p), see [`phf-stream-pi`](https://github.com/libphf/phf-stream-pi) for Raspberry Pi CSI/USB, or [`phf-stream-webcam`](https://github.com/libphf/phf-stream-webcam) for PC webcams.*

---

## 📐 Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                 ESP32-CAM (AI-Thinker)                      │
│                                                             │
│   [OV2640 Sensor @ 16MHz]                                   │
│            │ (Hardware JPEG DMA into PSRAM)                 │
│            ▼                                                │
│   [fmt2rgb888 in PSRAM] ──► [BGR-to-RGB Color Swap]         │
│                                    │                        │
│                                    ▼                        │
│             [libphf C Encoder] (2D MED + N-Huffman)         │
│             (Dedicated 32KB Stack FreeRTOS Task)            │
│                                    │                        │
│                                    ▼                        │
│   [WebSocket Server :8765]  [HTTP Server :80 (Gzip Assets)] │
└──────────────────────────┬──────────────────────────────────┘
                           │ Wi-Fi LAN
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                 Web Browser (Client)                        │
│                                                             │
│                 phf.wasm (SIMD128)                          │
│                         │                                   │
│                         ▼                                   │
│                  HTML5 <canvas>                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 🛠️ Arduino IDE Setup & Compilation Guide

### 1. Requirements

- **Arduino IDE**: 1.8.19+ or 2.x
- **ESP32 Board Package**: Version 2.0.x or newer (by Espressif Systems).
  - Additional Boards Manager URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
- **Additional Library**:
  - `WebSockets` by **Markus Sattler** (install via *Sketch -> Include Library -> Manage Libraries...* by searching for `WebSockets`).

### 2. Board Configuration (Tools Menu)

Configure the following options in the Arduino IDE:

| Option | Recommended Value |
| :--- | :--- |
| **Board** | `AI Thinker ESP32-CAM` |
| **CPU Frequency** | `240MHz (WiFi/BT)` |
| **Flash Frequency** | `80MHz` |
| **Flash Mode** | `QIO` |
| **Partition Scheme** | `Huge APP (3MB No OTA / 1MB SPIFFS)` |
| **Core Debug Level** | `None` or `Info` |
| **PSRAM** | **`Enabled`** (MANDATORY) |

> [!IMPORTANT]
> The **PSRAM: Enabled** setting is mandatory, as large RGB image buffers and PHF compression tables are allocated directly in the 4MB external PSRAM.

### 3. Configure Wi-Fi Credentials

Open `phf-stream-esp32.ino` and enter your Wi-Fi credentials:
```cpp
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
```

### 4. Flashing and Uploading

1. Connect the ESP32-CAM module to your computer using an FTDI / USB-to-TTL adapter:
   - `ESP32 TX (GPIO 1)` -> `FTDI RX`
   - `ESP32 RX (GPIO 3)` -> `FTDI TX`
   - `ESP32 GND` -> `FTDI GND`
   - `ESP32 5V` -> `FTDI 5V`
   - **`GPIO 0` connected to `GND`** (to enter flash programming mode).
2. Press the **Reset** button on the ESP32-CAM board.
3. In the Arduino IDE, select the correct COM port and click **Upload**.
4. Once uploading is complete:
   - Disconnect `GPIO 0` from `GND`.
   - Open the **Serial Monitor** at **`115200 baud`**.
   - Press the **Reset** button on the ESP32-CAM board.

---

## 🌐 Accessing the Live Dashboard

1. Check the Serial Monitor output for the Wi-Fi connection message:
   ```text
   [WIFI] Connected successfully!
   [WIFI] Web Dashboard: http://192.168.15.xx/
   [WIFI] WebSocket Stream: ws://192.168.15.xx:8765
   ```
2. Open any modern web browser (Chrome, Edge, Firefox, Safari) on a computer, phone, or tablet connected to the same Wi-Fi network and navigate to:
   ```text
   http://192.168.15.xx/
   ```
3. The dashboard loads instantly from the ESP32 flash memory and begins streaming live lossless frames decoded in real time with WebAssembly!

---

## 📄 License

MIT License. Developed as part of the Pure High-Fidelity Format (`libphf`) project.
