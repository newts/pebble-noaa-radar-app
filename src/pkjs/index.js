/**
 * NOAA US Weather Radar: PebbleKit JS (watchapp)
 *
 * Fetches a USGS Topo basemap and the NOAA MRMS reflectivity overlay at the
 * watch's display size, blends them into one 4bpp PNG (composite.js) so the
 * watch holds a single frame, and streams it in AppMessage chunks unless a
 * content hash shows the watch already has those bytes.
 *
 * The phone owns the map state: zoom level and radar mode. The watch sends
 * button commands (CMD) and a once-a-minute heartbeat (REQUEST_IMAGES); the
 * phone answers with a status message (ZOOM_KM, RADAR_MODE, BUSY) and a fresh
 * frame. Nothing is cached across launches: the basemap cache and the
 * transfer hash live in memory and die with the app.
 *
 * All floating point lives here; the watch only sees bytes and integers.
 */

var Clay = require('@rebble/clay');
// PNG codec for the fetched layers. Each 256-color layer is re-quantized to
// 16 colors (shrinkPng) before the blend: blending the originals gives more
// output colors, no byte saving and a muddy result.
var UPNG = require('upng-js');
// The blend, the palette fold and the 4bpp PNG encoder.
var composite = require('./composite');
var clayConfig = require('./config');
var customClay = require('./custom-clay');
var clay = new Clay(clayConfig, customClay, { autoHandleEvents: false });

var BASEMAP_URL = 'https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/export';
var RADAR_URL = 'https://mapservices.weather.noaa.gov/eventdriven/rest/services/radar/radar_base_reflectivity/MapServer/export';

// ---------------------------------------------------------------------------
// Map state
// ---------------------------------------------------------------------------

// Ground width across the screen, in km. Up zooms toward the start.
var ZOOM_KM = [25, 50, 100, 150, 250, 400, 600, 1000];
var DEFAULT_ZOOM = 4;               // 250 km, the watchface's old "State"

// Radar modes, as composite.buildComposite and the watch number them.
var MODE_OFF = 0, MODE_TRANSLUCENT = 1, MODE_OPAQUE = 2;
var DEFAULT_MODE = MODE_TRANSLUCENT;
// Select cycles Translucent -> Opaque -> Off -> Translucent.
var MODE_NEXT = {};
MODE_NEXT[MODE_TRANSLUCENT] = MODE_OPAQUE;
MODE_NEXT[MODE_OPAQUE] = MODE_OFF;
MODE_NEXT[MODE_OFF] = MODE_TRANSLUCENT;

// Watch commands; must match the CMD_* enum in main.c.
var CMD_ZOOM_IN = 1, CMD_ZOOM_OUT = 2, CMD_MODE = 3,
    CMD_RECENTER = 4, CMD_REFRESH = 5;

// BUSY values sent to the watch.
var BUSY_IDLE = 0, BUSY_WORKING = 1, BUSY_FAILED = 2;

// Zoom and mode are remembered between launches as the user's last choice
// (they are settings, not cached imagery). Out-of-range or absent values fall
// back to the defaults.
function intSetting(key, lo, hi, def) {
  var raw = localStorage.getItem(key);
  var v = (raw === null || raw === '') ? def : Number(raw);
  return (isNaN(v) || v < lo || v > hi) ? def : v;
}
function zoomIdx()   { return intSetting('ZoomIdx', 0, ZOOM_KM.length - 1, DEFAULT_ZOOM); }
function radarMode() { return intSetting('RadarMode', 0, 2, DEFAULT_MODE); }

// ---------------------------------------------------------------------------
// Display size
// ---------------------------------------------------------------------------

// Both layers are requested at exactly the display's size: the composite
// fills the watch's full bounds, and the firmware crops a mismatched bitmap
// rather than scaling it. Defaults are emery's; 'ready' narrows them to the
// watch actually connected.
var PLATFORM_SIZES = {
  basalt:  [144, 168],
  emery:   [200, 228],
  gabbro:  [260, 260],
  chalk:   [180, 180]
};
var ROUND_PLATFORMS = { chalk: true, gabbro: true };
var IMG_W = 200;
var IMG_H = 228;
var WATCH_ROUND = false;

// ---------------------------------------------------------------------------
// Location
// ---------------------------------------------------------------------------

