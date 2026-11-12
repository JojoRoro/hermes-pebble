// Hermes 2 for Pebble Time 2: shared declarations.
#pragma once
#include <pebble.h>

// ---- protocol (mirrored in src/pkjs/proto.js) ---------------------------------------------

#define PROTO_VERSION 2

// watch -> phone
enum {
  W_HELLO = 1,       // a=protocol version
  W_LISTEN = 2,      // a=1 the microphone stream is about to start, 0 cancelled: drop the audio
  W_ASK = 4,         // rid, a=flags (ASK_*), txt=question
  W_OPEN = 5,        // rid, a=byte offset of the answer page wanted (-1: status only)
  W_STOP = 6,        // rid
  W_RECENT = 7,
  W_NEWCONV = 8,
  W_MEMO_BEGIN = 9,  // rid=memo id, a=bytes, b=seconds, c=crc32, txt=recorded-at epoch
  W_MEMO_DATA = 10,  // rid, a=offset, bin
  W_MEMO_END = 11,   // rid
  W_NOTE = 12,       // rid=note id, a=offset, b=total, c=crc32, bin
  W_MODEL = 13,      // a=MODEL_*
  W_PING = 14,       // a=nonce
};

// phone -> watch
enum {
  P_READY = 64,
  P_STATE = 65,      // a=flags (PF_*), b=download percent, c=idle minutes, txt=model, txt2=size
  P_PARTIAL = 66,    // txt=transcript so far
  P_FINAL = 67,      // a=FINAL_*, txt=transcript
  P_VAD = 68,        // the speaker stopped talking: end the stream
  P_STORED = 69,     // rid: the phone holds the question
  P_ITEM = 70,       // rid, a=state, b=answer bytes, c=age seconds, txt=question, txt2=detail
  P_PAGE = 71,       // rid, a=offset, b=total, txt=text
  P_RECENT = 72,     // a=index, b=count, rid, c=(age seconds << 4)|state, txt=question
  P_MEMO_ACK = 73,   // rid, a=MEMO_ACK_*, b=offset the phone has
  P_NOTE_ACK = 74,   // rid
  P_PONG = 75,       // a=nonce
  P_TOAST = 76,      // txt
  P_DONE = 77,       // rid: an answer just arrived
};

#define ASK_NEW_CONV 1

enum { MODEL_STATUS = 0, MODEL_DOWNLOAD = 1, MODEL_CANCEL = 2, MODEL_USE_PEBBLE = 3, MODEL_USE_PHONE = 4 };
enum { FINAL_OK = 0, FINAL_EMPTY = 1, FINAL_UNAVAILABLE = 2 };
enum { MEMO_ACK_CONTINUE = 0, MEMO_ACK_STORED = 1, MEMO_ACK_RESTART = 2 };

#define PF_CONFIGURED   (1 << 0)
#define PF_STT_SUPPORT  (1 << 1)
#define PF_STT_PHONE    (1 << 2)
#define PF_MODEL_READY  (1 << 3)
#define PF_DOWNLOADING  (1 << 4)
#define PF_CONV_ACTIVE  (1 << 6)
#define PF_MODEL_ERROR  (1 << 7)

// Request states. 0 exists only on the watch.
typedef enum {
  ST_WAITING_PHONE = 0, ST_QUEUED, ST_SUBMITTING, ST_ACCEPTED, ST_WORKING, ST_APPROVAL,
  ST_ANSWERED, ST_FAILED, ST_STOPPED, ST_CANCELLED, ST_UNKNOWN, ST_STOPPING,
} ReqState;

static inline bool state_is_final(int s) {
  return s == ST_ANSWERED || s == ST_FAILED || s == ST_STOPPED || s == ST_CANCELLED;
}
const char *state_label(int state);
GColor state_color(int state);

typedef struct {
  uint8_t op;
  int32_t rid, a, b, c;
  const char *txt, *txt2;
  const uint8_t *bin;
  uint16_t bin_len;
} Msg;

#define TEXT_MAX 1000          // bytes of a question
#define PAGE_MAX 900           // bytes of one answer page
#define MEMO_CHUNK 1400        // bytes of memo audio per message

// ---- comm.c --------------------------------------------------------------------------------

