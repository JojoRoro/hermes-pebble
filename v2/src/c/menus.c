// The More menu, watch settings, the speech-model prompt and the shared action menu.
#include "app.h"
#include "ui.h"
#include "menu.h"

// ---- action menu ---------------------------------------------------------------------------

static ActionMenuLevel *s_level;
static void (*s_chosen)(void);

static void menu_perform(ActionMenu *menu, const ActionMenuItem *item, void *context) {
  s_chosen = (void (*)(void))action_menu_item_get_action_data(item);
}

static void menu_closed(ActionMenu *menu, const ActionMenuItem *item, void *context) {
  action_menu_hierarchy_destroy(s_level, NULL, NULL);
  s_level = NULL;
  // Run the choice once the menu is gone, so it can push windows freely.
  void (*run)(void) = s_chosen;
  s_chosen = NULL;
  if (run) run();
}

void menu_open(const MenuItem *items, int count) {
  if (s_level || count <= 0) return;
  s_level = action_menu_level_create(count);
  for (int i = 0; i < count; i++) {
    action_menu_level_add_action(s_level, items[i].label, menu_perform, (void *)items[i].run);
  }
  ActionMenuConfig config = {
    .root_level = s_level,
    .colors = { .background = C_GOLD, .foreground = C_INK },
    .did_close = menu_closed,
    .align = ActionMenuAlignCenter,
  };
  action_menu_open(&config);
}

// ---- the speech model ---------------------------------------------------------------------

static bool s_model_open;
static bool s_model_then_listen;     // opened on the way to a question

static void model_closed(void) { s_model_open = false; }

static void model_show(void);

static void model_download(void) {
  comm_send(W_MODEL, 0, MODEL_DOWNLOAD, 0, 0, NULL, NULL, 0);
  g_phone.flags |= PF_DOWNLOADING;
  g_phone.flags &= ~PF_MODEL_ERROR;
  g_phone.percent = 0;
  model_show();
}

static void model_use_pebble(void) {
  comm_send(W_MODEL, 0, MODEL_USE_PEBBLE, 0, 0, NULL, NULL, 0);
  g_phone.flags &= ~PF_STT_PHONE;
  const bool listen = s_model_then_listen;
  dialog_pop();
  if (listen) dictation_start();
}

static void model_use_phone(void) {
  comm_send(W_MODEL, 0, MODEL_USE_PHONE, 0, 0, NULL, NULL, 0);
  g_phone.flags |= PF_STT_PHONE;
  model_show();
}

static void model_dictate_now(void) {
  dialog_pop();
  dictation_start();
}

static void model_cancel(void) {
  comm_send(W_MODEL, 0, MODEL_CANCEL, 0, 0, NULL, NULL, 0);
  g_phone.flags &= ~PF_DOWNLOADING;
  dialog_pop();
}