var FALLBACK_LAT = 40.69;                     // Statue of Liberty
var FALLBACK_LON = -74.04;
var lastFix = null;                           // {lat, lon} of the last good fix

// "lat, lon" (comma or space separated) in decimal degrees -> {lat, lon},
// or null. Must stay in sync with parseLoc() in custom-clay.js, which cannot
// share this function: Clay injects it into the config page by toString().
function parseManualLoc(s) {
  var m = /^\s*(-?\d+(?:\.\d+)?)[\s,]+(-?\d+(?:\.\d+)?)\s*$/
            .exec(String(s || ''));
  if (!m) return null;
  var lat = parseFloat(m[1]);
  var lon = parseFloat(m[2]);
  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return null;
  return { lat: lat, lon: lon };
}

// cb(lat, lon). A manual location bypasses GPS entirely. `fresh` (Recenter)
// demands a new fix instead of accepting the phone's cached one.
function locate(fresh, cb) {
  var m = parseManualLoc(localStorage.getItem('ManualLoc'));   // '' = GPS
  if (m) {
    cb(m.lat, m.lon);
    return;
  }
  navigator.geolocation.getCurrentPosition(
    function (pos) {
      lastFix = { lat: pos.coords.latitude, lon: pos.coords.longitude };
      cb(lastFix.lat, lastFix.lon);
    },
    function (err) {
      console.log('Location error: ' + (err && err.message));
      var f = lastFix || { lat: FALLBACK_LAT, lon: FALLBACK_LON };
      cb(f.lat, f.lon);
    },
    { timeout: 15000, maximumAge: fresh ? 0 : 60000, enableHighAccuracy: !!fresh }
  );
}

// ---------------------------------------------------------------------------
// Transfer state machine
// ---------------------------------------------------------------------------

var CHUNK = 4096;      // the inbox is 8200 B on all four platforms; the
                       // header tuples add ~50 B
// The single-slot serialiser carries two kinds of work, dispatched on `kind`:
//   {kind: 'img', bytes, hash, radarTime}: a chunked composite
//   {kind: 'msg', dict: {...}}: one whole AppMessage
// The chunked protocol depends on strictly ordered ACKs, and an unrelated
// sendAppMessage mid-transfer risks a NACK on the chunk in flight, so every
// AppMessage goes through this queue.
var tx = null;         // current item (+ offset/retries while sending)
var queue = [];        // pending items
// Hash of the composite the watch is known to display (set at the final
// chunk's ACK) and of the one queued or in flight. Both in memory only.
var txHash = null;
var pendingHash = null;

function enqueue(item) {
  queue.push(item);
  pump();
}

function pump() {
  if (tx || queue.length === 0) return;
  tx = queue.shift();
  tx.retries = 0;
  tx.offset = 0;
  send(tx);
}

function dictFor(t) {
  if (t.kind === 'msg') return t.dict;
  var end = Math.min(t.offset + CHUNK, t.bytes.length);
  var d = {
    'IMG_OFFSET': t.offset,
    // Must be a plain Array of numbers: a raw Uint8Array is not reliably
    // marshalled as a byte array by PebbleKit JS.
    'IMG_DATA': Array.prototype.slice.call(t.bytes.subarray(t.offset, end))
  };
  if (t.offset === 0) d['IMG_TOTAL'] = t.bytes.length;
  return d;
}

// Every callback carries the item it belongs to, so an ACK that arrives
// after resetTransfers() moved on cannot advance the item that replaced it.
function send(t) {
  if (t !== tx) return;
  Pebble.sendAppMessage(dictFor(t),
                        function () { onAck(t); },
                        function () { onNack(t); });
}

function onAck(t) {
  if (t !== tx) return;
  t.retries = 0;
  if (t.kind === 'img') {
    t.offset += CHUNK;
    if (t.offset < t.bytes.length) {
      send(t);
      return;
    }
  }
  tx = null;
  // The final chunk's ACK is the commit point.
  if (t.kind === 'img') {
    txHash = t.hash;
    if (pendingHash === t.hash) pendingHash = null;
    enqueue({ kind: 'msg', dict: { 'RADAR_TIME': t.radarTime, 'BUSY': BUSY_IDLE } });
  }
  pump();
}

