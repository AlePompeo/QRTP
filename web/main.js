// Browser-side half of the ILink boundary (see src/wasm_link.cpp's JsLink for the C++ half).
// Owns everything the C++ side has no access to: camera capture, QR canvas rendering, file
// picking/downloading. Talks to the WASM module only through `window.qrtpBridge` (called from
// C++) and the embind-exposed `Module.WebSession`/`Module.decodeQrRgba` (called from here).
import createQrtpModule from "./qrtp_web.js";

const $ = (id) => document.getElementById(id);

let Module = null;
// A second, independent WASM instance dedicated to decodeQrRgba(). Asyncify only supports one
// suspended call in flight per module instance at a time — Module itself spends the whole
// transfer suspended inside runSend()/runReceive() (awaiting waitForFrame), so decoding every
// captured camera frame on that same instance from the rAF loop would collide with it. Decoding
// is a stateless, independent operation, so a dedicated instance sidesteps the conflict entirely
// rather than trying to time-share one.
let DecodeModule = null;
let session = null;
let mediaStream = null;
let currentCameraDeviceId; // tracks which device startCamera() last opened, so selecting a
                           // different one in the dropdown actually switches the stream
let decodeLoopHandle = null;

// De-dup state for waitForFrame(), mirroring OpticalLink::try_receive()'s own "only return a
// decode that differs from the last one we returned" behavior (see optical_link.cpp) — otherwise
// the same still-on-screen QR would be handed to the protocol layer over and over.
let lastSeenText = "";
let lastReturnedText = "";
let pendingWaiter = null; // { resolve, timer } | null

// EXPERIMENTAL color-multiplex mode (see src/wasm_link.cpp's JsLink): up to 3 independent QR
// codes in the R/G/B channels of one image instead of one QR per frame. Needs its own per-channel
// de-dup state -- a channel repeating its content must not mask a *different* channel that
// changed in the same tick, so this can't reuse the single lastSeenText/lastReturnedText pair
// above.
let colorMultiplexActive = false;
const colorLastSeen = ["", "", ""];
const colorLastReturned = ["", "", ""];
let colorPendingWaiter = null; // { resolve, timer } | null

// EXPERIMENTAL JAB Code mode (see src/wasm_link.cpp's JsLink -- jabcode_): a different barcode
// symbology from QR (colored modules, own LDPC error correction), mutually exclusive with the
// color-multiplex mode above (see the change-listener wiring in main() below). Decoded frames are
// raw bytes, not text -- de-dup compares byte arrays instead of strings, so this can't reuse
// lastSeenText/lastReturnedText either.
let jabcodeActive = false;
let jabLastSeen = null;
let jabLastReturned = null;
let jabPendingWaiter = null; // { resolve, timer } | null

