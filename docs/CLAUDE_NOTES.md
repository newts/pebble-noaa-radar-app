# Claude working notes: watchface → watchapp conversion

Working notes for turning this fork (of `leoherzog/pebble-noaa-radar-watchface`)
into a Pebble **watchapp** with interactive controls. Primary target is the
**Pebble Time 2 (emery, 200x228)**.

## Requirements

1. Convert from watchface to app.
2. Remove all time/date/weather slot machinery.
3. Buttons: Up = zoom in, Down = zoom out, Select = cycle radar mode,
   long Select = recenter, long Up = refresh, long Down = toggle overlays.
4. Update the companion JavaScript.

Agreed earlier (2026-11-18):
- The watch holds one image at a time.
- A zoom change flushes the cache and fetches a fresh frame.
- The image updates on zoom or radar-mode change, and once a minute while open.
- No persistent-storage caching for startup; only what is needed while running.

## What branch `claude/watchapp-controls` does

**Build (`package.json`)**: `watchface: false`, new UUID
(`7d0cc50d-…`) so it installs alongside the original face instead of
replacing it, display name "NOAA Radar Zoom", author `newts`, version 2.0.0. `health` capability and
`suncalc` dependency dropped. Message keys reduced to the image transfer plus
`CMD`, `ZOOM_KM`, `RADAR_MODE`, `BUSY`.

**Watch (`src/c/main.c`, ~650 lines, was ~1430)**:
- One full-screen layer draws the frame, center marker, status line
  (`250 km | 3 min` or `Loading...`), toast, and Bluetooth badge.
- Buttons send `CMD` (1 zoom in, 2 zoom out, 3 mode, 4 recenter, 5 refresh);
  long Down toggles overlays locally. Long presses buzz briefly.
- Commands retry for ~1.5 s if the outbox is busy; "No phone", "Phone busy",
  "Not sent", "No response" (45 s timeout) and "Update failed" toasts cover
  failures.
- Minute tick sends `REQUEST_IMAGES` (2 = no frame held, 1 = frame held).
- Removed: text slots, fonts, weather, health, settings struct, all persist
  code (settings and cached composite).

**Phone (`src/pkjs/index.js`, ~550 lines, was ~1870)**:
- Owns zoom (`ZoomIdx` in localStorage, ladder 25/50/100/150/250/400/600/1000
  km, default 250) and radar mode (`RadarMode`, Select toggles Translucent ↔
  Opaque; no Off). These are remembered as user choices, not cached imagery.
- Basemap cache and transfer hash are in memory only. Zoom flushes both.
- Passes: `startPass({user, fresh})`. User passes (buttons, launch) show
  Loading on the watch and report failures; heartbeats skip while a pass is
  fetching. Zoom presses are debounced 400 ms. Refresh re-sends even an
  unchanged frame. Recenter asks GPS for a fresh fix (`maximumAge: 0`).
- Weather, alerts, timeline pins, fonts handshake, replay cache and base64
  helpers removed. `timeline.js` deleted. Legacy localStorage keys from the
  watchface are deleted on `ready`.
- Settings page reduced to GPS / manual location.

## Testing done

- Phone code exercised in Node with mocked Pebble, XHR, geolocation and
  localStorage: launch, heartbeat (unchanged and changed radar), debounced
  zoom, zoom clamp, mode toggle, recenter, refresh, manual location,
  NACK give-up.
- `main.c` syntax-checked with gcc against a stub `pebble.h` for emery,
  basalt and a round build. **Not yet built with the real Pebble SDK or run on
  hardware/emulator** (SDK download host blocked in the cloud workspace).

## Still open

- Panning: not wanted (decided 2026-11-18). Recenter = fresh GPS fix.
- Whether to restore severe-alert timeline pins (removed; in app form they
  would only update while the app is open).
- `STORE.md` rewritten for the app. Store screenshots and banners in
  `screenshots/store/` and `screenshots/banner/` are still watchface images and
  must be recaptured before publishing; the capture tooling is watchface-era.
- Support email for the store listing not chosen yet.