function onNack(t) {
  if (t !== tx) return;
  t.retries++;
  if (t.retries <= 3) {
    setTimeout(function () { send(t); }, 500);
    return;
  }
  console.log('Giving up on ' + (t.kind === 'msg' ? 'message' : 'image'));
  tx = null;
  if (t.kind === 'img') {
    // The watch keeps whatever it had, but we no longer know what that is.
    txHash = null;
    pendingHash = null;
    enqueue({ kind: 'msg', dict: { 'BUSY': BUSY_FAILED } });
  }
  pump();
}

// Drop every queued and in-flight image: the pass about to run supersedes
// it. msg items are kept.
function resetTransfers() {
  var dropped = false;
  queue = queue.filter(function (q) {
    if (q.kind === 'img') { dropped = true; return false; }
    return true;
  });
  if (tx && tx.kind === 'img') { tx = null; dropped = true; }
  if (dropped) {
    txHash = null;
    pendingHash = null;
  }
  pump();
}

function sendStatus(busy) {
  enqueue({ kind: 'msg', dict: {
    'ZOOM_KM': ZOOM_KM[zoomIdx()],
    'RADAR_MODE': radarMode(),
    'BUSY': busy
  } });
}

// ---------------------------------------------------------------------------
// Fetching
// ---------------------------------------------------------------------------

function shrinkPng(bytes) {
  var img = UPNG.decode(bytes.buffer);
  var rgba = UPNG.toRGBA8(img)[0];
  return new Uint8Array(UPNG.encode([rgba], img.width, img.height, 16));
}

// PNG bytes -> RGBA. UPNG.toRGBA8(img)[0] is an ArrayBuffer, not a typed
// array, so the wrapper is required.
function rgbaOf(bytes) {
  var img = UPNG.decode(bytes.buffer);
  return new Uint8Array(UPNG.toRGBA8(img)[0]);
}

// cb(rgba) on success, cb(null) on any failure. Every path calls back: the
// two layers join before the blend.
function fetchLayer(url, cb) {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url);
  xhr.responseType = 'arraybuffer';
  xhr.timeout = 20000;
  xhr.onload = function () {
    // Read xhr.response exactly once: the pkjs bridge hands back a real
    // ArrayBuffer only on the first read.
    var buf = xhr.response;
    if (xhr.status !== 200 || !buf) {
      console.log('Fetch failed (' + xhr.status + '): ' + url);
      cb(null);
      return;
    }
    var b = new Uint8Array(buf);
    // ArcGIS answers a bad bbox or an outage with HTTP 200 and a JSON error.
    if (b.length < 8 || b[0] !== 0x89 || b[1] !== 0x50 ||
        b[2] !== 0x4E || b[3] !== 0x47) {
      console.log('Not a PNG (' + b.length + ' bytes): ' + url);
      cb(null);
      return;
    }
    try {
      cb(rgbaOf(shrinkPng(b)));
    } catch (e) {
      console.log('Transcode failed: ' + e);
      cb(null);
    }
  };
  xhr.onerror = function () { console.log('Fetch error: ' + url); cb(null); };
  xhr.ontimeout = function () { console.log('Fetch timeout: ' + url); cb(null); };
  xhr.send();
}

function exportUrl(base, bbox, transparent) {
  return base +
    '?bbox=' + bbox +
    '&bboxSR=3857&imageSR=3857&size=' + IMG_W + ',' + IMG_H + '&format=png8' +
    '&transparent=' + (transparent ? 'true' : 'false') +
    '&f=image';
}

// ---------------------------------------------------------------------------
// Refresh passes
// ---------------------------------------------------------------------------

// The basemap for the current view, in memory only. Radar is refetched every
// pass; the topo map only when the view (size, zoom, center) changes. Zoom
// changes clear it outright.
var bmCache = { key: null, rgba: null };

function flushCache() {
  bmCache = { key: null, rgba: null };
  txHash = null;
  pendingHash = null;
}

var passGen = 0;        // bumped by every pass; stale results drop out
var passBusy = false;   // a pass is fetching; heartbeats wait for it
var passWatchdog = null;

