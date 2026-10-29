# phf-stream-esp32 📷

Ultra-low latency, **100% bit-exact lossless** camera streaming server designed for the **AI-Thinker ESP32-CAM** (OV2640), powered by the **Pure High-Fidelity Format (PHF v1.1)**.

Streams live uncompressed RGB video from the ESP32 micro-controller to modern web browsers decoding in real-time with WebAssembly (`phf.wasm` SIMD128).

---

## 🚀 Features

- **100% Standalone & In-Chip ("Totalmente Contido")**: Serves the entire web dashboard (HTML, CSS, JS, WASM) and the live WebSocket stream directly from the ESP32-CAM.
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

*Note: For higher framerates (30-60 FPS at 720p/1080p), see [`phf-stream-pi`](../phf-stream-pi/) for Raspberry Pi CSI/USB, or [`phf-stream-webcam`](../phf-stream-webcam/) for PC webcams.*

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

- **Arduino IDE**: 1.8.19+ ou 2.x
- **ESP32 Board Package**: Versão 2.0.x ou mais recente (by Espressif Systems).
  - URL do Gerenciador de Placas: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
- **Biblioteca Adicional**:
  - `WebSockets` por **Markus Sattler** (instale via *Sketch -> Incluir Biblioteca -> Gerenciar Bibliotecas...* buscando por `WebSockets`).

### 2. Configurações da Placa (Menu Ferramentas / Tools)

Configure as seguintes opções no Arduino IDE:

| Opção | Valor Recomendado |
| :--- | :--- |
| **Placa (Board)** | `AI Thinker ESP32-CAM` |
| **CPU Frequency** | `240MHz (WiFi/BT)` |
| **Flash Frequency** | `80MHz` |
| **Flash Mode** | `QIO` |
| **Partition Scheme** | `Huge APP (3MB No OTA / 1MB SPIFFS)` |
| **Core Debug Level** | `None` ou `Info` |
| **PSRAM** | **`Enabled`** (OBRIGATÓRIO) |

### 3. Configurar Credenciais Wi-Fi

No arquivo `phf-stream-esp32.ino`:
```cpp
const char* ssid     = "SUA_REDE_WIFI";
const char* password = "SUA_SENHA_WIFI";
```

### 4. Compilar e Gravar (Upload)

1. Conecte o módulo ESP32-CAM ao computador via adaptador FTDI / USB-TTL:
   - `ESP32 TX (GPIO 1)` -> `FTDI RX`
   - `ESP32 RX (GPIO 3)` -> `FTDI TX`
   - `ESP32 GND` -> `FTDI GND`
   - `ESP32 5V` -> `FTDI 5V`
   - **`GPIO 0` conectado ao `GND`** (para entrar no modo de gravação/flash).
2. Pressione o botão **Reset** no ESP32-CAM.
3. No Arduino IDE, selecione a porta COM e clique em **Carregar (Upload)**.
4. Ao término da gravação:
   - Desconecte o `GPIO 0` do `GND`.
   - Abra o **Monitor Serial** em **`115200 baud`**.
   - Pressione o botão **Reset** no ESP32-CAM.

---

## 🌐 Como Acessar a Interface

1. No Monitor Serial, aguarde a conexão Wi-Fi:
   ```text
   [WIFI] Connected successfully!
   [WIFI] Web Dashboard: http://192.168.15.xx/
   [WIFI] WebSocket Stream: ws://192.168.15.xx:8765
   ```
2. Abra qualquer navegador moderno (Chrome, Edge, Firefox, Safari) no computador, celular ou tablet na mesma rede e acesse:
   ```text
   http://192.168.15.xx/
   ```

---

## 📄 Licença

MIT License. Desenvolvido no projeto Pure High-Fidelity Format (libphf).
