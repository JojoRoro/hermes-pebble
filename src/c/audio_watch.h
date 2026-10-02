#ifndef HERMES_AUDIO_WATCH_H
#define HERMES_AUDIO_WATCH_H

// Included by the watch app after its transport declarations. Keep audio out of
// persistent storage and feed the speaker without blocking the app event loop.
static bool audio_send_status(uint32_t session, uint32_t total, uint32_t checksum,
                              uint32_t received, uint8_t status);
static bool audio_dictation_active(void);

typedef struct {
  uint8_t *bytes;
  uint32_t session, total, checksum, received, written, play_request, idle_ms;
  uint8_t phase, terminal_status;
  bool draining;
} WatchAudio;
typedef struct {
  uint32_t session, request, total, checksum, received, age_ms;
  uint8_t status;
  bool pending;
} AudioReply;
static WatchAudio s_audio;
static AudioReply s_audio_reply;
static AppTimer *s_audio_timer;
static uint32_t s_audio_tick_ms;

static void audio_tick(void *context);
static void audio_schedule(void) {
  if (s_audio_timer == NULL && (s_audio.phase == 1u || s_audio.phase == 2u || s_audio_reply.pending)) {
    s_audio_tick_ms = s_audio.phase == 2u ? 20u : 100u;
    s_audio_timer = app_timer_register(s_audio_tick_ms, audio_tick, NULL);
  }
}

static uint32_t audio_checksum(const uint8_t *bytes, uint32_t length) {
  uint32_t hash = 0x811c9dc5u;
  for (uint32_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * 0x01000193u;
  return hash;
}

static void audio_reply(uint32_t session, uint32_t request, uint32_t total,
                        uint32_t checksum, uint32_t received, uint8_t status) {
  s_audio_reply = (AudioReply){ .session = session, .request = request, .total = total,
    .checksum = checksum, .received = received, .status = status, .pending = true };
  audio_schedule();
}

static void audio_terminal(uint8_t status) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Audio end status=%u received=%lu written=%lu total=%lu",
    status, (unsigned long)s_audio.received, (unsigned long)s_audio.written, (unsigned long)s_audio.total);
  bool was_playing = s_audio.phase == 2u;
  s_audio.phase = 3u;
  s_audio.terminal_status = status;
  if (was_playing) {
    speaker_set_finish_callback(NULL, NULL);
    if (status != HERMES_AUDIO_COMPLETE) speaker_stop();
  }
  free(s_audio.bytes);
  s_audio.bytes = NULL;
  if (s_audio.play_request != 0u) {
    audio_reply(s_audio.session, s_audio.play_request, s_audio.total, s_audio.checksum,
                s_audio.received, status);
  }
}

static void audio_finished(SpeakerFinishReason reason, void *context) {
  (void)context;
  APP_LOG(APP_LOG_LEVEL_INFO, "Audio speaker finish reason=%d", (int)reason);
  if (s_audio.phase != 2u) return;
  uint8_t status = reason == SpeakerFinishReasonDone && s_audio.draining &&
    s_audio.written == s_audio.total ? HERMES_AUDIO_COMPLETE : HERMES_AUDIO_FAILED;
  if (reason == SpeakerFinishReasonStopped || reason == SpeakerFinishReasonPreempted) status = HERMES_AUDIO_CANCELLED;
  if (quiet_time_is_active()) status = HERMES_AUDIO_QUIET_TIME;
  audio_terminal(status);
}

static void audio_tick(void *context) {
  (void)context;
  s_audio_timer = NULL;
  s_audio.idle_ms += s_audio_tick_ms;
  if ((s_audio.phase == 1u || s_audio.phase == 2u) && quiet_time_is_active()) {
    audio_terminal(HERMES_AUDIO_QUIET_TIME);
  }
  if (s_audio.phase == 1u && s_audio.idle_ms >= 30000u) audio_terminal(HERMES_AUDIO_FAILED);
  if (s_audio.phase == 2u) {
    if (speaker_is_muted()) {
      audio_terminal(HERMES_AUDIO_MUTED);
    } else if (s_audio.idle_ms >= 10000u) {
      audio_terminal(HERMES_AUDIO_FAILED);
    } else if (!s_audio.draining) {
      uint32_t count = s_audio.total - s_audio.written;
      if (count > 1024u) count = 1024u;
      uint32_t written = speaker_stream_write(s_audio.bytes + s_audio.written, count);
      if (written > count) {
        audio_terminal(HERMES_AUDIO_FAILED);
      } else {
        s_audio.written += written; // Includes zero/partial writes when the speaker buffer is full.
        if (s_audio.written == s_audio.total) {
          s_audio.draining = true;
          speaker_stream_close();
        }
      }
    }
  }
  if (s_audio_reply.pending) {
    s_audio_reply.age_ms += s_audio_tick_ms;
    if (audio_send_status(s_audio_reply.session, s_audio_reply.total, s_audio_reply.checksum,
                          s_audio_reply.received, s_audio_reply.status) || s_audio_reply.age_ms >= 30000u) {
      s_audio_reply.pending = false;
    }
  }
  audio_schedule();
}

