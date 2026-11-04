#ifndef HERMES_MEMO_STORE_H
#define HERMES_MEMO_STORE_H
/* Watch memos: voice memos kept on the watch. Data Logging cannot be read back by an app, so
 * these live in app persistent storage (1 MiB per app since well before firmware 4.43), in
 * 256-byte values. Each memo can be listed, played on the watch speaker and deleted, and is sent
 * to the phone in the background while Hermes is open, and stays here until deleted.
 *
 * Every persist write scans all the app's records on the app task, where the microphone's
 * two-frame queue also drains, so the store is capped well below 1 MiB: more records would make
 * each write too slow for live audio, and would slow every other persist call. Deletes run one
 * value per timer tick for the same reason. A memo's values are written before its index entry;
 * the active-slot key marks an unfinished recording so a crash cannot leak its values. */
#define WM_KEY_CHUNK 0x48570000u /* + (slot << 12) + chunk */
#define WM_KEY_INDEX 0x4857f000u
#define WM_KEY_ACTIVE 0x4857f001u
#define WM_INDEX_MAGIC 0x314d5748u /* "HWM1" */
#define WM_CHUNK 256u
#define WM_SLOTS 14u
#define WM_MAX_SECONDS 120u
#define WM_MAX_CHUNKS (WM_MAX_SECONDS * MEMO_BYTES_PER_SECOND / WM_CHUNK + 1u)
#define WM_MAX_STORE (512u * 1024u)
#define WM_PERSIST_RESERVE (16u * 1024u) /* Settings, drafts and ink, plus record headers below. */
#define WM_RING 4096u
#define WM_SEND_CHUNKS 3u
#define WM_DELETE_TICK_MS 60u
#define WM_PLAY_TICK_MS 50u
#define WM_PLAY_RING 4096u
#define WM_RETRY_MS 1000u
#define WM_FAILED_RETRY_MS 30000u
#define WM_MIN_SAMPLES (MEMO_RATE / 2u)
#define WM_ROW_HEIGHT 44
#define WM_STORAGE_HEIGHT 22

#define WM_FLAG_ON_PHONE 1u
#define WM_FLAG_DELETING 2u
#define WM_FLAG_ODD 4u /* The last data byte holds one sample. */

#define WM_VIEW_LIST 0u
#define WM_VIEW_RECORD 1u
#define WM_VIEW_ITEM 2u

#define WM_SEND_IDLE 0u
#define WM_SEND_DATA 1u
#define WM_SEND_RECEIPT 2u

typedef struct __attribute__((packed)) {
  uint32_t capture, recorded_at, crc, length;
  uint8_t flags, coverage;
} WmEntry;

typedef struct {
  MemoPlayback decoder;
  uint32_t head, tail, offset;
  bool closing;
  uint8_t chunk[WM_CHUNK];
  int8_t pcm[WM_PLAY_RING];
} WmPlayer;

typedef struct {
  WmEntry entries[WM_SLOTS]; /* capture == 0: free. */
  MemoEncoder encoder;
  uint8_t *ring;
  WmPlayer *player;
  uint8_t *send_buffer;
  time_t started_s;
  uint32_t capacity, written, crc, callbacks;
  uint32_t send_offset, send_flight, send_transfer, delete_left, delete_total;
  uint16_t started_ms, failed;
  int8_t rec_slot, play_slot, send_slot, delete_slot;
  uint8_t view, selected, item, level, send_state, send_retries, send_rounds;
  bool in_flight, flight_end, confirm_delete, delete_resume;
  char note[80];
  char subtitle[32];
} WatchMemos;

static WatchMemos *s_wm;
static Window *s_wm_window;
static Layer *s_wm_layer;
static AppTimer *s_wm_work_timer, *s_wm_send_timer;

static void wm_send_pump(void);
static void wm_work_schedule(void);

static void wm_redraw(void) { if (s_wm_layer) layer_mark_dirty(s_wm_layer); }

static void wm_set_note(const char *text) {
  snprintf(s_wm->note, sizeof(s_wm->note), "%s", text);
  wm_redraw();
}

static uint32_t wm_key(uint8_t slot, uint32_t chunk) { return WM_KEY_CHUNK + ((uint32_t)slot << 12) + chunk; }
static uint32_t wm_chunks(uint32_t length) { return (length + WM_CHUNK - 1u) / WM_CHUNK; }

/* Bytes of audio held, counting only what a running delete has not freed yet. */
static uint32_t wm_used(void) {
  uint32_t used = s_wm->written;
  for (uint8_t i = 0; i < WM_SLOTS; i++) {
    const WmEntry *e = &s_wm->entries[i];
    if (e->capture == 0u) continue;
    if ((int8_t)i == s_wm->delete_slot) used += s_wm->delete_left * WM_CHUNK;
    else used += e->length;
  }
  return used;
}

static bool wm_save_index(void) {
  /* Two persist values would cost two scans, so all of it fits in one: 4 + 14 * 18 = 256. */
  uint8_t bytes[4u + sizeof(s_wm->entries)];
  uint32_t magic = WM_INDEX_MAGIC;
  memcpy(bytes, &magic, 4u);
  memcpy(bytes + 4u, s_wm->entries, sizeof(s_wm->entries));
  return persist_write_data(WM_KEY_INDEX, bytes, sizeof(bytes)) == (int)sizeof(bytes);
}

static bool wm_audio_busy(void) { return s_wm && (s_wm->rec_slot >= 0 || s_wm->player != NULL); }

/* ---- Deleting ------------------------------------------------------------------------ */

