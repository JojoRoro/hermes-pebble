#ifndef HERMES_MEMO_WATCH_H
#define HERMES_MEMO_WATCH_H
#include "memo_core.h"
#include "mic_shim.h"

/* Voice memo: record with the local microphone API (firmware 4.43+, see mic_shim.h) in one of
 * three modes, chosen with UP and remembered:
 *  - RAM: the clip stays in watch RAM and is sent after recording in ACK-paced blocks (kind 11),
 *    each acknowledged once the phone has written it; a receipt confirms the checksum.
 *  - Stream: ADPCM leaves while recording (kind 12), so RAM is only a buffer; the phone can play
 *    it live. Up to MEMO_STREAM_SECONDS.
 *  - Watch flash: ADPCM goes into a Data Logging session while recording. The system keeps it in
 *    flash and uploads it by itself, even after Hermes closes. Up to MEMO_LOG_SECONDS. */
#define MEMO_MAX_SECONDS 30u
#define MEMO_STREAM_SECONDS 600u
#define MEMO_LOG_SECONDS 120u
#define MEMO_MIN_SECONDS 3u
#define MEMO_HEAP_RESERVE 8192u
#define MEMO_STREAM_RING 49152u /* About 12 s of slack when Bluetooth falls behind. */
#define MEMO_LOG_RING 16384u
#define MEMO_MIN_RING 4096u
#define MEMO_BLOCK_BYTES 768u
#define MEMO_MIN_BLOCK_BYTES 192u
#define MEMO_STREAM_MIN_SEND 240u /* While recording, send once 60 ms of audio is waiting. */
#define MEMO_MAX_RETRIES 6u
#define MEMO_RECEIPT_TIMEOUT_MS 15000u
#define MEMO_FLIGHT_TIMEOUT_MS 20000u
#define MEMO_YIELD_MS 200u
#define MEMO_OFFLINE_RETRY_MS 1000u
/* Below this share of the elapsed time, frames were dropped and the memo has gaps. */
#define MEMO_MIN_COVERAGE 80u
#define MEMO_COVERAGE_MIN_ELAPSED_MS 2000u
/* Watch flash items: capture u32, data offset u32, type u8 (0 data, 1 end), 0, length u16,
 * 4 reserved bytes, then the payload. The end item carries the HVM1 header, data CRC and length. */
#define MEMO_LOG_TAG 0x48564c31u /* "HVL1" */
#define MEMO_LOG_ITEM 240u
#define MEMO_LOG_HEAD 16u
#define MEMO_LOG_PAYLOAD (MEMO_LOG_ITEM - MEMO_LOG_HEAD)
#define MEMO_LOG_DRAIN_MS 40u

#define MEMO_MODE_RAM 0u
#define MEMO_MODE_STREAM 1u
#define MEMO_MODE_LOG 2u
#define MEMO_MODES 3u

#define MEMO_IDLE 0u
#define MEMO_RECORDING 1u
#define MEMO_READY 2u
#define MEMO_SENDING 3u /* Stream and flash modes: recording has ended, the rest is leaving. */
#define MEMO_WAIT_RECEIPT 4u
#define MEMO_SENT 5u
#define MEMO_FAILED 6u

typedef struct {
  MemoEncoder encoder;
  uint8_t *buffer;
  DataLoggingSessionRef log;
  /* RAM mode: length and crc cover the whole file. Stream and flash: the ADPCM data only. */
  uint32_t capacity, length, crc, capture, offset, final_offset, flight_length, transfer_id, callbacks;
  time_t started_s;
  uint16_t started_ms;
  uint16_t block;
  uint8_t header[MEMO_HEADER];
  uint8_t state, mode, retries, receipt_waits, level;
  bool in_flight, flight_end, confirm_delete, mode_loaded;
  char status[160];
} VoiceMemo;

static VoiceMemo s_memo;
static Window *s_memo_window;
static Layer *s_memo_layer;
static AppTimer *s_memo_timer;

static void memo_pump(void);
static void memo_quiet_speaker(void); // Provided by the app: stop reply playback before recording.
static void memo_redraw(void) { if (s_memo_layer) layer_mark_dirty(s_memo_layer); }

static void memo_set_status(const char *text) {
  snprintf(s_memo.status, sizeof(s_memo.status), "%s", text);
  memo_redraw();
}

static void memo_cancel_timer(void) {
  if (s_memo_timer) app_timer_cancel(s_memo_timer);
  s_memo_timer = NULL;
}

/* Stream mode sends while recording; every mode sends (or saves) in MEMO_SENDING. */
static bool memo_transferring(void) {
  return s_memo.state == MEMO_SENDING || (s_memo.state == MEMO_RECORDING && s_memo.mode == MEMO_MODE_STREAM);
}

