# Hermes Pebble protocol v1

This document is the source of truth for the watch/phone wire format. The matching constants are committed in `src/c/protocol.h` and `android/app/src/main/java/dev/hermespebble/companion/pebble/Protocol.kt`.

## Constants and limits

| Constant | Value |
| --- | ---: |
| Protocol version | `1` |
| Pebble AppMessage input buffer | 1,024 bytes |
| Pebble AppMessage output buffer | 1,024 bytes |
| Maximum chunks in one transfer | 6 |
| Maximum payload bytes per chunk | 192; audio blocks allow 768 since v0.1.16 |
| Maximum logical transfer bytes | 1,024; a full transfer uses six chunks and caps its final chunk at 64 bytes |
| Maximum dictation/note bytes | 1,024 UTF-8 bytes, excluding the terminating NUL |
| Maximum recent items returned to watch | 3 |
| Maximum encoded watch result page | 768 UTF-8 bytes |
| Per-chunk transfer timeout | 15 seconds |
| Application durable-receipt timeout | 30 seconds |
| Bounded transport retry delays | 1, 2, 4, 8 seconds |

A maximum-size ordinary message uses six chunks. Each dictionary contains one chunk, numeric metadata, a one-byte tuple count, and seven header bytes per tuple. Since v0.1.16, audio blocks allow 768 payload bytes: even with all 17 numeric values widened to uint32, the full 18-tuple dictionary is 963 bytes, below the 1,024-byte inbox. Other kinds retain the 192-byte payload limit.

Text chunking operates on UTF-8 bytes. Handwriting and audio use separate binary blocks described below. A text sender chooses boundaries that do not split a code point. Receivers concatenate bytes, validate bounds and the complete UTF-8 sequence, then decode. A character count is never used as a transfer limit.

## Dictionary keys

Every numeric key has one fixed type. Android receives every numeric tuple normalized as `UInt32` or `Int32`; it validates the integer value instead of expecting the original C width.

| Key | Name | Type | Meaning |
| ---: | --- | --- | --- |
| 0 | `ProtocolVersion` | `uint8_t` / `UInt8` | Must be `1` |
| 1 | `MessageKind` | `uint8_t` / `UInt8` | Operation below |
| 2 | `TransferId` | `uint32_t` / `UInt32` | Identifies one logical message and all of its chunks |
| 3 | `CaptureId` | `uint32_t` / `UInt32` | Durable watch capture; absent means zero |
| 4 | `ChunkIndex` | `uint8_t` / `UInt8` | Zero-based chunk index |
| 5 | `ChunkCount` | `uint8_t` / `UInt8` | Always 1–6, including empty payloads |
| 6 | `Payload` | `byte[]` / `Bytes` | UTF-8 bytes, binary ink for kind 9, or binary PCM for kind 111; never NUL-terminated |
| 7 | `Status` | `uint8_t` / `UInt8` | Status below |
| 8 | `ErrorCode` | `uint8_t` / `UInt8` | Error below; zero means none |
| 9 | `ItemId` | `uint32_t` / `UInt32` | Android command row ID, when safe and relevant |
| 10 | `ItemState` | `uint8_t` / `UInt8` | Alias reserved for compact item rendering; v1 uses `Status` |
| 11 | `ItemKind` | `uint8_t` / `UInt8` | 0 none, 1 Hermes request, 2 local note |
| 12 | `PageOffset` | `uint32_t` / `UInt32` | Recent-item offset or result-byte offset, as specified by the operation |
| 13 | `PageCount` | `uint8_t` / `UInt8` | Requested/returned recent-item count or result-page count |
| 14 | `TotalBytes` | `uint32_t` / `UInt32` | Full logical result length for result pages |
| 15 | `ConversationGeneration` | `uint32_t` / `UInt32` | Monotonic local generation incremented by New conversation |
| 16 | `Flags` | `uint8_t` / `UInt8` | Bit flags below |
| 17 | `CorrelationId` | `uint32_t` / `UInt32` | Transfer ID being answered; zero when not applicable |

`ItemState` is not interpreted in v1. It is present so a later minor revision can render compact list state without renumbering keys.

## Operations

