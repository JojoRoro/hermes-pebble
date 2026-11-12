// Voice memos: recorded to the watch's flash as 8 kHz IMA ADPCM, kept until the phone has them.
#include "app.h"

#define KEY_INDEX 100
#define KEY_AUDIO 0x10000          // + slot * 0x1000 + block
#define BLOCK 256
#define MIN_BYTES 2000             // half a second
#define RESERVE (48 * 1024)

static Memo s_memos[MEMO_MAX];
static int s_count;

// ---- ADPCM ---------------------------------------------------------------------------------

static const int8_t INDEX_STEP[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t STEPS[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
  80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
  544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
  2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
  12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };

typedef struct {
  int16_t predicted;
  int8_t index;
} Adpcm;

static uint8_t adpcm_encode(Adpcm *s, int16_t sample) {
  int step = STEPS[s->index];
  int diff = sample - s->predicted;
  uint8_t code = 0;
  if (diff < 0) {
    code = 8;
    diff = -diff;
  }
  int delta = step >> 3;
  if (diff >= step) { code |= 4; diff -= step; delta += step; }
  if (diff >= step >> 1) { code |= 2; diff -= step >> 1; delta += step >> 1; }
  if (diff >= step >> 2) { code |= 1; delta += step >> 2; }
  int next = s->predicted + ((code & 8) ? -delta : delta);
  s->predicted = next > 32767 ? 32767 : next < -32768 ? -32768 : next;
  int index = s->index + INDEX_STEP[code & 7];
  s->index = index < 0 ? 0 : index > 88 ? 88 : index;
  return code;
}

static int16_t adpcm_decode(Adpcm *s, uint8_t code) {
  const int step = STEPS[s->index];
  int delta = step >> 3;
  if (code & 4) delta += step;
  if (code & 2) delta += step >> 1;
  if (code & 1) delta += step >> 2;
  int next = s->predicted + ((code & 8) ? -delta : delta);
  s->predicted = next > 32767 ? 32767 : next < -32768 ? -32768 : next;
  int index = s->index + INDEX_STEP[code & 7];
  s->index = index < 0 ? 0 : index > 88 ? 88 : index;
  return s->predicted;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
  }
  return ~crc;
}

// ---- index ---------------------------------------------------------------------------------

static void index_save(void) {
  persist_write_int(KEY_INDEX, s_count);
  for (int i = 0; i < s_count; i++) persist_write_data(KEY_INDEX + 1 + i, &s_memos[i], sizeof(Memo));
}

static uint32_t audio_key(uint8_t slot, uint32_t block) { return KEY_AUDIO + slot * 0x1000u + block; }

static Memo *find(int32_t id) {
  for (int i = 0; i < s_count; i++) {
    if (s_memos[i].id == id) return &s_memos[i];
  }
  return NULL;
}

int memo_count(void) { return s_count; }
const Memo *memo_at(int index) { return index >= 0 && index < s_count ? &s_memos[index] : NULL; }

int memo_queued_count(void) {
  int n = 0;
  for (int i = 0; i < s_count; i++) n += s_memos[i].status == MEMO_QUEUED;
  return n;
}

uint32_t memo_free_seconds(void) {
  uint32_t used = 0;
  for (int i = 0; i < s_count; i++) {
    if (s_memos[i].status != MEMO_ON_PHONE) used += s_memos[i].bytes;
  }
  const uint32_t max = (uint32_t)persist_get_max_size();
  if (s_count >= MEMO_MAX || max < RESERVE + used) return 0;
  uint32_t seconds = (max - RESERVE - used) / MEMO_BYTES_PER_SECOND;
  return seconds > MEMO_MAX_SECONDS ? MEMO_MAX_SECONDS : seconds;
}

// ---- erasing audio, a few blocks at a time ---------------------------------------------------

static uint8_t s_erase_slots;      // bit per slot waiting
static int8_t s_erase_slot = -1;   // the slot being erased now; the cursor belongs to it
static uint32_t s_erase_block;
static AppTimer *s_erase_timer;

