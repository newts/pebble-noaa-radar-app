/**
 * NOAA US Weather Radar: Pebble watchapp.
 *
 * Fullscreen radar map centered on the user's location. PebbleKit JS blends a
 * USGS Topo basemap with NOAA MRMS reflectivity into one 16-color PNG and
 * streams it here over AppMessage; the watch decodes and draws that frame.
 *
 * The watch owns no map state. Zoom level and radar mode live on the phone;
 * the buttons send commands, and the phone answers with a status message
 * (zoom in km, radar mode, busy flag) followed by a fresh frame.
 *
 *   Up            zoom in            Long Up      refresh now
 *   Down          zoom out           Long Down    toggle overlays
 *   Select        radar mode toggle  Long Select  recenter (fresh GPS fix)
 *   Back          exit
 *
 * The backlight stays on for 30 s after launch and after every button press,
 * longer than the system's glance timeout, so the map can be studied.
 *
 * Nothing is persisted: the app starts grey and fills in when the first frame
 * arrives. The frame refreshes once a minute while the app is open.
 */

#include <pebble.h>

// ---- Commands to the phone (CMD key) ---------------------------------------
// Must match CMD_* in src/pkjs/index.js.
enum {
  CMD_ZOOM_IN  = 1,
  CMD_ZOOM_OUT = 2,
  CMD_MODE     = 3,
  CMD_RECENTER = 4,
  CMD_REFRESH  = 5,
};

// ---- Radar modes (RADAR_MODE key) ------------------------------------------
enum { MODE_TRANSLUCENT = 1, MODE_OPAQUE = 2 };

// ---- BUSY key values -------------------------------------------------------
enum { BUSY_IDLE = 0, BUSY_WORKING = 1, BUSY_FAILED = 2 };

// A decoded frame is 4bpp, rows padded to a byte. The firmware decoder
// inflates into one buffer of FRAME_BYTES plus a byte per row, which the
// GBitmap adopts; a decode needs FRAME_BYTES + PBL_DISPLAY_HEIGHT + about
// 1.8 KB free beyond the compressed input. DECODE_HEADROOM's 1.5x leaves the
// rest of half a frame as slack, thinnest on basalt at about 4 KB. A decode
// that runs short can fault inside the decoder, so the guard stays a hard
// floor. The budget is one frame, never two, because the resident frame is
// destroyed before its replacement decodes.
#define FRAME_BYTES     (((PBL_DISPLAY_WIDTH + 1) / 2) * PBL_DISPLAY_HEIGHT)
#define DECODE_HEADROOM (FRAME_BYTES * 3 / 2)

#define TOAST_MS          1500    // how long a toast stays up
#define BUSY_TIMEOUT_MS   45000   // give up on a phone that never answers
#define CMD_RETRY_MS      150     // outbox busy: try the command again
#define CMD_MAX_RETRIES   10
// The SDK has no per-app backlight timeout, so the app forces the light on
// with light_enable(true) and hands it back to the system with
// light_enable(false) when this timer runs out. Forced on means on even in
// daylight, where the system would leave it off: the cost is battery.
#define BACKLIGHT_HOLD_MS 30000

// ==== Global state ==========================================================

static Window   *s_window;
static Layer    *s_map_layer;       // full-bounds, draws everything
static GBitmap  *s_image;           // the composite; NULL until first decode

// PNG receive state machine
static uint8_t  *s_rx_buf;          // malloc'd PNG accumulator, NULL when idle
static uint32_t  s_rx_total;        // 0 when idle
static uint32_t  s_rx_len;          // bytes written so far
static bool      s_decode_retry;    // the last decode failed; clears on success

// Map status, as last reported by the phone
static time_t    s_radar_time;      // fetch time of the radar on screen; 0 = none
static int32_t   s_zoom_km;         // 0 until the phone reports it
static int       s_radar_mode = -1; // -1 until the phone reports it