| Value | Direction | Name | Required fields |
| ---: | --- | --- | --- |
| 1 | Watch → phone | `HANDSHAKE` | Transfer, chunk metadata |
| 2 | Watch → phone | `SUBMIT_REQUEST` | Transfer, capture, chunk metadata, concatenated transcript |
| 3 | Watch → phone | `SAVE_NOTE` | Transfer, capture, chunk metadata, concatenated transcript |
| 4 | Watch → phone | `DISCARD_CAPTURE` | Transfer, capture, conversation generation |
| 5 | Watch → phone | `FETCH_RECENT` | Transfer, page offset, page count |
| 6 | Watch → phone | `FETCH_RESULT` | Transfer, capture, result byte offset |
| 7 | Watch → phone | `START_CONVERSATION` | Transfer, current conversation generation |
| 8 | Watch → phone | `STOP_REQUEST` | Transfer, original capture |
| 9 | Watch → phone | `INK_BLOCK` | Capture, one binary block, byte offset, total ink bytes, whole-file CRC in generation |
| 10 | Watch → phone | `AUDIO_STATUS` | Correlation, audio session in capture, audio status, confirmed byte offset, total audio bytes, checksum in generation |
| 101 | Phone → watch | `HANDSHAKE_ACK` | Transfer, correlation, protocol version, conversation generation |
| 102 | Phone → watch | `DURABLE_RECEIPT` | Transfer, correlation, capture, item kind, item ID when assigned, status |
| 103 | Phone → watch | `STATUS_UPDATE` | Transfer, correlation zero, capture, item ID, status, error code, flags |
| 104 | Phone → watch | `RECENT_PAGE` | Transfer, correlation, page offset, page count, JSON payload |
| 105 | Phone → watch | `RESULT_PAGE` | Transfer, correlation, capture, page offset, total bytes, flags, JSON payload |
| 106 | Phone → watch | `NEW_CONVERSATION_ACK` | Transfer, correlation, previous and new generation |
| 107 | Phone → watch | `STRUCTURED_ERROR` | Transfer, correlation, error code, bounded UTF-8 diagnostic |
| 108 | Phone → watch | `CAPTURE_DISCARDED` | Transfer, correlation, capture |
| 109 | Phone → watch | `INK_RECEIPT` | Correlation, capture, end offset, total ink bytes, CRC, durable flag; status 13 when complete |
| 110 | Phone → watch | `AUDIO_BEGIN` | Audio session in capture, total audio bytes, checksum in generation, format in flags |
| 111 | Phone → watch | `AUDIO_BLOCK` | Session, offset, binary PCM block, total, checksum, format |
| 112 | Phone → watch | `AUDIO_PLAY` | Session, total, checksum, format; empty payload |
| 113 | Phone → watch | `AUDIO_CANCEL` | Session to discard/stop; empty payload |

A phone reply is stale and ignored when its `CorrelationId` does not match the current screen's outstanding transfer. A status for an older capture never replaces a newer visible screen.

For phone-initiated diagnostics, a successful `HANDSHAKE_ACK` with zero correlation and a nonzero transfer ID is a probe invitation, not proof of connectivity. The watch queues a `HANDSHAKE` correlated to that probe ID, preserving any unrelated transfer. The phone replies with an ACK correlated to the watch handshake ID. The diagnostic succeeds only after receiving that request and successfully delivering its reply. This extension is supported by watch version 0.1.2 onward.

## Statuses and errors

| Status | Display meaning |
| ---: | --- |
| 0 | None/not applicable |
| 1 | Waiting for phone |
| 2 | Saved and queued on phone |
| 3 | Submitting to Hermes |
| 4 | Accepted by Hermes |
| 5 | Hermes working |
| 6 | Approval needed in Hermes |
| 7 | Completed |
| 8 | Failed |
| 9 | Stop requested |
| 10 | Cancelled |
| 11 | Interrupted |
| 12 | Outcome unknown — review before retrying |
| 13 | Local note saved |
| 14 | Waiting for the previous connection profile |
| 15 | Discarded |

| Error | Meaning |
| ---: | --- |
| 0 | None |
| 1 | Incompatible protocol version |
| 2 | Unsupported message kind |
| 3 | Malformed message |
| 4 | Transfer exceeds a bound |
| 5 | Conflicting duplicate chunk |
| 6 | Android could not durably store the command |
| 7 | Connection settings are incomplete or invalid |
| 8 | Authentication rejected; Hermes versus access proxy is indeterminate |
| 9 | Network or TLS failure |
| 10 | Redirect rejected |
| 11 | Required Hermes route is unsupported |
| 12 | Invalid or non-JSON Hermes response |
| 13 | Submission outcome unknown |
| 14 | Requested command/result was not found |
| 15 | Durable state or idempotency conflict |
| 16 | Hermes server error |
| 17 | Local note could not be saved |
| 18 | Watch/Pebble host unavailable |

