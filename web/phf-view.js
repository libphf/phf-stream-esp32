/**
 * PHF (Pereira-Huffman Format) Web Component & Decoder
 * Author: Ygor Pereira & Antigravity (Google DeepMind)
 * License: MIT
 */

const BUILD_VERSION = "1.0.2";

const PHF_ERRORS = {
    [-1]: "invalid argument",
    [-2]: "not a PHF file",
    [-3]: "unsupported PHF version",
    [-4]: "dimension mismatch",
    [-5]: "file is truncated",
    [-6]: "stride smaller than channel count",
    [-7]: "corrupt payload",
    [-8]: "checksum mismatch (file is corrupted)",
    [-9]: "out of memory",
};
let phfModuleInstance = null;
let phfModulePromise = null;

export async function getPHFModule() {
    if (phfModuleInstance) return phfModuleInstance;
    if (!phfModulePromise) {
        phfModulePromise = new Promise((resolve, reject) => {
            const initModule = () => {
                if (typeof window.PHFModule !== 'function') {
                    return reject(new Error('PHFModule initialization failed'));
                }
                window.PHFModule({
                    locateFile: (path) => new URL(`${path}?v=${BUILD_VERSION}`, import.meta.url).href
                }).then(mod => {
                    phfModuleInstance = mod;
                    resolve(mod);
                }).catch(reject);
            };

            const script = document.createElement('script');
            script.src = new URL(`phf.js?v=${BUILD_VERSION}`, import.meta.url).href;
            script.onload = initModule;
            script.onerror = reject;
            document.head.appendChild(script);
        });
    }
    return phfModulePromise;
}

/**
 * Decodes a binary PHF array buffer into raw RGBA ImageData.
 */
export async function decodePHF(arrayBuffer) {
    const mod = await getPHFModule();
    const u8 = new Uint8Array(arrayBuffer);
    const n = u8.byteLength;
    if (n < 20) throw new Error("File too small to be a valid PHF file");

    // Validate magic 'PHF1'
    if (u8[0] !== 0x50 || u8[1] !== 0x48 || u8[2] !== 0x46 || u8[3] !== 0x31) {
        throw new Error("Invalid PHF magic identifier");
    }

    const dv = new DataView(arrayBuffer);
    const version = dv.getUint8(4);
    const channels = dv.getUint8(5);
    const flags = dv.getUint8(6);
    const width = dv.getUint32(8, true);
    const height = dv.getUint32(12, true);
    const payloadSize = dv.getUint32(16, true);

    if (version !== 1) throw new Error(`Unsupported PHF version ${version}`);
    if (channels !== 3 && channels !== 4) throw new Error(`Unsupported channel count ${channels}`);
    // Same limits as the encoder (PHF_MAX_DIM / PHF_MAX_PIXELS); also keeps
    // outBytes far below 2^32 so it cannot wrap when passed to _malloc.
    if (width === 0 || height === 0 || width > 65535 || height > 65535 || width * height > 67108864) {
        throw new Error(`Invalid PHF dimensions ${width}x${height}`);
    }

    const outBytes = width * height * 4; // Always decode to 4-channel RGBA for Canvas
    const srcPtr = mod._malloc(n);
    const dstPtr = srcPtr ? mod._malloc(outBytes) : 0;
    if (!srcPtr || !dstPtr) {
        if (srcPtr) mod._free(srcPtr);
        throw new Error("PHF decoding failed: out of memory");
    }
    mod.HEAPU8.set(u8, srcPtr);

    const t0 = performance.now();
    // Call into WASM decoder with stride = 4 (Canvas RGBA)
    const rc = mod._phf_decode_memory(srcPtr, n, dstPtr, width, height, 4);
    const decodeTimeMs = performance.now() - t0;

    if (rc !== 0) {
        mod._free(srcPtr);
        mod._free(dstPtr);
        throw new Error(`PHF decoding failed: ${PHF_ERRORS[rc] || "unknown error"} (code ${rc})`);
    }

    const decodedU8 = new Uint8Array(mod.HEAPU8.buffer, dstPtr, outBytes);
    // Copy into a standalone Uint8ClampedArray for ImageData
    const rgbaClamped = new Uint8ClampedArray(decodedU8);

    mod._free(srcPtr);
    mod._free(dstPtr);

    const imageData = new ImageData(rgbaClamped, width, height);
    return {
        width,
        height,
        channels,
        imageData,
        decodeTimeMs,
        originalSize: width * height * channels,
        compressedSize: n,
        ratio: (width * height * channels) / n
    };
}

/**
 * <phf-img src="path/to/image.phf" alt="description">
 */
export class PHFImageElement extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.canvas = document.createElement('canvas');
        this.canvas.style.maxWidth = '100%';
        this.canvas.style.height = 'auto';
        this.canvas.style.display = 'block';
        this.shadowRoot.appendChild(this.canvas);
    }

    static get observedAttributes() {
        return ['src'];
    }

    attributeChangedCallback(name, oldValue, newValue) {
        if (name === 'src' && newValue && newValue !== oldValue) {
            this.loadSrc(newValue);
        }
    }

    async loadSrc(url) {
        try {
            const resp = await fetch(url);
            if (!resp.ok) throw new Error(`HTTP error ${resp.status}`);
            const buf = await resp.arrayBuffer();
            const { width, height, imageData, decodeTimeMs, ratio } = await decodePHF(buf);
            
            this.canvas.width = width;
            this.canvas.height = height;
            const ctx = this.canvas.getContext('2d');
            ctx.putImageData(imageData, 0, 0);

            this.dispatchEvent(new CustomEvent('load', {
                detail: { width, height, decodeTimeMs, ratio }
            }));
        } catch (err) {
            console.error('[PHF]', err);
            this.dispatchEvent(new CustomEvent('error', { detail: err }));
        }
    }
}

if (!customElements.get('phf-img')) {
    customElements.define('phf-img', PHFImageElement);
}