function bytesEqual(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

function log(msg) {
  const el = $("logArea");
  el.value += msg + "\n";
  const kLogMaxChars = 20000; // same order of magnitude as gui_win32.cpp/gui_gtk.cpp's own cap
  if (el.value.length > kLogMaxChars) el.value = el.value.slice(el.value.length - kLogMaxChars);
  el.scrollTop = el.scrollHeight;
}

function offerDecoded(text) {
  lastSeenText = text;
  if (text && text !== lastReturnedText && pendingWaiter) {
    clearTimeout(pendingWaiter.timer);
    lastReturnedText = text;
    const resolve = pendingWaiter.resolve;
    pendingWaiter = null;
    resolve(text);
  }
}

// Collects every channel whose current value differs from what was last returned -- not just
// `channel`, so a waitForFrameColor() call picks up several channels that changed in the same
// tick instead of needing to poll repeatedly to catch them one at a time.
function collectNewColorChannels() {
  const newly = [];
  for (let c = 0; c < 3; c++) {
    if (colorLastSeen[c] && colorLastSeen[c] !== colorLastReturned[c]) {
      newly.push(colorLastSeen[c]);
      colorLastReturned[c] = colorLastSeen[c];
    }
  }
  return newly;
}

function offerDecodedColor(channel, text) {
  colorLastSeen[channel] = text;
  if (!colorPendingWaiter) return;
  const newly = collectNewColorChannels();
  if (newly.length === 0) return;
  clearTimeout(colorPendingWaiter.timer);
  const resolve = colorPendingWaiter.resolve;
  colorPendingWaiter = null;
  resolve(newly);
}

function offerDecodedJab(bytes) {
  jabLastSeen = bytes;
  if (bytes.length && !bytesEqual(bytes, jabLastReturned) && jabPendingWaiter) {
    clearTimeout(jabPendingWaiter.timer);
    jabLastReturned = bytes;
    const resolve = jabPendingWaiter.resolve;
    jabPendingWaiter = null;
    resolve(bytes);
  }
}

// Module.qrtpBridge: the JS-side implementation JsLink (src/wasm_link.cpp) calls into, via
// emscripten::val::module_property("qrtpBridge") -- read off this WASM instance's own Module
// object rather than a page-global, assigned once Module exists (see main() below).
const qrtpBridge = {
  renderQr(width, modules) {
    const canvas = $("qrCanvas");
    const target = Math.floor(Math.min(window.innerWidth, 700) * 0.9);
    if (canvas.width !== target) { canvas.width = target; canvas.height = target; }
    const ctx = canvas.getContext("2d");
    const quiet = 4; // same quiet-zone convention as optical_link.cpp's send()
    const side = width + 2 * quiet;
    const scale = Math.max(1, Math.floor(canvas.width / side));
    const offset = Math.floor((canvas.width - side * scale) / 2);
    ctx.fillStyle = "#fff";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = "#000";
    for (let y = 0; y < width; y++) {
      for (let x = 0; x < width; x++) {
        if (modules[y * width + x]) {
          ctx.fillRect(offset + (x + quiet) * scale, offset + (y + quiet) * scale, scale, scale);
        }
      }
    }
  },

  waitForFrame(timeoutMs) {
    return new Promise((resolve) => {
      if (lastSeenText && lastSeenText !== lastReturnedText) {
        lastReturnedText = lastSeenText;
        resolve(lastSeenText);
        return;
      }
      const timer = setTimeout(() => {
        pendingWaiter = null;
        resolve("");
      }, timeoutMs);
      pendingWaiter = { resolve, timer };
    });
  },

  // EXPERIMENTAL color-multiplex counterpart to renderQr(): `channels` is an array of up to 3
  // {width, modules} entries (R, G, B, in that fixed order; a missing/shorter one just leaves that
  // color channel blank in its region). Builds the RGBA pixel buffer directly instead of using
  // fillRect -- fillRect can't set just one color channel per pixel while leaving the others
  // alone, which is exactly what overlaying 3 independent QR codes needs.
  renderQrColor(channels) {
    const canvas = $("qrCanvas");
    const target = Math.floor(Math.min(window.innerWidth, 700) * 0.9);
    if (canvas.width !== target) { canvas.width = target; canvas.height = target; }
    const ctx = canvas.getContext("2d");
    const quiet = 4;

    const present = channels.filter((c) => c && c.width > 0);
    const maxWidth = present.length ? Math.max(...present.map((c) => c.width)) : 1;
    const side = maxWidth + 2 * quiet;
    const scale = Math.max(1, Math.floor(canvas.width / side));
    const pixSide = side * scale;
    const gridOffset = Math.floor((canvas.width - pixSide) / 2);

    const imgData = ctx.createImageData(canvas.width, canvas.height);
    imgData.data.fill(255); // white background, full alpha (byte 3/7/11/... also 255, correct as-is)

    for (let ch = 0; ch < 3; ch++) {
      const info = channels[ch];
      if (!info || !info.width) continue;
      const w = info.width;
      const modules = info.modules;
      // Centers a channel that happens to be a smaller QR version than the widest one in this
      // batch (AIMD could in principle have shifted between the 3 send() calls that filled it)
      // within the shared grid, so all 3 channels still align pixel-for-pixel on screen.
      const chOffset = Math.floor((maxWidth - w) / 2);
      for (let y = 0; y < w; y++) {
        for (let x = 0; x < w; x++) {
          if (!modules[y * w + x]) continue; // light module -> leave this channel at 255 here
          const px0 = gridOffset + (chOffset + x + quiet) * scale;
          const py0 = gridOffset + (chOffset + y + quiet) * scale;
          for (let dy = 0; dy < scale; dy++) {
            for (let dx = 0; dx < scale; dx++) {
              const idx = ((py0 + dy) * canvas.width + (px0 + dx)) * 4;
              imgData.data[idx + ch] = 0; // dark module -> this channel's byte goes to 0
            }
          }
        }
      }
    }
    ctx.putImageData(imgData, 0, 0);
  },

  waitForFrameColor(timeoutMs) {
    return new Promise((resolve) => {
      const newly = collectNewColorChannels();
      if (newly.length > 0) {
        resolve(newly);
        return;
      }
      const timer = setTimeout(() => {
        colorPendingWaiter = null;
        resolve([]);
      }, timeoutMs);
      colorPendingWaiter = { resolve, timer };
    });
  },

  // EXPERIMENTAL JAB Code counterpart to renderQr()/renderQrColor(): `pixelsRgba` is already a
  // ready-to-display RGBA buffer (the JAB Code library rasterizes its own bitmap, unlike QR's
  // module grid) -- a single putImageData at native resolution, no manual module-to-pixel scaling
  // loop needed.
  renderJabCode(width, height, pixelsRgba) {
    const canvas = $("qrCanvas");
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    const ctx = canvas.getContext("2d");
    const imgData = new ImageData(new Uint8ClampedArray(pixelsRgba), width, height);
    ctx.putImageData(imgData, 0, 0);
  },

  waitForFrameJabCode(timeoutMs) {
    return new Promise((resolve) => {
      if (jabLastSeen && jabLastSeen.length && !bytesEqual(jabLastSeen, jabLastReturned)) {
        jabLastReturned = jabLastSeen;
        resolve(jabLastSeen);
        return;
      }
      const timer = setTimeout(() => {
        jabPendingWaiter = null;
        resolve(new Uint8Array(0));
      }, timeoutMs);
      jabPendingWaiter = { resolve, timer };
    });
  },

  sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
  },

  setFps(fps) { /* informational only today; fps pacing itself lives in JsLink::send()'s sleep() call */ },
  setEcLevel(_level) { /* EC level only affects encoding (qr_encode_bytes), nothing to do here */ },

  onLog(msg) { log(msg); },
};