static void memo_tick(void *context) {
  (void)context;
  s_memo_timer = NULL;
  if (memo_transferring()) {
    // A lost ACK/NACK callback must not wedge the transfer.
    s_memo.in_flight = false;
    memo_pump();
  } else if (s_memo.state == MEMO_WAIT_RECEIPT) {
    // Resend the final block (RAM) or end message (stream); the phone repeats its receipt.
    if (s_memo.mode == MEMO_MODE_RAM) s_memo.offset = s_memo.final_offset;
    s_memo.state = MEMO_SENDING;
    if (++s_memo.receipt_waits > 3u) {
      s_memo.state = MEMO_FAILED;
      memo_set_status("The phone has not confirmed the memo. Check Hermes Pebble on Android, then press SELECT to resend.");
      return;
    }
    memo_pump();
  }
}

static void memo_schedule(uint32_t ms) {
  memo_cancel_timer();
  s_memo_timer = app_timer_register(ms, memo_tick, NULL);
}

static void memo_release(void) {
  free(s_memo.buffer);
  s_memo.buffer = NULL;
  // Finishing an unfinished session lets the phone keep what arrived.
  if (s_memo.log) data_logging_finish(s_memo.log);
  s_memo.log = NULL;
  s_memo.capacity = s_memo.length = s_memo.offset = 0u;
  s_memo.in_flight = false;
  s_memo.confirm_delete = false;
}

static void memo_load_mode(void) {
  if (s_memo.mode_loaded) return;
  s_memo.mode_loaded = true;
  int32_t mode = persist_read_int(HERMES_STORAGE_KEY_MEMO_MODE);
  s_memo.mode = mode > 0 && mode < (int32_t)MEMO_MODES ? (uint8_t)mode : MEMO_MODE_RAM;
}

static const char *memo_mode_name(void) {
  return s_memo.mode == MEMO_MODE_STREAM ? "Stream" : s_memo.mode == MEMO_MODE_LOG ? "Watch flash" : "RAM";
}

/* Bytes to allocate for a new memo: all free heap above a reserve for the rest of the app,
 * up to a whole RAM-mode memo or a stream/flash ring. */
static uint32_t memo_capacity_for(size_t free_heap) {
  uint32_t capacity = s_memo.mode == MEMO_MODE_STREAM ? MEMO_STREAM_RING :
      s_memo.mode == MEMO_MODE_LOG ? MEMO_LOG_RING : MEMO_HEADER + MEMO_MAX_SECONDS * MEMO_BYTES_PER_SECOND;
  uint32_t minimum = s_memo.mode == MEMO_MODE_RAM ? MEMO_HEADER + MEMO_MIN_SECONDS * MEMO_BYTES_PER_SECOND : MEMO_MIN_RING;
  if (free_heap < MEMO_HEAP_RESERVE + minimum) return 0u;
  if (capacity > free_heap - MEMO_HEAP_RESERVE) capacity = (uint32_t)(free_heap - MEMO_HEAP_RESERVE);
  return capacity;
}

static uint32_t memo_max_seconds(uint32_t capacity) {
  if (s_memo.mode == MEMO_MODE_STREAM) return MEMO_STREAM_SECONDS;
  if (s_memo.mode == MEMO_MODE_LOG) return MEMO_LOG_SECONDS;
  return capacity > MEMO_HEADER ? (capacity - MEMO_HEADER) / MEMO_BYTES_PER_SECOND : 0u;
}

static const char *memo_start_error(MicCaptureStartResult result) {
  switch (result) {
    case MicCaptureStartErrBusy: return "The microphone is busy (dictation or another app). Try again in a moment.";
    case MicCaptureStartErrNotForeground: return "Keep Hermes open on screen to record.";
    case MicCaptureStartErrPermissionDenied:
      return "Microphone not allowed. In the Pebble phone app, open Hermes Pebble and allow Microphone (needs the permission preview), then reopen Hermes.";
    case MicCaptureStartErrUnavailable: return "This watch firmware has no microphone API. Update to 4.43 or newer.";
    case MicCaptureStartErrNoMemory: return "The watch is low on memory. Close Hermes and try again.";
    default: return "The microphone could not start.";
  }
}

/* Percent of the wall-clock recording time covered by the samples that arrived (1-100). The
 * firmware drops frames when its two-frame queue overflows, which leaves gaps in the memo. */
static uint8_t memo_coverage(uint32_t *elapsed_ms_out) {
  time_t now_s = 0;
  uint16_t now_ms = 0u;
  time_ms(&now_s, &now_ms);
  int64_t elapsed = (int64_t)(now_s - s_memo.started_s) * 1000 + (int64_t)now_ms - (int64_t)s_memo.started_ms;
  uint32_t elapsed_ms = elapsed > 0 ? (uint32_t)elapsed : 0u;
  uint32_t captured_ms = (uint32_t)((uint64_t)s_memo.encoder.samples * 1000u / MEMO_RATE);
  if (elapsed_ms_out) *elapsed_ms_out = elapsed_ms;
  if (elapsed_ms == 0u || captured_ms >= elapsed_ms) return 100u;
  uint32_t percent = captured_ms * 100u / elapsed_ms;
  return (uint8_t)(percent < 1u ? 1u : percent);
}