static bool s_recording;

#define ERASE_TICK_MS 100
#define ERASE_PER_TICK 6

static void erase_step(void *data) {
  s_erase_timer = NULL;
  // Every storage call scans all of the app's records on the app task, which also drains the
  // microphone: while recording, erasing waits, or the recording would lose audio.
  if (s_recording) {
    s_erase_timer = app_timer_register(500, erase_step, NULL);
    return;
  }
  if (s_erase_slot < 0) {
    // Finish one slot before starting the next, or the cursor would skip blocks.
    for (uint8_t slot = 0; slot < MEMO_MAX; slot++) {
      if (s_erase_slots & (1 << slot)) {
        s_erase_slot = (int8_t)slot;
        s_erase_block = 0;
        break;
      }
    }
  }
  if (s_erase_slot >= 0) {
    for (int n = 0; n < ERASE_PER_TICK; n++) {
      const uint32_t key = audio_key((uint8_t)s_erase_slot, s_erase_block);
      if (!persist_exists(key)) {
        s_erase_slots &= ~(1 << s_erase_slot);
        s_erase_slot = -1;
        break;
      }
      persist_delete(key);
      s_erase_block++;
    }
  }
  if (s_erase_slots) s_erase_timer = app_timer_register(ERASE_TICK_MS, erase_step, NULL);
}

static void erase_audio(uint8_t slot) {
  s_erase_slots |= 1 << slot;
  if (!s_erase_timer) s_erase_timer = app_timer_register(ERASE_TICK_MS, erase_step, NULL);
}

static bool slot_busy(uint8_t slot) {
  if (s_erase_slots & (1 << slot)) return true;
  for (int i = 0; i < s_count; i++) {
    if (s_memos[i].slot == slot && s_memos[i].status != MEMO_ON_PHONE) return true;
  }
  return false;
}

// ---- recording -----------------------------------------------------------------------------

// The microphone is quiet: normal speech sits near ADPCM's smallest steps, where 4-bit coding
// loses it. Like dictation, the watch boosts it first; only button clicks then saturate.
#define INPUT_GAIN 4
#define FILTER_TAPS 7

static Memo s_new;
static int16_t s_hist[FILTER_TAPS];
static uint8_t s_hist_filled;
static bool s_odd;
static Adpcm s_enc;
static uint8_t s_block[BLOCK];
static uint16_t s_block_len;
static uint32_t s_block_no;
static uint8_t s_nibble;
static bool s_have_nibble;
static uint16_t s_level;

static void flush_block(void) {
  if (!s_block_len) return;
  persist_write_data(audio_key(s_new.slot, s_block_no), s_block, s_block_len);
  s_new.crc = crc32_update(s_new.crc, s_block, s_block_len);
  s_new.bytes += s_block_len;
  s_block_no++;
  s_block_len = 0;
}

static void captured(const int16_t *samples, size_t count, void *context) {
  if (!s_recording) return;
  int32_t peak = 0, sum = 0;
  for (size_t i = 0; i < count; i++) sum += samples[i];
  const int32_t mean = count ? sum / (int32_t)count : 0;
  for (size_t i = 0; i < count; i++) {
    // 16 kHz in, 8 kHz kept, through a half-band low-pass (-1 0 9 16 9 0 -1)/32 so what is
    // above 4 kHz does not fold back into the speech.
    memmove(&s_hist[1], &s_hist[0], (FILTER_TAPS - 1) * sizeof(int16_t));
    s_hist[0] = samples[i];
    if (s_hist_filled < FILTER_TAPS) s_hist_filled++;
    s_odd = !s_odd;
    if (s_hist_filled < FILTER_TAPS || !s_odd) continue;
    const int16_t *h = s_hist;
    int32_t y = 16 * (int32_t)h[3] + 9 * ((int32_t)h[2] + h[4]) - ((int32_t)h[0] + h[6]);
    y = y * INPUT_GAIN / 32;
    const int16_t sample = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    // The meter ignores the microphone's DC offset.
    const int32_t level = samples[i] > mean ? samples[i] - mean : mean - samples[i];
    if (level > peak) peak = level;
    const uint8_t code = adpcm_encode(&s_enc, sample);
    if (!s_have_nibble) {
      s_nibble = code;
      s_have_nibble = true;
    } else {
      s_block[s_block_len++] = (uint8_t)(s_nibble | (code << 4));
      s_have_nibble = false;
      if (s_block_len == BLOCK) flush_block();
    }
  }
  // Speech peaks at a few percent of full scale: the meter shows it at that size.
  s_level = (uint16_t)(peak * INPUT_GAIN * 100 / 32768);
  if (s_new.bytes + s_block_len >= (uint32_t)MEMO_MAX_SECONDS * MEMO_BYTES_PER_SECOND) {
    s_recording = false;       // full: the screen notices and wraps up
  }
}