// UI state (RAM only)
static bool      s_overlays = true; // center marker + status line
static bool      s_busy = true;     // a fetch the user is waiting on
static AppTimer *s_busy_timer;
static char      s_toast[24];
static AppTimer *s_toast_timer;

// A command waiting for a free outbox
static uint8_t   s_pending_cmd;
static int       s_cmd_retries;
static AppTimer *s_cmd_timer;

static AppTimer *s_light_timer;     // returns the backlight to the system

// ==== Helpers ===============================================================

static void redraw(void) {
  if (s_map_layer) {
    layer_mark_dirty(s_map_layer);
  }
}

static const char *mode_name(int mode) {
  switch (mode) {
    case MODE_TRANSLUCENT: return "Translucent";
    case MODE_OPAQUE:      return "Opaque";
    default:               return "";
  }
}

static void toast_timeout(void *ctx) {
  s_toast_timer = NULL;
  s_toast[0] = '\0';
  redraw();
}

static void show_toast(const char *msg) {
  strncpy(s_toast, msg, sizeof(s_toast) - 1);
  s_toast[sizeof(s_toast) - 1] = '\0';
  if (s_toast_timer) {
    app_timer_reschedule(s_toast_timer, TOAST_MS);
  } else {
    s_toast_timer = app_timer_register(TOAST_MS, toast_timeout, NULL);
  }
  redraw();
}

static void busy_timeout(void *ctx) {
  s_busy_timer = NULL;
  if (s_busy) {
    s_busy = false;
    show_toast("No response");
  }
}

static void set_busy(bool busy) {
  s_busy = busy;
  if (s_busy_timer) {
    app_timer_cancel(s_busy_timer);
    s_busy_timer = NULL;
  }
  if (busy) {
    s_busy_timer = app_timer_register(BUSY_TIMEOUT_MS, busy_timeout, NULL);
  }
  redraw();
}

// ==== Backlight =============================================================

static void backlight_release(void *ctx) {
  s_light_timer = NULL;
  light_enable(false);   // back to the system's own timeout and ambient rules
}

// Light on now, and off BACKLIGHT_HOLD_MS after the latest call.
static void backlight_hold(void) {
  light_enable(true);
  if (s_light_timer) {
    app_timer_reschedule(s_light_timer, BACKLIGHT_HOLD_MS);
  } else {
    s_light_timer = app_timer_register(BACKLIGHT_HOLD_MS, backlight_release, NULL);
  }
}

// ==== Messages to the phone =================================================

// The heartbeat. 2 = the watch holds no frame (the phone must not skip an
// unchanged composite), 1 = it holds one. Never 0: pkjs gates on truthiness.
static void request_images(bool need_image) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_uint8(iter, MESSAGE_KEY_REQUEST_IMAGES, need_image ? 2 : 1);
    app_message_outbox_send();
  }
}

static bool try_send_cmd(uint8_t cmd) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return false;
  }
  dict_write_uint8(iter, MESSAGE_KEY_CMD, cmd);
  return app_message_outbox_send() == APP_MSG_OK;
}

static void cmd_retry(void *ctx) {
  s_cmd_timer = NULL;
  if (!s_pending_cmd) {
    return;
  }
  if (try_send_cmd(s_pending_cmd)) {
    s_pending_cmd = 0;
  } else if (++s_cmd_retries < CMD_MAX_RETRIES) {
    s_cmd_timer = app_timer_register(CMD_RETRY_MS, cmd_retry, NULL);
  } else {
    s_pending_cmd = 0;
    set_busy(false);
    show_toast("Phone busy");
  }
}