async function populateCameraList() {
  const devices = await navigator.mediaDevices.enumerateDevices();
  const cams = devices.filter((d) => d.kind === "videoinput");
  const sel = $("camSelect");
  sel.innerHTML = "";
  for (const cam of cams) {
    const opt = document.createElement("option");
    opt.value = cam.deviceId;
    opt.textContent = cam.label || `Camera ${sel.length + 1}`;
    sel.appendChild(opt);
  }
  // Rebuilding the <select> above resets it to the first option regardless of what's actually
  // streaming -- restore it to the device startCamera() actually opened, so the dropdown doesn't
  // visually disagree with the live camera feed after a switch.
  if (currentCameraDeviceId) sel.value = currentCameraDeviceId;
}

// Optical/hardware zoom via the MediaTrackConstraints "zoom" capability (part of the Image
// Capture / extended Media Capture spec) -- deliberately NOT a digital/CSS zoom (crop+scale on
// the canvas), which was explicitly what this feature is meant to avoid: a real optical zoom step
// keeps the camera's own full sensor resolution feeding the decoder instead of throwing away
// pixels by stretching a crop, and on phones with multiple lenses, the OS/browser can use it to
// switch to a longer lens automatically past a given zoom threshold (device-dependent, outside
// what a web page can otherwise choose directly beyond picking a specific deviceId from
// enumerateDevices() when separate lenses do show up as separate devices).
// Currently only well-supported in Chromium-based browsers (mainly Android Chrome) -- feature-
// detected via getCapabilities().zoom, not assumed, per this project's own "verify, don't assume
// cross-browser support" discipline (see PROGRESS.md/not_working.md throughout).
function setupZoomControl(track) {
  const control = $("zoomControl");
  const unsupportedHint = $("zoomUnsupportedHint");
  const slider = $("zoomSlider");
  const valueLabel = $("zoomValue");

  const capabilities = track.getCapabilities ? track.getCapabilities() : {};
  if (!capabilities.zoom) {
    control.classList.add("hidden");
    unsupportedHint.classList.remove("hidden");
    return;
  }
  unsupportedHint.classList.add("hidden");
  control.classList.remove("hidden");
  slider.min = capabilities.zoom.min;
  slider.max = capabilities.zoom.max;
  slider.step = capabilities.zoom.step || 0.1;
  const settings = track.getSettings ? track.getSettings() : {};
  slider.value = settings.zoom ?? capabilities.zoom.min;
  valueLabel.textContent = `${slider.value}x`;

  slider.oninput = async () => {
    const zoom = parseFloat(slider.value);
    valueLabel.textContent = `${zoom}x`;
    try {
      await track.applyConstraints({ advanced: [{ zoom }] });
    } catch (e) {
      log("[camera] impossibile applicare lo zoom: " + e.message);
    }
  };
}

