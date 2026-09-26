# Architecture

Hermes Pebble separates watch capture, phone persistence, Hermes transport, and server-side actions. The official Pebble Android app remains the Bluetooth host; Hermes Pebble is a standalone companion selected through PBW metadata.

## Components

### Native watch app

The Pebble Time 2 app targets `emery` and uses standard Pebble dictation. It captures a transcript, requires an explicit review action, and can send a request or save a local note. One pending transcript is retained until Android sends an application-level durable receipt or the user explicitly discards it. A capture identifier is stable across retries and app restarts. The watch stores transcript bytes, not audio.

The native build uses the root `wscript` and the pinned watch protocol UUID. CloudPebble supplies the hosted SDK and may regenerate its own equivalent build script; the root metadata remains authoritative.

### Android companion

The Android application ID and namespace are `dev.hermespebble.companion`. Its intended layers are:

- Settings and secret storage for the HTTPS API root, Hermes credential, and independent access header.
- PebbleKit 2 listener and sender integration for validated AppMessages, host selection, and watch lifecycle.
- Room-backed command, note, history, session, run, and duplicate-receipt state.
- WorkManager-backed durable submission and bounded result monitoring.
- Hermes HTTP client for capability inspection, sessions, runs, polling, and stop.
- Compose screens for readiness, recent items, request detail, notes, typed fallback requests, and diagnostics.

The Gradle files pin the researched dependencies and use the AGP built-in Kotlin path. Room schema export is configured for a real build; generated schemas are not fabricated in this repository.

### Hermes and external systems

Hermes owns Matrix integration, tool authorization, approvals, run execution, and external actions. The companion sends an input and persisted session identifier, then displays Hermes output. It does not log in to Matrix or infer delivery from a transport acknowledgment. Recipients' replies and Matrix synchronization remain outside the watch protocol.

## Watch/phone protocol

`protocol/README.md` is the wire-format source of truth; the native and Android implementations must use matching constants. Numeric keys are fixed:

| Key | Name | Type |
| ---: | --- | --- |
| 0 | ProtocolVersion | UInt8 |
| 1 | MessageKind | UInt8 |
| 2 | TransferId | UInt32 |
| 3 | CaptureId | UInt32 |
| 4 | ChunkIndex | UInt8 |
| 5 | ChunkCount | UInt8 |
| 6 | Payload | Bytes |
| 7 | Status | UInt8 |
| 8 | ErrorCode | UInt8 |
| 9 | ItemId | UInt32 |
| 10 | ItemState | UInt8 |
| 11 | ItemKind | UInt8 |
| 12 | PageOffset | UInt32 |
| 13 | PageCount | UInt8 |
| 14 | TotalBytes | UInt32 |
| 15 | ConversationGeneration | UInt32 |
| 16 | Flags | UInt8 |
| 17 | CorrelationId | UInt32 |

Android validates numeric values and bounds because PebbleKit normalizes received numeric tuples to UInt32 or Int32. A logical transfer is at most 1,024 bytes, uses at most six chunks, and limits each payload chunk to 192 bytes. Chunking operates on UTF-8 bytes and never silently truncates a code point. Outbound transfers are serialized per direction and retried with bounded backoff.

The transport ACK/NACK is distinct from `DURABLE_RECEIPT`. Android sends the receipt only after the command or note and its duplicate-receipt record commit locally. A stale correlation is ignored. A watch capture is deduplicated by opaque `WatchIdentifier` plus `CaptureId`; Android's HTTP idempotency key is separate and persisted before the first submission.

## Hermes transport

The API root preserves an optional path prefix. The v1 contract uses:

- `GET /v1/capabilities` for nested run-submission and idempotency capabilities.
- `POST /api/sessions` with a client-generated `pt2_<UUID>` identifier.
- `GET /api/sessions/{session_id}` for reconciliation.
- `POST /v1/runs` with the confirmed input, persisted session ID, and persisted `Idempotency-Key`.
- `GET /v1/runs/{run_id}` for bounded polling.
- `POST /v1/runs/{run_id}/stop` for an explicit stop request.
- `GET /api/sessions/{session_id}/messages` for bounded history reconciliation.

The accepted run response is not a finished answer. The run ID is persisted before monitoring begins. Server statuses are mapped explicitly, including queued, started, running, approval-waiting, terminal, and unknown states. A queued follow-up retains its original conversation. A New conversation action creates a new dedicated session generation.

If a create response is lost, an automatic retry is allowed only when capabilities advertise supported and durable idempotency and the conservative retention window is still valid. Otherwise the state is `Outcome unknown — review before retrying`; the app never generates a fresh key for an ambiguous submission automatically. A terminal run is never automatically re-executed.

## Authentication and network safety

Every Hermes request uses the centralized client and attaches the Hermes Authorization credential. When enabled, the separately stored NetBird value is sent exactly as entered under the validated custom header. The header is for reverse-proxy access, not a NetBird management token. TLS is required, redirects are disabled for authenticated requests, HTML login pages are failures, and sensitive values are excluded from logs, exceptions, exports, backups, and watch messages.

A 401 or 403 is reported without guessing which layer rejected the request. A network/TLS failure, unsupported route, invalid JSON, authentication rejection, and unknown outcome are separate categories.

## Background and recovery

WorkManager and transactional command ownership recover pending work after process death. Submission and result monitoring are separate. Polling is prompt while a screen is active, slower in the background, bounded, and backed off on network failure. No permanent foreground service or tight polling loop is used. WorkManager is best effort under Doze, app standby, force-stop, and GrapheneOS policies; reopening the UI reconciles state.

The watch result remains available on the phone when the watch app is closed. Optional notifications are private and are not a guarantee of forwarding into a closed watch app.

## PBW metadata boundary

CloudPebble can compile the watch without an Android artifact, but its assembly path can drop `companionApp`. `tools/finalize_pbw.py` restores the repository declaration in a separate PBW, checks the fixed UUID, rejects duplicate members, and verifies preservation of all other uncompressed member bytes. It never compiles code or handles credentials.

## v1 boundaries

There is one dedicated Hermes conversation, no Matrix login, no raw watch audio, no offline transcription, no watch TTS, no full inbox or room browser, no streaming-token UI, and no server approval-management UI. A completed run is not an independent external-delivery receipt. These are intentional v1 limitations, not promises of future behavior.

## Review corrections

Each HTTP operation resolves credentials for its expected profile, so changing settings cannot redirect queued work to another endpoint. Old-profile items remain visible on the phone but cannot execute through the new profile. Inspect their session/run identifiers in the original Hermes server; v1 does not store old credentials or migrate those requests automatically. Refresh can resume a paused current-profile item when it was never submitted or has a known run ID. Repeating an unresolved request is an explicit new request in a new conversation.

The phone shows recovered conversation messages on the detail screen. HTTP reads are bounded and accept short bodies, numeric history identifiers/timestamps, tool-only messages, and completed idempotent replays. Stop responses trigger authoritative run polling rather than treating a sparse stop response as a completed result.

The watch uses the SDK's MenuLayer callbacks, AppTimer handles, and typed Dictionary tuples. Persistent reads/writes require exact byte counts, and transcript records are checksum-validated. The phone keeps transfer state separately per watch and acknowledges repeated final chunks; outgoing pages account for JSON escaping. An explicit discard is committed before acknowledgment.