static void audio_handle(uint8_t kind, uint32_t session, uint32_t request, uint32_t total,
                          uint32_t checksum, uint8_t format, uint32_t offset,
                          const uint8_t *bytes, uint16_t length) {
  uint8_t status = HERMES_AUDIO_INVALID;
  uint32_t received = 0u;
  if (kind == HERMES_KIND_AUDIO_CANCEL) {
    if (session == s_audio.session && (s_audio.phase == 1u || s_audio.phase == 2u)) {
      audio_terminal(HERMES_AUDIO_CANCELLED);
    }
    return;
  }
  // Quiet Time is independent of the user's optional "mute during Quiet Time" setting.
  // Reject every stage, including blocks, and discard any already buffered clip.
  if (quiet_time_is_active()) {
    if (s_audio.phase == 1u || s_audio.phase == 2u) audio_terminal(HERMES_AUDIO_QUIET_TIME);
    status = HERMES_AUDIO_QUIET_TIME;
    goto reply;
  }
  if (session == 0u || total == 0u || total > HERMES_AUDIO_MAX_BYTES || format != HERMES_AUDIO_FORMAT) goto reply;
  if (kind == HERMES_KIND_AUDIO_BEGIN) {
    if (length != 0u || offset != 0u) goto reply;
    if (s_audio.phase == 2u || audio_dictation_active() || speaker_get_status() != SpeakerStatusIdle) {
      status = HERMES_AUDIO_BUSY;
    } else if (speaker_is_muted()) {
      status = HERMES_AUDIO_MUTED;
    } else if (session == s_audio.session && s_audio.phase != 0u) {
      if (total == s_audio.total && checksum == s_audio.checksum) {
        status = s_audio.phase == 1u ? HERMES_AUDIO_READY : s_audio.terminal_status;
      }
    } else {
      free(s_audio.bytes);
      memset(&s_audio, 0, sizeof(s_audio));
      s_audio.bytes = malloc(total);
      if (s_audio.bytes == NULL) {
        status = HERMES_AUDIO_FAILED;
        goto reply;
      }
      s_audio.session = session;
      s_audio.total = total;
      s_audio.checksum = checksum;
      s_audio.phase = 1u;
      status = HERMES_AUDIO_READY;
    }
    goto reply;
  }
  if (session != s_audio.session || total != s_audio.total || checksum != s_audio.checksum) goto reply;
  received = s_audio.received;
  if (kind == HERMES_KIND_AUDIO_BLOCK) {
    if (s_audio.phase != 1u || length == 0u || bytes == NULL || offset > total || length > total - offset) goto reply;
    if (offset == s_audio.received) {
      memcpy(s_audio.bytes + offset, bytes, length);
      s_audio.received += length;
    } else if (offset > s_audio.received || length > s_audio.received - offset ||
                memcmp(s_audio.bytes + offset, bytes, length) != 0) goto reply;
    s_audio.idle_ms = 0u;
    received = s_audio.received;
    status = HERMES_AUDIO_BUFFERED;
  } else if (kind == HERMES_KIND_AUDIO_PLAY) {
    if (length != 0u || offset != 0u) goto reply;
    if (s_audio.phase == 3u) {
      status = s_audio.terminal_status; // A duplicate play never starts the same clip again.
    } else if (s_audio.phase == 2u) {
      if (request == s_audio.play_request) return;
      status = HERMES_AUDIO_BUSY;
    } else if (s_audio.phase == 1u && received == total && audio_checksum(s_audio.bytes, total) == checksum) {
      if (speaker_is_muted()) {
        status = HERMES_AUDIO_MUTED;
      } else if (audio_dictation_active() || speaker_get_status() != SpeakerStatusIdle) {
        status = HERMES_AUDIO_BUSY;
      } else {
        speaker_set_finish_callback(audio_finished, NULL);
        if (!speaker_stream_open(SpeakerPcmFormat_8kHz_8bit, 80)) {
          APP_LOG(APP_LOG_LEVEL_WARNING, "Audio speaker stream open failed");
          speaker_set_finish_callback(NULL, NULL);
          status = HERMES_AUDIO_FAILED;
        } else {
          s_audio.phase = 2u;
          s_audio.play_request = request;
          s_audio.idle_ms = 0u;
          s_audio.written = 0u;
          s_audio.draining = false;
          audio_schedule();
          return; // Completion comes from the speaker callback, not the transport ACK.
        }
      }
    }
  }
reply:
  audio_reply(session, request, total, checksum, received, status);
}

static void audio_shutdown(void) {
  bool playing = s_audio.phase == 2u;
  s_audio.phase = 0u;
  s_audio_reply.pending = false;
  if (s_audio_timer != NULL) app_timer_cancel(s_audio_timer);
  s_audio_timer = NULL;
  if (playing) {
    speaker_set_finish_callback(NULL, NULL);
    speaker_stop();
  }
  free(s_audio.bytes);
  s_audio.bytes = NULL;
}
#endif