static void capture_stopped(MicCaptureStopReason reason, void *context) {
  s_recording = false;
}

bool memo_record_start(void) {
  if (s_recording || memo_free_seconds() < 5) return false;
  memset(&s_new, 0, sizeof(s_new));
  uint8_t slot = 0;
  while (slot < MEMO_MAX && slot_busy(slot)) slot++;
  if (slot == MEMO_MAX) return false;
  s_new.slot = slot;
  s_new.id = app_new_id();
  s_new.recorded = time(NULL);
  memset(&s_enc, 0, sizeof(s_enc));
  s_block_len = 0;
  s_block_no = 0;
  s_have_nibble = false;
  s_level = 0;
  memset(s_hist, 0, sizeof(s_hist));
  s_hist_filled = 0;
  s_odd = false;
  const MicCaptureStartResult result = mic_capture_start(
      (MicCaptureHandlers){ .data = captured, .stopped = capture_stopped }, NULL);
  if (result != MicCaptureStartOk) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "mic capture: %d", (int)result);
    return false;
  }
  s_recording = true;
  return true;
}

int32_t memo_record_stop(void) {
  mic_capture_stop();
  s_recording = false;
  flush_block();
  if (s_new.id == 0) return 0;
  if (s_new.bytes < MIN_BYTES) {
    erase_audio(s_new.slot);
    s_new.id = 0;
    return 0;
  }
  s_new.seconds = (uint16_t)((s_new.bytes + MEMO_BYTES_PER_SECOND / 2) / MEMO_BYTES_PER_SECOND);
  s_new.status = MEMO_DRAFT;
  // Make room in the list by forgetting the oldest memo that is already on the phone.
  if (s_count == MEMO_MAX) {
    for (int i = 0; i < s_count; i++) {
      if (s_memos[i].status == MEMO_ON_PHONE) {
        memmove(&s_memos[i], &s_memos[i + 1], (s_count - i - 1) * sizeof(Memo));
        s_count--;
        break;
      }
    }
  }
  if (s_count == MEMO_MAX) {
    erase_audio(s_new.slot);
    s_new.id = 0;
    return 0;
  }
  s_memos[s_count++] = s_new;
  index_save();
  const int32_t id = s_new.id;
  s_new.id = 0;
  return id;
}

bool memo_recording(void) { return s_recording; }
uint16_t memo_record_seconds(void) {
  return (uint16_t)((s_new.bytes + s_block_len) / MEMO_BYTES_PER_SECOND);
}
uint16_t memo_record_level(void) { return s_level; }

// ---- playback ------------------------------------------------------------------------------

static int32_t s_play_id;
static uint32_t s_play_block, s_play_blocks;
static uint8_t s_play_slot;
static Adpcm s_dec;
static AppTimer *s_play_timer;
static int16_t s_pcm[BLOCK * 2];
static uint16_t s_pcm_len, s_pcm_pos;       // bytes
static uint8_t s_play_stalled;              // feeds in a row the speaker took nothing
static uint32_t s_play_samples;             // decoded so far
static int32_t s_hp_in, s_hp_out;           // high-pass state
static int32_t s_play_level;                // recent peak, for the gain
static bool s_play_closing;                 // all audio handed over; the speaker is playing out
static uint8_t s_play_stale;                // finish events still due for sounds we stopped