static void memo_assign_capture(void) {
  if (!storage_next_capture_id(&s_memo.capture)) {
    s_memo.capture = (uint32_t)time(NULL) ^ s_memo.crc ^ s_memo.encoder.samples;
    if (s_memo.capture == 0u) s_memo.capture = 1u;
  }
}

static void memo_send_failed(const char *text) {
  if (s_memo.state == MEMO_RECORDING) memo_mic_stop();
  s_memo.state = MEMO_FAILED;
  s_memo.in_flight = false;
  memo_cancel_timer();
  memo_set_status(text);
}

static void memo_log_drain(void);

static void memo_finish_recording(const char *note) {
  if (s_memo.state != MEMO_RECORDING) return;
  memo_mic_stop();
  uint32_t elapsed_ms = 0u;
  s_memo.encoder.coverage = memo_coverage(&elapsed_ms);
  APP_LOG(APP_LOG_LEVEL_INFO, "Memo: %lu samples in %lu ms (%u%%)", (unsigned long)s_memo.encoder.samples,
      (unsigned long)elapsed_ms, (unsigned)s_memo.encoder.coverage);
  s_memo.length = memo_encoder_finish(&s_memo.encoder, (uint32_t)time(NULL));
  s_memo.confirm_delete = false;
  char text[sizeof(s_memo.status)];
  if (elapsed_ms >= MEMO_COVERAGE_MIN_ELAPSED_MS && s_memo.encoder.coverage < MEMO_MIN_COVERAGE) {
    snprintf(text, sizeof(text), "%s%sThe watch missed audio: only %u%% of the time was captured.",
        note, note[0] ? " " : "", (unsigned)s_memo.encoder.coverage);
  } else {
    snprintf(text, sizeof(text), "%s", note);
  }
  if (s_memo.mode != MEMO_MODE_RAM) {
    // Part of the memo has already left: always finish it, however short.
    s_memo.crc = ~s_memo.encoder.crc;
    memo_write_header(&s_memo.encoder, s_memo.header, (uint32_t)time(NULL));
    s_memo.state = MEMO_SENDING;
    s_memo.retries = 0u;
    memo_set_status(text);
    memo_pump();
    return;
  }
  if (s_memo.encoder.samples < MEMO_RATE / 2u) {
    memo_release();
    s_memo.state = MEMO_IDLE;
    memo_set_status("Too short to keep. Press SELECT and speak for at least a second.");
    return;
  }
  // Give back the unused part of the buffer to the rest of the app.
  uint8_t *shrunk = realloc(s_memo.buffer, s_memo.length);
  if (shrunk) { s_memo.buffer = shrunk; s_memo.encoder.bytes = shrunk; }
  s_memo.capacity = s_memo.length;
  s_memo.crc = memo_crc32(s_memo.buffer, s_memo.length);
  memo_assign_capture();
  s_memo.state = MEMO_READY;
  memo_set_status(text);
}

static void memo_on_audio(const int16_t *samples, size_t count, void *context) {
  (void)context;
  if (s_memo.state != MEMO_RECORDING || samples == NULL || count == 0u) return;
  // Meter the sound, not the microphone's DC offset: peak distance from the frame's mean.
  int32_t sum = 0, peak = 0;
  for (size_t i = 0; i < count; i++) sum += samples[i];
  int32_t mean = sum / (int32_t)count;
  for (size_t i = 0; i < count; i++) {
    int32_t v = samples[i] - mean;
    if (v < 0) v = -v;
    if (v > peak) peak = v;
  }
  // Rough log scale: each doubling of amplitude adds one of twelve meter steps.
  uint8_t level = 0u;
  while (peak > 8 && level < 12u) { peak >>= 1; level++; }
  s_memo.level = level > s_memo.level ? level : (uint8_t)(s_memo.level > 0u ? s_memo.level - 1u : 0u);
  bool room = memo_encoder_push(&s_memo.encoder, samples, count);
  if (!room) {
    vibes_short_pulse();
    memo_finish_recording(s_memo.mode == MEMO_MODE_STREAM ?
        "The phone fell behind and the buffer filled, so recording stopped. The rest is being sent." :
        s_memo.mode == MEMO_MODE_LOG ? "Watch storage fell behind, so recording stopped. What was recorded is kept." :
        "Memory full, recording stopped. SELECT sends it to your phone.");
    return;
  }
  if (s_memo.mode != MEMO_MODE_RAM && s_memo.encoder.samples >= memo_max_seconds(0u) * MEMO_RATE) {
    vibes_short_pulse();
    memo_finish_recording("Time limit reached, recording stopped.");
    return;
  }
  if (s_memo.mode != MEMO_MODE_RAM) memo_pump();
  if (++s_memo.callbacks % 10u == 0u) memo_redraw(); // Five screen updates per second.
}