// Sends a command, retrying briefly while the outbox is busy (a heartbeat or
// an earlier command still in flight). A newer command replaces a pending one.
static void send_cmd(uint8_t cmd) {
  backlight_hold();      // every press that reaches here restarts the 30 s
  if (!connection_service_peek_pebble_app_connection()) {
    show_toast("No phone");
    return;
  }
  set_busy(true);
  if (try_send_cmd(cmd)) {
    s_pending_cmd = 0;
    return;
  }
  s_pending_cmd = cmd;
  s_cmd_retries = 0;
  if (!s_cmd_timer) {
    s_cmd_timer = app_timer_register(CMD_RETRY_MS, cmd_retry, NULL);
  }
}

// ==== Buttons ===============================================================

// A short buzz confirms a long press registered, since nothing on screen
// changes until the phone answers.
static void long_press_buzz(void) {
  static const uint32_t seg[] = { 40 };
  VibePattern pat = { .durations = seg, .num_segments = 1 };
  vibes_enqueue_custom_pattern(pat);
}

static void up_click(ClickRecognizerRef r, void *ctx)     { send_cmd(CMD_ZOOM_IN); }
static void down_click(ClickRecognizerRef r, void *ctx)   { send_cmd(CMD_ZOOM_OUT); }
static void select_click(ClickRecognizerRef r, void *ctx) { send_cmd(CMD_MODE); }

static void select_long(ClickRecognizerRef r, void *ctx) {
  long_press_buzz();
  show_toast("Recentering");
  send_cmd(CMD_RECENTER);
}

static void up_long(ClickRecognizerRef r, void *ctx) {
  long_press_buzz();
  show_toast("Refreshing");
  send_cmd(CMD_REFRESH);
}

static void down_long(ClickRecognizerRef r, void *ctx) {
  backlight_hold();
  long_press_buzz();
  s_overlays = !s_overlays;
  show_toast(s_overlays ? "Overlays on" : "Overlays off");
}

static void click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  // Handlers fire on press once the hold passes the delay; a shorter press
  // falls through to the single-click handler on release.
  window_long_click_subscribe(BUTTON_ID_UP, 500, up_long, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 500, down_long, NULL);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long, NULL);
}

// ==== Ticks and system events ===============================================

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  redraw();                    // the radar age in the status line
  request_images(!s_image);    // once-a-minute refresh
}

static void connection_callback(bool connected) {
  redraw();                    // the Bluetooth badge
  if (connected) {
    request_images(!s_image);
  }
}

// ==== AppMessage: image transfer ============================================

static uint32_t bitmap_bytes(GBitmap *bmp) {
  if (!bmp) {
    return 0;
  }
  return (uint32_t)gbitmap_get_bytes_per_row(bmp) * gbitmap_get_bounds(bmp).size.h;
}

static void rx_reset(void) {
  free(s_rx_buf);
  s_rx_buf = NULL;
  s_rx_total = 0;
  s_rx_len = 0;
}

// On a failed decode the firmware returns a zeroed GBitmap, not NULL, so
// failure shows only as a NULL pixel buffer.
static void decode_frame(const uint8_t *png, uint32_t len) {
  s_image = gbitmap_create_from_png_data(png, len);
  if (s_image && !gbitmap_get_data(s_image)) {
    gbitmap_destroy(s_image);
    s_image = NULL;
  }
}