#define PLAY_LEAD_MUTE 2400                 // 300 ms: the press that started the recording
#define PLAY_TARGET 20000
#define PLAY_FLOOR 300                      // below this, pauses: no more gain
#define PLAY_MAX_GAIN_Q8 (12 * 256)

#define PLAY_FEED_MS 40
#define PLAY_STALL_FEEDS 75                 // three seconds without progress: give up
#define PLAY_TAIL_MS 3000                   // after closing, end anyway if Done never arrives

// Stops what this app has on the speaker (the firmware never lets it stop another's sound).
// The finish event for it arrives later, possibly after a new stream is open: count it, so
// that event is not taken for the end of the new stream.
static void speaker_stop_ours(void) {
  if (speaker_get_status() == SpeakerStatusIdle) return;
  if (s_play_stale < 8) s_play_stale++;
  speaker_stop();
}

static void play_ended(void) {
  if (s_play_timer) app_timer_cancel(s_play_timer);
  s_play_timer = NULL;
  s_play_id = 0;
  s_play_closing = false;
  memos_refresh();
}

static void play_finished(SpeakerFinishReason reason, void *context) {
  if (reason != SpeakerFinishReasonDone && s_play_stale) {
    s_play_stale--;
    return;
  }
  if (!s_play_id) return;
  if (reason == SpeakerFinishReasonDone) {
    // Only our own stream's end counts; an earlier sound's Done can arrive late.
    if (s_play_closing) play_ended();
    return;
  }
  // Preempted, stopped by the system or failed: the list must not keep saying Playing, and
  // nothing of ours may stay open (speaker_stop only ever stops this app's sound).
  speaker_stop_ours();
  play_ended();
}

static void play_tail_done(void *data) {
  s_play_timer = NULL;
  play_ended();
}