static void memo_on_stopped(MicCaptureStopReason reason, void *context) {
  (void)context;
  if (s_memo.state != MEMO_RECORDING) return;
  if (reason == MicCaptureStopReasonPermissionRevoked) {
    s_memo.state = MEMO_IDLE;
    memo_release();
    memo_set_status("Microphone permission was revoked, so the recording was deleted.");
    return;
  }
  memo_finish_recording(reason == MicCaptureStopReasonInterrupted ?
      "Recording interrupted by another microphone user. What was recorded is kept." :
      "Recording stopped when Hermes lost focus. What was recorded is kept.");
}

static void memo_start_recording(void) {
  if (s_memo.state == MEMO_RECORDING || s_memo.state == MEMO_SENDING || s_memo.state == MEMO_WAIT_RECEIPT) return;
  memo_cancel_timer();
  memo_release();
  memo_load_mode();
  s_memo.state = MEMO_IDLE;
  if (!memo_mic_available()) {
    memo_set_status(memo_start_error(MicCaptureStartErrUnavailable));
    return;
  }
  if (s_memo.mode == MEMO_MODE_STREAM && (!s_app_message_open || !connection_service_peek_pebble_app_connection())) {
    memo_set_status("Stream mode needs the phone connected. Press UP for RAM or Watch flash.");
    return;
  }
  memo_quiet_speaker(); // Also frees a reply's audio buffer before sizing the memo.
  uint32_t capacity = memo_capacity_for(heap_bytes_free());
  s_memo.buffer = capacity ? malloc(capacity) : NULL;
  bool ring = s_memo.mode != MEMO_MODE_RAM;
  if (s_memo.buffer == NULL || !(ring ? memo_encoder_begin_ring(&s_memo.encoder, s_memo.buffer, capacity) :
                                        memo_encoder_begin(&s_memo.encoder, s_memo.buffer, capacity))) {
    memo_release();
    memo_set_status("Not enough free memory on the watch to record.");
    return;
  }
  s_memo.capacity = capacity;
  s_memo.crc = 0u;
  if (ring) memo_assign_capture();
  if (s_memo.mode == MEMO_MODE_LOG) {
    s_memo.log = data_logging_create(MEMO_LOG_TAG, DATA_LOGGING_BYTE_ARRAY, MEMO_LOG_ITEM, false);
    if (s_memo.log == NULL) {
      memo_release();
      memo_set_status("Watch storage is not available for memos right now. Try RAM or Stream.");
      return;
    }
  }
  s_memo.callbacks = 0u;
  s_memo.level = 0u;
  s_memo.offset = 0u;
  s_memo.block = MEMO_BLOCK_BYTES;
  s_memo.retries = s_memo.receipt_waits = 0u;
  s_memo.in_flight = false;
  s_memo.state = MEMO_RECORDING;
  time_ms(&s_memo.started_s, &s_memo.started_ms);
  MicCaptureStartResult result = memo_mic_start((MicCaptureHandlers){
    .data = memo_on_audio, .stopped = memo_on_stopped,
  }, NULL);
  if (result != MicCaptureStartOk) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Memo mic start failed: %d", (int)result);
    s_memo.state = MEMO_IDLE;
    memo_release();
    memo_set_status(memo_start_error(result));
    return;
  }
  memo_set_status("");
}

static void memo_start_send(void) {
  if (s_memo.buffer == NULL || (s_memo.mode == MEMO_MODE_RAM && s_memo.length <= MEMO_HEADER)) return;
  s_memo.state = MEMO_SENDING;
  // RAM mode restarts and the phone rewrites its partial file; stream mode resumes.
  if (s_memo.mode == MEMO_MODE_RAM) s_memo.offset = 0u;
  s_memo.retries = 0u;
  s_memo.receipt_waits = 0u;
  s_memo.block = MEMO_BLOCK_BYTES;
  s_memo.in_flight = false;
  s_memo.confirm_delete = false;
  memo_set_status("");
  memo_pump();
}

/* Watch flash: moves whole items from the ring into the Data Logging session. The session
 * buffers one item; BUSY means the system task has not written the last one to flash yet. */
