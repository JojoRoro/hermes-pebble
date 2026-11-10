// The link to the phone: one AppMessage in flight at a time, the rest queued.
#include "app.h"

#define QUEUE_MAX 10
#define INBOX_SIZE 1280
#define OUTBOX_SIZE 1560

typedef struct {
  uint8_t op;
  int32_t rid, a, b, c;
  char *txt;
  uint8_t *bin;
  uint16_t bin_len;
} Pending;

static Pending s_queue[QUEUE_MAX];
static int s_head, s_count;
static bool s_busy;
static int s_failures;
static AppTimer *s_retry;
static AppTimer *s_hello;

static void pump(void);

static void pending_free(Pending *p) {
  free(p->txt);
  free(p->bin);
  memset(p, 0, sizeof(*p));
}

static void drop_head(void) {
  pending_free(&s_queue[s_head]);
  s_head = (s_head + 1) % QUEUE_MAX;
  s_count--;
}

static void retry_cb(void *data) {
  s_retry = NULL;
  pump();
}

static void pump(void) {
  if (s_busy || s_retry) return;
  if (s_count == 0) {
    memo_pump();
    if (s_count == 0) return;
  }
  Pending *p = &s_queue[s_head];
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) {
    s_retry = app_timer_register(300, retry_cb, NULL);
    return;
  }
  dict_write_uint8(it, MESSAGE_KEY_op, p->op);
  dict_write_int32(it, MESSAGE_KEY_rid, p->rid);
  dict_write_int32(it, MESSAGE_KEY_a, p->a);
  dict_write_int32(it, MESSAGE_KEY_b, p->b);
  dict_write_int32(it, MESSAGE_KEY_c, p->c);
  if (p->txt) dict_write_cstring(it, MESSAGE_KEY_txt, p->txt);
  if (p->bin) dict_write_data(it, MESSAGE_KEY_bin, p->bin, p->bin_len);
  if (app_message_outbox_send() == APP_MSG_OK) {
    s_busy = true;
  } else {
    s_retry = app_timer_register(500, retry_cb, NULL);
  }
}

static void sent(DictionaryIterator *it, void *context) {
  s_busy = false;
  s_failures = 0;
  if (s_count > 0) drop_head();
  pump();
}

static void failed(DictionaryIterator *it, AppMessageResult reason, void *context) {
  s_busy = false;
  if (s_count == 0) return;
  // Memo audio is resent from the phone's last offset, and status asks are cheap to repeat, so
  // only what the user cannot redo is retried for long.
  const uint8_t op = s_queue[s_head].op;
  const int limit = op == W_ASK ? 20 : 4;
  if (++s_failures > limit) {
    s_failures = 0;
    drop_head();
  }
  if (!s_retry) s_retry = app_timer_register(s_failures > 3 ? 2000 : 600, retry_cb, NULL);
}

static void received(DictionaryIterator *it, void *context) {
  Msg m = { 0 };
  for (Tuple *t = dict_read_first(it); t; t = dict_read_next(it)) {
    if (t->key == MESSAGE_KEY_op) m.op = (uint8_t)t->value->int32;
    else if (t->key == MESSAGE_KEY_rid) m.rid = t->value->int32;
    else if (t->key == MESSAGE_KEY_a) m.a = t->value->int32;
    else if (t->key == MESSAGE_KEY_b) m.b = t->value->int32;
    else if (t->key == MESSAGE_KEY_c) m.c = t->value->int32;
    else if (t->key == MESSAGE_KEY_txt && t->type == TUPLE_CSTRING) m.txt = t->value->cstring;
    else if (t->key == MESSAGE_KEY_txt2 && t->type == TUPLE_CSTRING) m.txt2 = t->value->cstring;
    else if (t->key == MESSAGE_KEY_bin && t->type == TUPLE_BYTE_ARRAY) {
      m.bin = t->value->data;
      m.bin_len = t->length;
    }
  }
  if (!m.txt) m.txt = "";
  if (!m.txt2) m.txt2 = "";
  if (m.op) app_dispatch(&m);
}

bool comm_send(uint8_t op, int32_t rid, int32_t a, int32_t b, int32_t c,
               const char *txt, const uint8_t *bin, uint16_t bin_len) {
  if (s_count >= QUEUE_MAX) return false;
  Pending *p = &s_queue[(s_head + s_count) % QUEUE_MAX];
  memset(p, 0, sizeof(*p));
  p->op = op;
  p->rid = rid;
  p->a = a;
  p->b = b;
  p->c = c;
  if (txt) {
    const size_t n = strlen(txt);
    p->txt = malloc(n + 1);
    if (!p->txt) return false;
    memcpy(p->txt, txt, n + 1);
  }
  if (bin && bin_len) {
    p->bin = malloc(bin_len);
    if (!p->bin) {
      free(p->txt);
      p->txt = NULL;
      return false;
    }
    memcpy(p->bin, bin, bin_len);
    p->bin_len = bin_len;
  }
  s_count++;
  pump();
  return true;
}

bool comm_connected(void) { return connection_service_peek_pebble_app_connection(); }
bool comm_idle(void) { return s_count == 0 && !s_busy; }

static void hello_cb(void *data) {
  s_hello = NULL;
  // Keep knocking until the phone side has answered: it may start after the watch app.
  if (!g_phone.seen) comm_hello();
}

void comm_hello(void) {
  if (comm_connected()) comm_send(W_HELLO, 0, PROTO_VERSION, 0, 0, NULL, NULL, 0);
  if (!s_hello) s_hello = app_timer_register(4000, hello_cb, NULL);
}

static void connection(bool connected) {
  if (connected) {
    comm_hello();
  } else {
    g_phone.seen = false;
  }
  home_refresh();
}

void comm_init(void) {
  app_message_register_inbox_received(received);
  app_message_register_outbox_sent(sent);
  app_message_register_outbox_failed(failed);
  app_message_open(INBOX_SIZE, OUTBOX_SIZE);
  connection_service_subscribe((ConnectionHandlers){ .pebble_app_connection_handler = connection });
  comm_hello();
}

void comm_deinit(void) {
  connection_service_unsubscribe();
  app_message_deregister_callbacks();
  while (s_count > 0) drop_head();
}