static void play_feed(void *data) {
  s_play_timer = NULL;
  if (!s_play_id || s_play_closing) return;
  for (;;) {
    if (s_pcm_pos < s_pcm_len) {
      const uint32_t wrote = speaker_stream_write((uint8_t *)s_pcm + s_pcm_pos, s_pcm_len - s_pcm_pos);
      s_pcm_pos += wrote;
      if (s_pcm_pos < s_pcm_len) {
        // The speaker's buffer is full for now. If it never drains, stop rather than hang.
        s_play_stalled = wrote ? 0 : s_play_stalled + 1;
        if (s_play_stalled >= PLAY_STALL_FEEDS) {
          speaker_stop_ours();
          play_ended();
          return;
        }
        break;
      }
      s_play_stalled = 0;
    }
    if (s_play_block >= s_play_blocks) {
      // Still playing until the speaker says Done: a replay meanwhile stops this one first.
      speaker_stream_close();
      s_play_closing = true;
      s_play_timer = app_timer_register(PLAY_TAIL_MS, play_tail_done, NULL);
      return;
    }
    const int got = persist_read_data(audio_key(s_play_slot, s_play_block++), s_block, BLOCK);
    if (got <= 0) {
      // The audio is gone from flash: end here.
      speaker_stop_ours();
      play_ended();
      return;
    }
    s_pcm_len = 0;
    s_pcm_pos = 0;
    int32_t peak = 0;
    for (int i = 0; i < got * 2; i++) {
      const int32_t x = adpcm_decode(&s_dec, i & 1 ? s_block[i / 2] >> 4 : s_block[i / 2] & 15);
      // DC and rumble out: y = x - x' + 0.94 y'.
      s_hp_out = x - s_hp_in + (s_hp_out * 30802 >> 15);
      s_hp_in = x;
      int32_t y = s_play_samples++ < PLAY_LEAD_MUTE ? 0 : s_hp_out;
      y = y > 32767 ? 32767 : y < -32768 ? -32768 : y;
      s_pcm[i] = (int16_t)y;
      if (y < 0) y = -y;
      if (y > peak) peak = y;
    }
    s_pcm_len = (uint16_t)(got * 4);
    // One gain per block, from the recent peak level: quiet speech becomes audible, loud stays.
    s_play_level = peak > s_play_level ? peak : s_play_level - s_play_level / 8;
    const int32_t level = s_play_level > PLAY_FLOOR ? s_play_level : PLAY_FLOOR;
    int32_t gain = PLAY_TARGET * 256 / level;
    if (gain > PLAY_MAX_GAIN_Q8) gain = PLAY_MAX_GAIN_Q8;
    if (gain < 256) gain = 256;
    for (int i = 0; i < got * 2; i++) {
      const int32_t v = s_pcm[i] * gain >> 8;
      s_pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
  }
  s_play_timer = app_timer_register(PLAY_FEED_MS, play_feed, NULL);
}

MemoPlayResult memo_play(int32_t id) {
  const Memo *m = find(id);
  if (!m || m->status == MEMO_ON_PHONE || s_recording) return MEMO_PLAY_UNAVAILABLE;
  // Quiet Time means quiet, even if the speaker itself is not muted.
  if (quiet_time_is_active()) return MEMO_PLAY_QUIET_TIME;
  if (speaker_is_muted()) return MEMO_PLAY_MUTED;
  memo_play_stop();
  // Before opening, so the stream's own finish is never missed.
  speaker_set_finish_callback(play_finished, NULL);
  // Anything of ours still on the speaker (a test tone's tail, a stream left open) would make
  // the open fail. Its finish event must not end the new stream, or that stream would stay
  // open unfed and every later open would be refused as busy.
  speaker_stop_ours();
  if (!speaker_stream_open(SpeakerPcmFormat_8kHz_16bit, 80)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "memo play: open refused, status %d", (int)speaker_get_status());
    return MEMO_PLAY_BUSY;
  }
  s_play_id = id;
  s_play_slot = m->slot;
  s_play_block = 0;
  s_play_blocks = (m->bytes + BLOCK - 1) / BLOCK;
  s_pcm_len = s_pcm_pos = 0;
  s_play_stalled = 0;
  s_play_samples = 0;
  s_play_closing = false;
  s_hp_in = s_hp_out = 0;
  s_play_level = 0;
  memset(&s_dec, 0, sizeof(s_dec));
  play_feed(NULL);
  return MEMO_PLAY_OK;
}

void memo_play_stop(void) {
  if (s_play_timer) app_timer_cancel(s_play_timer);
  s_play_timer = NULL;
  const bool was = s_play_id != 0;
  s_play_id = 0;                   // first, so the finish callback has nothing left to end
  s_play_closing = false;
  if (was) speaker_stop_ours();
}

bool memo_playing(void) { return s_play_id != 0; }

// ---- upload --------------------------------------------------------------------------------

typedef enum { UP_IDLE, UP_BEGUN, UP_SENDING, UP_ENDED } UpState;

static UpState s_up;
static int32_t s_up_id;
static uint32_t s_up_offset;
static time_t s_up_since;

static Memo *next_queued(void) {
  Memo *oldest = NULL;
  for (int i = 0; i < s_count; i++) {
    if (s_memos[i].status == MEMO_QUEUED && (!oldest || s_memos[i].recorded < oldest->recorded)) {
      oldest = &s_memos[i];
    }
  }
  return oldest;
}

int memo_progress(int32_t id) {
  if (id != s_up_id || s_up == UP_IDLE) return -1;
  const Memo *m = find(id);
  return m && m->bytes ? (int)(s_up_offset * 100 / m->bytes) : 0;
}