static void wm_delete_step(void) {
  if (s_wm->delete_slot < 0) {
    for (uint8_t i = 0; i < WM_SLOTS; i++) {
      WmEntry *e = &s_wm->entries[i];
      if (e->capture != 0u && (e->flags & WM_FLAG_DELETING)) {
        s_wm->delete_slot = (int8_t)i;
        s_wm->delete_left = s_wm->delete_total = wm_chunks(e->length);
        break;
      }
    }
    if (s_wm->delete_slot < 0) return;
  }
  uint8_t slot = (uint8_t)s_wm->delete_slot;
  if (s_wm->delete_left > 0u) {
    s_wm->delete_left--;
    uint32_t key = wm_key(slot, s_wm->delete_left);
    /* After a restart some values may already be gone: do not add tombstones for them. */
    if (!s_wm->delete_resume || persist_exists(key)) persist_delete(key);
  }
  if (s_wm->delete_left == 0u) {
    memset(&s_wm->entries[slot], 0, sizeof(WmEntry));
    s_wm->delete_slot = -1;
    s_wm->delete_resume = false;
    (void)wm_save_index();
  }
  wm_redraw();
}

/* ---- Playback ------------------------------------------------------------------------ */

static void wm_play_stop(bool stop_speaker, const char *note) {
  if (!s_wm->player) return;
  speaker_set_finish_callback(NULL, NULL);
  if (stop_speaker) speaker_stop();
  free(s_wm->player);
  s_wm->player = NULL;
  s_wm->play_slot = -1;
  if (note) wm_set_note(note);
  wm_redraw();
  wm_work_schedule();
  wm_send_pump();
}

static void wm_play_finished(SpeakerFinishReason reason, void *context) {
  (void)context;
  if (!s_wm->player) return;
  wm_play_stop(false, reason == SpeakerFinishReasonDone && s_wm->player->closing ? "" : "Playback was interrupted.");
}

static void wm_play_tick(void) {
  WmPlayer *p = s_wm->player;
  const WmEntry *e = &s_wm->entries[s_wm->play_slot];
  if (speaker_is_muted() || quiet_time_is_active()) { wm_play_stop(true, "Muted, playback stopped."); return; }
  if (p->closing) return;
  /* One value is 512 samples plus at most a window; keep the speaker ahead of the flash. */
  if (p->offset < e->length && WM_PLAY_RING - (p->head - p->tail) >= WM_CHUNK * 2u + MEMO_PLAY_WINDOW) {
    uint32_t count = e->length - p->offset;
    if (count > WM_CHUNK) count = WM_CHUNK;
    if (persist_read_data(wm_key((uint8_t)s_wm->play_slot, p->offset / WM_CHUNK), p->chunk, count) != (int)count) {
      wm_play_stop(true, "This memo could not be read.");
      return;
    }
    memo_playback_feed(&p->decoder, p->chunk, count, p->pcm, WM_PLAY_RING, &p->head);
    p->offset += count;
    if (p->offset >= e->length) memo_playback_flush(&p->decoder, p->pcm, WM_PLAY_RING, &p->head);
  }
  for (uint8_t i = 0; i < 2u && p->tail != p->head; i++) {
    uint32_t at = p->tail % WM_PLAY_RING;
    uint32_t count = p->head - p->tail;
    if (count > WM_PLAY_RING - at) count = WM_PLAY_RING - at;
    if (count > 1024u) count = 1024u;
    uint32_t accepted = speaker_stream_write(p->pcm + at, count);
    if (accepted > count) { wm_play_stop(true, "The speaker failed."); return; }
    p->tail += accepted;
    if (accepted < count) break;
  }
  if (p->offset >= e->length && p->tail == p->head) {
    p->closing = true;
    speaker_stream_close();
  }
  wm_redraw();
}

static void wm_play_start(uint8_t slot) {
  if (wm_audio_busy() || audio_dictation_active()) return;
  if (speaker_is_muted()) { wm_set_note("The watch speaker is muted."); return; }
  if (quiet_time_is_active()) { wm_set_note("Quiet Time is on, so playback is off."); return; }
  memo_quiet_speaker();
  WmPlayer *player = malloc(sizeof(WmPlayer));
  if (!player) { wm_set_note("Not enough free memory to play."); return; }
  memset(player, 0, offsetof(WmPlayer, chunk));
  speaker_set_finish_callback(wm_play_finished, NULL);
  if (speaker_get_status() != SpeakerStatusIdle || !speaker_stream_open(SpeakerPcmFormat_8kHz_8bit, 100)) {
    speaker_set_finish_callback(NULL, NULL);
    free(player);
    wm_set_note("The speaker is busy.");
    return;
  }
  s_wm->player = player;
  s_wm->play_slot = (int8_t)slot;
  s_wm->note[0] = '\0';
  wm_work_schedule();
  wm_redraw();
}

/* Work timer: playback when playing, otherwise deletes; both pause while recording. */
static void wm_work_tick(void *context) {
  (void)context;
  s_wm_work_timer = NULL;
  if (s_wm->player) wm_play_tick();
  else if (s_wm->rec_slot < 0) wm_delete_step();
  wm_work_schedule();
}

static void wm_work_schedule(void) {
  if (s_wm_work_timer) return;
  bool deleting = s_wm->delete_slot >= 0;
  for (uint8_t i = 0; i < WM_SLOTS && !deleting; i++) deleting = s_wm->entries[i].capture != 0u && (s_wm->entries[i].flags & WM_FLAG_DELETING);
  if (s_wm->player) s_wm_work_timer = app_timer_register(WM_PLAY_TICK_MS, wm_work_tick, NULL);
  else if (deleting && s_wm->rec_slot < 0) s_wm_work_timer = app_timer_register(WM_DELETE_TICK_MS, wm_work_tick, NULL);
}

/* ---- Sending to the phone ------------------------------------------------------------ */
/* Kind 12 with HERMES_FLAG_MEMO_STORED: the data in order, then the HVM1 header. The phone writes
 * pieces at their offset, so restarting a memo is harmless; a receipt marks it as on the phone. */