Flags are `0x01` more content is available on the phone, `0x02` stop was requested, `0x04` Hermes replayed the idempotent create, and `0x08` Android committed the command before sending the acknowledgment.

## Correlation and acknowledgment rules

`TransferId` is generated independently by each sender and is not the durable user-command identity. `CaptureId` is generated by the watch from a persisted monotonic counter and remains unchanged across retries, reopen, and app restarts. Android scopes it with the opaque `WatchIdentifier`, so the pair `(WatchIdentifier, CaptureId)` is the duplicate-prevention key.

Every transfer starts at `ChunkIndex=0`. Each chunk contains `ProtocolVersion`, `MessageKind`, `TransferId`, its zero-based `ChunkIndex`, and the same `ChunkCount`. Metadata remains constant; payload lengths may differ to avoid splitting UTF-8 code points. Duplicate chunks are idempotent only when their bytes match; a conflicting duplicate produces `STRUCTURED_ERROR` with error 5.

Pebble's inbox ACK/NACK proves only AppMessage transport handling. `DURABLE_RECEIPT` is sent only after the command/note and duplicate-receipt record commit in Room. The watch retains its pending transcript until that application acknowledgment arrives or the user explicitly discards it.

Only one outbound transfer is allowed per direction. A sender keeps it pending across the bounded retries above. It cannot start a second transfer until success, terminal failure, timeout, or explicit discard. On timeout it may restart the same `TransferId`, capture, payload, and chunks. A restart cannot create a second Android command.

## Payload formats

Watch request and note payloads are raw confirmed transcript bytes. There is no terminal NUL on the wire.

Recent pages use UTF-8 JSON:

```json
{"items":[{"captureId":42,"kind":1,"state":7,"itemId":9,"preview":"Two reminders remain"}]}
```

Result pages use UTF-8 JSON:

```json
{"captureId":42,"itemId":9,"state":7,"output":"Two reminders remain.","more":false}
```

`output` is the actual Hermes answer. A completed HTTP run is not converted into a Matrix or other external-delivery receipt. If `more` is true, the watch automatically requests the next result chunk (at most 768 bytes including JSON encoding) with `FETCH_RESULT` and appends the decoded text without changing the reading position. Its 8-KiB text window also respects the drawing-height limit. Up/Down load adjacent windows at the boundaries; earlier window byte offsets allow backward reading without retaining the whole answer in watch RAM. The full answer remains stored on the phone.

Structured errors have a short human-readable payload but remain categorized by `ErrorCode`; UI logic must not parse prose to determine state.

## Example exchanges

Initial handshake:

```text
Watch  -> Phone: 1, transfer=100, chunk=0/1
Phone  -> Watch: 101, transfer=500, correlation=100, protocol=1, generation=3
```

A 1,024-byte request sent as six chunks:

```text
Watch  -> Phone: 2, transfer=101, capture=42, chunk=0/6, bytes[0:192]
Watch  -> Phone: 2, transfer=101, capture=42, chunk=1/6, bytes[192:384]
Watch  -> Phone: 2, transfer=101, capture=42, chunk=2/6, bytes[384:576]
Watch  -> Phone: 2, transfer=101, capture=42, chunk=3/6, bytes[576:768]
Watch  -> Phone: 2, transfer=101, capture=42, chunk=4/6, bytes[768:960]
Watch  -> Phone: 2, transfer=101, capture=42, chunk=5/6, bytes[960:1024]
Phone  -> Watch: 102, transfer=501, correlation=101, capture=42, status=2, flags=0x08
Phone  -> Watch: 103, capture=42, itemId=9, status=4
Phone  -> Watch: 103, capture=42, itemId=9, status=7
Watch  -> Phone: 6, transfer=102, capture=42, resultOffset=0
Phone  -> Watch: 105, transfer=502, correlation=102, capture=42, resultOffset=0, totalBytes=…
```

Stopping is explicit and never undo work already performed:

```text
Watch  -> Phone: 8, transfer=103, capture=42
Phone  -> Watch: 103, capture=42, status=9, flags=0x02
```

If Android and the watch report different major protocol versions, neither side interprets the payload as a command. The phone returns error 1 and both UIs direct the user to update the apps.