static void memo_log_drain(void) {
  bool done = s_memo.state == MEMO_SENDING;
  if (s_memo.log == NULL || (!done && s_memo.state != MEMO_RECORDING)) return;
  for (uint8_t n = 0; n < 4u; n++) {
    uint8_t item[MEMO_LOG_ITEM];
    uint32_t pending = memo_ring_pending(&s_memo.encoder);
    uint32_t count = pending > MEMO_LOG_PAYLOAD ? MEMO_LOG_PAYLOAD : pending;
    bool end = done && pending == 0u;
    if (!end && (count == 0u || (!done && count < MEMO_LOG_PAYLOAD))) return; // Wait for a full item.
    memset(item, 0, sizeof(item));
    memo_put_u32(item, 0u, s_memo.capture);
    memo_put_u32(item, 4u, s_memo.encoder.consumed);
    if (end) {
      item[8] = 1u;
      memcpy(item + MEMO_LOG_HEAD, s_memo.header, MEMO_HEADER);
      memo_put_u32(item, MEMO_LOG_HEAD + MEMO_HEADER, s_memo.crc);
      memo_put_u32(item, MEMO_LOG_HEAD + MEMO_HEADER + 4u, s_memo.length);
      count = MEMO_HEADER + 8u;
    } else {
      memo_ring_copy(&s_memo.encoder, s_memo.encoder.consumed, item + MEMO_LOG_HEAD, count);
    }
    item[10] = (uint8_t)count;
    item[11] = (uint8_t)(count >> 8);
    DataLoggingResult result = data_logging_log(s_memo.log, item, 1u);
    if (result == DATA_LOGGING_BUSY) break;
    if (result != DATA_LOGGING_SUCCESS) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "Memo log failed: %d", (int)result);
      memo_send_failed("Watch storage refused the memo. What was saved so far still reaches the phone.");
      memo_release();
      return;
    }
    if (end) {
      memo_release(); // Finishes the session; the system uploads it from flash.
      s_memo.state = MEMO_SENT;
      vibes_short_pulse();
      memo_set_status("Saved in watch storage. It reaches your phone on its own, even if you close Hermes.");
      return;
    }
    s_memo.encoder.consumed += count;
    s_memo.offset = s_memo.encoder.consumed;
  }
  if (done) memo_schedule(MEMO_LOG_DRAIN_MS);
}

static void memo_pump(void) {
  DictionaryIterator *iter = NULL;
  bool recording = s_memo.state == MEMO_RECORDING;
  if (!memo_transferring() && !(recording && s_memo.mode == MEMO_MODE_LOG)) return;
  if (s_memo.mode == MEMO_MODE_LOG) { memo_log_drain(); return; }
  if (s_memo.in_flight || s_exiting) return;
  if (!s_app_message_open || !connection_service_peek_pebble_app_connection()) {
    // While streaming, keep recording into the buffer and try again shortly.
    if (recording) { memo_schedule(MEMO_OFFLINE_RETRY_MS); return; }
    memo_send_failed(s_app_message_open ? "Phone not connected. The recording is kept; press SELECT to send once connected." :
                                          "The phone channel is closed. Reopen Hermes and try again.");
    return;
  }
  if (s_outbound.active) { memo_schedule(MEMO_YIELD_MS); return; } // Requests and receipts go first.
  const uint8_t *data;
  uint32_t offset, count, total = s_memo.length, crc = s_memo.crc;
  uint8_t kind = HERMES_KIND_MEMO_BLOCK, flags;
  bool end = false;
  if (s_memo.mode == MEMO_MODE_STREAM) {
    uint32_t pending = memo_ring_pending(&s_memo.encoder);
    kind = HERMES_KIND_MEMO_STREAM;
    if (pending == 0u) {
      if (recording) return; // The next audio frame calls back.
      end = true;
      data = s_memo.header;
      offset = s_memo.length;
      count = MEMO_HEADER;
      flags = HERMES_FLAG_MEMO_END;
    } else {
      if (recording && pending < MEMO_STREAM_MIN_SEND) return;
      offset = s_memo.encoder.consumed;
      uint32_t at = offset % s_memo.encoder.capacity;
      count = pending;
      if (count > s_memo.encoder.capacity - at) count = s_memo.encoder.capacity - at;
      if (count > s_memo.block) count = s_memo.block;
      data = s_memo.buffer + at;
      total = crc = 0u;
      flags = HERMES_FLAG_MORE;
    }
  } else {
    offset = s_memo.offset;
    count = s_memo.length - offset;
    if (count > s_memo.block) count = s_memo.block;
    data = s_memo.buffer + offset;
    flags = offset + count < s_memo.length ? HERMES_FLAG_MORE : 0u;
  }
  if (app_message_outbox_begin(&iter) != APP_MSG_OK || iter == NULL) { memo_schedule(MEMO_YIELD_MS); return; }
  s_memo.transfer_id = next_transfer_id();
  if (dict_write_uint8(iter, HERMES_KEY_PROTOCOL_VERSION, (uint8_t)HERMES_PROTOCOL_VERSION) != DICT_OK ||
      dict_write_uint8(iter, HERMES_KEY_MESSAGE_KIND, kind) != DICT_OK ||
      dict_write_uint32(iter, HERMES_KEY_TRANSFER_ID, s_memo.transfer_id) != DICT_OK ||
      dict_write_uint32(iter, HERMES_KEY_CAPTURE_ID, s_memo.capture) != DICT_OK ||
      dict_write_uint32(iter, HERMES_KEY_PAGE_OFFSET, offset) != DICT_OK ||
      dict_write_uint32(iter, HERMES_KEY_TOTAL_BYTES, total) != DICT_OK ||
      dict_write_uint32(iter, HERMES_KEY_CONVERSATION_GENERATION, crc) != DICT_OK ||
      dict_write_uint8(iter, HERMES_KEY_FLAGS, flags) != DICT_OK ||
      dict_write_data(iter, HERMES_KEY_PAYLOAD, data, (uint16_t)count) != DICT_OK) {
    if (s_memo.block > MEMO_MIN_BLOCK_BYTES) { s_memo.block /= 2u; memo_schedule(MEMO_YIELD_MS); }
    else memo_send_failed("Could not encode the memo message.");
    return;
  }
  if (app_message_outbox_send() != APP_MSG_OK) { memo_schedule(MEMO_YIELD_MS); return; }
  s_memo.in_flight = true;
  s_memo.flight_end = end;
  s_memo.flight_length = end ? 0u : count;
  memo_schedule(MEMO_FLIGHT_TIMEOUT_MS);
}

