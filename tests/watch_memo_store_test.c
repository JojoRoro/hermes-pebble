// Drive the production watch memo store (memo_store.h) against simulated persistent storage,
// microphone, speaker, outbox and phone. Uses tests/memo_stub/pebble.h, not the SDK.
#include <pebble.h>
#include <assert.h>
#include <math.h>
#include "../src/c/protocol.h"

// The test clock also dates memos, so the list order is reproducible.
static uint64_t clock_ms = 1700000000000ull;
#define time(x) ((time_t)(clock_ms / 1000u))

#define UI_HEADER_HEIGHT 24
#define UI_HINT_HEIGHT 20
#define UI_MARGIN 8
#define UI_ACCENT GColorCobaltBlue

typedef struct { uint32_t capture_id, generation, total_bytes; uint8_t status, error_code; } InboundTransfer;
static struct { bool active; } s_outbound;
static struct { uint8_t phase; } s_audio;
static bool s_exiting, s_app_message_open = true;
static uint32_t s_capture_counter = 700u, s_transfer_counter = 9000u;
static bool storage_next_capture_id(uint32_t *capture_id) { *capture_id = ++s_capture_counter; return true; }
static uint32_t next_transfer_id(void) { return ++s_transfer_counter; }

struct DictionaryIterator {
  uint32_t kind, transfer, capture, offset, total, crc, flags, version;
  uint8_t payload[1024];
  uint16_t length;
};
static bool read_u32(DictionaryIterator *iter, uint8_t key, uint32_t *value, bool required) {
  (void)required; (void)key;
  *value = iter->transfer;
  return true;
}

static bool audio_dictation_active(void);
#include "../src/c/memo_watch.h"
#include "../src/c/memo_store.h"
static uint32_t quiet_calls;
static void memo_quiet_speaker(void) { quiet_calls++; }
static bool audio_dictation_active(void) { return wm_audio_busy(); }
// The app wires up the rest of the voice memo screen; this test only needs its helpers.
static void *const memo_screen_entry_points[] = { (void *)memo_subtitle, (void *)memo_open, (void *)memo_receipt, (void *)memo_on_failed, (void *)memo_on_sent };

// ---- Persistent storage: small map, 256-byte values, write and read counters -------------
#define MAP_SIZE 4000
static struct { uint32_t key; uint16_t length; uint8_t data[256]; bool used; } map[MAP_SIZE];
static uint32_t map_writes, map_deletes, map_tombstones;
static int map_fail_after = -1;
static int find(uint32_t key) {
  for (int i = 0; i < MAP_SIZE; i++) if (map[i].used && map[i].key == key) return i;
  return -1;
}
bool persist_exists(uint32_t key) { return find(key) >= 0; }
int persist_read_data(uint32_t key, void *buffer, size_t size) {
  int at = find(key);
  if (at < 0) return -1;
  size_t count = size < map[at].length ? size : map[at].length;
  memcpy(buffer, map[at].data, count);
  return (int)count;
}
int persist_write_data(uint32_t key, const void *data, size_t size) {
  assert(size > 0u && size <= 256u); // PERSIST_DATA_MAX_LENGTH: the firmware truncates anything longer.
  if (map_fail_after == 0) return -1;
  if (map_fail_after > 0) map_fail_after--;
  map_writes++;
  int at = find(key);
  for (int i = 0; at < 0 && i < MAP_SIZE; i++) if (!map[i].used) at = i;
  assert(at >= 0);
  map[at].key = key; map[at].used = true; map[at].length = (uint16_t)size;
  memcpy(map[at].data, data, size);
  return (int)size;
}
int persist_delete(uint32_t key) {
  int at = find(key);
  map_deletes++;
  if (at >= 0) map[at].used = false;
  else map_tombstones++;
  return 0;
}
size_t persist_get_max_size(void) { return 1024u * 1024u; }
int32_t persist_read_int(uint32_t key) { (void)key; return 0; }
int persist_write_int(uint32_t key, int32_t value) { (void)key; (void)value; return 4; }
static uint32_t audio_chunks(void) {
  uint32_t n = 0u;
  for (int i = 0; i < MAP_SIZE; i++) if (map[i].used && map[i].key < WM_KEY_INDEX && map[i].key >= WM_KEY_CHUNK) n++;
  return n;
}