// Header, chunks in strict order, then decode.
static void rx_image(DictionaryIterator *iter) {
  Tuple *total_t = dict_find(iter, MESSAGE_KEY_IMG_TOTAL);
  if (total_t) {
    rx_reset();
    s_rx_total = total_t->value->uint32;

    // Refuse up front: a decode that runs short faults the app. The resident
    // frame is destroyed before the decode, so its bytes count as available.
    uint32_t avail = heap_bytes_free() + bitmap_bytes(s_image);
    if (s_rx_total == 0 || s_rx_total > FRAME_BYTES * 2 ||
        avail < s_rx_total + DECODE_HEADROOM) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "Rejecting transfer of %d bytes (avail %d)",
              (int)s_rx_total, (int)avail);
      s_rx_total = 0;
      return;
    }
    s_rx_buf = malloc(s_rx_total);
    if (!s_rx_buf) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "Rx malloc of %d bytes failed",
              (int)s_rx_total);
      s_rx_total = 0;
      return;
    }
  }

  Tuple *data_t = dict_find(iter, MESSAGE_KEY_IMG_DATA);
  Tuple *off_t  = dict_find(iter, MESSAGE_KEY_IMG_OFFSET);
  if (!s_rx_buf || !data_t || !off_t) {
    return;
  }

  uint32_t off = off_t->value->uint32;
  uint16_t len = data_t->length;
  // Ordering invariant; duplicates after a lost ACK land here and are dropped.
  if (off != s_rx_len || off + len > s_rx_total) {
    return;
  }
  memcpy(s_rx_buf + off, data_t->value->data, len);
  s_rx_len += len;

  if (s_rx_len == s_rx_total) {
    // Destroy before decode: the guard above counted these bytes as free.
    if (s_image) {
      gbitmap_destroy(s_image);
      s_image = NULL;
    }
    decode_frame(s_rx_buf, s_rx_total);
    APP_LOG(APP_LOG_LEVEL_INFO, "Decoded composite (%d bytes), heap now %d",
            (int)s_rx_total, (int)heap_bytes_free());
    rx_reset();

    if (s_image) {
      s_decode_retry = false;
    } else if (!s_decode_retry) {
      // One immediate retry per run of failures; after that the minute tick
      // re-requests, so a bad image never loops.
      APP_LOG(APP_LOG_LEVEL_ERROR, "Decode failed");
      s_decode_retry = true;
      request_images(true);
    }
    redraw();
  }
}

// ==== AppMessage: inbox =====================================================

static void inbox_received_callback(DictionaryIterator *iter, void *ctx) {
  // ---- Status from the phone ----
  Tuple *zoom_t = dict_find(iter, MESSAGE_KEY_ZOOM_KM);
  Tuple *mode_t = dict_find(iter, MESSAGE_KEY_RADAR_MODE);
  Tuple *busy_t = dict_find(iter, MESSAGE_KEY_BUSY);

  if (zoom_t) {
    int32_t km = zoom_t->value->int32;
    if (s_zoom_km && km != s_zoom_km) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d km", (int)km);
      show_toast(buf);
    }
    s_zoom_km = km;
  }
  if (mode_t) {
    int mode = (int)mode_t->value->int32;
    if (s_radar_mode >= 0 && mode != s_radar_mode) {
      show_toast(mode_name(mode));
    }
    s_radar_mode = mode;
  }
  if (busy_t) {
    int b = (int)busy_t->value->int32;
    set_busy(b == BUSY_WORKING);
    if (b == BUSY_FAILED) {
      show_toast("Update failed");
    }
  }

  // ---- Radar timestamp ----
  // Sent at the phone's commit, so a transfer that dies halfway advances
  // nothing. Ignored while the last decode has failed.
  Tuple *rt_t = dict_find(iter, MESSAGE_KEY_RADAR_TIME);
  if (rt_t && !s_decode_retry) {
    s_radar_time = (time_t)rt_t->value->int32;
  }

  if (zoom_t || mode_t || busy_t || rt_t) {
    redraw();
  }

  rx_image(iter);   // last: its early returns skip nothing
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped: %d", (int)reason);
  // A dropped chunk breaks the offset chain while the phone still commits the
  // transfer, so ask for the frame again past the phone's unchanged-skip.
  bool mid_transfer = s_rx_buf != NULL;
  rx_reset();
  if (mid_transfer) {
    request_images(true);
  }
}

static void outbox_failed_callback(DictionaryIterator *iter,
                                   AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed: %d", (int)reason);
  if (dict_find(iter, MESSAGE_KEY_CMD)) {
    set_busy(false);
    show_toast("Not sent");
  }
}

// ==== Drawing ===============================================================

#if defined(PBL_ROUND)
static int32_t isqrt32(int32_t v) {
  int32_t r = 0;
  while ((r + 1) * (r + 1) <= v) {
    r++;
  }
  return r;
}

