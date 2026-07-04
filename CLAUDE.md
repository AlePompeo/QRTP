# CLAUDE.md

Behavioral guidelines to reduce common LLM coding mistakes. Merge with project-specific instructions as needed.

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.

## 5. Framework to use in the project

- ISO/OSI Layered Model (physical/optical → framing → transport/ARQ → application)
- Finite State Machine design for the connection lifecycle
- L99 Level
- UltraThink
- Godmode
- SOLID Model
- C++17 (no external dependencies for the protocol core; libqrencode loaded via dlopen; OpenCV optional, gated behind `WITH_OPENCV=1`, for the real screen/camera backend)

## 6. Project Purpose
Build QRTP (QR Transfer Protocol): an air-gapped, TCP-like file transfer protocol that moves data
between two computers using only a monitor and a webcam on each side — no network, cable, or
internet link involved. Sending a file renders it in real time as a sequence of QR codes on the
sender's screen; a camera on the receiving computer reads that sequence back into bytes. The
transport layer must behave like TCP: 3-way handshake, sequence numbers, a sliding window with
selective-repeat ARQ, cumulative + selective ACKs, retransmission on loss, per-frame CRC32 plus an
end-to-end file hash, and adaptive performance (QR density/error-correction level, frame rate, and
window size all adjust automatically to the measured optical read-error rate). Built in C++17,
step by step, using the references below for the underlying standards and prior art.

## 7. Link for Reference Documents
QR Code (structure, versions, error correction levels): https://en.wikipedia.org/wiki/QR_code

Reed–Solomon error correction (used internally by QR codes): https://en.wikipedia.org/wiki/Reed%E2%80%93Solomon_error_correction

Cyclic redundancy check (CRC): https://en.wikipedia.org/wiki/Cyclic_redundancy_check

Sliding window protocol: https://en.wikipedia.org/wiki/Sliding_window_protocol

Selective Repeat ARQ: https://en.wikipedia.org/wiki/Selective_Repeat_ARQ

TCP — RFC 793 (Transmission Control Protocol): https://www.rfc-editor.org/rfc/rfc793

TCP Congestion Control — RFC 5681: https://www.rfc-editor.org/rfc/rfc5681

Computing TCP's Retransmission Timer — RFC 6298 (Jacobson/Karels RTO estimation): https://www.rfc-editor.org/rfc/rfc6298

AIMD (Additive Increase / Multiplicative Decrease): https://en.wikipedia.org/wiki/Additive_increase/multiplicative_decrease

pyzbar (Python ZBar bindings, QR/barcode decoding): https://pypi.org/project/pyzbar/

qrcode (Python QR code generation library): https://pypi.org/project/qrcode/

opencv-python (camera capture + image processing): https://pypi.org/project/opencv-python/

ZBar barcode reader library: https://github.com/mchehab/zbar