async function startCamera(deviceId) {
  if (mediaStream && deviceId === currentCameraDeviceId) return;
  if (mediaStream) stopCamera(); // switching to a different camera: tear down the old stream first
  currentCameraDeviceId = deviceId;
  mediaStream = await navigator.mediaDevices.getUserMedia({
    video: deviceId ? { deviceId: { exact: deviceId } } : true,
    audio: false,
  });
  const video = $("camVideo");
  video.srcObject = mediaStream;
  await video.play();
  setupZoomControl(mediaStream.getVideoTracks()[0]);

  const capCanvas = $("captureCanvas");
  capCanvas.width = video.videoWidth || 640;
  capCanvas.height = video.videoHeight || 480;
  const capCtx = capCanvas.getContext("2d", { willReadFrequently: true });

  async function tick() {
    // The whole body is wrapped in try/catch (not just try/finally): decodeQrRgba can throw
    // (e.g. ZXing::ImageView rejects a 0-width/height frame, which can legitimately happen for a
    // stray tick or two right as the camera stream starts) and this function recurses into the
    // next requestAnimationFrame() call at the very end of its body -- if an exception escaped
    // uncaught past this point, that reschedule would never run, silently and permanently killing
    // the whole camera polling loop for the rest of the page's life with no visible error at all.
    try {
      if (video.readyState >= 2) {
        capCtx.drawImage(video, 0, 0, capCanvas.width, capCanvas.height);
        const frame = capCtx.getImageData(0, 0, capCanvas.width, capCanvas.height);
        // Allocate/free/decode all on DecodeModule -- ptr is only valid in the linear memory of
        // whichever instance allocated it, so mixing Module._malloc with DecodeModule.decodeQrRgba
        // here would read garbage (or an out-of-bounds pointer) in a completely separate memory.
        const ptr = DecodeModule._malloc(frame.data.length);
        DecodeModule.HEAPU8.set(frame.data, ptr);
        try {
          // decodeQrRgba/decodeQrChannelsRgba/decodeJabcodeRgba are plain synchronous C++
          // functions, but Asyncify wraps every embind-bound function to return a Promise (it
          // can't know ahead of time which calls will actually suspend) -- so even these
          // non-suspending calls must be awaited.
          if (jabcodeActive) {
            const bytes = await DecodeModule.decodeJabcodeRgba(ptr, capCanvas.width, capCanvas.height);
            if (bytes && bytes.length) offerDecodedJab(bytes);
          } else if (colorMultiplexActive) {
            const texts = await DecodeModule.decodeQrChannelsRgba(ptr, capCanvas.width, capCanvas.height);
            for (let c = 0; c < 3; c++) if (texts[c]) offerDecodedColor(c, texts[c]);
          } else {
            const text = await DecodeModule.decodeQrRgba(ptr, capCanvas.width, capCanvas.height);
            if (text) offerDecoded(text);
          }
        } finally {
          DecodeModule._free(ptr);
        }
      }
    } catch (e) {
      log("[decode] errore ignorato su questo frame: " + e.message);
    }
    decodeLoopHandle = requestAnimationFrame(tick);
  }
  tick();
}

