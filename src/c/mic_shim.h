#ifndef HERMES_MIC_SHIM_H
#define HERMES_MIC_SHIM_H
/* Local microphone capture (firmware 4.43+, SDK revision 111) without requiring that SDK.
 *
 * The public SDK is still 4.33.1, and building with a revision-111 SDK would also stop
 * the whole app from installing on older firmware. With an SDK that already exports
 * mic_capture_* this file just forwards to it. Otherwise it calls the firmware's
 * exported-function table directly, by slot, after checking the firmware version.
 *
 * Slots come from PebbleOS tools/generate_native_sdk/exported_symbols.json: functions
 * are ordered by (addedRevision, sortName) and only ever appended. Revision 110 added
 * mic_stream_to_phone_* at 677-679; revision 111 added mic_capture_is_active,
 * mic_capture_start and mic_capture_stop at 680-682. Firmware 4.42 (revision 109)
 * ends at slot 676, so the version check must come first. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef MIC_CAPTURE_SAMPLE_RATE

static bool memo_mic_available(void) { return true; } // Stubs report unavailability themselves.
static MicCaptureStartResult memo_mic_start(MicCaptureHandlers handlers, void *context) {
  return mic_capture_start(handlers, context);
}
static void memo_mic_stop(void) { mic_capture_stop(); }

#else
#include <pebble_process_info.h>

#define MIC_CAPTURE_SAMPLE_RATE 16000
typedef enum MicCaptureStartResult {
  MicCaptureStartOk = 0,
  MicCaptureStartErrBusy,
  MicCaptureStartErrNotForeground,
  MicCaptureStartErrInvalidArgs,
  MicCaptureStartErrWatchface,
  MicCaptureStartErrUnavailable,
  MicCaptureStartErrPermissionDenied,
  MicCaptureStartErrNoMemory,
} MicCaptureStartResult;
typedef enum MicCaptureStopReason {
  MicCaptureStopReasonFocusLost = 0,
  MicCaptureStopReasonInterrupted,
  MicCaptureStopReasonPermissionRevoked,
} MicCaptureStopReason;
typedef void (*MicCaptureDataHandler)(const int16_t *samples, size_t sample_count, void *context);
typedef void (*MicCaptureStoppedHandler)(MicCaptureStopReason reason, void *context);
typedef struct MicCaptureHandlers {
  MicCaptureDataHandler data;
  MicCaptureStoppedHandler stopped;
} MicCaptureHandlers;

#define MEMO_MIC_FIRMWARE_MAJOR 4
#define MEMO_MIC_FIRMWARE_MINOR 43
#define MEMO_MIC_SLOT_START 681u
#define MEMO_MIC_SLOT_STOP 682u

/* The firmware is built with short enums: enum results and arguments are only a byte wide,
 * so they cross this boundary as words and are masked. */
typedef struct {
  MicCaptureDataHandler data;
  void (*stopped)(uint32_t reason, void *context);
} MemoFirmwareMicHandlers;
typedef uint32_t (*MemoMicStartFn)(MemoFirmwareMicHandlers handlers, void *context);
typedef void (*MemoMicStopFn)(void);

/* The app header the SDK places first in the binary. Its sym_table_addr is built as a
 * placeholder and patched afterwards to the offset of the word into which the loader
 * writes the table's address; both are unknown to the compiler, hence volatile reads. */
extern const PebbleProcessInfo __pbl_app_info;
static MicCaptureStoppedHandler s_memo_mic_stopped;

static void *const *memo_mic_table(void) {
  const volatile uint8_t *base = (const volatile uint8_t *)&__pbl_app_info;
  uint32_t offset = 0u;
  void *const *table = NULL;
  for (size_t i = 0; i < sizeof(offset); i++) ((uint8_t *)&offset)[i] = base[offsetof(PebbleProcessInfo, sym_table_addr) + i];
  for (size_t i = 0; i < sizeof(table); i++) ((uint8_t *)&table)[i] = base[offset + i];
  return table;
}

static void *memo_mic_slot(uint32_t slot) {
  void *const *table = memo_mic_table();
  return table ? table[slot] : NULL;
}

static bool memo_mic_available(void) {
  static int8_t s_available = -1;
  if (s_available < 0) {
    WatchInfoVersion version = watch_info_get_firmware_version();
    bool firmware = version.major > MEMO_MIC_FIRMWARE_MAJOR ||
        (version.major == MEMO_MIC_FIRMWARE_MAJOR && version.minor >= MEMO_MIC_FIRMWARE_MINOR);
    s_available = firmware && memo_mic_slot(MEMO_MIC_SLOT_START) != NULL && memo_mic_slot(MEMO_MIC_SLOT_STOP) != NULL;
  }
  return s_available > 0;
}

static void memo_mic_stopped_word(uint32_t reason, void *context) {
  MicCaptureStoppedHandler stopped = s_memo_mic_stopped;
  if (stopped) stopped((MicCaptureStopReason)(reason & 0xffu), context);
}

static MicCaptureStartResult memo_mic_start(MicCaptureHandlers handlers, void *context) {
  if (!memo_mic_available()) return MicCaptureStartErrUnavailable;
  if (handlers.data == NULL || handlers.stopped == NULL) return MicCaptureStartErrInvalidArgs;
  MemoMicStartFn start = (MemoMicStartFn)memo_mic_slot(MEMO_MIC_SLOT_START);
  s_memo_mic_stopped = handlers.stopped;
  uint32_t result = start((MemoFirmwareMicHandlers){ .data = handlers.data, .stopped = memo_mic_stopped_word },
      context) & 0xffu;
  if (result != MicCaptureStartOk) s_memo_mic_stopped = NULL;
  return (MicCaptureStartResult)result;
}

/* Stops without a stopped callback, like mic_capture_stop; safe while idle. */
static void memo_mic_stop(void) {
  s_memo_mic_stopped = NULL;
  if (!memo_mic_available()) return;
  ((MemoMicStopFn)memo_mic_slot(MEMO_MIC_SLOT_STOP))();
}
#endif
#endif
