import { getPHFModule } from './phf-view.js?v=1.0.2';

// Protocol Packet IDs
const PKT_VIDEO = 0x01;
const PKT_JSON  = 0x03;

let ws = null;
let phfMod = null;
let wsPort = 8765;

// Video Canvas
const canvas = document.getElementById('videoCanvas');
const ctx = canvas.getContext('2d');
const loadingOverlay = document.getElementById('loadingOverlay');
const loadingText = document.getElementById('loadingText');
const connStatus = document.getElementById('connStatus');

// HUD elements
const hudFps = document.getElementById('hudFps');
const hudLatency = document.getElementById('hudLatency');
const hudBitrate = document.getElementById('hudBitrate');
const hudDecodeTime = document.getElementById('hudDecodeTime');
const hudRatio = document.getElementById('hudRatio');

// Control elements
const resSelect = document.getElementById('resSelect');
const chkFlip = document.getElementById('chkFlip');
const chkMirror = document.getElementById('chkMirror');
const chkFlash = document.getElementById('chkFlash');
const btnApplyCamera = document.getElementById('btnApplyCamera');

// Metrics tracking
let frameCounter = 0;
let bytesCounter = 0;
let lastFpsTime = performance.now();

// Persistent memory buffers for WASM zero-allocation loop
let wasmSrcPtr = 0;
let wasmSrcCapacity = 0;
let wasmDstPtr = 0;
let wasmDstCapacity = 0;
let cachedImgData = null;

function setConnectionState(state, text) {
    const dot = connStatus.querySelector('.dot');
    const label = connStatus.querySelector('.text');
    dot.className = `dot ${state}`;
    label.innerText = text;
}

// ---------------------------------------------------------------------------
// Video Pipeline (WASM SIMD128 PHF Frame Decoder)
// ---------------------------------------------------------------------------
function handleVideoPacket(u8) {
    if (!phfMod) return;

    // Packet header layout (13 bytes):
    // Offset 0: 0x01 (PKT_VIDEO)
    // Offset 1..8: capture_ts (Float64 / double LE)
    // Offset 9..12: enc_ms (Float32 LE)
    // Offset 13..end: PHF raw bytes
    const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    const captureTsSec = dv.getFloat64(1, true);
    const encMs = dv.getFloat32(9, true);

    const phfBytes = u8.subarray(13);
    const phfLen = phfBytes.byteLength;
    if (phfLen < 20) return;

    // Parse PHF header: width at offset 8, height at 12
    const width = dv.getUint32(13 + 8, true);
    const height = dv.getUint32(13 + 12, true);
    const channels = dv.getUint8(13 + 5);

    if (width === 0 || height === 0) return;

    // Adjust canvas resolution if changed
    if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
    }

    // Allocate / resize WASM input buffer
    if (wasmSrcCapacity < phfLen) {
        if (wasmSrcPtr) phfMod._free(wasmSrcPtr);
        wasmSrcCapacity = Math.max(phfLen * 2, 65536);
        wasmSrcPtr = phfMod._malloc(wasmSrcCapacity);
        if (!wasmSrcPtr) {
            wasmSrcCapacity = 0;
            console.error('[PHF] Out of WASM memory for input buffer');
            return;
        }
    }
    phfMod.HEAPU8.set(phfBytes, wasmSrcPtr);

    const outBytes = width * height * 4;
    // Allocate / resize WASM output buffer (RGBA)
    if (wasmDstCapacity < outBytes) {
        if (wasmDstPtr) phfMod._free(wasmDstPtr);
        wasmDstCapacity = Math.max(outBytes * 2, 1048576);
        wasmDstPtr = phfMod._malloc(wasmDstCapacity);
        if (!wasmDstPtr) {
            wasmDstCapacity = 0;
            console.error('[PHF] Out of WASM memory for output buffer');
            return;
        }
    }

    const t0 = performance.now();
    // Decode into RGBA with stride = 4
    const rc = phfMod._phf_decode_memory(wasmSrcPtr, phfLen, wasmDstPtr, width, height, 4);
    const decMs = performance.now() - t0;

    if (rc === 0) {
        if (!cachedImgData || cachedImgData.width !== width || cachedImgData.height !== height) {
            cachedImgData = ctx.createImageData(width, height);
        }
        cachedImgData.data.set(new Uint8Array(phfMod.HEAPU8.buffer, wasmDstPtr, outBytes));
        ctx.putImageData(cachedImgData, 0, 0);

        loadingOverlay.classList.add('hidden');

        // Latency: current wall clock vs capture wall clock
        const nowSec = Date.now() / 1000.0;
        let totalLatencyMs = (nowSec - captureTsSec) * 1000.0;
        if (totalLatencyMs < 0 || totalLatencyMs > 10000) {
            totalLatencyMs = Math.round(decMs + encMs);
        }

        frameCounter++;
        bytesCounter += phfLen;

        // Update HUD
        hudLatency.innerText = `${Math.max(0, Math.round(totalLatencyMs))} ms`;
        hudDecodeTime.innerText = `${decMs.toFixed(1)} ms`;

        const rawBytes = width * height * channels;
        const ratio = (rawBytes / phfLen).toFixed(2);
        const savings = ((1.0 - phfLen / rawBytes) * 100).toFixed(0);
        hudRatio.innerText = `${ratio}x (-${savings}%)`;
    } else {
        console.error(`[PHF] Decode failed with error code ${rc}`);
    }
}

