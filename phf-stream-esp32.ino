/**
 * phf-stream-esp32 📷
 * 
 * Ultra-low latency, 100% bit-exact lossless camera streaming server
 * for the AI-Thinker ESP32-CAM (OV2640), powered by the Pure High-Fidelity Format (PHF v1.1).
 * 
 * Architecture:
 * - In-chip Web Server (Port 80) serving HTML, CSS, JS, WASM from PROGMEM (Gzip compressed).
 * - Real-time WebSocket Server (Port 8765) streaming lossless PHF frames.
 * - WebAssembly SIMD128 decoder running directly in the browser canvas.
 * - Resolution: QVGA (320x240) standard, with QQVGA (160x120) high-fps mode.
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include "web_assets.h"

#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

extern "C" {
#include "phf.h"
}

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ---------------------------------------------------------------------------
// Wi-Fi Credentials (Configure your Wi-Fi network here)
// ---------------------------------------------------------------------------
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// ---------------------------------------------------------------------------
// Network Servers
// ---------------------------------------------------------------------------
const uint16_t HTTP_PORT = 80;
const uint16_t WS_PORT   = 8765;

WebServer server(HTTP_PORT);
WebSocketsServer webSocket(WS_PORT);

// ---------------------------------------------------------------------------
// Camera & Sensor State
// ---------------------------------------------------------------------------
bool is_vflip = true;
bool is_hmirror = true;
bool is_flash_on = false;
framesize_t current_framesize = FRAMESIZE_QVGA; // 320x240 QVGA default

// Packet Protocol: 0x01 = PKT_VIDEO, 0x03 = PKT_JSON
const uint8_t PKT_VIDEO = 0x01;
const uint8_t PKT_JSON  = 0x03;

#pragma pack(push, 1)
typedef struct {
    uint8_t opcode;     // 0x01
    double  capture_ts; // seconds (monotonic wall-clock)
    float   enc_ms;     // encode duration in ms
} PhfVideoHeader;
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Flash LED Control (GPIO 4 on AI-Thinker)
// ---------------------------------------------------------------------------
void setFlashLed(bool enable) {
    is_flash_on = enable;
    pinMode(LED_GPIO_NUM, OUTPUT);
    digitalWrite(LED_GPIO_NUM, enable ? HIGH : LOW);
}

// ---------------------------------------------------------------------------
// Web Server Gzip Asset Handler
// ---------------------------------------------------------------------------
void serveGzip(const char* contentType, const uint8_t* content, size_t length) {
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "public, max-age=86400");
    server.send_P(200, contentType, (const char*)content, length);
}

void setupHttpRoutes() {
    server.on("/", HTTP_GET, []() {
        serveGzip("text/html; charset=utf-8", index_html_gz, index_html_gz_len);
    });

    server.on("/index.html", HTTP_GET, []() {
        serveGzip("text/html; charset=utf-8", index_html_gz, index_html_gz_len);
    });

    server.on("/style.css", HTTP_GET, []() {
        serveGzip("text/css", style_css_gz, style_css_gz_len);
    });

    server.on("/phf-view.js", HTTP_GET, []() {
        serveGzip("application/javascript", phf_view_js_gz, phf_view_js_gz_len);
    });

    server.on("/phf.js", HTTP_GET, []() {
        serveGzip("application/javascript", phf_js_gz, phf_js_gz_len);
    });

    server.on("/phf.wasm", HTTP_GET, []() {
        serveGzip("application/wasm", phf_wasm_gz, phf_wasm_gz_len);
    });

    server.on("/stream-client.js", HTTP_GET, []() {
        serveGzip("application/javascript", stream_client_js_gz, stream_client_js_gz_len);
    });

    server.on("/config.json", HTTP_GET, []() {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"ws_port\":%u,\"resolution\":\"320x240\",\"device\":\"ESP32-CAM\"}", WS_PORT);
        server.send(200, "application/json", buf);
    });

    server.on("/status", HTTP_GET, []() {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"uptime_ms\":%lu,\"free_psram\":%u,\"free_heap\":%u,\"wifi_rssi\":%d,\"clients\":%u}",
            millis(), ESP.getFreePsram(), ESP.getFreeHeap(), WiFi.RSSI(), webSocket.connectedClients());
        server.send(200, "application/json", buf);
    });
}

// ---------------------------------------------------------------------------
// Camera Hardware Initialization
// ---------------------------------------------------------------------------
bool initCamera() {
    // Hardware Power Cycle on OV2640 sensor via PWDN pin (GPIO 32)
    // Recovers sensor from stuck I2C/SCCB bus state after software resets
    if (PWDN_GPIO_NUM != -1) {
        pinMode(PWDN_GPIO_NUM, OUTPUT);
        digitalWrite(PWDN_GPIO_NUM, HIGH); // Power Down ON (Sensor OFF)
        delay(30);
        digitalWrite(PWDN_GPIO_NUM, LOW);  // Power Down OFF (Sensor ON)
        delay(30);
    }

    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 16000000; // 16MHz eliminates DVP/DMA sampling scanline noise

    // OV2640 hardware setup: High-quality JPEG DMA capture into PSRAM
    config.frame_size   = current_framesize;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 6; // High visual quality (0-63, lower is higher quality)
    config.fb_count     = 2; // Double buffering in PSRAM

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAMERA] Camera init failed with error 0x%x. Retrying in DRAM...\n", err);
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.fb_count    = 1;
        err = esp_camera_init(&config);
        if (err != ESP_OK) {
            Serial.printf("[CAMERA] Fatal: Camera init failed: 0x%x\n", err);
            return false;
        }
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, is_vflip ? 1 : 0);
        s->set_hmirror(s, is_hmirror ? 1 : 0);

        // Hardware Image Enhancements: Fix dark image, scanlines and color fidelity
        s->set_brightness(s, 1);     // Increase brightness (+1)
        s->set_contrast(s, 1);       // Increase contrast (+1)
        s->set_saturation(s, 1);     // Natural vivid saturation
        s->set_whitebal(s, 1);       // Auto White Balance (AWB) ON
        s->set_awb_gain(s, 1);       // Auto White Balance Gain ON
        s->set_wb_mode(s, 0);        // WB Mode Auto
        s->set_exposure_ctrl(s, 1);  // Auto Exposure (AEC) ON
        s->set_aec2(s, 1);           // DSP Auto Exposure ON
        s->set_gain_ctrl(s, 1);      // Auto Gain Control (AGC) ON
        s->set_gainceiling(s, (gainceiling_t)2); // Limit gain to eliminate sensor noise
        s->set_bpc(s, 1);            // Black pixel correction ON
        s->set_wpc(s, 1);            // White pixel correction ON
        s->set_raw_gma(s, 1);        // Gamma curve correction ON
        s->set_lenc(s, 1);           // Lens correction ON
        s->set_dcw(s, 1);            // Downsize interpolation filter ON
    }
    Serial.println("[CAMERA] Sensor OV2640 initialized and calibrated successfully.");
    return true;
}

// ---------------------------------------------------------------------------
// Send Status JSON via WebSocket
// ---------------------------------------------------------------------------
void sendWsStatus(uint8_t client_num) {
    char json[200];
    const char* resStr = (current_framesize == FRAMESIZE_QQVGA) ? "160x120" : "320x240";
    snprintf(json, sizeof(json),
        "{\"type\":\"status\",\"resolution\":\"%s\",\"vflip\":%s,\"hmirror\":%s,\"flash\":%s}",
        resStr, is_vflip ? "true" : "false", is_hmirror ? "true" : "false", is_flash_on ? "true" : "false");

    webSocket.sendTXT(client_num, json);
}

// ---------------------------------------------------------------------------
// WebSocket Event Handler
// ---------------------------------------------------------------------------
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.printf("[WS] Client [%u] disconnected\n", num);
            if (webSocket.connectedClients() == 0 && is_flash_on) {
                setFlashLed(false); // Turn off flash when no viewers
            }
            break;

        case WStype_CONNECTED: {
            IPAddress ip = webSocket.remoteIP(num);
            Serial.printf("[WS] Client [%u] connected from %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
            sendWsStatus(num);
            break;
        }

        case WStype_TEXT: {
            String text = String((char*)payload).substring(0, length);
            sensor_t *s = esp_camera_sensor_get();

            if (text.indexOf("vflip") >= 0) {
                is_vflip = (text.indexOf("\"vflip\":true") >= 0);
                if (s) s->set_vflip(s, is_vflip ? 1 : 0);
            }
            if (text.indexOf("hmirror") >= 0) {
                is_hmirror = (text.indexOf("\"hmirror\":true") >= 0);
                if (s) s->set_hmirror(s, is_hmirror ? 1 : 0);
            }
            if (text.indexOf("flash") >= 0) {
                bool flashReq = (text.indexOf("\"flash\":true") >= 0);
                setFlashLed(flashReq);
            }
            if (text.indexOf("resolution") >= 0 && s) {
                if (text.indexOf("160x120") >= 0) {
                    if (current_framesize != FRAMESIZE_QQVGA) {
                        current_framesize = FRAMESIZE_QQVGA;
                        s->set_framesize(s, FRAMESIZE_QQVGA);
                    }
                } else if (text.indexOf("320x240") >= 0) {
                    if (current_framesize != FRAMESIZE_QVGA) {
                        current_framesize = FRAMESIZE_QVGA;
                        s->set_framesize(s, FRAMESIZE_QVGA);
                    }
                }
            }
            break;
        }

        case WStype_BIN:
        case WStype_ERROR:
        case WStype_FRAGMENT_TEXT_START:
        case WStype_FRAGMENT_BIN_START:
        case WStype_FRAGMENT:
        case WStype_FRAGMENT_FIN:
        case WStype_PING:
        case WStype_PONG:
            break;
    }
}

// Forward declaration
void streamTask(void *pvParameters);

// ---------------------------------------------------------------------------
// Arduino Setup
// ---------------------------------------------------------------------------
void setup() {
    // Force Maximum CPU Performance (240 MHz)
    setCpuFrequencyMhz(240);

    // Disable brownout detector to prevent resets during Wi-Fi transmission bursts
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    Serial.println("\n\n==========================================");
    Serial.println("  PHF Live Stream Server (ESP32-CAM)     ");
    Serial.println("  Pure High-Fidelity v1.1 Lossless Codec ");
    Serial.println("==========================================");

    // Turn off Flash LED initially
    pinMode(LED_GPIO_NUM, OUTPUT);
    digitalWrite(LED_GPIO_NUM, LOW);

    // Verify PSRAM
    if (psramFound()) {
        Serial.printf("[SYSTEM] PSRAM initialized: %d KB total, %d KB free\n",
            ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024);
    } else {
        Serial.println("[SYSTEM] ERROR: PSRAM not found! ESP32-CAM requires PSRAM.");
    }

    // Camera Init
    if (!initCamera()) {
        Serial.println("[CAMERA] ERROR: Halting execution due to camera failure.");
        while (true) { delay(1000); }
    }

    // Wi-Fi Connection
    Serial.printf("[WIFI] Connecting to SSID: %s\n", ssid);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false); // Disable modem sleep now that Wi-Fi stack is active
        Serial.println("\n[WIFI] Connected successfully!");
        Serial.printf("[WIFI] Web Dashboard: http://%s/\n", WiFi.localIP().toString().c_str());
        Serial.printf("[WIFI] WebSocket Stream: ws://%s:%u\n", WiFi.localIP().toString().c_str(), WS_PORT);
    } else {
        Serial.println("\n[WIFI] Connection timed out! Check SSID/Password.");
    }

    // Setup HTTP & WebSocket Servers
    setupHttpRoutes();
    server.begin();
    Serial.printf("[HTTP] Web Server started on port %u\n", HTTP_PORT);

    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    Serial.printf("[WS] WebSocket Server listening on port %u\n", WS_PORT);

    // Launch Dedicated FreeRTOS Task for PHF Streaming with 32KB Stack
    // Prevents stack overflow canary trips from heavy image compression
    xTaskCreatePinnedToCore(
        streamTask,         // Task function
        "streamTask",       // Task name
        32768,              // Stack depth (32 KB dedicated stack)
        NULL,               // Parameters
        1,                  // Priority
        NULL,               // Task handle
        1                   // Pin to Core 1 (Application Core)
    );

    Serial.println("[SYSTEM] Ready! Open the dashboard URL in your browser.\n");
}

// ---------------------------------------------------------------------------
// Dedicated High-Stack Streaming Task (FreeRTOS)
// ---------------------------------------------------------------------------
void streamTask(void *pvParameters) {
    while (true) {
        if (webSocket.connectedClients() > 0) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (!fb) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            uint32_t width  = fb->width;
            uint32_t height = fb->height;
            size_t rgb_size = width * height * 3;

            // Allocate buffer in PSRAM for RGB888 pixel plane
            uint8_t *rgb888 = (uint8_t*)heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!rgb888) {
                esp_camera_fb_return(fb);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // Convert camera frame buffer to RGB888
            bool converted = fmt2rgb888(fb->buf, fb->len, fb->format, rgb888);
            esp_camera_fb_return(fb); // Release hardware frame buffer immediately

            if (!converted) {
                free(rgb888);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // Fix BGR -> RGB: fmt2rgb888 outputs BGR on OV2640 JPEG
            // Swap Blue (byte 0) and Red (byte 2) so colors are 100% accurate
            size_t num_pixels = width * height;
            for (size_t i = 0; i < num_pixels; i++) {
                size_t idx = i * 3;
                uint8_t temp = rgb888[idx + 0];
                rgb888[idx + 0] = rgb888[idx + 2];
                rgb888[idx + 2] = temp;
            }

            // Encode with Pure High-Fidelity (PHF v1.1) in fast streaming mode
            phf_encode_options_t opts;
            opts.mode   = PHF_MODE_MED;     // 2D MED spatial gradient prediction
            opts.effort = PHF_EFFORT_FAST;  // Single-pass ultra-low latency encode

            size_t phf_size = 0;
            uint32_t t_enc_start = millis();
            uint8_t *phf_data = phf_encode_memory(rgb888, width, height, 3, &opts, &phf_size);
            float enc_duration_ms = (float)(millis() - t_enc_start);

            // rgb888 buffer is no longer needed
            free(rgb888);

            if (phf_data && phf_size > 0) {
                // Pack packet: 13-byte header + PHF file payload
                size_t packet_size = sizeof(PhfVideoHeader) + phf_size;
                uint8_t *packet = (uint8_t*)heap_caps_malloc(packet_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

                if (packet) {
                    PhfVideoHeader *hdr = (PhfVideoHeader*)packet;
                    hdr->opcode     = PKT_VIDEO;
                    hdr->capture_ts = (double)millis() / 1000.0;
                    hdr->enc_ms     = enc_duration_ms;

                    memcpy(packet + sizeof(PhfVideoHeader), phf_data, phf_size);

                    // Broadcast binary packet to all connected web clients
                    bool sent = webSocket.broadcastBIN(packet, packet_size);
                    Serial.printf("[FRAME %ux%u] PHF: %u B (%.1f ms) -> WS: %s\n",
                        width, height, (unsigned int)phf_size, enc_duration_ms, sent ? "OK" : "FAIL");
                    free(packet);
                }

                phf_free(phf_data);
            } else {
                Serial.println("[PHF] Encoding returned NULL");
            }

            vTaskDelay(pdMS_TO_TICKS(1)); // Yield briefly to FreeRTOS scheduler
        } else {
            vTaskDelay(pdMS_TO_TICKS(50)); // No viewers connected: idle sleep 50ms
        }
    }
}

// ---------------------------------------------------------------------------
// Arduino Main Loop (Lightweight & Super Responsive HTTP + WebSocket loop)
// ---------------------------------------------------------------------------
void loop() {
    webSocket.loop();
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(1));
}
