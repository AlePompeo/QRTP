// Encrypted QR messaging, adapted from https://github.com/AlePompeo/qr-messenger (same author).
// Deliberately independent from main.js/wasm_link.cpp: no WASM, no shared state -- QR encode via
// vendor/qrcode_core.js+qrcode.js (Kazuhiko Arase, MIT), QR decode via vendor/jsQR.js (cozmo,
// Apache 2.0), crypto via the browser's own Web Crypto (AES-256-GCM + PBKDF2). Loaded as a plain
// classic script (not type="module") so the vendor libraries' own globals (`qrcode`, `jsQR`) are
// visible here without an import shim.
(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };

  // ---------------------------------------------------------------- tab switching
  // Coordinates with main.js only through the one global it exports (qrtpStopTransferCamera) --
  // no reverse dependency, main.js has no idea this file exists.
  var tabTransferBtn = $('tabTransferBtn');
  var tabMessengerBtn = $('tabMessengerBtn');
  var panelTransfer = $('panel-transfer');
  var panelMessenger = $('panel-messenger');

  function activateTransferTab() {
    stopCamera();
    panelTransfer.classList.remove('hidden');
    panelMessenger.classList.add('hidden');
    tabTransferBtn.classList.add('active');
    tabMessengerBtn.classList.remove('active');
    tabTransferBtn.setAttribute('aria-selected', 'true');
    tabMessengerBtn.setAttribute('aria-selected', 'false');
  }
  function activateMessengerTab() {
    var startBtn = $('startBtn');
    if (startBtn && startBtn.disabled) {
      alert('Termina il trasferimento file in corso prima di passare a Messaggi cifrati.');
      return;
    }
    if (typeof window.qrtpStopTransferCamera === 'function') window.qrtpStopTransferCamera();
    panelMessenger.classList.remove('hidden');
    panelTransfer.classList.add('hidden');
    tabMessengerBtn.classList.add('active');
    tabTransferBtn.classList.remove('active');
    tabMessengerBtn.setAttribute('aria-selected', 'true');
    tabTransferBtn.setAttribute('aria-selected', 'false');
  }
  tabTransferBtn.addEventListener('click', activateTransferTab);
  tabMessengerBtn.addEventListener('click', activateMessengerTab);

  // ---------------------------------------------------------------- view switching (within the panel)
  var views = {
    home: $('msgHomeView'),
    write: $('msgWriteView'),
    read: $('msgReadView'),
    result: $('msgResultView'),
  };
  function showView(name) {
    Object.keys(views).forEach(function (k) { views[k].classList.toggle('hidden', k !== name); });
    if (name !== 'read') stopCamera();
  }

  // ---------------------------------------------------------------- passphrase
  var passInput = $('msgPassphrase');
  passInput.value = localStorage.getItem('qrtp.messenger.passphrase') || '';
  passInput.addEventListener('input', function () {
    localStorage.setItem('qrtp.messenger.passphrase', passInput.value);
  });
  function requirePassphrase() {
    var p = passInput.value.trim();
    if (!p) {
      alert('Imposta prima una chiave condivisa (identica sull\'altro dispositivo).');
      return null;
    }
    return p;
  }

  // ---------------------------------------------------------------- base64 helpers
  function bytesToBase64(bytes) {
    var binary = '';
    for (var i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i]);
    return btoa(binary);
  }
  function base64ToBytes(b64) {
    var binary = atob(b64);
    var bytes = new Uint8Array(binary.length);
    for (var i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
    return bytes;
  }

  // ---------------------------------------------------------------- crypto (AES-GCM + PBKDF2)
  // Salt and IV are random per message and travel with the ciphertext (salt || iv || ciphertext),
  // so any two people who know the shared passphrase can derive the same key and decrypt, but
  // scanning the QR with an unrelated app just yields opaque bytes -- no plaintext leaks.
  var PBKDF2_ITERATIONS = 100000;

  function deriveKey(passphrase, salt) {
    var enc = new TextEncoder();
    return crypto.subtle.importKey('raw', enc.encode(passphrase), 'PBKDF2', false, ['deriveKey'])
      .then(function (keyMaterial) {
        return crypto.subtle.deriveKey(
          { name: 'PBKDF2', salt: salt, iterations: PBKDF2_ITERATIONS, hash: 'SHA-256' },
          keyMaterial,
          { name: 'AES-GCM', length: 256 },
          false,
          ['encrypt', 'decrypt']
        );
      });
  }

  function encryptMessage(passphrase, plaintext) {
    var salt = crypto.getRandomValues(new Uint8Array(16));
    var iv = crypto.getRandomValues(new Uint8Array(12));
    return deriveKey(passphrase, salt).then(function (key) {
      var enc = new TextEncoder();
      return crypto.subtle.encrypt({ name: 'AES-GCM', iv: iv }, key, enc.encode(plaintext)).then(function (cipherBuf) {
        var cipher = new Uint8Array(cipherBuf);
        var combined = new Uint8Array(salt.length + iv.length + cipher.length);
        combined.set(salt, 0);
        combined.set(iv, salt.length);
        combined.set(cipher, salt.length + iv.length);
        return bytesToBase64(combined);
      });
    });
  }

  function decryptMessage(passphrase, base64Text) {
    var combined = base64ToBytes(base64Text);
    if (combined.length < 29) return Promise.reject(new Error('dati troppo corti'));
    var salt = combined.slice(0, 16);
    var iv = combined.slice(16, 28);
    var cipher = combined.slice(28);
    return deriveKey(passphrase, salt).then(function (key) {
      return crypto.subtle.decrypt({ name: 'AES-GCM', iv: iv }, key, cipher).then(function (plainBuf) {
        return new TextDecoder().decode(plainBuf);
      });
    });
  }

  // ---------------------------------------------------------------- write flow
  var messageInput = $('msgTextInput');
  var charCount = $('msgCharCount');
  var writeError = $('msgWriteError');
  var qrCanvas = $('msgQrCanvas');

  messageInput.addEventListener('input', function () {
    charCount.textContent = String(messageInput.value.length);
  });

  $('msgWriteBtn').addEventListener('click', function () {
    if (!requirePassphrase()) return;
    messageInput.value = '';
    charCount.textContent = '0';
    writeError.textContent = '';
    qrCanvas.classList.add('hidden');
    showView('write');
  });
  $('msgWriteBackBtn').addEventListener('click', function () { showView('home'); });
  $('msgReadBackBtn').addEventListener('click', function () { showView('home'); });
  $('msgResultBackBtn').addEventListener('click', function () { showView('home'); });

  $('msgGenerateBtn').addEventListener('click', function () {
    var passphrase = requirePassphrase();
    if (!passphrase) return;
    var text = messageInput.value.trim();
    if (!text) { writeError.textContent = 'Scrivi prima un messaggio.'; return; }
    writeError.textContent = '';

    encryptMessage(passphrase, text).then(function (encoded) {
      // EC 'L': meno ridondanza integrata, ma più capacità utile a parità di versione QR --
      // a distanza il fattore limitante reale è la risoluzione ottica (pixel-per-modulo che
      // il modulo occupa sul sensore), non la resistenza alla corruzione.
      var qr = qrcode(0, 'L');
      qr.addData(encoded);
      qr.make();
      renderQr(qr);
      qrCanvas.classList.remove('hidden');
      showView('write');
    }).catch(function (e) {
      writeError.textContent = 'Errore nella generazione del QR: ' + e.message;
    });
  });

  function renderQr(qr) {
    var count = qr.getModuleCount();
    var quiet = 4;
    var side = count + quiet * 2;
    qrCanvas.width = side;
    qrCanvas.height = side;
    var ctx = qrCanvas.getContext('2d');
    ctx.fillStyle = '#ffffff';
    ctx.fillRect(0, 0, side, side);
    ctx.fillStyle = '#000000';
    for (var row = 0; row < count; row++) {
      for (var col = 0; col < count; col++) {
        if (qr.isDark(row, col)) ctx.fillRect(col + quiet, row + quiet, 1, 1);
      }
    }
  }
  qrCanvas.addEventListener('click', function () {
    if (qrCanvas.requestFullscreen) qrCanvas.requestFullscreen().catch(function () {});
  });

  // ---------------------------------------------------------------- read flow
  var video = $('msgCameraVideo');
  var scanCanvas = document.createElement('canvas');
  var scanCtx = scanCanvas.getContext('2d', { willReadFrequently: true });
  var zoomControl = $('msgZoomControl');
  var zoomSlider = $('msgZoomSlider');
  var zoomValue = $('msgZoomValue');
  var readStatus = $('msgReadStatus');
  var debugPreview = $('msgDebugPreview');
  debugPreview.addEventListener('click', function () {
    debugPreview.classList.toggle('expanded');
  });
  var stream = null, track = null, rafId = null, scanning = false;

  $('msgReadBtn').addEventListener('click', function () {
    if (!requirePassphrase()) return;
    readStatus.textContent = 'Inquadra il QR...';
    debugPreview.classList.add('hidden');
    debugPreview.classList.remove('expanded');
    showView('read');
    startCamera();
  });

  function startCamera(deviceId) {
    var constraints = deviceId
      ? { deviceId: { exact: deviceId }, width: { ideal: 3840 }, height: { ideal: 2160 } }
      : { facingMode: { ideal: 'environment' }, width: { ideal: 3840 }, height: { ideal: 2160 } };
    navigator.mediaDevices.getUserMedia({ video: constraints, audio: false })
      .then(function (s) {
        stream = s;
        video.srcObject = stream;
        track = stream.getVideoTracks()[0];
        setupZoom();
        populateCameraList();
        scanning = true;
        rafId = requestAnimationFrame(scanLoop);
      })
      .catch(function (e) {
        readStatus.textContent = 'Impossibile aprire la fotocamera: ' + e.message;
      });
  }

  var cameraSelect = $('msgCameraSelect');
  var listedCameras = false;
  function populateCameraList() {
    if (listedCameras || !navigator.mediaDevices.enumerateDevices) return;
    navigator.mediaDevices.enumerateDevices().then(function (devices) {
      var cams = devices.filter(function (d) { return d.kind === 'videoinput'; });
      if (cams.length <= 1) return;
      listedCameras = true;
      cameraSelect.innerHTML = '';
      cams.forEach(function (cam, i) {
        var opt = document.createElement('option');
        opt.value = cam.deviceId;
        opt.textContent = cam.label || ('Fotocamera ' + (i + 1));
        cameraSelect.appendChild(opt);
      });
      var currentId = track.getSettings ? track.getSettings().deviceId : null;
      if (currentId) cameraSelect.value = currentId;
      cameraSelect.classList.remove('hidden');
    }).catch(function () {});
  }
  cameraSelect.addEventListener('change', function () {
    stopCamera();
    startCamera(cameraSelect.value);
  });

  function stopCamera() {
    scanning = false;
    if (rafId) { cancelAnimationFrame(rafId); rafId = null; }
    if (stream) { stream.getTracks().forEach(function (t) { t.stop(); }); stream = null; track = null; }
    zoomControl.classList.add('hidden');
  }

  var torchOn = false;
  var btnTorch = $('msgTorchBtn');

  function setupZoom() {
    var caps = track.getCapabilities ? track.getCapabilities() : {};
    if (caps.zoom) {
      zoomSlider.min = caps.zoom.min;
      zoomSlider.max = caps.zoom.max;
      zoomSlider.step = caps.zoom.step || 0.1;
      var settings = track.getSettings ? track.getSettings() : {};
      zoomSlider.value = settings.zoom || caps.zoom.min;
      zoomValue.textContent = zoomSlider.value + 'x';
      zoomControl.classList.remove('hidden');
    } else {
      zoomControl.classList.add('hidden');
    }
    torchOn = false;
    btnTorch.classList.toggle('hidden', !caps.torch);
  }
  zoomSlider.addEventListener('input', function () {
    if (!track) return;
    zoomValue.textContent = zoomSlider.value + 'x';
    track.applyConstraints({ advanced: [{ zoom: Number(zoomSlider.value) }] }).catch(function () {});
  });
  btnTorch.addEventListener('click', function () {
    if (!track) return;
    torchOn = !torchOn;
    track.applyConstraints({ advanced: [{ torch: torchOn }] }).catch(function () { torchOn = !torchOn; });
  });

  // Bounds decode cost independent of capture resolution -- see qr-messenger's own writeup for why
  // (jsQR on a full 4K frame every rAF tick is slow enough to feel like it "never recognizes"
  // anything on a phone CPU).
  var SCAN_MAX_DIM = 1000;

  function scanLoop() {
    if (!scanning) return;
    if (video.readyState === video.HAVE_ENOUGH_DATA && video.videoWidth > 0) {
      var vw = video.videoWidth, vh = video.videoHeight;
      var scale = Math.min(1, SCAN_MAX_DIM / Math.max(vw, vh));
      scanCanvas.width = Math.round(vw * scale);
      scanCanvas.height = Math.round(vh * scale);
      scanCtx.drawImage(video, 0, 0, scanCanvas.width, scanCanvas.height);
      var imageData = scanCtx.getImageData(0, 0, scanCanvas.width, scanCanvas.height);
      var code = jsQR(imageData.data, imageData.width, imageData.height, { inversionAttempts: 'dontInvert' });
      if (code && code.data) {
        handleScanned(code.data);
        return;
      }
    }
    rafId = requestAnimationFrame(scanLoop);
  }

  function handleScanned(text) {
    var passphrase = passInput.value.trim();
    readStatus.textContent = 'Decodifica in corso...';
    decryptMessage(passphrase, text).then(function (plain) {
      stopCamera();
      $('msgResultText').textContent = plain;
      showView('result');
    }).catch(function () {
      readStatus.textContent = 'QR non leggibile con questa chiave -- riprovo a scansionare...';
      setTimeout(function () {
        if (!views.read.classList.contains('hidden')) {
          readStatus.textContent = 'Inquadra il QR...';
          rafId = requestAnimationFrame(scanLoop);
        }
      }, 1500);
    });
  }

  $('msgScanAgainBtn').addEventListener('click', function () {
    readStatus.textContent = 'Inquadra il QR...';
    debugPreview.classList.add('hidden');
    debugPreview.classList.remove('expanded');
    showView('read');
    startCamera();
  });

  // ---------------------------------------------------------------- read from an uploaded photo
  var UPLOAD_SCAN_DIMS = [1000, 700, 1600, 2400, null]; // null = native resolution, tried last

  function tryDecodeAt(source, naturalW, naturalH, maxDim) {
    var scale = maxDim ? Math.min(1, maxDim / Math.max(naturalW, naturalH)) : 1;
    scanCanvas.width = Math.round(naturalW * scale);
    scanCanvas.height = Math.round(naturalH * scale);
    scanCtx.drawImage(source, 0, 0, scanCanvas.width, scanCanvas.height);
    var imageData = scanCtx.getImageData(0, 0, scanCanvas.width, scanCanvas.height);
    return jsQR(imageData.data, imageData.width, imageData.height, { inversionAttempts: 'attemptBoth' });
  }

  var fileInput = $('msgFileInput');
  $('msgUploadBtn').addEventListener('click', function () { fileInput.click(); });
  fileInput.addEventListener('change', function (e) {
    var file = e.target.files[0];
    fileInput.value = '';
    if (!file) return;
    debugPreview.classList.add('hidden');
    debugPreview.classList.remove('expanded');
    readStatus.textContent = 'Analisi della foto in corso...';

    createImageBitmap(file, { imageOrientation: 'from-image' }).then(function (bitmap) {
      var code = null;
      for (var i = 0; i < UPLOAD_SCAN_DIMS.length && (!code || !code.data); i++) {
        code = tryDecodeAt(bitmap, bitmap.width, bitmap.height, UPLOAD_SCAN_DIMS[i]);
      }
      bitmap.close();
      if (code && code.data) {
        debugPreview.classList.add('hidden');
        debugPreview.classList.remove('expanded');
        handleScanned(code.data);
      } else {
        readStatus.textContent = 'Nessun QR trovato in questa foto -- riprova con un\'altra.';
        try {
          debugPreview.src = scanCanvas.toDataURL('image/jpeg', 0.7);
          debugPreview.classList.remove('hidden');
        } catch (err) { /* toDataURL can fail on some browsers for very large canvases -- non-critical */ }
      }
    }).catch(function (e) {
      readStatus.textContent = 'Impossibile caricare questa immagine: ' + e.message;
    });
  });

  showView('home');
})();