// ---------------------------------------------------------------------------
// WebSocket Networking Hub
// ---------------------------------------------------------------------------
function connectWebSocket() {
    setConnectionState('connecting', 'Connecting...');
    const wsHost = window.location.hostname || 'localhost';
    const wsUrl = `ws://${wsHost}:${wsPort}`;

    ws = new WebSocket(wsUrl);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
        setConnectionState('connected', 'Live Stream Active');
        loadingText.innerText = 'Receiving ESP32-CAM PHF stream...';
        // Request initial status / sync settings
        sendSettings();
    };

    ws.onmessage = (event) => {
        if (typeof event.data === 'string') {
            try {
                handleJsonMessage(JSON.parse(event.data));
            } catch (e) {
                console.error(e);
            }
            return;
        }

        const u8 = new Uint8Array(event.data);
        if (u8.length === 0) return;

        const opcode = u8[0];
        console.log(`[WS] Packet received: ${u8.length} bytes, opcode: 0x0${opcode.toString(16)}`);
        if (opcode === PKT_VIDEO) {
            handleVideoPacket(u8);
        } else if (opcode === PKT_JSON) {
            try {
                const jsonText = new TextDecoder().decode(u8.subarray(1));
                handleJsonMessage(JSON.parse(jsonText));
            } catch (e) {
                console.error('Failed to parse PKT_JSON:', e);
            }
        }
    };

    ws.onclose = () => {
        setConnectionState('disconnected', 'Disconnected. Retrying...');
        loadingOverlay.classList.remove('hidden');
        loadingText.innerText = 'Reconnecting to ESP32-CAM...';
        setTimeout(connectWebSocket, 2000);
    };

    ws.onerror = () => {
        setConnectionState('disconnected', 'Connection Error');
    };
}

function handleJsonMessage(msg) {
    if (msg.type === 'status') {
        if (msg.resolution && resSelect) {
            resSelect.value = msg.resolution;
        }
        if (typeof msg.vflip === 'boolean') chkFlip.checked = msg.vflip;
        if (typeof msg.hmirror === 'boolean') chkMirror.checked = msg.hmirror;
        if (typeof msg.flash === 'boolean') chkFlash.checked = msg.flash;
    }
}

function sendSettings() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const payload = {
        action: 'set_camera',
        resolution: resSelect.value,
        vflip: chkFlip.checked,
        hmirror: chkMirror.checked,
        flash: chkFlash.checked
    };
    ws.send(JSON.stringify(payload));
}

// ---------------------------------------------------------------------------
// UI Event Handlers
// ---------------------------------------------------------------------------
btnApplyCamera.addEventListener('click', () => {
    sendSettings();
});

chkFlash.addEventListener('change', () => {
    sendSettings();
});

chkFlip.addEventListener('change', () => {
    sendSettings();
});

chkMirror.addEventListener('change', () => {
    sendSettings();
});

resSelect.addEventListener('change', () => {
    sendSettings();
});

// Periodic FPS & Bitrate calculation loop (every 1 second)
setInterval(() => {
    const now = performance.now();
    const dt = (now - lastFpsTime) / 1000.0;

    if (dt >= 1.0) {
        const fps = (frameCounter / dt).toFixed(1);
        const mbps = ((bytesCounter * 8) / (dt * 1000000)).toFixed(2);

        hudFps.innerText = fps;
        hudBitrate.innerText = `${mbps} Mbps`;

        frameCounter = 0;
        bytesCounter = 0;
        lastFpsTime = now;
    }
}, 1000);

// Initialize WASM Module and start networking
(async () => {
    try {
        loadingText.innerText = 'Initializing WebAssembly SIMD128 decoder...';
        phfMod = await getPHFModule();
        try {
            const cfg = await (await fetch('config.json', { cache: 'no-store' })).json();
            if (Number.isInteger(cfg.ws_port)) wsPort = cfg.ws_port;
        } catch (e) {
            console.warn('No server config.json, using default WebSocket port', wsPort);
        }
        connectWebSocket();
    } catch (err) {
        console.error('Failed to initialize PHF WASM Module:', err);
        loadingText.innerText = 'Error loading WebAssembly engine: ' + err.message;
    }
})();
