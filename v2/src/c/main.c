// Hermes 2 for Pebble Time 2.
#include "app.h"
#include "ui.h"

#define KEY_PREFS 1
#define KEY_LAST_TIME 2
#define KEY_LAST_TEXT 3
#define KEY_LAST_RID 4
#define KEY_DRAFT_META 10
#define KEY_DRAFT_TEXT 11      // ..14: four 250-byte pieces

PhoneState g_phone;
Prefs g_prefs = { .touch = true };

static char s_last_question[120];
static time_t s_last_time;
static int32_t s_last_rid;
static int s_last_state = -1;
static bool s_quick_ask;

// ---- labels ------------------------------------------------------------------------------

const char *state_label(int state) {
  switch (state) {
    case ST_WAITING_PHONE: return "Waiting for phone";
    case ST_QUEUED: return "Queued";
    case ST_SUBMITTING: return "Submitting";
    case ST_ACCEPTED: return "Accepted";
    case ST_WORKING: return "Working";
    case ST_APPROVAL: return "Needs approval";
    case ST_ANSWERED: return "Answered";
    case ST_FAILED: return "Failed";
    case ST_STOPPED: return "Stopped";
    case ST_CANCELLED: return "Cancelled";
    case ST_STOPPING: return "Stopping";
    default: return "Outcome unknown";
  }
}

GColor state_color(int state) {
  switch (state) {
    case ST_ANSWERED: return GColorIslamicGreen;
    case ST_FAILED: return GColorRed;
    case ST_APPROVAL: return GColorOrange;
    case ST_STOPPED:
    case ST_CANCELLED:
    case ST_UNKNOWN: return GColorDarkGray;
    case ST_WAITING_PHONE:
    case ST_QUEUED: return GColorLightGray;
    default: return GColorVividCerulean;
  }
}

// ---- prefs and the last question ---------------------------------------------------------

void prefs_save(void) {
  persist_write_data(KEY_PREFS, &g_prefs, sizeof(g_prefs));
  app_touch_navigation_enable(g_prefs.touch);
}

void last_question_set(int32_t rid, const char *text) {
  s_last_rid = rid;
  s_last_state = ST_WAITING_PHONE;
  persist_write_int(KEY_LAST_RID, rid);
  strncpy(s_last_question, text, sizeof(s_last_question) - 1);
  s_last_question[sizeof(s_last_question) - 1] = 0;
  s_last_time = time(NULL);
  persist_write_string(KEY_LAST_TEXT, s_last_question);
  persist_write_int(KEY_LAST_TIME, (int32_t)s_last_time);
}

const char *last_question(void) { return s_last_question; }
time_t last_question_time(void) { return s_last_time; }
int last_question_state(void) { return s_last_state; }

static void last_question_track(const Msg *m, int state) {
  if (!s_last_rid || m->rid != s_last_rid || s_last_state == state) return;
  s_last_state = state;
  home_refresh();
}

int32_t app_new_id(void) {
  int32_t id = (int32_t)(((uint32_t)rand() << 15) ^ (uint32_t)rand()) & 0x7fffffff;
  return id ? id : 1;
}

// ---- draft -------------------------------------------------------------------------------

typedef struct {
  int32_t rid;
  uint16_t len;
  uint8_t flags;
} DraftMeta;

static bool draft_meta(DraftMeta *meta) {
  return persist_read_data(KEY_DRAFT_META, meta, sizeof(*meta)) == (int)sizeof(*meta) && meta->rid;
}

bool draft_exists(void) {
  DraftMeta meta;
  return draft_meta(&meta);
}

int32_t draft_rid(void) {
  DraftMeta meta;
  return draft_meta(&meta) ? meta.rid : 0;
}

uint8_t draft_flags(void) {
  DraftMeta meta;
  return draft_meta(&meta) ? meta.flags : 0;
}

bool draft_load(char *out) {
  DraftMeta meta;
  out[0] = 0;
  if (!draft_meta(&meta) || meta.len > TEXT_MAX) return false;
  for (int piece = 0; piece * 250 < meta.len; piece++) {
    const int want = meta.len - piece * 250 > 250 ? 250 : meta.len - piece * 250;
    if (persist_read_data(KEY_DRAFT_TEXT + piece, out + piece * 250, want) != want) {
      out[0] = 0;
      return false;
    }
  }
  out[meta.len] = 0;
  return true;
}

