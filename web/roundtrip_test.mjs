// Node-only, hardware-free sanity check for the WASM build — same role LoopbackLink plays for
// the native protocol tests (tests/test_main.cpp): validates the whole chain (Sender/Receiver
// running unmodified over a JsLink, qr_encode_bytes via libqrencode-for-WASM, decode via
// zxing-cpp-for-WASM, Asyncify suspending/resuming through it all) without a real browser,
// camera, or screen. Two independent module instances stand in for two browser tabs; a plain JS
// object replaces getUserMedia/canvas with an in-memory "whatever side X is currently displaying"
// slot that the other side's decode polling reads from — not a lossless queue, deliberately: a
// real screen only ever shows its *latest* frame, exactly like this.
//
// Not part of the served app (index.html never loads this) — run manually with:
//   node web/roundtrip_test.mjs
import assert from "node:assert";
import createQrtpModule from "./qrtp_web.js";

function rasterizeQr(width, modules) {
  const quiet = 4;
  const scale = 4; // no real optics/lighting involved, so a small scale is enough
  const side = width + 2 * quiet;
  const pix = side * scale;
  const data = new Uint8Array(pix * pix * 4).fill(255);
  for (let y = 0; y < width; y++) {
    for (let x = 0; x < width; x++) {
      if (!modules[y * width + x]) continue;
      for (let dy = 0; dy < scale; dy++) {
        for (let dx = 0; dx < scale; dx++) {
          const px = (x + quiet) * scale + dx;
          const py = (y + quiet) * scale + dy;
          const idx = (py * pix + px) * 4;
          data[idx] = data[idx + 1] = data[idx + 2] = 0;
          data[idx + 3] = 255;
        }
      }
    }
  }
  return { width: pix, height: pix, data };
}

function makeSide(decodeMod, who, channel) {
  // decodeMod is a WASM instance dedicated to this side's decode polling only -- never the same
  // instance running that side's own WebSession (which spends the whole transfer suspended
  // inside runSend()/runReceive(), awaiting waitForFrame). Asyncify allows only one suspended
  // call in flight per instance, so decoding on the session's own module would collide with that;
  // a separate instance per polling loop sidesteps it (and each side needs its own, not one
  // shared between A and B, since their two poll loops could otherwise overlap on it too).
  const other = who === "A" ? "B" : "A";
  let lastSeenText = "";
  let lastReturnedText = "";
  let pendingWaiter = null;

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

  const poll = setInterval(async () => {
    const frame = channel[other];
    if (!frame) return;
    const ptr = decodeMod._malloc(frame.data.length);
    decodeMod.HEAPU8.set(frame.data, ptr);
    let text = "";
    try {
      // decodeQrRgba is a plain synchronous C++ function, but Asyncify wraps *every* embind-bound
      // function/method uniformly to return a Promise (it can't know ahead of time which calls
      // will actually suspend) -- so even non-suspending calls like this one must be awaited.
      text = await decodeMod.decodeQrRgba(ptr, frame.width, frame.height);
    } finally {
      decodeMod._free(ptr);
    }
    if (text) offerDecoded(text);
  }, 5);
  poll.unref();

  const bridge = {
    renderQr(width, modules) { channel[who] = rasterizeQr(width, modules); },
    waitForFrame(timeoutMs) {
      return new Promise((resolve) => {
        if (lastSeenText && lastSeenText !== lastReturnedText) {
          lastReturnedText = lastSeenText;
          resolve(lastSeenText);
          return;
        }
        const timer = setTimeout(() => { pendingWaiter = null; resolve(""); }, timeoutMs);
        pendingWaiter = { resolve, timer };
      });
    },
    sleep(ms) { return new Promise((resolve) => setTimeout(resolve, ms)); },
    setFps() {},
    setEcLevel() {},
    onLog(msg) { console.log(`[${who}] ${msg}`); },
  };
  return { bridge, stop: () => clearInterval(poll) };
}

async function runScenario(label, payload, sendFn, receiveFn) {
  console.log(`\n--- ${label} ---`);
  const [ModA, ModB, DecodeA, DecodeB] = await Promise.all([
    createQrtpModule(), createQrtpModule(), createQrtpModule(), createQrtpModule(),
  ]);
  const channel = { A: null, B: null };
  const a = makeSide(DecodeA, "A", channel);
  const b = makeSide(DecodeB, "B", channel);
  ModA.qrtpBridge = a.bridge;
  ModB.qrtpBridge = b.bridge;

  ModA.FS.writeFile("/input_file", new Uint8Array(payload));

  const sessionA = new ModA.WebSession();
  const sessionB = new ModB.WebSession();

  const [sendResult, receiveResult] = await Promise.all([
    sendFn(sessionA),
    receiveFn(sessionB),
  ]);

  console.log("send result:", sendResult);
  console.log("receive success:", receiveResult.success, receiveResult.filename);

  assert.ok(sendResult.success, "send failed: " + sendResult.message);
  assert.ok(receiveResult.success, "receive failed");
  const received = Buffer.from(receiveResult.data);
  assert.ok(received.equals(payload), "received bytes do not match sent bytes");
  assert.strictEqual(receiveResult.filename, "input_file");

  console.log(`${label}: OK, ${received.length} bytes matched`);

  a.stop();
  b.stop();
  sessionA.delete();
  sessionB.delete();
}

async function main() {
  const payload = Buffer.from("QRTP web round-trip test payload. ".repeat(80));

  await runScenario(
    "bidirectional",
    payload,
    (s) => s.runSend("/input_file", 10, 180, 1),
    (s) => s.runReceive(10, 1)
  );

  await runScenario(
    "broadcast (unidirectional)",
    payload,
    (s) => s.runBroadcastSend("/input_file", 5.0, 180, 2, 5), // loops=5: no ARQ, reliability via repetition
    (s) => s.runBroadcastReceive(5.0, 1)
  );

  console.log("\nALL SCENARIOS OK");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