function stopCamera() {
  if (decodeLoopHandle) cancelAnimationFrame(decodeLoopHandle);
  decodeLoopHandle = null;
  if (mediaStream) {
    for (const track of mediaStream.getTracks()) track.stop();
    mediaStream = null;
  }
}

function selectedRole() { return document.querySelector('input[name="role"]:checked').value; }
function selectedMode() { return document.querySelector('input[name="mode"]:checked').value; }

function updateModeUI() {
  const isBroadcast = selectedMode() === "broadcast";
  $("bidirSection").querySelectorAll("input, select").forEach((el) => (el.disabled = isBroadcast));
  $("broadcastSection").querySelectorAll("input, select").forEach((el) => (el.disabled = !isBroadcast));
  $("fileSection").classList.toggle("hidden", selectedRole() !== "send");
}

async function onStart() {
  const role = selectedRole();
  const mode = selectedMode();
  const needsCamera = !(mode === "broadcast" && role === "send");

  $("startBtn").disabled = true;
  $("stopBtn").disabled = false;
  $("downloadArea").classList.add("hidden");
  $("statusLabel").textContent = "In corso...";

  if (needsCamera && !mediaStream) {
    try {
      await startCamera($("camSelect").value);
    } catch (e) {
      log("[camera] errore: " + e.message);
      $("statusLabel").textContent = "Errore fotocamera.";
      $("startBtn").disabled = false;
      $("stopBtn").disabled = true;
      return;
    }
  }

  session = new Module.WebSession();
  const ecLevel = parseInt($("ecLevelSelect").value, 10);

  // EXPERIMENTAL color-multiplex opt-in (see src/wasm_link.cpp's JsLink) -- reset the per-channel
  // de-dup state at the start of every transfer so a stale value from a previous run (or from the
  // classic single-QR path) can never leak in as a false "already seen" match.
  colorMultiplexActive = $("colorMultiplexCheck").checked;
  colorLastSeen.fill("");
  colorLastReturned.fill("");

  // EXPERIMENTAL JAB Code opt-in -- same reset-per-transfer discipline as the color-multiplex
  // state above, so a stale value from a previous run can never leak in as a false match.
  jabcodeActive = $("jabCodeCheck").checked;
  jabLastSeen = null;
  jabLastReturned = null;

  try {
    if (mode === "bidir" && role === "send") {
      const file = $("fileInput").files[0];
      if (!file) throw new Error("nessun file selezionato");
      const buf = new Uint8Array(await file.arrayBuffer());
      // Il nome scritto qui è quello che Sender::send_file() (protocol.cpp) legge via
      // path.filename().string() e trasmette nell'header -- usare il nome reale del file
      // scelto (invece di un percorso fisso tipo "/input_file") è ciò che permette al
      // ricevitore di scaricare il file con la sua estensione originale.
      const memPath = "/" + file.name;
      Module.FS.writeFile(memPath, buf);
      const fps = parseFloat($("fpsInput").value);
      const chunkMax = parseInt($("chunkMaxInput").value, 10);
      const result = await session.runSend(memPath, fps, chunkMax, ecLevel, colorMultiplexActive, jabcodeActive);
      log("[send] " + (result.success ? "completato: " : "fallito: ") + result.message);
      $("statusLabel").textContent = result.success ? "Invio completato." : "Invio fallito.";
    } else if (mode === "bidir" && role === "receive") {
      const fps = parseFloat($("fpsInput").value);
      const result = await session.runReceive(fps, 30, colorMultiplexActive, jabcodeActive);
      handleReceiveResult(result);
    } else if (mode === "broadcast" && role === "send") {
      const file = $("fileInput").files[0];
      if (!file) throw new Error("nessun file selezionato");
      const buf = new Uint8Array(await file.arrayBuffer());
      // Vedi commento nel ramo bidirezionale sopra: nome reale invece del percorso fisso.
      const memPath = "/" + file.name;
      Module.FS.writeFile(memPath, buf);
      const chunk = parseInt($("broadcastChunkInput").value, 10);
      const loops = parseInt($("loopsInput").value, 10);
      const broadcastFps = parseFloat($("broadcastFpsInput").value);
      const result = await session.runBroadcastSend(
        memPath, broadcastFps, chunk, ecLevel, loops, colorMultiplexActive, jabcodeActive);
      log("[broadcast-send] " + (result.success ? "completato: " : "fallito: ") + result.message);
      $("statusLabel").textContent = result.success ? "Invio completato." : "Invio fallito.";
    } else {
      const broadcastFps = parseFloat($("broadcastFpsInput").value);
      const result = await session.runBroadcastReceive(broadcastFps, 30, colorMultiplexActive, jabcodeActive);
      handleReceiveResult(result);
    }
  } catch (e) {
    log("[error] " + e.message);
    $("statusLabel").textContent = "Errore.";
  } finally {
    session.delete();
    session = null;
    $("startBtn").disabled = false;
    $("stopBtn").disabled = true;
  }
}