/* Outbox callbacks are shared with the main transport; true when the message was a memo block. */
static bool memo_on_sent(DictionaryIterator *iterator) {
  uint32_t transfer_id = 0u;
  if (!s_memo.in_flight || !read_u32(iterator, HERMES_KEY_TRANSFER_ID, &transfer_id, true) ||
      transfer_id != s_memo.transfer_id) return false;
  s_memo.in_flight = false;
  s_memo.retries = 0u;
  bool finished;
  if (s_memo.mode == MEMO_MODE_STREAM) {
    s_memo.encoder.consumed += s_memo.flight_length;
    s_memo.offset = s_memo.encoder.consumed;
    finished = s_memo.flight_end;
  } else {
    if (s_memo.offset + s_memo.flight_length >= s_memo.length) s_memo.final_offset = s_memo.offset;
    s_memo.offset += s_memo.flight_length;
    finished = s_memo.offset >= s_memo.length;
  }
  if (finished) {
    s_memo.state = MEMO_WAIT_RECEIPT;
    memo_schedule(MEMO_RECEIPT_TIMEOUT_MS);
  } else {
    memo_cancel_timer();
    memo_pump();
  }
  memo_redraw();
  return true;
}

static bool memo_on_failed(DictionaryIterator *iterator) {
  uint32_t transfer_id = 0u;
  if (!s_memo.in_flight || !read_u32(iterator, HERMES_KEY_TRANSFER_ID, &transfer_id, true) ||
      transfer_id != s_memo.transfer_id) return false;
  s_memo.in_flight = false;
  if (++s_memo.retries > MEMO_MAX_RETRIES) {
    // While streaming, keep the microphone running and retry slowly until the buffer fills.
    if (s_memo.state == MEMO_RECORDING) s_memo.retries = MEMO_MAX_RETRIES;
    else {
      memo_send_failed("The phone did not accept the memo. Update Hermes Pebble on Android, then press SELECT to retry.");
      return true;
    }
  }
  // Repeated refusals may mean an oversized message for this phone host; shrink blocks.
  if (s_memo.retries >= 2u && s_memo.block > MEMO_MIN_BLOCK_BYTES) s_memo.block /= 2u;
  memo_schedule(250u << (s_memo.retries > 4u ? 4u : s_memo.retries));
  return true;
}

static void memo_receipt(const InboundTransfer *message) {
  bool final_in_flight = s_memo.state == MEMO_SENDING && s_memo.in_flight &&
      (s_memo.mode == MEMO_MODE_STREAM ? s_memo.flight_end : s_memo.offset + s_memo.flight_length >= s_memo.length);
  if ((s_memo.state != MEMO_WAIT_RECEIPT && !final_in_flight) || message->capture_id != s_memo.capture ||
      message->generation != s_memo.crc || message->total_bytes != s_memo.length) return;
  memo_cancel_timer();
  if (message->error_code == HERMES_ERROR_NONE && message->status == HERMES_STATUS_NOTE_SAVED) {
    s_memo.state = MEMO_SENT;
    memo_release();
    vibes_short_pulse();
    memo_set_status("Saved on phone. Open Notes in Hermes Pebble on Android to listen or delete it.");
  } else if (s_memo.mode == MEMO_MODE_STREAM) {
    // The streamed audio has left the watch, so there is nothing to resend.
    memo_send_failed("The phone could not verify the streamed memo, so it was not saved.");
    memo_release();
  } else {
    memo_send_failed("The phone rejected the memo checksum. Press SELECT to resend it.");
  }
}