static void model_show(void) {
  static char body[160];
  DialogSpec spec = { .label = "SPEECH", .progress = -1, .on_close = model_closed };
  const uint8_t f = g_phone.flags;
  if (!(f & PF_STT_SUPPORT)) {
    spec.title = "Pebble dictation";
    spec.body = "This phone cannot run the offline speech model, so Hermes uses Pebble dictation.";
  } else if (!(f & PF_STT_PHONE)) {
    spec.title = "Pebble dictation";
    spec.body = "Hermes can transcribe on your phone instead, with no cloud service.";
    spec.select_icon = ICON_CHECK;
    spec.select_label = "Use phone model";
    spec.on_select = model_use_phone;
  } else if (f & PF_DOWNLOADING) {
    snprintf(body, sizeof(body), "%s, %s. Keep Hermes open till it is done.", g_phone.model,
             g_phone.model_size);
    spec.title = "Downloading";
    spec.body = body;
    spec.progress = g_phone.percent;
    if (s_model_then_listen) {
      spec.select_icon = ICON_MIC;
      spec.select_label = "Dictate for now";
      spec.on_select = model_dictate_now;
    }
    spec.down_icon = ICON_CLOSE;
    spec.down_label = "Cancel download";
    spec.on_down = model_cancel;
  } else if (f & PF_MODEL_READY) {
    snprintf(body, sizeof(body), "%s transcribes on your phone. Nothing goes to a cloud service.",
             g_phone.model);
    spec.title = "Offline speech is on";
    spec.body = body;
    spec.down_icon = ICON_CLOSE;
    spec.down_label = "Use Pebble dictation";
    spec.on_down = model_use_pebble;
  } else {
    snprintf(body, sizeof(body), "%s%s, %s. Runs on your phone, no cloud.",
             (f & PF_MODEL_ERROR) ? "Download failed. " : "", g_phone.model, g_phone.model_size);
    spec.title = "Offline speech";
    spec.body = body;
    spec.select_icon = ICON_DOWN;
    spec.select_label = (f & PF_MODEL_ERROR) ? "Try again" : "Download";
    spec.on_select = model_download;
    spec.down_icon = ICON_CLOSE;
    spec.down_label = "Pebble dictation";
    spec.on_down = model_use_pebble;
  }
  dialog_push(&spec);
  s_model_open = true;
}

void model_push(void) {
  s_model_then_listen = false;
  model_show();
}

void model_on_state(void) {
  if (!s_model_open || !dialog_open()) return;
  const uint8_t f = g_phone.flags;
  if ((f & PF_DOWNLOADING) && !(f & PF_MODEL_ERROR)) {
    dialog_update(NULL, NULL, g_phone.percent);
    return;
  }
  if ((f & PF_MODEL_READY) && (f & PF_STT_PHONE) && s_model_then_listen) {
    // The download finished while the user waited to ask: go straight on.
    dialog_pop();
    listen_push();
    return;
  }
  model_show();
}

static void need(const char *title, const char *body) {
  dialog_push(&(DialogSpec){ .label = "HERMES", .title = title, .body = body, .progress = -1 });
}

void ask_start(void) {
  if (!comm_connected()) {
    need("No phone", "Hermes talks through the Pebble app on your phone. Check Bluetooth.");
    return;
  }
  if (!g_phone.seen) {
    comm_hello();
    need("Finding phone", "The Pebble app has not answered yet. Open it on your phone, then try again.");
    return;
  }
  if (!(g_phone.flags & PF_CONFIGURED)) {
    need("Set up Hermes", "In the Pebble app, open Hermes 2 settings and enter your server address and API key.");
    return;
  }
  const uint8_t f = g_phone.flags;
  if ((f & PF_STT_PHONE) && (f & PF_STT_SUPPORT)) {
    if (f & PF_MODEL_READY) {
      listen_push();
    } else {
      s_model_then_listen = true;
      model_show();
    }
  } else {
    dictation_start();
  }
}

// ---- settings ------------------------------------------------------------------------------

static ListScreen *s_settings;
static int32_t s_ping_nonce;
static uint16_t s_ping_ms;
static time_t s_ping_s;
static AppTimer *s_ping_timer;

static void ping_closed(void) {
  // Gone before the answer: a late pong must not write into some other dialog.
  s_ping_nonce = 0;
  if (s_ping_timer) app_timer_cancel(s_ping_timer);
  s_ping_timer = NULL;
}

static void ping_timeout(void *data) {
  s_ping_timer = NULL;
  if (!s_ping_nonce) return;
  s_ping_nonce = 0;
  dialog_update("No answer", "Hermes on your phone did not answer. Open the Pebble app on your phone and try again.", -1);
}

enum { SET_TOUCH, SET_SPEECH, SET_LINK, SET_DRAFT, SET_SOUND, SET_ABOUT, SET_COUNT };

static int settings_count(void) { return SET_COUNT; }