void memo_pump(void) {
  if (!g_phone.seen || !comm_connected() || !comm_idle() || s_recording) return;
  if (s_up != UP_IDLE && s_up != UP_SENDING && time(NULL) - s_up_since > 12) s_up = UP_IDLE;
  if (s_up == UP_IDLE) {
    Memo *m = next_queued();
    if (!m) return;
    static char when[16];
    snprintf(when, sizeof(when), "%ld", (long)m->recorded);
    s_up_id = m->id;
    s_up_offset = 0;
    if (comm_send(W_MEMO_BEGIN, m->id, (int32_t)m->bytes, m->seconds, (int32_t)m->crc, when, NULL, 0)) {
      s_up = UP_BEGUN;
      s_up_since = time(NULL);
    }
    return;
  }
  if (s_up != UP_SENDING) return;
  Memo *m = find(s_up_id);
  if (!m || m->status != MEMO_QUEUED) {
    s_up = UP_IDLE;
    return;
  }
  if (s_up_offset >= m->bytes) {
    if (comm_send(W_MEMO_END, m->id, 0, 0, 0, NULL, NULL, 0)) {
      s_up = UP_ENDED;
      s_up_since = time(NULL);
    }
    return;
  }
  // Whole blocks, as many as one message holds. Offsets stay on block boundaries.
  static uint8_t chunk[MEMO_CHUNK];
  uint16_t len = 0;
  uint32_t block = s_up_offset / BLOCK;
  while (len + BLOCK <= MEMO_CHUNK && s_up_offset + len < m->bytes) {
    const int got = persist_read_data(audio_key(m->slot, block++), chunk + len, BLOCK);
    if (got <= 0) break;
    len += (uint16_t)got;
  }
  if (len == 0) {
    // The audio is gone from flash: nothing left to send.
    m->status = MEMO_DRAFT;
    index_save();
    s_up = UP_IDLE;
    return;
  }
  if (comm_send(W_MEMO_DATA, m->id, (int32_t)s_up_offset, 0, 0, NULL, chunk, len)) {
    s_up_offset += len;
    if ((s_up_offset / len) % 8 == 0) memos_refresh();
  }
}

void memo_on_msg(const Msg *m) {
  if (m->rid != s_up_id) return;
  Memo *memo = find(m->rid);
  if (!memo) {
    s_up = UP_IDLE;
    return;
  }
  if (m->a == MEMO_ACK_STORED) {
    // Only now does the watch let go of its copy.
    memo->status = MEMO_ON_PHONE;
    erase_audio(memo->slot);
    index_save();
    s_up = UP_IDLE;
  } else if (m->a == MEMO_ACK_RESTART) {
    s_up = UP_IDLE;
  } else {
    s_up_offset = (uint32_t)m->b - ((uint32_t)m->b % BLOCK);
    s_up = UP_SENDING;
  }
  memos_refresh();
  home_refresh();
  memo_pump();
}

void memo_send(int32_t id) {
  Memo *m = find(id);
  if (!m || m->status != MEMO_DRAFT) return;
  m->status = MEMO_QUEUED;
  index_save();
  memo_pump();
}

void memo_delete(int32_t id) {
  Memo *m = find(id);
  if (!m) return;
  if (s_play_id == id) memo_play_stop();
  if (s_up_id == id) s_up = UP_IDLE;
  if (m->status != MEMO_ON_PHONE) erase_audio(m->slot);
  const int index = (int)(m - s_memos);
  memmove(&s_memos[index], &s_memos[index + 1], (s_count - index - 1) * sizeof(Memo));
  s_count--;
  index_save();
}

void memo_init(void) {
  s_count = persist_exists(KEY_INDEX) ? persist_read_int(KEY_INDEX) : 0;
  if (s_count < 0 || s_count > MEMO_MAX) s_count = 0;
  for (int i = 0; i < s_count; i++) {
    if (persist_read_data(KEY_INDEX + 1 + i, &s_memos[i], sizeof(Memo)) != (int)sizeof(Memo)) {
      s_count = i;
      break;
    }
  }
}

void memo_deinit(void) {
  if (s_recording) memo_record_stop();
  memo_play_stop();
}