static void wm_send_finish(bool failed);

static void wm_send_timer_tick(void *context) {
  (void)context;
  s_wm_send_timer = NULL;
  s_wm->in_flight = false;
  if (s_wm->send_state == WM_SEND_RECEIPT) {
    /* Resend the end message; the phone repeats its receipt. */
    if (++s_wm->send_retries > 3u) wm_send_finish(true);
    else s_wm->send_state = WM_SEND_DATA;
  } else if (s_wm->send_slot < 0) {
    s_wm->failed = 0u; /* Nothing else to do: memos that failed get another try. */
  }
  wm_send_pump();
}

static void wm_send_after(uint32_t ms) {
  if (s_wm_send_timer) app_timer_cancel(s_wm_send_timer);
  s_wm_send_timer = app_timer_register(ms, wm_send_timer_tick, NULL);
}

static void wm_send_finish(bool failed) {
  if (failed && s_wm->send_slot >= 0) s_wm->failed |= (uint16_t)(1u << s_wm->send_slot);
  s_wm->send_state = WM_SEND_IDLE;
  s_wm->send_slot = -1;
  s_wm->send_rounds = 0u;
  free(s_wm->send_buffer);
  s_wm->send_buffer = NULL;
  wm_redraw();
}

static void wm_send_pump(void) {
  if (!s_wm || s_exiting || s_wm->in_flight || s_wm->send_state == WM_SEND_RECEIPT) return;
  if (wm_audio_busy()) return; /* Stopping restarts the pump. */
  if (s_wm->send_slot < 0) {
    /* The oldest memo that is only on the watch. */
    int8_t best = -1;
    for (uint8_t i = 0; i < WM_SLOTS; i++) {
      const WmEntry *e = &s_wm->entries[i];
      if (e->capture == 0u || (e->flags & (WM_FLAG_ON_PHONE | WM_FLAG_DELETING)) || (s_wm->failed & (1u << i))) continue;
      if (best < 0 || e->recorded_at < s_wm->entries[best].recorded_at) best = (int8_t)i;
    }
    if (best < 0) {
      /* Failed memos get another try later, without a reconnect. */
      if (s_wm->failed) wm_send_after(WM_FAILED_RETRY_MS);
      return;
    }
    s_wm->send_slot = best;
    s_wm->send_offset = 0u;
    s_wm->send_retries = 0u;
  }
  if (!s_app_message_open || !connection_service_peek_pebble_app_connection()) return; /* The link event resumes. */
  /* Audio first, and share the outbox with requests and phone-bound memos. */
  if (audio_dictation_active() || memo_transferring() || s_outbound.active || s_audio.phase == 1u || s_audio.phase == 2u) {
    wm_send_after(WM_RETRY_MS);
    return;
  }
  const WmEntry *e = &s_wm->entries[s_wm->send_slot];
  if (!s_wm->send_buffer) s_wm->send_buffer = malloc(WM_SEND_CHUNKS * WM_CHUNK);
  if (!s_wm->send_buffer) { wm_send_after(WM_RETRY_MS); return; }
  uint32_t count = 0u;
  uint8_t flags = HERMES_FLAG_MEMO_STORED;
  bool end = s_wm->send_offset >= e->length;
  if (end) {
    MemoEncoder header = { .samples = e->length * 2u - ((e->flags & WM_FLAG_ODD) ? 1u : 0u), .coverage = e->coverage };
    memo_write_header(&header, s_wm->send_buffer, e->recorded_at);
    count = MEMO_HEADER;
    flags |= HERMES_FLAG_MEMO_END;
  } else {
    flags |= HERMES_FLAG_MORE;
    for (uint8_t i = 0; i < WM_SEND_CHUNKS && s_wm->send_offset + count < e->length; i++) {
      uint32_t size = e->length - (s_wm->send_offset + count);
      if (size > WM_CHUNK) size = WM_CHUNK;
      if (persist_read_data(wm_key((uint8_t)s_wm->send_slot, (s_wm->send_offset + count) / WM_CHUNK),
                            s_wm->send_buffer + count, size) != (int)size) {
        wm_send_finish(true);
        wm_send_pump();
        return;
      }
      count += size;
    }
  }
  s_wm->send_transfer = next_transfer_id();
  uint8_t result = memo_send_message(HERMES_KIND_MEMO_STREAM, s_wm->send_transfer, e->capture, s_wm->send_offset,
      end ? e->length : 0u, end ? e->crc : 0u, flags, s_wm->send_buffer, count);
  if (result != MEMO_SEND_OK) { wm_send_after(WM_RETRY_MS); return; }
  s_wm->in_flight = true;
  s_wm->flight_end = end;
  s_wm->send_flight = end ? 0u : count;
  wm_send_after(MEMO_FLIGHT_TIMEOUT_MS);
  wm_redraw();
}

/* Outbox callbacks are shared; true when the message was one of ours. */
static bool wm_on_sent(DictionaryIterator *iterator) {
  uint32_t transfer = 0u;
  if (!s_wm || !s_wm->in_flight || !read_u32(iterator, HERMES_KEY_TRANSFER_ID, &transfer, true) || transfer != s_wm->send_transfer) return false;
  s_wm->in_flight = false;
  if (s_wm->send_slot < 0) { wm_send_pump(); return true; } /* Deleted meanwhile. */
  s_wm->send_offset += s_wm->send_flight;
  if (s_wm->flight_end) {
    s_wm->send_state = WM_SEND_RECEIPT;
    wm_send_after(MEMO_RECEIPT_TIMEOUT_MS);
  } else {
    s_wm->send_retries = 0u;
    if (s_wm_send_timer) app_timer_cancel(s_wm_send_timer);
    s_wm_send_timer = NULL;
    wm_send_pump();
  }
  wm_redraw();
  return true;
}