static void settings_row(int index, ListRow *row) {
  switch (index) {
    case SET_TOUCH:
      snprintf(row->title, sizeof(row->title), "Touch");
      snprintf(row->sub, sizeof(row->sub), g_prefs.touch ? "On: tap and swipe" : "Off: buttons only");
      break;
    case SET_SPEECH:
      snprintf(row->title, sizeof(row->title), "Speech");
      if (!g_phone.seen) snprintf(row->sub, sizeof(row->sub), "Waiting for phone");
      else if (!(g_phone.flags & PF_STT_PHONE) || !(g_phone.flags & PF_STT_SUPPORT))
        snprintf(row->sub, sizeof(row->sub), "Pebble dictation");
      else if (g_phone.flags & PF_DOWNLOADING)
        snprintf(row->sub, sizeof(row->sub), "Downloading %d%%", g_phone.percent);
      else if (g_phone.flags & PF_MODEL_READY)
        snprintf(row->sub, sizeof(row->sub), "On phone: %s", g_phone.model);
      else snprintf(row->sub, sizeof(row->sub), "Model not downloaded");
      break;
    case SET_LINK:
      snprintf(row->title, sizeof(row->title), "Phone link");
      snprintf(row->sub, sizeof(row->sub), !comm_connected() ? "Not connected"
               : g_phone.seen ? "Connected: test it" : "Reconnect");
      break;
    case SET_DRAFT:
      snprintf(row->title, sizeof(row->title), "Saved draft");
      snprintf(row->sub, sizeof(row->sub), draft_exists() ? "Resume or discard" : "None");
      break;
    case SET_SOUND:
      snprintf(row->title, sizeof(row->title), "Test sound");
      snprintf(row->sub, sizeof(row->sub), quiet_time_is_active() ? "Quiet Time is on"
               : speaker_is_muted() ? "Speaker is muted" : "Plays a short tone");
      break;
    case SET_ABOUT:
      snprintf(row->title, sizeof(row->title), "Hermes 2");
      snprintf(row->sub, sizeof(row->sub), "Version 2.0.0");
      break;
  }
}

static void draft_resume(void) {
  static char text[TEXT_MAX + 1];
  if (!draft_load(text)) return;
  g_prefs.next_new_conv = (draft_flags() & ASK_NEW_CONV) != 0;
  // The draft stays saved until it is sent again, under the same id.
  const int32_t rid = draft_rid();
  dialog_pop();
  review_push_draft(rid, text);
}

static void draft_discard(void) {
  draft_clear();
  dialog_pop();
  home_refresh();
}

static void settings_select(int index) {
  static char text[TEXT_MAX + 1];
  switch (index) {
    case SET_TOUCH:
      g_prefs.touch = !g_prefs.touch;
      prefs_save();
      break;
    case SET_SPEECH:
      if (g_phone.seen) model_push();
      break;
    case SET_LINK:
      if (!comm_connected()) {
        need("No phone", "The watch is not connected to the Pebble app. Check Bluetooth on your phone.");
      } else {
        g_phone.seen = false;
        comm_hello();
        s_ping_nonce = app_new_id();
        time_ms(&s_ping_s, &s_ping_ms);
        comm_send(W_PING, 0, s_ping_nonce, 0, 0, NULL, NULL, 0);
        dialog_push(&(DialogSpec){ .label = "PHONE LINK", .title = "Asking the phone",
                                   .body = "Waiting for Hermes on your phone to answer.", .progress = -1,
                                   .on_close = ping_closed });
        if (s_ping_timer) app_timer_cancel(s_ping_timer);
        s_ping_timer = app_timer_register(10000, ping_timeout, NULL);
      }
      break;
    case SET_DRAFT:
      if (draft_load(text)) {
        dialog_push(&(DialogSpec){
          .label = "SAVED DRAFT", .title = "Not sent yet", .body = text, .progress = -1,
          .select_icon = ICON_SEND, .select_label = "Review and send", .on_select = draft_resume,
          .down_icon = ICON_CLOSE, .down_label = "Discard", .on_down = draft_discard });
      } else {
        need("No saved draft", "Every question you sent has reached your phone.");
      }
      break;
    case SET_SOUND:
      if (!quiet_time_is_active() && !speaker_is_muted()) {
        static const SpeakerNote notes[] = {
          { .midi_note = 76, .waveform = SpeakerWaveformSine, .duration_ms = 140 },
          { .midi_note = 81, .waveform = SpeakerWaveformSine, .duration_ms = 220 },
        };
        speaker_play_notes(notes, ARRAY_LENGTH(notes), 70);
      }
      break;
  }
  list_reload(s_settings);
}