Conversation generations start at zero. The handshake returns the current phone generation; a submitted capture retains its saved generation. Repeated new-conversation requests are compared atomically against the phone generation. Discard of an unseen capture creates a durable cancellation record so delayed submissions cannot execute it. Result pagination offsets count decoded UTF-8 output bytes; JSON escaping is included in the page-size limit. Embedded NUL in result display is rendered as a space (one byte); the full original result remains on the phone.


## Handwritten notes

Added in 0.1.6; install matching APK/PBW versions. Handwriting has a separate local Room table and never creates a Hermes request or text note.

### HIN1 stroke file

All multi-byte integers are little-endian. Maximum file size is 2,048 bytes, including the 16-byte header. The canvas is 160 × 144 pixels.

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 4 | ASCII `HIN1` |
| 4 | 1 | Width, 160 |
| 5 | 1 | Height, 144 |
| 6 | 2 | Stroke stream length |
| 8 | 4 | Watch capture time, Unix seconds |
| 12 | 4 | CRC-32 (IEEE) of the stroke stream |
| 16 | variable | Two-byte coordinate or control pairs |

A coordinate is `x, y`, with x < 160 and y < 144. `255,0` ends a stroke; `255,1` advances to the next character; `255,2` ends the current character if present and inserts a word space. Strokes must end before character/space markers and before EOF. Empty drawings are rejected. The watch simplifies nearly collinear samples and reserves space for the final pen-up marker. The phone lays the character cells out across rows without OCR.

### Durability and 4 KiB budget

The watch uses one immutable pending ink record, independent of its existing pending text capture. Ink chunks occupy keys 17–25, with up to 240 bytes per value. Key 16 is a 20-byte `HIS1` commit record: magic, capture ID, total length, whole-file CRC, metadata CRC. Chunks are written first and the commit record last. Loading requires every chunk, both CRCs, and valid stroke structure. Damaged committed data is retained until the user explicitly discards it. Unsaved drawings remain in RAM only.

Maximum value bytes are 2,048 ink + 20 ink metadata + 1,024 existing transcript + 48 text metadata + 28 identity/preferences = 3,168. Allowing a conservative 32 bytes per used key across 26 keys gives 4,000 bytes, below 4,096. Tests enforce this budget with a maximum transcript and ink note present together. Data Logging consumes a separate shared OS spool and is an additional delivery route, not the sole retained copy.

### Direct transfer

Each kind-9 AppMessage is an independent one-chunk transfer with up to 192 binary bytes. `CaptureId` is the existing durable watch counter; `PageOffset` is the block offset, always a multiple of 192; `TotalBytes` is the full ink file length; `ConversationGeneration` holds its CRC-32 **only for kinds 9/109**. Other operations retain their conversation semantics and text limits.

Android commits each block transactionally before sending kind 109 with durable flag `0x08`. The receipt echoes the capture, total, CRC, and request correlation, and advances `PageOffset` to the end of the committed block. Partial receipts use status 1. A complete receipt uses status 13 only after every block is present, the complete checksum matches, and the drawing decodes. The watch checks all fields and deletes its saved copy only after the final complete receipt. A lost receipt or app restart safely resends the same capture. Android deduplicates by watch identity/capture and block bitmap, rejecting conflicting bytes or metadata.

### Background Data Logging route

Tag `0x48494e31`, byte-array items of 212 bytes. Every item contains:

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 4 | ASCII `IHC1` |
| 4 | 4 | Capture ID |
| 8 | 2 | Total HIN1 bytes |
| 10 | 2 | Block offset |
| 12 | 2 | Payload count, up to 192 |
| 14 | 2 | Reserved zero |
| 16 | 4 | Whole-file CRC-32 |
| 20 | 192 | Block bytes, zero-padded |

The companion validates each envelope, stores its blocks in the same Room transaction path, and ACKs a batch only after storage. Duplicate or out-of-order blocks are safe across both routes. Incomplete phone assemblies older than 30 days are cleaned up; completed notes are retained.

Data Logging delivery is host-dependent and its transport acknowledgment is not proof of companion storage. The independent watch copy remains until direct confirmation, even if background delivery already created the phone note. Reopening the watch app retries and clears that slot when confirmed. A sync notification is emitted after complete storage if notification permission is granted; opening it shows the local handwriting. Nothing is sent to Hermes.

## Speaker test