static bool wm_on_failed(DictionaryIterator *iterator) {
  uint32_t transfer = 0u;
  if (!s_wm || !s_wm->in_flight || !read_u32(iterator, HERMES_KEY_TRANSFER_ID, &transfer, true) || transfer != s_wm->send_transfer) return false;
  s_wm->in_flight = false;
  if (s_wm->send_slot >= 0 && ++s_wm->send_retries > MEMO_MAX_RETRIES) {
    wm_send_finish(true);
    wm_send_pump(); /* The next memo, or the slow retry when none is left. */
    return true;
  }
  wm_send_after(250u << (s_wm->send_retries > 4u ? 4u : s_wm->send_retries));
  return true;
}

/* True when the receipt belongs to the memo being sent. */
static bool wm_receipt(const InboundTransfer *message) {
  if (!s_wm || s_wm->send_slot < 0) return false;
  WmEntry *e = &s_wm->entries[s_wm->send_slot];
  if (message->capture_id != e->capture || message->generation != e->crc || message->total_bytes != e->length ||
      (s_wm->send_state != WM_SEND_RECEIPT && !(s_wm->in_flight && s_wm->flight_end))) return false;
  s_wm->in_flight = false;
  if (s_wm_send_timer) app_timer_cancel(s_wm_send_timer);
  s_wm_send_timer = NULL;
  if (message->error_code == HERMES_ERROR_NONE && message->status == HERMES_STATUS_NOTE_SAVED) {
    e->flags |= WM_FLAG_ON_PHONE;
    (void)wm_save_index();
    wm_send_finish(false);
  } else if (++s_wm->send_rounds > 1u) {
    wm_send_finish(true);
  } else {
    s_wm->send_state = WM_SEND_DATA; /* The phone found a bad copy and dropped it: once more. */
    s_wm->send_offset = 0u;
  }
  wm_send_pump();
  return true;
}

/* ---- Recording ----------------------------------------------------------------------- */

static bool wm_write_pending(uint32_t count) {
  uint8_t chunk[WM_CHUNK];
  uint32_t chunk_index = s_wm->written / WM_CHUNK;
  memo_ring_copy(&s_wm->encoder, s_wm->encoder.consumed, chunk, count);
  if (chunk_index >= WM_MAX_CHUNKS || persist_write_data(wm_key((uint8_t)s_wm->rec_slot, chunk_index), chunk, count) != (int)count) return false;
  for (uint32_t i = 0; i < count; i++) s_wm->crc = memo_crc_byte(s_wm->crc, chunk[i]);
  s_wm->encoder.consumed += count;
  s_wm->written += count;
  return true;
}

static void wm_finish_recording(const char *note) {
  if (s_wm->rec_slot < 0) return;
  memo_mic_stop();
  uint32_t elapsed_ms = 0u;
  s_wm->encoder.coverage = memo_coverage_of(&s_wm->encoder, s_wm->started_s, s_wm->started_ms, &elapsed_ms);
  uint32_t recorded_at = (uint32_t)time(NULL);
  memo_encoder_finish(&s_wm->encoder, recorded_at);
  bool complete = true;
  while (complete && memo_ring_pending(&s_wm->encoder) > 0u) {
    uint32_t count = memo_ring_pending(&s_wm->encoder);
    complete = wm_write_pending(count > WM_CHUNK ? WM_CHUNK : count);
  }
  uint8_t slot = (uint8_t)s_wm->rec_slot;
  WmEntry *e = &s_wm->entries[slot];
  /* Only the written data counts, and its checksum was kept as it went. */
  *e = (WmEntry){ .capture = e->capture, .recorded_at = recorded_at, .crc = ~s_wm->crc,
    .length = s_wm->written, .coverage = s_wm->encoder.coverage,
    .flags = (uint8_t)(complete && (s_wm->encoder.samples & 1u) ? WM_FLAG_ODD : 0u) };
  char text[sizeof(s_wm->note)];
  if (!complete) snprintf(text, sizeof(text), "%s", "Storage refused more audio. Saved so far is kept.");
  else if (elapsed_ms >= MEMO_COVERAGE_MIN_ELAPSED_MS && e->coverage < MEMO_MIN_COVERAGE)
    snprintf(text, sizeof(text), "The watch missed audio: only %u%% of the time was captured.", (unsigned)e->coverage);
  else snprintf(text, sizeof(text), "%s", note);
  bool keep = e->length * 2u >= WM_MIN_SAMPLES;
  if (!keep) {
    e->flags = WM_FLAG_DELETING;
    snprintf(text, sizeof(text), "%s", "Too short to keep.");
  }
  s_wm->written = 0u;
  s_wm->rec_slot = -1;
  free(s_wm->ring);
  s_wm->ring = NULL;
  s_wm->view = WM_VIEW_LIST;
  s_wm->selected = keep ? 1u : 0u;
  (void)wm_save_index();
  persist_delete(WM_KEY_ACTIVE);
  if (keep) vibes_short_pulse();
  wm_set_note(text);
  wm_work_schedule();
  wm_send_pump();
}

static void wm_on_audio(const int16_t *samples, size_t count, void *context) {
  (void)context;
  if (s_wm->rec_slot < 0 || samples == NULL || count == 0u) return;
  s_wm->level = memo_meter(samples, count, s_wm->level);
  bool room = memo_encoder_push(&s_wm->encoder, samples, count);
  /* One value per frame: the write is the slow part, and the next frame must find a free queue. */
  if (memo_ring_pending(&s_wm->encoder) >= WM_CHUNK && !wm_write_pending(WM_CHUNK)) {
    vibes_short_pulse();
    wm_finish_recording("");
    return;
  }
  uint32_t pending = memo_ring_pending(&s_wm->encoder);
  if (!room) {
    vibes_short_pulse();
    wm_finish_recording("Storage fell behind, recording stopped.");
  } else if (s_wm->encoder.samples >= WM_MAX_SECONDS * MEMO_RATE) {
    vibes_short_pulse();
    wm_finish_recording("Time limit reached, recording stopped.");
  } else if (wm_used() + pending + WM_CHUNK > s_wm->capacity) {
    vibes_short_pulse();
    wm_finish_recording("Storage full, recording stopped.");
  } else if (++s_wm->callbacks % 10u == 0u) {
    wm_redraw();
  }
}