// One fetch -> blend -> send cycle. opts.user: the user is waiting (button
// press or launch), so the watch shows Loading and hears about failures.
// opts.fresh: demand a new GPS fix (Recenter). Heartbeats run with neither
// and step aside while another pass is fetching.
function startPass(opts) {
  if (!opts.user && passBusy) return;
  var g = ++passGen;
  passBusy = true;
  clearTimeout(passWatchdog);
  passWatchdog = setTimeout(function () {
    if (g === passGen) passBusy = false;   // a hung fetch must not stall heartbeats
  }, 60000);
  resetTransfers();
  if (opts.user) sendStatus(BUSY_WORKING);

  function finish(ok) {
    if (g !== passGen) return false;
    passBusy = false;
    clearTimeout(passWatchdog);
    if (!ok && opts.user) {
      enqueue({ kind: 'msg', dict: { 'BUSY': BUSY_FAILED } });
    }
    return true;
  }

  locate(!!opts.fresh, function (lat, lon) {
    if (g !== passGen) return;
    render(lat, lon, g, finish);
  });
}

function render(rawLat, rawLon, g, finish) {
  // Round once, so the cache key and the bbox describe the same place.
  var lat = Math.round(rawLat * 100) / 100;
  var lon = Math.round(rawLon * 100) / 100;
  var zoom = zoomIdx();
  var mode = radarMode();

  // EPSG:3857 units are metres only at the equator; a projected span W covers
  // W*cos(lat) of ground, so divide it out to make the km labels true.
  var W = ZOOM_KM[zoom] * 1000 / Math.cos(lat * Math.PI / 180);
  var H = W * IMG_H / IMG_W;
  var cx = lon * 20037508.34 / 180;
  var cy = Math.log(Math.tan((90 + lat) * Math.PI / 360)) /
           (Math.PI / 180) * 20037508.34 / 180;
  var bbox = [cx - W / 2, cy - H / 2, cx + W / 2, cy + H / 2].join(',');
  var key = IMG_W + 'x' + IMG_H + '_' + zoom + '_' + lat.toFixed(2) + '_' + lon.toFixed(2);

  var want = (mode === MODE_OFF) ? 1 : 2;
  var got = 0, bm = null, rd = null, failed = false;

  function part(which, rgba) {
    if (g !== passGen) return;
    if (!rgba) failed = true;
    else if (which === 0) bm = rgba; else rd = rgba;
    if (++got < want) return;
    if (failed || !bm) {
      // Send nothing: the watch keeps its last good frame and the radar age
      // keeps climbing, which is the truth.
      console.log('Composite skipped: a layer is missing');
      finish(false);
      return;
    }
    if (finish(true)) composeAndSend(bm, rd, mode);
  }

  if (bmCache.key === key && bmCache.rgba) {
    part(0, bmCache.rgba);
  } else {
    fetchLayer(exportUrl(BASEMAP_URL, bbox, false), function (rgba) {
      if (g !== passGen) return;
      if (rgba) bmCache = { key: key, rgba: rgba };
      part(0, rgba);
    });
  }
  if (mode !== MODE_OFF) {
    fetchLayer(exportUrl(RADAR_URL, bbox, true), function (rgba) {
      part(1, rgba);
    });
  }
}