void draft_save(int32_t rid, uint8_t flags, const char *text) {
  size_t len = strlen(text);
  if (len > TEXT_MAX) len = TEXT_MAX;
  for (size_t piece = 0; piece * 250 < len; piece++) {
    const size_t n = len - piece * 250 > 250 ? 250 : len - piece * 250;
    persist_write_data(KEY_DRAFT_TEXT + piece, text + piece * 250, n);
  }
  // The meta record goes last: a draft exists only once all of its text does.
  DraftMeta meta = { .rid = rid, .len = (uint16_t)len, .flags = flags };
  persist_write_data(KEY_DRAFT_META, &meta, sizeof(meta));
}

void draft_clear(void) {
  persist_delete(KEY_DRAFT_META);
  for (int piece = 0; piece < 4; piece++) persist_delete(KEY_DRAFT_TEXT + piece);
}

// ---- messages from the phone -------------------------------------------------------------

static void resend_draft(void) {
  static char text[TEXT_MAX + 1];
  if (draft_load(text)) comm_send(W_ASK, draft_rid(), draft_flags(), 0, 0, text, NULL, 0);
}

static void on_state(const Msg *m) {
  const bool first = !g_phone.seen;
  g_phone.seen = true;
  g_phone.flags = (uint8_t)m->a;
  g_phone.percent = (uint8_t)m->b;
  g_phone.idle_min = m->c;
  strncpy(g_phone.model, m->txt, sizeof(g_phone.model) - 1);
  strncpy(g_phone.model_size, m->txt2, sizeof(g_phone.model_size) - 1);
  if (first) {
    // Anything that was waiting for the phone goes now.
    resend_draft();
    note_sync();
    memo_pump();
    if (s_quick_ask) {
      s_quick_ask = false;
      ask_start();
    }
  }
  model_on_state();
  home_refresh();
}

void app_dispatch(const Msg *m) {
  switch (m->op) {
    case P_READY:
      comm_send(W_HELLO, 0, PROTO_VERSION, 0, 0, NULL, NULL, 0);
      break;
    case P_STATE:
      on_state(m);
      break;
    case P_PARTIAL:
    case P_FINAL:
    case P_VAD:
      listen_on_msg(m);
      break;
    case P_STORED:
      if (m->rid == draft_rid()) draft_clear();
      if (s_last_state == ST_WAITING_PHONE) last_question_track(m, ST_QUEUED);
      thread_on_msg(m);
      home_refresh();
      break;
    case P_DONE:
      // One pulse per answer, and none during Quiet Time.
      if (!quiet_time_is_active()) vibes_short_pulse();
      last_question_track(m, ST_ANSWERED);
      // An answer to a question the user has stepped away from opens itself, but only from home:
      // it never pushes in front of something else the user is doing.
      if (home_on_top()) thread_push(m->rid, NULL, ST_ANSWERED);
      else thread_on_msg(m);
      break;
    case P_ITEM:
      last_question_track(m, m->a);
      thread_on_msg(m);
      break;
    case P_PAGE:
      thread_on_msg(m);
      break;
    case P_RECENT:
      recent_on_msg(m);
      break;
    case P_MEMO_ACK:
      memo_on_msg(m);
      break;
    case P_NOTE_ACK:
      note_on_msg(m);
      break;
    case P_PONG:
      settings_on_msg(m);
      break;
    case P_TOAST:
      app_toast(m->txt);
      break;
  }
}

void app_toast(const char *text) {
  dialog_push(&(DialogSpec){ .label = "HERMES", .title = "Heads up", .body = text, .progress = -1 });
}

// ---- lifecycle -----------------------------------------------------------------------------

static void init(void) {
  srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)&g_phone);
  if (persist_exists(KEY_PREFS)) persist_read_data(KEY_PREFS, &g_prefs, sizeof(g_prefs));
  g_prefs.next_new_conv = false;
  if (persist_exists(KEY_LAST_TEXT)) {
    persist_read_string(KEY_LAST_TEXT, s_last_question, sizeof(s_last_question));
    s_last_time = persist_read_int(KEY_LAST_TIME);
    s_last_rid = persist_read_int(KEY_LAST_RID);
  }
  app_touch_navigation_enable(g_prefs.touch);
  memo_init();
  home_push();
  comm_init();
  // Quick Launch goes straight to listening, as soon as the phone has said what it can do.
  s_quick_ask = launch_reason() == APP_LAUNCH_QUICK_LAUNCH;
}

static void deinit(void) {
  listen_deinit();
  memo_deinit();
  comm_deinit();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