static void wm_on_stopped(MicCaptureStopReason reason, void *context) {
  (void)context;
  if (s_wm->rec_slot < 0) return;
  wm_finish_recording(reason == MicCaptureStopReasonPermissionRevoked ? "Microphone permission was revoked." :
      reason == MicCaptureStopReasonInterrupted ? "Stopped: another app took the microphone." :
      "Stopped: Hermes lost focus.");
}

static void wm_start_recording(void) {
  if (wm_audio_busy()) return;
  if (!memo_mic_available()) { wm_set_note(memo_start_error(MicCaptureStartErrUnavailable)); return; }
  int8_t slot = -1;
  for (uint8_t i = 0; i < WM_SLOTS && slot < 0; i++) if (s_wm->entries[i].capture == 0u) slot = (int8_t)i;
  if (slot < 0) { wm_set_note("Storage full. Delete a memo first."); return; }
  if (wm_used() + MEMO_BYTES_PER_SECOND > s_wm->capacity) { wm_set_note("Storage full. Delete a memo first."); return; }
  memo_quiet_speaker();
  uint32_t capture = 0u;
  if (!storage_next_capture_id(&capture)) capture = (uint32_t)time(NULL) | 1u;
  uint8_t *ring = heap_bytes_free() >= MEMO_HEAP_RESERVE + WM_RING ? malloc(WM_RING) : NULL;
  if (!ring) { wm_set_note("Not enough free memory on the watch to record."); return; }
  (void)memo_encoder_begin_ring(&s_wm->encoder, ring, WM_RING);
  int32_t active = slot + 1;
  if (persist_write_data(WM_KEY_ACTIVE, &active, sizeof(active)) != (int)sizeof(active)) {
    free(ring);
    wm_set_note("Watch storage is not available.");
    return;
  }
  s_wm->ring = ring;
  s_wm->entries[slot].capture = capture;
  s_wm->entries[slot].length = 0u;
  s_wm->rec_slot = slot;
  s_wm->written = 0u;
  s_wm->crc = 0xffffffffu;
  s_wm->callbacks = 0u;
  s_wm->level = 0u;
  s_wm->view = WM_VIEW_RECORD;
  s_wm->note[0] = '\0';
  time_ms(&s_wm->started_s, &s_wm->started_ms);
  MicCaptureStartResult result = memo_mic_start((MicCaptureHandlers){ .data = wm_on_audio, .stopped = wm_on_stopped }, NULL);
  if (result != MicCaptureStartOk) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Watch memo mic start failed: %d", (int)result);
    s_wm->rec_slot = -1;
    memset(&s_wm->entries[slot], 0, sizeof(WmEntry));
    free(ring);
    s_wm->ring = NULL;
    s_wm->view = WM_VIEW_LIST;
    persist_delete(WM_KEY_ACTIVE);
    wm_set_note(memo_start_error(result));
    return;
  }
  wm_redraw();
}

/* ---- Screen -------------------------------------------------------------------------- */

/* Slots newest first; returns how many. */
static uint8_t wm_order(uint8_t *order) {
  uint8_t count = 0u;
  for (uint8_t i = 0; i < WM_SLOTS; i++) {
    if (s_wm->entries[i].capture == 0u || (int8_t)i == s_wm->rec_slot) continue;
    uint8_t at = count++;
    while (at > 0u && s_wm->entries[order[at - 1u]].recorded_at < s_wm->entries[i].recorded_at) {
      order[at] = order[at - 1u];
      at--;
    }
    order[at] = i;
  }
  return count;
}

static uint32_t wm_percent(uint32_t part, uint32_t whole) { return whole ? part * 100u / whole : 0u; }

static const char *wm_status(uint8_t slot, char *text, size_t size) {
  const WmEntry *e = &s_wm->entries[slot];
  if (e->flags & WM_FLAG_DELETING) {
    snprintf(text, size, "Deleting %lu%%", (int8_t)slot == s_wm->delete_slot ?
        (unsigned long)(100u - wm_percent(s_wm->delete_left, s_wm->delete_total)) : 0ul);
  } else if (e->flags & WM_FLAG_ON_PHONE) {
    return "On phone";
  } else if ((int8_t)slot == s_wm->send_slot && s_wm->send_state == WM_SEND_RECEIPT) {
    return "Confirming on phone…";
  } else if ((int8_t)slot == s_wm->send_slot && (s_wm->in_flight || s_wm->send_offset > 0u)) {
    snprintf(text, size, "Sending %lu%%", (unsigned long)wm_percent(s_wm->send_offset, e->length));
  } else if (s_wm->failed & (1u << slot)) {
    return "Send failed · will retry";
  } else {
    return "Only on watch";
  }
  return text;
}

static void wm_format_time(uint32_t at, uint32_t length, char *text, size_t size) {
  time_t when = (time_t)at;
  char date[16];
  uint32_t seconds = length / MEMO_BYTES_PER_SECOND;
  strftime(date, sizeof(date), "%d %b %H:%M", localtime(&when));
  snprintf(text, size, "%s  %lu:%02lu", date, (unsigned long)(seconds / 60u), (unsigned long)(seconds % 60u));
}