// ---- Simulated platform ------------------------------------------------------------------
static MemoFirmwareMicHandlers mic;
static bool mic_active;
static void *firmware_table[683];
const PebbleProcessInfo __pbl_app_info = {
  .header = "PBLAPP", .sym_table_addr = offsetof(PebbleProcessInfo, loader_table), .loader_table = firmware_table,
};
WatchInfoVersion watch_info_get_firmware_version(void) { return (WatchInfoVersion){ 4, 43, 0 }; }
static uint32_t firmware_mic_start(MemoFirmwareMicHandlers handlers, void *context) {
  (void)context; assert(!mic_active); mic = handlers; mic_active = true; return 0u;
}
static void firmware_mic_stop(void) { mic_active = false; }

void layer_mark_dirty(Layer *layer) { (void)layer; }
Layer *layer_create(GRect frame) { (void)frame; return (Layer *)malloc(1); }
void layer_destroy(Layer *layer) { free(layer); }
void layer_set_update_proc(Layer *layer, LayerUpdateProc proc) { (void)layer; (void)proc; }
void layer_add_child(Layer *parent, Layer *child) { (void)parent; (void)child; }
GRect layer_get_bounds(const Layer *layer) { (void)layer; return GRect(0, 0, 200, 228); }
Window *window_create(void) { return (Window *)malloc(1); }
void window_destroy(Window *window) { free(window); }
Layer *window_get_root_layer(const Window *window) { (void)window; return NULL; }
void window_set_click_config_provider(Window *window, ClickConfigProvider provider) { (void)window; (void)provider; }
void window_set_window_handlers(Window *window, WindowHandlers handlers) { (void)window; (void)handlers; }
void window_stack_push(Window *window, bool animated) { (void)window; (void)animated; }
Window *window_stack_pop(bool animated) { (void)animated; return NULL; }
void window_single_click_subscribe(ButtonId button, ClickHandler handler) { (void)button; (void)handler; }
void window_single_repeating_click_subscribe(ButtonId button, uint16_t ms, ClickHandler handler) { (void)button; (void)ms; (void)handler; }
size_t heap_bytes_free(void) { return 60000u; }
uint16_t time_ms(time_t *tloc, uint16_t *out_ms) {
  if (tloc) *tloc = (time_t)(clock_ms / 1000u);
  if (out_ms) *out_ms = (uint16_t)(clock_ms % 1000u);
  return (uint16_t)(clock_ms % 1000u);
}
void vibes_short_pulse(void) {}
GFont fonts_get_system_font(const char *key) { return (GFont)key; }
void graphics_context_set_fill_color(GContext *ctx, GColor color) { (void)ctx; (void)color; }
void graphics_context_set_stroke_color(GContext *ctx, GColor color) { (void)ctx; (void)color; }
void graphics_context_set_text_color(GContext *ctx, GColor color) { (void)ctx; (void)color; }
void graphics_fill_rect(GContext *ctx, GRect rect, uint16_t radius, GCornerMask corners) { (void)ctx; (void)rect; (void)radius; (void)corners; }
void graphics_fill_circle(GContext *ctx, GPoint p, uint16_t radius) { (void)ctx; (void)p; (void)radius; }
void graphics_draw_round_rect(GContext *ctx, GRect rect, uint16_t radius) { (void)ctx; (void)rect; (void)radius; }
static char drawn[2048];
void graphics_draw_text(GContext *ctx, const char *text, GFont font, GRect box, GTextOverflowMode mode, GTextAlignment alignment, void *attributes) {
  (void)ctx; (void)font; (void)box; (void)mode; (void)alignment; (void)attributes;
  strncat(drawn, text, sizeof(drawn) - strlen(drawn) - 2u);
  strcat(drawn, "|");
}
static const char *draw(void) { drawn[0] = '\0'; wm_draw(s_wm_layer, NULL); return drawn; }

