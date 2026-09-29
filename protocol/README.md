# Hermes Pebble protocol v1

This document is the source of truth for the watch/phone wire format. The matching constants are committed in `src/c/protocol.h` and `android/app/src/main/java/dev/hermespebble/companion/pebble/Protocol.kt`.

## Constants and limits

| Constant | Value |
| --- | ---: |
| Protocol version | `1` |
| Pebble AppMessage input buffer | 1,024 bytes |
| Pebble AppMessage output buffer | 1,024 bytes |
| Maximum chunks in one transfer | 6 |
| Maximum payload bytes per chunk | 192 |
| Maximum logical transfer bytes | 1,024; a full transfer uses six chunks and caps its final chunk at 64 bytes |
| Maximum dictation/note bytes | 1,024 UTF-8 bytes, excluding the terminating NUL |
| Maximum recent items returned to watch | 3 |
| Maximum encoded watch result page | 768 UTF-8 bytes |
| Per-chunk transfer timeout | 15 seconds |
| Application durable-receipt timeout | 30 seconds |
| Bounded transport retry delays | 1, 2, 4, 8 seconds |

A maximum-size message uses all 1,024 logical transfer bytes and therefore six chunks. The fixed tuples add at most 77 bytes when the key/type header and values are counted; a six-chunk dictionary remains below 1,101 bytes before platform tuple overhead and is not emitted as one dictionary. Each emitted dictionary contains one 192-byte payload chunk and a conservative full tuple set of approximately 274 bytes, well below the watch's 1,024-byte output limit. Android also rejects a decoded chunk larger than 192 bytes.

Text chunking operates on UTF-8 bytes. Handwriting uses separate binary blocks described below. A sender chooses boundaries that do not split a code point. Receivers concatenate bytes, validate bounds and the complete UTF-8 sequence, then decode. A character count is never used as a transfer limit.

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
| 6 | `Payload` | `byte[]` / `Bytes` | UTF-8 bytes, or binary ink for kind 9; never NUL-terminated |
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
| 101 | Phone → watch | `HANDSHAKE_ACK` | Transfer, correlation, protocol version, conversation generation |
| 102 | Phone → watch | `DURABLE_RECEIPT` | Transfer, correlation, capture, item kind, item ID when assigned, status |
| 103 | Phone → watch | `STATUS_UPDATE` | Transfer, correlation zero, capture, item ID, status, error code, flags |
| 104 | Phone → watch | `RECENT_PAGE` | Transfer, correlation, page offset, page count, JSON payload |
| 105 | Phone → watch | `RESULT_PAGE` | Transfer, correlation, capture, page offset, total bytes, flags, JSON payload |
| 106 | Phone → watch | `NEW_CONVERSATION_ACK` | Transfer, correlation, previous and new generation |
| 107 | Phone → watch | `STRUCTURED_ERROR` | Transfer, correlation, error code, bounded UTF-8 diagnostic |
| 108 | Phone → watch | `CAPTURE_DISCARDED` | Transfer, correlation, capture |
| 109 | Phone → watch | `INK_RECEIPT` | Correlation, capture, end offset, total ink bytes, CRC, durable flag; status 13 when complete |

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

`output` is the actual Hermes answer. A completed HTTP run is not converted into a Matrix or other external-delivery receipt. If `more` is true, the watch displays “More on phone” and requests the next result page (at most 768 bytes including JSON encoding) with `FETCH_RESULT`.

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