static void wm_draw_bar(GContext *ctx, GRect bar, uint32_t percent, GColor color) {
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_round_rect(ctx, bar, 3);
  int fill = bar.size.w * (int)(percent > 100u ? 100u : percent) / 100;
  graphics_context_set_fill_color(ctx, color);
  if (fill > 0) graphics_fill_rect(ctx, GRect(bar.origin.x, bar.origin.y, fill, bar.size.h), 3, GCornersAll);
}

static void wm_text(GContext *ctx, const char *text, const char *font, GRect box, GTextAlignment alignment) {
  graphics_draw_text(ctx, text, fonts_get_system_font(font), box, GTextOverflowModeTrailingEllipsis, alignment, NULL);
}

static void wm_draw_list(GContext *ctx, GRect b) {
  char line[40], status[24];
  uint8_t order[WM_SLOTS];
  uint8_t count = wm_order(order);
  uint32_t used = wm_used();
  uint32_t free_bytes = s_wm->capacity > used ? s_wm->capacity - used : 0u;
  uint32_t seconds = free_bytes / MEMO_BYTES_PER_SECOND;
  /* Storage: bar and text just under the title. */
  int y = UI_HEADER_HEIGHT;
  snprintf(line, sizeof(line), "%lu of %lu KB · %lu:%02lu free", (unsigned long)((used + 1023u) / 1024u),
      (unsigned long)(s_wm->capacity / 1024u), (unsigned long)(seconds / 60u), (unsigned long)(seconds % 60u));
  graphics_context_set_text_color(ctx, GColorBlack);
  wm_text(ctx, line, FONT_KEY_GOTHIC_14, GRect(UI_MARGIN, y - 1, b.size.w - 2 * UI_MARGIN, 16), GTextAlignmentCenter);
  wm_draw_bar(ctx, GRect(UI_MARGIN, y + 15, b.size.w - 2 * UI_MARGIN, 5), wm_percent(used, s_wm->capacity), UI_ACCENT);
  y += WM_STORAGE_HEIGHT + 2;
  if (s_wm->note[0]) {
    graphics_context_set_text_color(ctx, GColorRed);
    wm_text(ctx, s_wm->note, FONT_KEY_GOTHIC_14, GRect(UI_MARGIN, y, b.size.w - 2 * UI_MARGIN, 50), GTextAlignmentLeft);
    y += 50;
  }
  /* Rows: "record" first, then memos, scrolled so the selection stays visible. */
  int bottom = b.size.h - UI_HINT_HEIGHT;
  int visible = (bottom - y) / WM_ROW_HEIGHT;
  if (visible < 1) visible = 1;
  int total = count + 1;
  int top = (int)s_wm->selected - visible + 1;
  if (top < 0) top = 0;
  for (int row = top; row < total && row < top + visible; row++, y += WM_ROW_HEIGHT) {
    bool selected = row == s_wm->selected;
    if (selected) {
      graphics_context_set_fill_color(ctx, UI_ACCENT);
      graphics_fill_rect(ctx, GRect(0, y, b.size.w, WM_ROW_HEIGHT), 0, GCornerNone);
    }
    const char *title = "Record new memo", *subtitle;
    GColor dot = GColorRed;
    if (row == 0) {
      subtitle = "Up to 2 min";
    } else {
      uint8_t slot = order[row - 1];
      const WmEntry *e = &s_wm->entries[slot];
      wm_format_time(e->recorded_at, e->length, line, sizeof(line));
      title = line;
      subtitle = wm_status(slot, status, sizeof(status));
      dot = (e->flags & WM_FLAG_ON_PHONE) ? GColorIslamicGreen : GColorDarkGray;
    }
    graphics_context_set_fill_color(ctx, selected ? GColorWhite : dot);
    graphics_fill_circle(ctx, GPoint(UI_MARGIN + 4, y + WM_ROW_HEIGHT / 2), 4);
    graphics_context_set_text_color(ctx, selected ? GColorWhite : GColorBlack);
    wm_text(ctx, title, FONT_KEY_GOTHIC_18_BOLD, GRect(UI_MARGIN + 14, y, b.size.w - UI_MARGIN - 18, 22), GTextAlignmentLeft);
    graphics_context_set_text_color(ctx, selected ? GColorWhite : GColorDarkGray);
    wm_text(ctx, subtitle, FONT_KEY_GOTHIC_14, GRect(UI_MARGIN + 14, y + 22, b.size.w - UI_MARGIN - 18, 18), GTextAlignmentLeft);
  }
  memo_draw_hint(ctx, b, "SELECT: open · BACK: exit");
}

static void wm_draw_record(GContext *ctx, GRect b) {
  char big[16];
  uint32_t seconds = memo_encoder_seconds(&s_wm->encoder);
  snprintf(big, sizeof(big), "%lu:%02lu", (unsigned long)(seconds / 60u), (unsigned long)(seconds % 60u));
  int y = UI_HEADER_HEIGHT + 8;
  graphics_context_set_fill_color(ctx, GColorRed);
  graphics_fill_circle(ctx, GPoint(UI_MARGIN + 12, y + 26), 8);
  graphics_context_set_text_color(ctx, GColorBlack);
  wm_text(ctx, big, FONT_KEY_BITHAM_42_BOLD, GRect(0, y, b.size.w, 50), GTextAlignmentCenter);
  y += 56;
  wm_draw_bar(ctx, GRect(2 * UI_MARGIN, y, b.size.w - 4 * UI_MARGIN, 10), s_wm->level * 100u / 12u,
      s_wm->level >= 11u ? GColorRed : GColorIslamicGreen);
  graphics_context_set_text_color(ctx, GColorDarkGray);
  wm_text(ctx, "Saving to the watch", FONT_KEY_GOTHIC_18, GRect(UI_MARGIN, y + 18, b.size.w - 2 * UI_MARGIN, 44), GTextAlignmentCenter);
  memo_draw_hint(ctx, b, "SELECT: stop");
}