// Visible width of row y on a round display: the chord of the inscribed circle.
static int16_t chord_w(int y) {
  int32_t d = 2 * y - (PBL_DISPLAY_HEIGHT - 1);
  int32_t v = (int32_t)PBL_DISPLAY_WIDTH * PBL_DISPLAY_WIDTH - d * d;
  return v > 0 ? (int16_t)isqrt32(v) : 0;
}
#endif

// Text with a 1 px halo so it reads over any part of the map: eight outline
// copies, then the glyphs.
static void draw_halo_text(GContext *ctx, const char *s, GFont font, GRect r) {
  graphics_context_set_text_color(ctx, GColorWhite);
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (!dx && !dy) continue;
      GRect o = GRect(r.origin.x + dx, r.origin.y + dy, r.size.w, r.size.h);
      graphics_draw_text(ctx, s, font, o, GTextOverflowModeTrailingEllipsis,
                         GTextAlignmentCenter, NULL);
    }
  }
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, s, font, r, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

// ---- Bluetooth badge -------------------------------------------------------
// The Bluetooth rune with a red slash, drawn from line segments, shown only
// while the phone is unreachable. Always drawn, overlays or not: it explains
// why the map has stopped updating.
#define BT_HALF        6
#define BT_HEIGHT      20
#define BT_INSET       3
#define BT_SLASH_HALF  7
#define BT_SLASH_OVER  2
#define BT_BOX_H       (BT_HEIGHT + 2 * BT_SLASH_OVER + 1)

static void draw_bt_badge(GContext *ctx, GRect bounds) {
  int16_t left = bounds.origin.x + BT_INSET;
  int16_t top  = bounds.origin.y + BT_INSET;
#if defined(PBL_ROUND)
  // No corner on a round display: sit at 9 o'clock, inside the bezel.
  top = bounds.origin.y + (bounds.size.h - BT_BOX_H) / 2;
  int16_t cw = chord_w(top - 1);
  int16_t cb = chord_w(top + BT_BOX_H);
  if (cb < cw) cw = cb;
  left = bounds.origin.x + (bounds.size.w - cw) / 2 + BT_INSET;
#endif
  int16_t cx = left + BT_SLASH_HALF;
  int16_t y0 = top + BT_SLASH_OVER;
  int16_t q  = BT_HEIGHT / 4;
  GPoint p[6] = {
    GPoint(cx - BT_HALF, y0 + q),
    GPoint(cx + BT_HALF, y0 + 3 * q),
    GPoint(cx,           y0 + BT_HEIGHT),
    GPoint(cx,           y0),
    GPoint(cx + BT_HALF, y0 + q),
    GPoint(cx - BT_HALF, y0 + 3 * q),
  };
  GPoint s0 = GPoint(cx - BT_SLASH_HALF, y0 + BT_HEIGHT + BT_SLASH_OVER);
  GPoint s1 = GPoint(cx + BT_SLASH_HALF, y0 - BT_SLASH_OVER);

  // Halo pass, then glyph pass, so a halo never paints over a glyph.
  for (int pass = 0; pass < 2; pass++) {
    graphics_context_set_stroke_color(ctx, pass ? GColorBlack : GColorWhite);
    graphics_context_set_stroke_width(ctx, pass ? 1 : 3);
    for (int dx = 0; dx < 2; dx++) {
      for (int i = 0; i < 5; i++) {
        graphics_draw_line(ctx, GPoint(p[i].x + dx, p[i].y),
                           GPoint(p[i + 1].x + dx, p[i + 1].y));
      }
    }
  }
  for (int pass = 0; pass < 2; pass++) {
    graphics_context_set_stroke_color(ctx, pass ? GColorRed : GColorWhite);
    graphics_context_set_stroke_width(ctx, pass ? 1 : 3);
    graphics_draw_line(ctx, s0, s1);
  }
}