void comm_init(void);
void comm_deinit(void);
// Queues a message. txt and bin are copied. Returns false when the queue is full.
bool comm_send(uint8_t op, int32_t rid, int32_t a, int32_t b, int32_t c,
               const char *txt, const uint8_t *bin, uint16_t bin_len);
bool comm_connected(void);     // Bluetooth link to the Pebble app
bool comm_idle(void);          // nothing queued or in flight
void comm_hello(void);

// ---- app state (main.c) ------------------------------------------------------------------

typedef struct {
  bool seen;                   // the phone side answered since launch
  uint8_t flags;
  uint8_t percent;
  int32_t idle_min;
  char model[24];
  char model_size[12];
} PhoneState;

extern PhoneState g_phone;

typedef struct {
  bool touch;                  // touch navigation
  bool next_new_conv;          // the next question starts a new conversation
} Prefs;

extern Prefs g_prefs;
void prefs_save(void);

// The last question asked from this watch, for the home screen.
void last_question_set(int32_t rid, const char *text);
const char *last_question(void);
time_t last_question_time(void);
// Where the last question stands, as the phone last said; -1 when not heard since launch.
int last_question_state(void);

void app_dispatch(const Msg *m);
void app_toast(const char *text);
int32_t app_new_id(void);

// ---- draft.c: the unsent question --------------------------------------------------------

bool draft_exists(void);
int32_t draft_rid(void);
uint8_t draft_flags(void);
// Copies the saved text into out (size TEXT_MAX + 1).
bool draft_load(char *out);
void draft_save(int32_t rid, uint8_t flags, const char *text);
void draft_clear(void);

// ---- screens ---------------------------------------------------------------------------------

void home_push(void);
void home_refresh(void);
bool home_on_top(void);

// Starts a question: the on-phone model, the model prompt, or Pebble dictation.
void ask_start(void);
void listen_push(void);
void listen_on_msg(const Msg *m);
void dictation_start(void);
void listen_deinit(void);
void review_push(const char *text);
void review_push_draft(int32_t rid, const char *text);

void thread_push(int32_t rid, const char *question, int state);
void thread_on_msg(const Msg *m);

void recent_push(void);
void recent_on_msg(const Msg *m);

void more_push(void);
void settings_push(void);
void settings_on_msg(const Msg *m);
void model_push(void);
void model_on_state(void);

void memos_push(void);
void memo_record_push(void);
void memos_refresh(void);
void note_push(void);

// ---- memo.c: voice memos in flash -------------------------------------------------------

#define MEMO_MAX 6
#define MEMO_MAX_SECONDS 120
#define MEMO_BYTES_PER_SECOND 4000

typedef enum { MEMO_DRAFT = 0, MEMO_QUEUED = 1, MEMO_ON_PHONE = 2 } MemoStatus;

typedef struct {
  int32_t id;
  uint32_t bytes;
  uint32_t crc;
  time_t recorded;
  uint16_t seconds;
  uint8_t status;
  uint8_t slot;                // which block range in flash
} Memo;

void memo_init(void);
void memo_deinit(void);
int memo_count(void);
const Memo *memo_at(int index);
int memo_queued_count(void);
// Percent sent of the memo being uploaded, or -1 when it is not the one in flight.
int memo_progress(int32_t id);
bool memo_record_start(void);
// Ends the recording. Returns the new memo's id, or 0 when nothing worth keeping was recorded.
int32_t memo_record_stop(void);
bool memo_recording(void);
uint16_t memo_record_seconds(void);
uint16_t memo_record_level(void);        // 0..100
uint32_t memo_free_seconds(void);
typedef enum {
  MEMO_PLAY_OK,
  MEMO_PLAY_UNAVAILABLE,       // gone, already only on the phone, or recording
  MEMO_PLAY_QUIET_TIME,
  MEMO_PLAY_MUTED,
  MEMO_PLAY_BUSY,              // the speaker refused to open
} MemoPlayResult;
MemoPlayResult memo_play(int32_t id);
void memo_play_stop(void);
bool memo_playing(void);
void memo_send(int32_t id);
void memo_delete(int32_t id);
void memo_on_msg(const Msg *m);
void memo_pump(void);                    // send the next piece when the link is free

// ---- note.c ---------------------------------------------------------------------------------

void note_on_msg(const Msg *m);
void note_sync(void);
bool note_pending(void);