// Timers: fired by callback so a test can run deletes, playback or sending on their own.
#define TIMERS 8
static struct { AppTimerCallback callback; uint32_t ms; } timers[TIMERS];
AppTimer *app_timer_register(uint32_t ms, AppTimerCallback callback, void *data) {
  (void)data;
  for (int i = 0; i < TIMERS; i++) if (!timers[i].callback) { timers[i].callback = callback; timers[i].ms = ms; return (AppTimer *)&timers[i]; }
  assert(!"too many timers");
  return NULL;
}
void app_timer_cancel(AppTimer *timer) { ((__typeof__(&timers[0]))timer)->callback = NULL; }
static bool fire(AppTimerCallback callback) {
  for (int i = 0; i < TIMERS; i++) if (timers[i].callback == callback) { timers[i].callback = NULL; callback(NULL); return true; }
  return false;
}

// Outbox and phone.
static DictionaryIterator dict;
static bool connected = true, in_flight;
bool connection_service_peek_pebble_app_connection(void) { return connected; }
AppMessageResult app_message_outbox_begin(DictionaryIterator **iterator) {
  if (in_flight) return APP_MSG_BUSY;
  *iterator = &dict; dict.length = 0u; return APP_MSG_OK;
}
AppMessageResult app_message_outbox_send(void) { in_flight = true; return APP_MSG_OK; }
DictionaryResult dict_write_uint8(DictionaryIterator *iter, uint32_t key, uint8_t value) {
  if (key == HERMES_KEY_PROTOCOL_VERSION) iter->version = value;
  else if (key == HERMES_KEY_MESSAGE_KIND) iter->kind = value;
  else if (key == HERMES_KEY_FLAGS) iter->flags = value;
  return DICT_OK;
}
DictionaryResult dict_write_uint32(DictionaryIterator *iter, uint32_t key, uint32_t value) {
  if (key == HERMES_KEY_TRANSFER_ID) iter->transfer = value;
  else if (key == HERMES_KEY_CAPTURE_ID) iter->capture = value;
  else if (key == HERMES_KEY_PAGE_OFFSET) iter->offset = value;
  else if (key == HERMES_KEY_TOTAL_BYTES) iter->total = value;
  else if (key == HERMES_KEY_CONVERSATION_GENERATION) iter->crc = value;
  return DICT_OK;
}
DictionaryResult dict_write_data(DictionaryIterator *iter, uint32_t key, const uint8_t *data, uint16_t size) {
  (void)key; assert(size <= sizeof(iter->payload));
  memcpy(iter->payload, data, size); iter->length = size;
  return DICT_OK;
}

static uint8_t phone[600000], phone_header[MEMO_HEADER];
static uint32_t phone_max;
static bool phone_end;
static void phone_ack(void) {
  assert(in_flight && dict.kind == HERMES_KIND_MEMO_STREAM && (dict.flags & HERMES_FLAG_MEMO_STORED));
  in_flight = false;
  if (dict.flags & HERMES_FLAG_MEMO_END) {
    assert(dict.length == MEMO_HEADER && dict.total == dict.offset && dict.total == phone_max);
    memcpy(phone_header, dict.payload, MEMO_HEADER);
    phone_end = true;
  } else {
    assert(dict.total == 0u && dict.length > 0u && dict.length <= WM_SEND_CHUNKS * WM_CHUNK && dict.offset <= phone_max);
    memcpy(phone + dict.offset, dict.payload, dict.length);
    if (dict.offset + dict.length > phone_max) phone_max = dict.offset + dict.length;
  }
  assert(wm_on_sent(&dict));
}
static void phone_reset(void) { phone_max = 0u; phone_end = false; }
static void phone_receipt(uint32_t capture, uint32_t crc, uint32_t total, bool ok) {
  InboundTransfer t = { .capture_id = capture, .generation = crc, .total_bytes = total,
    .status = ok ? HERMES_STATUS_NOTE_SAVED : HERMES_STATUS_FAILED, .error_code = ok ? 0u : HERMES_ERROR_MALFORMED };
  assert(wm_receipt(&t));
}