// ---- Status line -----------------------------------------------------------
// "250 km | 3 min", or "Loading..." while the user waits on the phone.
static void format_status(char *buf, size_t size) {
  if (s_busy) {
    snprintf(buf, size, "Loading...");
    return;
  }
  char age[16];
  if (!s_radar_time) {
    snprintf(age, sizeof(age), "--");
  } else {
    // The phone's clock dates the radar, so clamp a small skew rather than
    // print a negative age.
    int mins = (int)((time(NULL) - s_radar_time) / 60);
    if (mins < 0) mins = 0;
    if (mins == 0) {
      snprintf(age, sizeof(age), "now");
    } else {
      snprintf(age, sizeof(age), "%d min", mins);
    }
  }
  if (s_zoom_km) {
    snprintf(buf, size, "%d km | %s", (int)s_zoom_km, age);
  } else {
    snprintf(buf, size, "%s", age);
  }
}

static void draw_status(GContext *ctx, GRect bounds) {
  char buf[32];
  format_status(buf, sizeof(buf));
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  // Round displays lift it off the bezel, where the chord is too narrow.
  int16_t y = bounds.size.h - PBL_IF_ROUND_ELSE(44, 24);
  draw_halo_text(ctx, buf, font, GRect(0, y, bounds.size.w, 22));
}

// ---- Toast -----------------------------------------------------------------
static void draw_toast(GContext *ctx, GRect bounds) {
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  int16_t w = bounds.size.w - PBL_IF_ROUND_ELSE(60, 24);
  int16_t h = 34;
  GRect box = GRect((bounds.size.w - w) / 2, (bounds.size.h - h) / 2, w, h);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, box, 6, GCornersAll);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_round_rect(ctx, box, 6);
  graphics_context_set_text_color(ctx, GColorBlack);
  GRect text = GRect(box.origin.x + 4, box.origin.y + 1, box.size.w - 8, h - 4);
  graphics_draw_text(ctx, s_toast, font, text, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

static void map_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  if (s_image) {
    graphics_draw_bitmap_in_rect(ctx, s_image, bounds);
  } else {
    graphics_context_set_fill_color(ctx, GColorLightGray);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  }

  if (s_overlays) {
    // Center marker: white ring, red dot.
    GPoint c = grect_center_point(&bounds);
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, c, 4);
    graphics_context_set_fill_color(ctx, GColorRed);
    graphics_fill_circle(ctx, c, 2);
  }

  if (!connection_service_peek_pebble_app_connection()) {
    draw_bt_badge(ctx, bounds);
  }

  // The status line also shows with overlays off while a fetch is pending,
  // so a button press always gets visible feedback.
  if (s_overlays || s_busy) {
    draw_status(ctx, bounds);
  }

  if (s_toast[0]) {
    draw_toast(ctx, bounds);
  }
}

// ==== Window ================================================================

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_map_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_map_layer, map_update_proc);
  layer_add_child(root, s_map_layer);
}

static void window_unload(Window *window) {
  layer_destroy(s_map_layer);
  s_map_layer = NULL;
  if (s_image) {
    gbitmap_destroy(s_image);
    s_image = NULL;
  }
  rx_reset();
}

// ==== Application lifecycle =================================================

static void init(void) {
  s_window = window_create();
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = connection_callback,
  });

  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  // Outbox: one small command or heartbeat at a time.
  app_message_open(app_message_inbox_size_maximum(), 64);

  // The busy flag starts set: the phone's 'ready' handler kicks off the first
  // fetch and clears it when the frame lands. The timeout covers a phone that
  // never connects.
  set_busy(true);
  backlight_hold();
}

static void deinit(void) {
  if (s_light_timer) {
    app_timer_cancel(s_light_timer);
    s_light_timer = NULL;
  }
  light_enable(false);   // never leave the light forced on after exit
  tick_timer_service_unsubscribe();
  connection_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