static void memo_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  char big[16], info[24];
  const char *hint;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, UI_ACCENT);
  graphics_fill_rect(ctx, GRect(0, 0, b.size.w, UI_HEADER_HEIGHT), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "Voice memo", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(UI_MARGIN, -1, b.size.w - 90, 22), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  uint32_t seconds = memo_encoder_seconds(&s_memo.encoder);
  bool have_clip = s_memo.state == MEMO_RECORDING || (s_memo.buffer != NULL && s_memo.state != MEMO_IDLE);
  if (!have_clip && s_memo.state != MEMO_SENT) seconds = 0u;
  snprintf(big, sizeof(big), "%lu:%02lu", (unsigned long)(seconds / 60u), (unsigned long)(seconds % 60u));
  if (s_memo.state == MEMO_RECORDING) {
    uint32_t max = memo_max_seconds(s_memo.capacity);
    snprintf(info, sizeof(info), "max %lu:%02lu", (unsigned long)(max / 60u), (unsigned long)(max % 60u));
  } else if (have_clip) {
    snprintf(info, sizeof(info), "%lu KB", (unsigned long)((s_memo.length + 1023u) / 1024u));
  } else {
    snprintf(info, sizeof(info), "%s", memo_mode_name());
  }
  graphics_draw_text(ctx, info, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(b.size.w - 88, 2, 80, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);

  int y = UI_HEADER_HEIGHT + 8;
  if (s_memo.state == MEMO_RECORDING) {
    graphics_context_set_fill_color(ctx, GColorRed);
    graphics_fill_circle(ctx, GPoint(UI_MARGIN + 12, y + 26), 8);
  }
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, big, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
      GRect(0, y, b.size.w, 50), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  y += 56;

  // Level meter while recording; transfer progress while sending.
  int bar_w = b.size.w - 4 * UI_MARGIN;
  GRect bar = GRect(2 * UI_MARGIN, y, bar_w, 10);
  int fill = -1;
  GColor bar_color = GColorIslamicGreen;
  if (s_memo.state == MEMO_RECORDING) {
    fill = bar_w * s_memo.level / 12;
    if (s_memo.level >= 11u) bar_color = GColorRed;
  } else if ((s_memo.state == MEMO_SENDING || s_memo.state == MEMO_WAIT_RECEIPT) && s_memo.length) {
    fill = (int)((uint64_t)bar_w * s_memo.offset / s_memo.length);
    bar_color = UI_ACCENT;
  }
  if (fill >= 0) {
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_draw_round_rect(ctx, bar, 3);
    graphics_context_set_fill_color(ctx, bar_color);
    if (fill > 0) graphics_fill_rect(ctx, GRect(bar.origin.x, bar.origin.y, fill, bar.size.h), 3, GCornersAll);
  }
  y += 18;

  char line[96];
  const char *text = s_memo.status;
  if (text[0] == '\0') {
    switch (s_memo.state) {
      case MEMO_RECORDING:
        if (s_memo.mode == MEMO_MODE_STREAM) {
          snprintf(line, sizeof(line), "Streaming to phone… %lu KB waiting",
              (unsigned long)(memo_ring_pending(&s_memo.encoder) / 1024u));
          text = line;
        } else {
          text = s_memo.mode == MEMO_MODE_LOG ? "Recording into watch storage…" : "Recording…";
        }
        break;
      case MEMO_READY: text = "Recorded. Send it to your phone or delete it."; break;
      case MEMO_SENDING:
      case MEMO_WAIT_RECEIPT:
        snprintf(line, sizeof(line), s_memo.mode == MEMO_MODE_LOG ? "Saving to watch storage… %lu%%" : "Sending to phone… %lu%%",
            (unsigned long)(s_memo.length ? (uint64_t)s_memo.offset * 100u / s_memo.length : 0u));
        text = line;
        break;
      default: {
        if (!memo_mic_available()) { text = "Voice memos need watch firmware 4.43 or newer."; break; }
        memo_load_mode();
        if (s_memo.mode == MEMO_MODE_STREAM) {
          text = "Stream: sent while you record, up to 10 min, and played live if the phone allows. Needs the phone nearby.";
        } else if (s_memo.mode == MEMO_MODE_LOG) {
          text = "Watch flash: saved to watch storage while you record, up to 2 min. Syncs to the phone on its own.";
        } else {
          uint32_t capacity = memo_capacity_for(heap_bytes_free());
          snprintf(line, sizeof(line), "RAM: up to %lu s with free memory. Leaving Hermes before sending discards it.",
              (unsigned long)memo_max_seconds(capacity));
          text = line;
        }
        break;
      }
    }
  }
  if (s_memo.confirm_delete) text = "Delete this recording? It has not been sent.";
  graphics_context_set_text_color(ctx, GColorDarkGray);
  graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_18),
      GRect(UI_MARGIN, y, b.size.w - 2 * UI_MARGIN, b.size.h - y - UI_HINT_HEIGHT - 2),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  switch (s_memo.state) {
    case MEMO_RECORDING: hint = "SELECT: stop"; break;
    case MEMO_READY: hint = s_memo.confirm_delete ? "DOWN: delete · UP: keep" : "SELECT: send · DOWN: delete"; break;
    case MEMO_SENDING:
    case MEMO_WAIT_RECEIPT: hint = "Keep Hermes open"; break;
    case MEMO_FAILED:
      hint = s_memo.buffer ? (s_memo.confirm_delete ? "DOWN: delete · UP: keep" : "SELECT: retry · DOWN: delete") : "SELECT: record · UP: mode";
      break;
    case MEMO_SENT: hint = "SELECT: new · UP: mode"; break;
    default: hint = "SELECT: record · UP: mode"; break;
  }
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_rect(ctx, GRect(0, b.size.h - UI_HINT_HEIGHT, b.size.w, UI_HINT_HEIGHT), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, hint, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
      GRect(0, b.size.h - UI_HINT_HEIGHT, b.size.w, UI_HINT_HEIGHT),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void memo_select(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  switch (s_memo.state) {
    case MEMO_RECORDING:
      memo_finish_recording("");
      break;
    case MEMO_READY:
      if (!s_memo.confirm_delete) memo_start_send();
      break;
    case MEMO_FAILED:
      if (s_memo.buffer) { if (!s_memo.confirm_delete) memo_start_send(); }
      else memo_start_recording();
      break;
    case MEMO_SENDING:
    case MEMO_WAIT_RECEIPT:
      break;
    default:
      memo_start_recording();
      break;
  }
  memo_redraw();
}

static void memo_down(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  if ((s_memo.state != MEMO_READY && s_memo.state != MEMO_FAILED) || s_memo.buffer == NULL) return;
  if (!s_memo.confirm_delete) {
    s_memo.confirm_delete = true;
  } else {
    memo_release();
    s_memo.state = MEMO_IDLE;
    memo_set_status("Recording deleted.");
  }
  memo_redraw();
}

static void memo_up(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  if (s_memo.confirm_delete) {
    s_memo.confirm_delete = false;
  } else if (s_memo.state == MEMO_IDLE || s_memo.state == MEMO_SENT || (s_memo.state == MEMO_FAILED && !s_memo.buffer)) {
    memo_load_mode();
    s_memo.mode = (uint8_t)((s_memo.mode + 1u) % MEMO_MODES);
    (void)persist_write_int(HERMES_STORAGE_KEY_MEMO_MODE, s_memo.mode);
    s_memo.state = MEMO_IDLE;
    s_memo.encoder.samples = 0u;
    s_memo.status[0] = '\0';
  }
  memo_redraw();
}

static void memo_back(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  if (s_memo.state == MEMO_RECORDING) { memo_finish_recording(""); return; }
  if (s_memo.confirm_delete) { s_memo.confirm_delete = false; memo_redraw(); return; }
  window_stack_pop(true);
}

static void memo_click_config(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_SELECT, memo_select);
  window_single_click_subscribe(BUTTON_ID_DOWN, memo_down);
  window_single_click_subscribe(BUTTON_ID_UP, memo_up);
  window_single_click_subscribe(BUTTON_ID_BACK, memo_back);
}

static void memo_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_memo_layer = layer_create(layer_get_bounds(root));
  if (!s_memo_layer) return;
  layer_set_update_proc(s_memo_layer, memo_draw);
  layer_add_child(root, s_memo_layer);
  if (s_memo.state == MEMO_SENT || s_memo.state == MEMO_IDLE) {
    s_memo.state = MEMO_IDLE;
    s_memo.status[0] = '\0';
    s_memo.encoder.samples = 0u;
  }
}