// Speaker.
static bool speaker_open, speaker_mute, speaker_closed, speaker_is_quiet;
static uint32_t speaker_accept = 100000u, speaker_bytes, speaker_stops;
static int64_t speaker_sum;
static int speaker_peak;
static SpeakerFinishedCallback speaker_finish;
bool speaker_stream_open(SpeakerPcmFormat format, uint8_t volume) {
  assert(format == SpeakerPcmFormat_8kHz_8bit && volume <= 100u && !speaker_open);
  speaker_open = true; speaker_closed = false; speaker_bytes = 0u; speaker_sum = 0; speaker_peak = 0;
  return true;
}
uint32_t speaker_stream_write(const void *data, uint32_t count) {
  assert(speaker_open && !speaker_closed && count > 0u);
  uint32_t accepted = count < speaker_accept ? count : speaker_accept;
  for (uint32_t i = 0; i < accepted; i++) {
    int v = ((const int8_t *)data)[i];
    speaker_sum += v;
    if (abs(v) > speaker_peak) speaker_peak = abs(v);
  }
  speaker_bytes += accepted;
  return accepted;
}
void speaker_stream_close(void) { assert(speaker_open); speaker_closed = true; }
void speaker_stop(void) { speaker_open = false; speaker_stops++; }
SpeakerStatus speaker_get_status(void) { return speaker_open ? SpeakerStatusPlaying : SpeakerStatusIdle; }
void speaker_set_finish_callback(SpeakerFinishedCallback cb, void *ctx) { (void)ctx; speaker_finish = cb; }
bool speaker_is_muted(void) { return speaker_mute; }
bool quiet_time_is_active(void) { return speaker_is_quiet; }

// ---- Helpers -----------------------------------------------------------------------------
static uint32_t phase;
static void speak(uint32_t frames) {
  int16_t frame[320];
  for (uint32_t f = 0; f < frames && s_wm->rec_slot >= 0; f++) {
    for (int i = 0; i < 320; i++, phase++) frame[i] = (int16_t)(200 + 2000.0 * sin(phase * 0.17) * (1.0 + sin(phase * 0.0011)));
    clock_ms += 20u;
    mic.data(frame, 320u, NULL);
  }
}
static void boot(void) {
  if (s_wm_layer) wm_window_unload(NULL);
  memset(timers, 0, sizeof(timers));
  wm_init();
  assert(s_wm);
  wm_open();
  wm_window_load(NULL);
}
static void record(uint32_t frames) {
  s_wm->selected = 0u;
  wm_select(NULL, NULL);
  assert(s_wm->rec_slot >= 0 && s_wm->view == WM_VIEW_RECORD && mic_active && persist_exists(WM_KEY_ACTIVE));
  speak(frames);
}
static const WmEntry *newest(uint8_t *slot) {
  uint8_t order[WM_SLOTS];
  assert(wm_order(order) > 0u);
  if (slot) *slot = order[0];
  return &s_wm->entries[order[0]];
}
static void send_all(void) {
  for (int i = 0; i < 20000 && in_flight; i++) phone_ack();
}
static void open_newest(void) {
  s_wm->view = WM_VIEW_LIST;
  s_wm->selected = 1u;
  wm_select(NULL, NULL);
  assert(s_wm->view == WM_VIEW_ITEM);
}
static void run_deletes(int limit) {
  for (int i = 0; i < limit && fire(wm_work_tick); i++) {}
}