function composeAndSend(bm, rd, mode) {
  var r;
  try {
    r = composite.buildComposite(bm, rd, mode, IMG_W, IMG_H, WATCH_ROUND);
  } catch (e) {
    console.log('Composite failed: ' + e);
    enqueue({ kind: 'msg', dict: { 'BUSY': BUSY_FAILED } });
    return;
  }
  var h = composite.hashBytes(r.bytes);
  var stamp = (mode === MODE_OFF) ? 0 : Math.floor(Date.now() / 1000);
  console.log('Composite ' + r.bytes.length + ' B, ' + r.colors +
              ' colors -> ' + r.folded + ', hash ' + h);
  if (h === txHash) {
    // The watch already shows these pixels: just re-date them.
    console.log('Composite unchanged, skipping transfer');
    enqueue({ kind: 'msg', dict: { 'RADAR_TIME': stamp, 'BUSY': BUSY_IDLE } });
    return;
  }
  if (h === pendingHash) {
    // Already queued; its own commit sends RADAR_TIME and clears BUSY.
    console.log('Composite already in flight, skipping');
    return;
  }
  pendingHash = h;
  enqueue({ kind: 'img', bytes: r.bytes, hash: h, radarTime: stamp });
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Zoom presses are debounced, so tapping Up three times fetches one map, not
// three. The status (and the watch's "N km" toast) goes out at once.
var ZOOM_DEBOUNCE_MS = 400;
var zoomTimer = null;

function changeZoom(delta) {
  var z = zoomIdx();
  var nz = Math.max(0, Math.min(ZOOM_KM.length - 1, z + delta));
  if (nz === z) {
    // At the end of the ladder: nothing new to fetch. Clear the watch's
    // Loading only if no earlier press still has a fetch coming.
    enqueue({ kind: 'msg', dict: {
      'BUSY': (zoomTimer || passBusy) ? BUSY_WORKING : BUSY_IDLE
    } });
    return;
  }
  localStorage.setItem('ZoomIdx', String(nz));
  flushCache();
  passGen++;              // drop any pass for the old zoom
  resetTransfers();
  sendStatus(BUSY_WORKING);
  clearTimeout(zoomTimer);
  zoomTimer = setTimeout(function () {
    zoomTimer = null;
    startPass({ user: true });
  }, ZOOM_DEBOUNCE_MS);
}

function cycleMode() {
  localStorage.setItem('RadarMode', String(MODE_NEXT[radarMode()]));
  txHash = null;          // the blend changes even where the view did not
  startPass({ user: true });
}

function onCommand(cmd) {
  console.log('Command ' + cmd);
  switch (cmd) {
    case CMD_ZOOM_IN:  changeZoom(-1); break;
    case CMD_ZOOM_OUT: changeZoom(+1); break;
    case CMD_MODE:     cycleMode(); break;
    case CMD_RECENTER: startPass({ user: true, fresh: true }); break;
    case CMD_REFRESH:
      // Re-send even an unchanged frame, so the user sees the refresh land.
      txHash = null;
      startPass({ user: true });
      break;
    default:
      console.log('Unknown command ' + cmd);
  }
}

// ---------------------------------------------------------------------------
// Pebble events
// ---------------------------------------------------------------------------

// Keys left behind by the watchface version: cached imagery, weather and
// timeline state, and the old settings blob. Removed once so they stop
// taking localStorage space.
var LEGACY_KEYS = ['bm_key', 'bm_data', 'tx_hash', 'tx_replay', 'cfg2',
                   'wx_payload', 'wx_obs', 'wx_fcst', 'wx_grid', 'wx_alerts',
                   'tl_pins', 'Zoom', 'WxUnits', 'TimelineAlerts'];

Pebble.addEventListener('ready', function () {
  console.log('PebbleKit JS ready!');
  LEGACY_KEYS.forEach(function (k) {
    try { localStorage.removeItem(k); } catch (e) {}
  });
  var info = Pebble.getActiveWatchInfo && Pebble.getActiveWatchInfo();
  var size = info && PLATFORM_SIZES[info.platform];
  if (size) {
    IMG_W = size[0];
    IMG_H = size[1];
  }
  WATCH_ROUND = !!(info && ROUND_PLATFORMS[info.platform]);
  console.log('Imagery size: ' + IMG_W + 'x' + IMG_H +
              ' (platform ' + ((info && info.platform) || 'unknown') + ')');
  startPass({ user: true });
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload;
  if (p['CMD'] !== undefined) {
    onCommand(p['CMD']);
    return;
  }
  // The minute heartbeat. 2 = the watch has no frame, so never skip.
  if (p['REQUEST_IMAGES']) {
    if (p['REQUEST_IMAGES'] === 2) txHash = null;
    startPass({ user: false });
  }
});

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  // convert=false keeps the keys by name; UseGps and ManualLoc are not
  // AppMessage keys.
  var d = clay.getSettings(e.response, false);
  // The page refuses to save with GPS off and an unparsable box
  // (custom-clay.js disables Save), so a pair that fails to parse here falls
  // back to GPS rather than guessing.
  var mloc = (d.UseGps && !Number(d.UseGps.value))
               ? parseManualLoc(d.ManualLoc && d.ManualLoc.value)
               : null;
  var val = mloc ? mloc.lat + ',' + mloc.lon : '';
  if (val !== (localStorage.getItem('ManualLoc') || '')) {
    localStorage.setItem('ManualLoc', val);
    startPass({ user: true, fresh: true });
  }
});