static void wm_draw_item(GContext *ctx, GRect b) {
  char line[40], status[24];
  uint8_t slot = s_wm->item;
  const WmEntry *e = &s_wm->entries[slot];
  bool playing = s_wm->play_slot == (int8_t)slot;
  bool sending = (int8_t)slot == s_wm->send_slot && !(e->flags & WM_FLAG_ON_PHONE);
  int y = UI_HEADER_HEIGHT + 6;
  wm_format_time(e->recorded_at, e->length, line, sizeof(line));
  graphics_context_set_text_color(ctx, GColorBlack);
  wm_text(ctx, line, FONT_KEY_GOTHIC_24_BOLD, GRect(UI_MARGIN, y, b.size.w - 2 * UI_MARGIN, 30), GTextAlignmentCenter);
  y += 32;
  snprintf(line, sizeof(line), "%lu KB", (unsigned long)((e->length + 1023u) / 1024u));
  graphics_context_set_text_color(ctx, GColorDarkGray);
  wm_text(ctx, line, FONT_KEY_GOTHIC_18, GRect(UI_MARGIN, y, b.size.w - 2 * UI_MARGIN, 22), GTextAlignmentCenter);
  y += 26;
  graphics_context_set_text_color(ctx, GColorBlack);
  wm_text(ctx, wm_status(slot, status, sizeof(status)), FONT_KEY_GOTHIC_18_BOLD, GRect(UI_MARGIN, y, b.size.w - 2 * UI_MARGIN, 22), GTextAlignmentCenter);
  y += 26;
  if (playing) {
    wm_draw_bar(ctx, GRect(2 * UI_MARGIN, y, b.size.w - 4 * UI_MARGIN, 10), wm_percent(s_wm->player->tail, e->length * 2u), GColorIslamicGreen);
  } else if (sending) {
    wm_draw_bar(ctx, GRect(2 * UI_MARGIN, y, b.size.w - 4 * UI_MARGIN, 10), wm_percent(s_wm->send_offset, e->length), UI_ACCENT);
  }
  y += 16;
  const char *text = s_wm->confirm_delete ? ((e->flags & WM_FLAG_ON_PHONE) ? "Delete here? The phone keeps its copy." :
      "Delete? It is not on the phone yet.") : s_wm->note;
  graphics_context_set_text_color(ctx, GColorDarkGray);
  wm_text(ctx, text, FONT_KEY_GOTHIC_18, GRect(UI_MARGIN, y, b.size.w - 2 * UI_MARGIN, b.size.h - y - UI_HINT_HEIGHT - 2), GTextAlignmentCenter);
  const char *hint;
  if (s_wm->confirm_delete) hint = "DOWN: delete · UP: keep";
  else if (e->flags & WM_FLAG_DELETING) hint = "BACK: list";
  else if (playing) hint = "SELECT: stop";
  else if (e->flags & WM_FLAG_ON_PHONE) hint = "SELECT: play · DOWN: delete";
  else hint = "SEL: play · UP: send · DOWN: del";
  memo_draw_hint(ctx, b, hint);
}

static void wm_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  memo_draw_header(ctx, b, s_wm->view == WM_VIEW_RECORD ? "Recording" : "Watch memos");
  if (s_wm->view == WM_VIEW_RECORD) wm_draw_record(ctx, b);
  else if (s_wm->view == WM_VIEW_ITEM) wm_draw_item(ctx, b);
  else wm_draw_list(ctx, b);
}

static void wm_item_select_row(void) {
  uint8_t order[WM_SLOTS];
  uint8_t count = wm_order(order);
  if (s_wm->selected == 0u) { wm_start_recording(); return; }
  if (s_wm->selected > count) return;
  s_wm->item = order[s_wm->selected - 1u];
  s_wm->view = WM_VIEW_ITEM;
  s_wm->confirm_delete = false;
  s_wm->note[0] = '\0';
}

static void wm_select(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  if (s_wm->view == WM_VIEW_RECORD) {
    wm_finish_recording("");
  } else if (s_wm->view == WM_VIEW_LIST) {
    s_wm->note[0] = '\0';
    wm_item_select_row();
  } else if (!s_wm->confirm_delete && !(s_wm->entries[s_wm->item].flags & WM_FLAG_DELETING)) {
    if (s_wm->player) wm_play_stop(true, "");
    else wm_play_start(s_wm->item);
  }
  wm_redraw();
}

static void wm_up(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  if (s_wm->view == WM_VIEW_LIST) {
    s_wm->note[0] = '\0';
    if (s_wm->selected > 0u) s_wm->selected--;
  } else if (s_wm->view == WM_VIEW_ITEM) {
    if (s_wm->confirm_delete) {
      s_wm->confirm_delete = false;
    } else {
      /* Send now: a memo that failed goes back in the queue. */
      s_wm->failed = (uint16_t)(s_wm->failed & ~(1u << s_wm->item));
      wm_send_pump();
    }
  }
  wm_redraw();
}

static void wm_down(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  if (s_wm->view == WM_VIEW_LIST) {
    uint8_t order[WM_SLOTS];
    s_wm->note[0] = '\0';
    if (s_wm->selected < wm_order(order)) s_wm->selected++;
  } else if (s_wm->view == WM_VIEW_ITEM && !(s_wm->entries[s_wm->item].flags & WM_FLAG_DELETING)) {
    if (!s_wm->confirm_delete) {
      s_wm->confirm_delete = true;
    } else {
      uint8_t slot = s_wm->item;
      if (s_wm->play_slot == (int8_t)slot) wm_play_stop(true, NULL);
      if (s_wm->send_slot == (int8_t)slot) wm_send_finish(false);
      s_wm->failed = (uint16_t)(s_wm->failed & ~(1u << slot));
      s_wm->entries[slot].flags |= WM_FLAG_DELETING;
      (void)wm_save_index();
      s_wm->confirm_delete = false;
      s_wm->view = WM_VIEW_LIST;
      s_wm->selected = 0u;
      wm_work_schedule();
    }
  }
  wm_redraw();
}