Added in 0.1.9; install matching APK/PBW versions. Kinds 10 and 110–113 use a separate transient audio session; they do not change conversation state. The diagnostic format stays in RAM; full replies use streaming format 3 described below. `CaptureId` is the phone-generated nonzero session ID, `TotalBytes` is 1–16,000, `ConversationGeneration` is the unsigned FNV-1a checksum of the complete PCM clip, and `Flags=1` selects mono signed 8-bit PCM at 8 kHz with no header. The checksum starts at `0x811c9dc5`, XORs each unsigned byte, then multiplies by `0x01000193` modulo 2³².

Android first performs a correlated probe of an already-open watch app without launching it. `AUDIO_BEGIN` reserves a temporary watch heap buffer. Android waits for `AUDIO_STATUS`, then sends successive `AUDIO_BLOCK` messages. Since v0.1.17, each block is at most 768 bytes and fits one dictionary; v0.1.16 sent 1,024-byte blocks in two dictionaries (previously six). Install the matching watch app before using the larger chunks. The watch packs received chunks into a single bounded 1,024-byte buffer and preserves chunk order and duplicate validation. PCM uses byte boundaries and bypasses UTF-8 validation. `PageOffset` is the block's start byte; a receipt reports the cumulative received byte count. Matching repeated blocks are accepted; holes, conflicting bytes, and inconsistent metadata are rejected.

`AUDIO_PLAY` requires an empty payload, offset zero, complete data, and a matching checksum. It starts speaker playback; only the speaker finish callback can produce COMPLETE. A duplicate play for the same terminal session returns its recorded status without replaying. `AUDIO_CANCEL` discards/stops only the matching session and needs no acknowledgment. Back, dictation, and app shutdown cancel playback. Receive inactivity expires at 30 seconds and playback at the PCM duration plus 10 seconds. Android bounds a diagnostic exchange to 90 seconds and a full reply exchange to nine minutes.

`AUDIO_STATUS` has an empty payload and correlates to the exact BEGIN, BLOCK, or PLAY transfer. Android validates watch identity, session, correlation, total, checksum, and confirmed offset. Its `Status` values are specific to kind 10:

| Value | Audio meaning |
| ---: | --- |
| 1 | READY: temporary buffer reserved |
| 2 | BUFFERED: block accepted |
| 3 | COMPLETE: speaker finished naturally after all bytes drained |
| 4 | MUTED: system speaker muted |
| 5 | BUSY: speaker or dictation in use |
| 6 | INVALID: format, bounds, session, payload, or checksum rejected |
| 7 | FAILED: allocation, stream open, playback, or timeout failure |
| 8 | CANCELLED: stopped or preempted |
| 9 | QUIET_TIME: Quiet Time rejects audio independently of speaker mute (since 0.1.10) |
| 10 | STORAGE: retired v0.1.17 cache error |
| 11 | BUFFER_FULL: streaming packet not accepted; retry after allowing playback to drain (v0.1.18) |

Speaker pumping uses partial writes and a timer, leaving the watch event loop responsive. Status replies wait for the existing watch outbox to become available. Transport ACKs do not establish audible playback, and a closed watch app does not queue audio for later.

Quiet Time is checked before BEGIN allocation, BLOCK acceptance, and PLAY, as well as by the active audio timer and finish callback. Enabling it discards any incomplete/buffered clip and stops playback. A terminal Quiet Time session cannot be replayed after Quiet Time is disabled; the phone must start a new session.

### Opt-in voice reply extension (v0.1.13)

`Flags & 0x10` on `FETCH_RESULT` at offset zero requests speech for a completed watch request. The watch sets it once after **Send + voice reply**, only for the current open-session capture. It is not included in the durable command or pending-record format. Normal sends, notes, later pages, and refreshes do not request speech.

The phone sends the result page first and deduplicates speech by watch/capture/transfer ID. Since v0.1.14, **Play voice reply** on a completed saved answer issues a fresh offset-zero voice fetch, allowing explicit replay while transport retries retain the same transfer ID. It emits `VOICE_STATUS` (114), with the originating `CaptureId` and a UTF-8 status payload (up to 120 characters). This is presentation only; it cannot change command state. Each ordinary audio clip uses a fresh audio-session CaptureId and carries the originating request capture in `ItemId`. Zero ItemId retains the independent diagnostic sound behavior. A nonzero ItemId that no longer matches the watch's open-session voice consent is rejected with `AUDIO_CANCELLED`, including BEGIN/BLOCK/PLAY arriving after Back. Through v0.1.16, replies used the diagnostic format in 16,000-byte sections. Version 0.1.17 used the full-reply mode below; v0.1.18 uses streaming format 3.