int main(void) {
  firmware_table[MEMO_MIC_SLOT_START] = (void *)firmware_mic_start;
  firmware_table[MEMO_MIC_SLOT_STOP] = (void *)firmware_mic_stop;
  (void)memo_screen_entry_points;
  assert(memo_init());
  boot();
  assert(s_wm->capacity == WM_MAX_STORE);
  assert(strstr(draw(), "Record new memo") && strstr(drawn, "0 of 512 KB"));

  // Recording: the phone is away, so the memo stays "Only on watch". Values never exceed 256 bytes
  // (the stub asserts) and only one is written per audio frame.
  connected = false;
  record(1500u); // 30 s
  assert(strstr(draw(), "Recording"));
  uint32_t writes_before = map_writes;
  speak(1u);
  assert(map_writes - writes_before <= 1u);
  uint32_t expected_chunks = 30u * MEMO_BYTES_PER_SECOND / WM_CHUNK;
  assert(audio_chunks() >= expected_chunks);
  wm_select(NULL, NULL);
  assert(s_wm->rec_slot < 0 && !mic_active && !persist_exists(WM_KEY_ACTIVE) && s_wm->view == WM_VIEW_LIST);
  uint8_t slot;
  const WmEntry *e = newest(&slot);
  uint32_t length = e->length, capture = e->capture, crc = e->crc;
  assert(length >= 30u * MEMO_BYTES_PER_SECOND - 100u && length <= 31u * MEMO_BYTES_PER_SECOND);
  assert(wm_chunks(length) == audio_chunks());
  // The checksum matches what is really in storage.
  static uint8_t stored[600000];
  for (uint32_t at = 0u; at < length; at += WM_CHUNK) {
    uint32_t count = length - at < WM_CHUNK ? length - at : WM_CHUNK;
    assert(persist_read_data(wm_key(slot, at / WM_CHUNK), stored + at, count) == (int)count);
  }
  assert(memo_crc32(stored, length) == crc);
  assert(strstr(draw(), "Only on watch") && strstr(drawn, "free"));
  assert(s_wm->send_state == WM_SEND_IDLE && !in_flight);
  assert(wm_used() == length);

  // A restart finds it again.
  wm_shutdown();
  boot();
  e = newest(&slot);
  assert(e->capture == capture && e->length == length && e->crc == crc);
  assert(strstr(wm_subtitle(), "1 saved"));

  // The phone returns: the memo goes out with visible progress, a NACK backs off and resends.
  connected = true;
  wm_connection();
  assert(in_flight && dict.offset == 0u && s_wm->send_slot == (int8_t)slot);
  uint32_t resend = dict.transfer;
  in_flight = false;
  assert(wm_on_failed(&dict));
  assert(fire(wm_send_timer_tick) && in_flight && dict.offset == 0u && dict.transfer != resend);
  for (int i = 0; i < 40; i++) phone_ack();
  assert(in_flight && strstr(draw(), "Sending") && strstr(wm_subtitle(), "Sending"));
  // An outage pauses it; the link event carries on from the same place.
  uint32_t paused_at = dict.offset;
  in_flight = false;
  connected = false;
  assert(wm_on_failed(&dict));
  assert(fire(wm_send_timer_tick) && !in_flight);
  connected = true;
  wm_connection();
  assert(in_flight && dict.offset == paused_at);
  send_all();
  assert(phone_end && phone_max == length && memcmp(phone, stored, length) == 0);
  assert(memcmp(phone_header, "HVM1", 4) == 0 && s_wm->send_state == WM_SEND_RECEIPT);
  uint32_t samples = (uint32_t)phone_header[8] | (uint32_t)phone_header[9] << 8 | (uint32_t)phone_header[10] << 16;
  assert(samples == length * 2u || samples == length * 2u - 1u);
  assert(strstr(draw(), "Confirming"));
  // No receipt: the end message goes again, then the phone answers.
  assert(fire(wm_send_timer_tick) && in_flight && (dict.flags & HERMES_FLAG_MEMO_END));
  phone_end = false;
  phone_ack();
  // A bad copy on the phone restarts the memo once.
  phone_receipt(capture, crc, length, false);
  assert(in_flight && dict.offset == 0u && !(s_wm->entries[slot].flags & WM_FLAG_ON_PHONE));
  phone_reset();
  send_all();
  phone_receipt(capture, crc, length, true);
  assert((s_wm->entries[slot].flags & WM_FLAG_ON_PHONE) && s_wm->send_slot < 0 && !s_wm->send_buffer);
  assert(strstr(draw(), "On phone"));
  // Other memos' receipts are not ours; "on phone" survives a restart and is not sent again.
  InboundTransfer other = { .capture_id = 1u };
  assert(!wm_receipt(&other));
  wm_shutdown();
  boot();
  assert(!in_flight);
  wm_connection();
  assert(!in_flight && (newest(&slot)->flags & WM_FLAG_ON_PHONE));

  // Playback: muted and Quiet Time refuse, then the speaker gets every sample, with partial writes.
  open_newest();
  assert(strstr(draw(), "SELECT: play"));
  speaker_mute = true;
  wm_select(NULL, NULL);
  assert(!s_wm->player && strstr(draw(), "muted"));
  speaker_mute = false;
  speaker_is_quiet = true;
  wm_select(NULL, NULL);
  assert(!s_wm->player && strstr(draw(), "Quiet Time"));
  speaker_is_quiet = false;
  speaker_accept = 300u;
  uint32_t quiet_before = quiet_calls;
  wm_select(NULL, NULL);
  assert(s_wm->player && speaker_open && quiet_calls == quiet_before + 1u && strstr(draw(), "SELECT: stop"));
  assert(audio_dictation_active()); // Reply audio stays out while the memo plays.
  speaker_accept = 100000u;
  for (int i = 0; i < 2000 && !speaker_closed; i++) assert(fire(wm_work_tick));
  if (!speaker_closed || speaker_bytes != length * 2u) printf("closed=%d bytes=%u samples=%u len=%u\n", speaker_closed, speaker_bytes, samples, length);
  assert(speaker_closed && speaker_bytes == length * 2u); // An odd last sample plays a silent extra one.
  assert(speaker_peak > 40); // Loud enough on an 8-bit speaker...
  assert(llabs(speaker_sum) < (int64_t)samples * 3); // ...and without the microphone's DC offset.
  speaker_finish(SpeakerFinishReasonDone, NULL);
  speaker_open = false;
  assert(!s_wm->player && s_wm->note[0] == '\0');
  // SELECT stops it early; so does leaving the screen; a preempted speaker says so.
  wm_select(NULL, NULL);
  assert(s_wm->player);
  wm_select(NULL, NULL);
  assert(!s_wm->player && !speaker_open && speaker_finish == NULL);
  wm_select(NULL, NULL);
  wm_window_disappear(NULL);
  assert(!s_wm->player && !speaker_open);
  wm_select(NULL, NULL);
  speaker_finish(SpeakerFinishReasonPreempted, NULL);
  speaker_open = false;
  assert(!s_wm->player && strstr(draw(), "interrupted"));

  // Delete asks first, then frees one value per tick, and a restart resumes it.
  uint32_t used_before = wm_used();
  wm_down(NULL, NULL);
  assert(strstr(draw(), "phone keeps") && strstr(drawn, "DOWN: delete"));
  wm_up(NULL, NULL); // Keep.
  assert(!s_wm->confirm_delete && !(s_wm->entries[slot].flags & WM_FLAG_DELETING));
  wm_down(NULL, NULL);
  wm_down(NULL, NULL);
  assert((s_wm->entries[slot].flags & WM_FLAG_DELETING) && s_wm->view == WM_VIEW_LIST);
  uint32_t chunks_before = audio_chunks();
  assert(fire(wm_work_tick) && audio_chunks() == chunks_before - 1u);
  assert(strstr(draw(), "Deleting") && wm_used() < used_before);
  for (int i = 0; i < 20; i++) assert(fire(wm_work_tick));
  wm_shutdown();
  boot();
  assert(s_wm->entries[slot].capture != 0u && (s_wm->entries[slot].flags & WM_FLAG_DELETING));
  uint32_t tombstones_before = map_tombstones;
  run_deletes(5000);
  assert(audio_chunks() == 0u && s_wm->entries[slot].capture == 0u && wm_used() == 0u);
  assert(map_tombstones == tombstones_before); // Values already gone are not deleted twice.
  wm_shutdown();
  boot();
  assert(strstr(draw(), "0 of 512 KB"));

  // Deleting a memo while it is being sent stops the send, and a late ACK does no harm.
  connected = true;
  record(100u);
  wm_select(NULL, NULL);
  assert(in_flight);
  open_newest();
  wm_down(NULL, NULL);
  assert(strstr(draw(), "not on the phone"));
  wm_down(NULL, NULL);
  assert(s_wm->send_slot < 0);
  phone_reset();
  phone_ack();
  assert(!in_flight);
  run_deletes(5000);
  assert(audio_chunks() == 0u);

  // Sending waits for recording; the recording's own slot is never chosen.
  connected = false;
  record(100u);
  wm_select(NULL, NULL);
  connected = true;
  record(100u);
  wm_connection();
  assert(!in_flight && s_wm->send_slot != s_wm->rec_slot);
  wm_select(NULL, NULL);
  assert(in_flight && s_wm->send_slot >= 0);
  uint8_t first = (uint8_t)s_wm->send_slot;
  send_all();
  phone_receipt(s_wm->entries[first].capture, s_wm->entries[first].crc, s_wm->entries[first].length, true);
  assert(in_flight && s_wm->send_slot != (int8_t)first); // Next in the queue.
  uint8_t second = (uint8_t)s_wm->send_slot;
  phone_reset();
  send_all();
  phone_receipt(s_wm->entries[second].capture, s_wm->entries[second].crc, s_wm->entries[second].length, true);
  assert(!in_flight);
  // A memo the phone never accepts shows as failed, and UP or time retries it.
  connected = false;
  record(100u);
  wm_select(NULL, NULL);
  newest(&slot);
  connected = true;
  wm_connection();
  for (int i = 0; i <= (int)MEMO_MAX_RETRIES; i++) {
    assert(in_flight);
    in_flight = false;
    assert(wm_on_failed(&dict));
    if (i < (int)MEMO_MAX_RETRIES) assert(fire(wm_send_timer_tick));
  }
  assert(!in_flight && (s_wm->failed & (1u << slot)) && strstr(draw(), "Send failed"));
  open_newest();
  wm_up(NULL, NULL);
  assert(in_flight && s_wm->failed == 0u);
  phone_reset();
  send_all();
  phone_receipt(s_wm->entries[slot].capture, s_wm->entries[slot].crc, s_wm->entries[slot].length, true);
  wm_back(NULL, NULL);
  assert(s_wm->view == WM_VIEW_LIST);

  // Too short to keep: nothing is listed and its values go away.
  uint32_t chunks = audio_chunks();
  record(10u);
  wm_select(NULL, NULL);
  assert(strstr(draw(), "Too short"));
  run_deletes(100);
  assert(audio_chunks() == chunks);

  // Time limit.
  connected = false;
  record(WM_MAX_SECONDS * 50u + 50u);
  assert(s_wm->rec_slot < 0 && !mic_active && strstr(draw(), "Time limit"));
  e = newest(&slot);
  assert(e->length >= (WM_MAX_SECONDS - 1u) * MEMO_BYTES_PER_SECOND && e->length <= WM_MAX_SECONDS * MEMO_BYTES_PER_SECOND + 256u);
  s_wm->entries[slot].flags |= WM_FLAG_DELETING; // Make room for the rest of the test.
  (void)wm_save_index();
  wm_work_schedule();
  run_deletes(5000);
  assert(s_wm->entries[slot].capture == 0u);
  // Storage refusing a write keeps what was saved, with a checksum that matches it.
  map_fail_after = 20;
  record(1000u);
  assert(s_wm->rec_slot < 0 && !mic_active);
  e = newest(&slot);
  assert(e->length == 19u * WM_CHUNK && strstr(draw(), "refused")); // The marker took one write.
  map_fail_after = -1;
  for (uint32_t at = 0u; at < e->length; at += WM_CHUNK) assert(persist_read_data(wm_key(slot, at / WM_CHUNK), stored + at, WM_CHUNK) == WM_CHUNK);
  assert(memo_crc32(stored, e->length) == e->crc);
  // A full store stops recording with a buzz, and then refuses to start.
  s_wm->capacity = wm_used() + 2u * MEMO_BYTES_PER_SECOND;
  record(1000u);
  assert(s_wm->rec_slot < 0 && strstr(draw(), "full"));
  s_wm->selected = 0u;
  wm_select(NULL, NULL);
  assert(s_wm->rec_slot < 0 && !mic_active && strstr(draw(), "full"));
  s_wm->capacity = WM_MAX_STORE;

  // Leaving the screen ends a recording and saves it; so does closing Hermes.
  uint8_t count_order[WM_SLOTS];
  uint8_t before_count = wm_order(count_order);
  record(100u);
  wm_window_disappear(NULL);
  assert(s_wm->rec_slot < 0 && wm_order(count_order) == before_count + 1u);
  record(100u);
  wm_shutdown();
  assert(!mic_active);
  boot();
  assert(wm_order(count_order) == before_count + 2u);

  // A crash mid-recording leaves values and the marker: they are removed, not listed.
  uint8_t free_slot = 0u;
  while (s_wm->entries[free_slot].capture != 0u) free_slot++;
  for (uint32_t i = 0u; i < 50u; i++) { uint8_t junk[WM_CHUNK] = { 1 }; persist_write_data(wm_key(free_slot, i), junk, WM_CHUNK); }
  int32_t marker = free_slot + 1;
  persist_write_data(WM_KEY_ACTIVE, &marker, sizeof(marker));
  uint32_t with_junk = audio_chunks();
  wm_shutdown();
  boot();
  assert(!persist_exists(WM_KEY_ACTIVE));
  assert(s_wm->entries[free_slot].flags & WM_FLAG_DELETING);
  run_deletes(5000);
  assert(audio_chunks() == with_junk - 50u && s_wm->entries[free_slot].capture == 0u);

  // The list holds WM_SLOTS memos, scrolls, and then says it is full.
  while (wm_order(count_order) < WM_SLOTS) { record(100u); wm_select(NULL, NULL); }
  for (int i = 0; i < 40; i++) { wm_down(NULL, NULL); draw(); }
  assert(s_wm->selected == WM_SLOTS);
  for (int i = 0; i < 40; i++) { wm_up(NULL, NULL); draw(); }
  assert(s_wm->selected == 0u);
  wm_select(NULL, NULL);
  assert(s_wm->rec_slot < 0 && strstr(draw(), "full"));

  wm_window_unload(NULL);
  wm_shutdown();
  memo_shutdown();
  assert(s_wm == NULL);
  printf("Watch memos: record into 256-byte values, list, storage use, restart, background send with "
         "progress and retries, receipts, speaker playback with level control, stepwise delete with "
         "resume, full store, time limit, storage refusal, exit and crash recovery passed\n");
  return 0;
}