static void memo_disappear(Window *window) {
  (void)window;
  // Covered by another window: stop recording but keep the clip.
  if (s_memo.state == MEMO_RECORDING) memo_finish_recording("Recording stopped when Hermes lost focus. What was recorded is kept.");
}

static void memo_unload(Window *window) {
  (void)window;
  if (s_memo_layer) layer_destroy(s_memo_layer);
  s_memo_layer = NULL;
}

static void memo_open(void) {
  if (!s_memo_window) {
    s_memo_window = window_create();
    if (!s_memo_window) return;
    window_set_click_config_provider(s_memo_window, memo_click_config);
    window_set_window_handlers(s_memo_window, (WindowHandlers){
      .load = memo_load, .disappear = memo_disappear, .unload = memo_unload,
    });
  }
  window_stack_push(s_memo_window, true);
}

/* Main-menu subtitle. */
static const char *memo_subtitle(void) {
  static char text[40];
  switch (s_memo.state) {
    case MEMO_READY:
    case MEMO_FAILED:
      return s_memo.buffer ? "Not sent yet · open to send" : "Record & send to phone";
    case MEMO_SENDING:
    case MEMO_WAIT_RECEIPT:
      snprintf(text, sizeof(text), s_memo.mode == MEMO_MODE_LOG ? "Saving… %lu%%" : "Sending… %lu%%",
          (unsigned long)(s_memo.length ? (uint64_t)s_memo.offset * 100u / s_memo.length : 0u));
      return text;
    default:
      return "Record & send to phone";
  }
}

static void memo_shutdown(void) {
  if (s_memo.state == MEMO_RECORDING) memo_mic_stop();
  memo_cancel_timer();
  memo_release();
  s_memo.state = MEMO_IDLE;
  if (s_memo_window) window_destroy(s_memo_window);
  s_memo_window = NULL;
}
#endif