### Historical full-reply buffering (v0.1.17 only)

`Flags=2` keeps the same headerless 8 kHz mono signed 8-bit PCM, with a maximum `TotalBytes` of 480,000 (60 seconds). The phone sends one BEGIN, uploads the complete reply, and sends one PLAY. Every block is at most 768 bytes, aligned to a 256-byte cache page; only the final block may have a partial page. The full reply is checksum-validated before PLAY is accepted, using the checksum accumulated from successfully stored new blocks. Retries compare against stored bytes without advancing the checksum or received offset.

The watch uses a 1,024-byte playback read buffer and temporary persistent pages at keys 1001–2875. Key 1000 records only the cleanup high-water mark and is written before any pages. Runtime storage capacity must cover the reply plus an 8 KiB reserve for the app's other data; failures report STORAGE. These keys are disjoint from settings, captures, and handwritten notes.

Playback uses one speaker stream. Partial speaker writes retain the unread part of the read buffer. Completion still requires the speaker callback, not the final upload receipt. Back, Quiet Time, cancellation, errors, and completion invalidate the active session and release its RAM. Cleanup erases up to 16 pages per timer event; app shutdown performs one bounded batch, and a later launch removes any remaining pages. The marker never enables resumed playback after a restart. New replies may replace a cache while cleanup is pending.

Receipts are sent immediately when the outbox is available, with the existing timer retry if it is busy. Voice progress is a single upload percentage followed by a single playing status. Android TTS requests 1.5× speed with normal pitch; the 400-character and 60-second output limits remain.

### Bounded streaming (v0.1.18)

`Flags=3` selects independent IMA ADPCM blocks, with `TotalBytes` and `PageOffset` measured in **decoded 8 kHz PCM samples**, not compressed bytes. Total is 1–480,000. Each packet encodes `min(1529, total-offset)` samples; offset must be a multiple of 1529. Payload length is exactly `4 + samples/2` (integer division), at most 768 bytes. Header: signed little-endian 16-bit initial predictor (also the first output sample), step index 0–88, reserved zero. Remaining samples use standard IMA nibbles, low nibble first. An unused final high nibble is zero from the encoder. Initial predictor/index are included in each packet, so packet boundaries do not reset prediction adaptation. Speaker output is the decoded signed 16-bit predictor shifted right by eight bits.

`ConversationGeneration` is FNV-1a over the concatenated **encoded packet bytes**, including headers. New packets update the checksum; the last packet checks it before acceptance. Unlike preloaded diagnostics, streaming can already have played earlier packets when a final checksum error is detected. AppMessage transport protects individual dictionaries. A matching retry of the immediately preceding block is acknowledged without decoding twice; changed retries, holes, malformed headers and excess capacity are rejected.

BEGIN allocates a 24,576-byte PCM ring. PLAY is accepted once at least 12,288 samples, or the entire shorter reply, are received. Android sends PLAY after the first block crossing that threshold (normally 13,761 samples / 1.72 s of audio), then continues uploading while awaiting PLAY completion concurrently. A BUFFERED receipt reports cumulative decoded samples received. If the next block does not fit, status 11 (BUFFER_FULL) reports the unchanged received offset without accepting any data. Android waits 100 ms and retries the same block with a fresh transfer ID; the normal nine-minute overall timeout still applies. Android permits one block in flight and spaces new blocks at least 75 ms apart on fast transports. This bounds both memory and event pressure.

The speaker stays in one stream while packets refill the ring. Partial writes and ring wrap retain exact sample ordering. If empty, the pump waits for 12,288 samples or the remaining complete tail before feeding it again. Streaming stalls expire after 30 seconds without accepted packets or speaker progress; Android bounds the whole exchange to nine minutes. Final completion still comes only from the speaker callback, after the final block receipt has been queued. Back, Quiet Time and cancellation discard the ring.

No new audio data is persisted. Format 2 is rejected by v0.1.18; the old cache namespace is retained only for migration cleanup, one page per idle 250 ms timer event, paused while any audio session is active. Status 10 remains reserved for compatibility with v0.1.17. Startup UI percentage describes the small initial buffer; remaining packets do not overwrite the playing footer.