void settings_on_msg(const Msg *m) {
  if (m->op != P_PONG || m->a != s_ping_nonce || !dialog_open()) return;
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  static char body[80];
  snprintf(body, sizeof(body), "Hermes on your phone answered in %d ms.",
           (int)((s - s_ping_s) * 1000 + (int)ms - (int)s_ping_ms));
  dialog_update("Link works", body, -1);
  s_ping_nonce = 0;
  if (s_ping_timer) app_timer_cancel(s_ping_timer);
  s_ping_timer = NULL;
}

static void settings_unload(void) { s_settings = NULL; }

static const ListSpec s_settings_spec = {
  .heading = "SETTINGS", .count = settings_count, .row = settings_row, .select = settings_select,
  .unload = settings_unload,
};

void settings_push(void) { s_settings = list_push(&s_settings_spec); }

// ---- more ----------------------------------------------------------------------------------

static ListScreen *s_more;

enum { MORE_NEW, MORE_RECORD, MORE_MEMOS, MORE_NOTE, MORE_SETTINGS, MORE_COUNT };

static int more_count(void) { return MORE_COUNT; }

static void more_row(int index, ListRow *row) {
  switch (index) {
    case MORE_NEW:
      snprintf(row->title, sizeof(row->title), "New conversation");
      snprintf(row->sub, sizeof(row->sub), g_prefs.next_new_conv ? "Next question starts it"
               : "Start with a clean context");
      row->has_accent = g_prefs.next_new_conv;
      row->accent = C_GOLD;
      break;
    case MORE_RECORD:
      snprintf(row->title, sizeof(row->title), "Record memo");
      snprintf(row->sub, sizeof(row->sub), "Works without your phone");
      break;
    case MORE_MEMOS:
      snprintf(row->title, sizeof(row->title), "Memos");
      if (memo_count() == 0) snprintf(row->sub, sizeof(row->sub), "None");
      else snprintf(row->sub, sizeof(row->sub), "%d saved, %d to send", memo_count(), memo_queued_count());
      break;
    case MORE_NOTE:
      snprintf(row->title, sizeof(row->title), "Write note");
      snprintf(row->sub, sizeof(row->sub), note_pending() ? "One note waiting to sync" : "Draw it by hand");
      break;
    case MORE_SETTINGS:
      snprintf(row->title, sizeof(row->title), "Settings");
      break;
  }
}

static void more_select(int index) {
  switch (index) {
    case MORE_NEW:
      // The phone starts the new context now, so the settings page agrees with the watch.
      g_prefs.next_new_conv = true;
      comm_send(W_NEWCONV, 0, 0, 0, 0, NULL, NULL, 0);
      home_refresh();
      window_stack_pop(true);
      break;
    case MORE_RECORD: memo_record_push(); break;
    case MORE_MEMOS: memos_push(); break;
    case MORE_NOTE: note_push(); break;
    case MORE_SETTINGS: settings_push(); break;
  }
}

static void more_unload(void) { s_more = NULL; }

static const ListSpec s_more_spec = {
  .heading = "MORE", .count = more_count, .row = more_row, .select = more_select,
  .unload = more_unload,
};

void more_push(void) { s_more = list_push(&s_more_spec); }