function handleReceiveResult(result) {
  if (result.success) {
    const blob = new Blob([result.data], { type: "application/octet-stream" });
    const url = URL.createObjectURL(blob);
    const link = $("downloadLink");
    link.href = url;
    link.download = result.filename || "received_file";
    link.textContent = `scarica ${result.filename || "received_file"}`;
    $("downloadArea").classList.remove("hidden");
    $("statusLabel").textContent = "Ricezione completata.";
    log("[receive] completato: " + (result.filename || ""));
  } else {
    $("statusLabel").textContent = "Ricezione fallita/annullata.";
    log("[receive] fallito o annullato" + (result.message ? ": " + result.message : ""));
  }
}

function onStop() {
  if (session) session.requestCancel();
  $("stopBtn").disabled = true;
}

async function main() {
  [Module, DecodeModule] = await Promise.all([createQrtpModule(), createQrtpModule()]);
  Module.qrtpBridge = qrtpBridge;
  log("[init] modulo WASM caricato");

  document.querySelectorAll('input[name="role"], input[name="mode"]').forEach((el) =>
    el.addEventListener("change", updateModeUI)
  );
  updateModeUI();

  // The two EXPERIMENTAL encoding opt-ins are alternative symbologies, not cumulative -- checking
  // one deselects the other. Additive wiring only: neither checkbox itself is touched/replaced.
  $("colorMultiplexCheck").addEventListener("change", () => {
    if ($("colorMultiplexCheck").checked) $("jabCodeCheck").checked = false;
  });
  $("jabCodeCheck").addEventListener("change", () => {
    if ($("jabCodeCheck").checked) $("colorMultiplexCheck").checked = false;
  });

  $("camPermBtn").addEventListener("click", async () => {
    try {
      // Read whatever's currently selected in the dropdown -- without this, every click always
      // requested the browser's default camera (deviceId=undefined), and since startCamera()'s
      // own "already on this device, no-op" guard compares against currentCameraDeviceId (also
      // undefined after that first call), re-clicking after picking a different camera did
      // nothing at all: stuck on whatever camera was granted first.
      await startCamera($("camSelect").value);
      await populateCameraList();
      log("[camera] accesso concesso");
    } catch (e) {
      log("[camera] permesso negato: " + e.message);
    }
  });

  $("startBtn").addEventListener("click", onStart);
  $("stopBtn").addEventListener("click", onStop);
  window.addEventListener("beforeunload", stopCamera);
}

main();