static void wm_back(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  if (s_wm->view == WM_VIEW_RECORD) {
    wm_finish_recording("");
  } else if (s_wm->view == WM_VIEW_ITEM) {
    if (s_wm->confirm_delete) s_wm->confirm_delete = false;
    else {
      if (s_wm->player) wm_play_stop(true, NULL);
      s_wm->view = WM_VIEW_LIST;
      s_wm->note[0] = '\0';
    }
  } else {
    window_stack_pop(true);
    return;
  }
  wm_redraw();
}

static void wm_click_config(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_SELECT, wm_select);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 150u, wm_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150u, wm_down);
  window_single_click_subscribe(BUTTON_ID_BACK, wm_back);
}

static void wm_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_wm_layer = layer_create(layer_get_bounds(root));
  if (!s_wm_layer) return;
  layer_set_update_proc(s_wm_layer, wm_draw);
  layer_add_child(root, s_wm_layer);
  s_wm->view = WM_VIEW_LIST;
  s_wm->selected = 0u;
  s_wm->note[0] = '\0';
}

static void wm_window_disappear(Window *window) {
  (void)window;
  /* Covered by another window, or leaving: the microphone and speaker stop; the rest goes on. */
  if (s_wm->rec_slot >= 0) wm_finish_recording("Stopped: Hermes lost focus.");
  if (s_wm->player) wm_play_stop(true, NULL);
}

static void wm_window_unload(Window *window) {
  (void)window;
  if (s_wm_layer) layer_destroy(s_wm_layer);
  s_wm_layer = NULL;
}

static void wm_open(void) {
  if (!s_wm) return;
  /* The call needs firmware with the capped-store API; this feature needs the microphone anyway. */
  if (s_wm->capacity == 0u && memo_mic_available()) {
    size_t max = persist_get_max_size();
    uint32_t room = max > WM_PERSIST_RESERVE ? (uint32_t)(max - WM_PERSIST_RESERVE) / 25u * 24u : 0u;
    s_wm->capacity = room > WM_MAX_STORE ? WM_MAX_STORE : room;
  }
  if (!s_wm_window) {
    s_wm_window = window_create();
    if (!s_wm_window) return;
    window_set_click_config_provider(s_wm_window, wm_click_config);
    window_set_window_handlers(s_wm_window, (WindowHandlers){
      .load = wm_window_load, .disappear = wm_window_disappear, .unload = wm_window_unload,
    });
  }
  window_stack_push(s_wm_window, true);
}

/* Main-menu subtitle. */
static const char *wm_subtitle(void) {
  if (!s_wm) return "Record & play on the watch";
  if (s_wm->send_slot >= 0) {
    return wm_status((uint8_t)s_wm->send_slot, s_wm->subtitle, sizeof(s_wm->subtitle));
  }
  uint8_t order[WM_SLOTS];
  uint8_t count = wm_order(order);
  if (count == 0u) return "Record & play on the watch";
  snprintf(s_wm->subtitle, sizeof(s_wm->subtitle), "%u saved · %lu KB", (unsigned)count, (unsigned long)((wm_used() + 1023u) / 1024u));
  return s_wm->subtitle;
}

/* Reads the index, clears what a crash left behind, and resumes deletes and sending. */
static void wm_init(void) {
  s_wm = calloc(1u, sizeof(WatchMemos));
  if (!s_wm) return;
  s_wm->rec_slot = s_wm->play_slot = s_wm->send_slot = s_wm->delete_slot = -1;
  uint8_t bytes[4u + sizeof(s_wm->entries)];
  uint32_t magic = 0u;
  if (persist_read_data(WM_KEY_INDEX, bytes, sizeof(bytes)) == (int)sizeof(bytes)) {
    memcpy(&magic, bytes, 4u);
    if (magic == WM_INDEX_MAGIC) memcpy(s_wm->entries, bytes + 4u, sizeof(s_wm->entries));
  }
  int32_t active = 0;
  if (persist_read_data(WM_KEY_ACTIVE, &active, sizeof(active)) == (int)sizeof(active) && active >= 1 && active <= (int32_t)WM_SLOTS) {
    /* A recording that never finished: the resuming delete skips values that do not exist. */
    uint8_t slot = (uint8_t)(active - 1);
    if (s_wm->entries[slot].capture == 0u || s_wm->entries[slot].length == 0u) {
      s_wm->entries[slot] = (WmEntry){ .capture = 1u, .length = WM_MAX_CHUNKS * WM_CHUNK, .flags = WM_FLAG_DELETING };
      (void)wm_save_index();
    }
    persist_delete(WM_KEY_ACTIVE);
  }
  s_wm->delete_resume = true; // Cleared when the first interrupted delete ends.
  wm_work_schedule();
}

/* A fresh link: everything gets a new try. */
static void wm_connection(void) {
  if (!s_wm) return;
  s_wm->failed = 0u;
  wm_send_pump();
}

static void wm_shutdown(void) {
  if (!s_wm) return;
  if (s_wm->rec_slot >= 0) wm_finish_recording("");
  if (s_wm->player) wm_play_stop(true, NULL);
  if (s_wm_work_timer) app_timer_cancel(s_wm_work_timer);
  if (s_wm_send_timer) app_timer_cancel(s_wm_send_timer);
  s_wm_work_timer = s_wm_send_timer = NULL;
  if (s_wm_window) window_destroy(s_wm_window);
  s_wm_window = NULL;
  free(s_wm->send_buffer);
  free(s_wm);
  s_wm = NULL;
}
#endif
