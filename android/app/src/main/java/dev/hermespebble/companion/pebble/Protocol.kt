package dev.hermespebble.companion.pebble

import io.rebble.pebblekit2.common.model.PebbleDictionary
import io.rebble.pebblekit2.common.model.PebbleDictionaryItem
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction

object WireProtocol {
    val APP_UUID: java.util.UUID = java.util.UUID.fromString("7d07aa22-7d13-48c1-a400-2602a5ae4647")
    const val VERSION = 1
    const val MAX_CHUNKS = 6
    const val MAX_CHUNK_PAYLOAD_BYTES = 192
    const val MAX_TRANSFER_BYTES = 1024
    const val MAX_WATCH_TEXT_BYTES = 1024
    const val MAX_RECENT_ITEMS = 3
    const val MAX_RESULT_PAGE_BYTES = 768

    const val KEY_PROTOCOL_VERSION = 0u
    const val KEY_MESSAGE_KIND = 1u
    const val KEY_TRANSFER_ID = 2u
    const val KEY_CAPTURE_ID = 3u
    const val KEY_CHUNK_INDEX = 4u
    const val KEY_CHUNK_COUNT = 5u
    const val KEY_PAYLOAD = 6u
    const val KEY_STATUS = 7u
    const val KEY_ERROR_CODE = 8u
    const val KEY_ITEM_ID = 9u
    const val KEY_ITEM_STATE = 10u
    const val KEY_ITEM_KIND = 11u
    const val KEY_PAGE_OFFSET = 12u
    const val KEY_PAGE_COUNT = 13u
    const val KEY_TOTAL_BYTES = 14u
    const val KEY_CONVERSATION_GENERATION = 15u
    const val KEY_FLAGS = 16u
    const val KEY_CORRELATION_ID = 17u

    const val FLAG_MORE = 0x01
    const val FLAG_STOP_REQUESTED = 0x02
    const val FLAG_REPLAYED = 0x04
    const val FLAG_DURABLE_COMMIT = 0x08
}

enum class WireMessageKind(val value: Int) {
    HANDSHAKE(1),
    SUBMIT_REQUEST(2),
    SAVE_NOTE(3),
    DISCARD_CAPTURE(4),
    FETCH_RECENT(5),
    FETCH_RESULT(6),
    START_CONVERSATION(7),
    STOP_REQUEST(8),
    HANDSHAKE_ACK(101),
    DURABLE_RECEIPT(102),
    STATUS_UPDATE(103),
    RECENT_PAGE(104),
    RESULT_PAGE(105),
    NEW_CONVERSATION_ACK(106),
    STRUCTURED_ERROR(107),
    CAPTURE_DISCARDED(108),
    ;

    companion object {
        fun from(value: Int): WireMessageKind? = entries.firstOrNull { it.value == value }
    }
}

enum class WireStatus(val value: Int) {
    NONE(0),
    WAITING_FOR_PHONE(1),
    SAVED_QUEUED(2),
    SUBMITTING(3),
    ACCEPTED(4),
    WORKING(5),
    APPROVAL_NEEDED(6),
    COMPLETED(7),
    FAILED(8),
    STOPPING(9),
    CANCELLED(10),
    INTERRUPTED(11),
    OUTCOME_UNKNOWN(12),
    NOTE_SAVED(13),
    WAITING_FOR_PROFILE(14),
    DISCARDED(15),
}

enum class WireError(val value: Int) {
    NONE(0),
    PROTOCOL_VERSION(1),
    UNSUPPORTED_KIND(2),
    MALFORMED(3),
    TRANSFER_BOUND(4),
    DUPLICATE_CHUNK(5),
    DURABLE_STORAGE(6),
    CONNECTION_SETTINGS(7),
    AUTHENTICATION(8),
    NETWORK_TLS(9),
    REDIRECT(10),
    UNSUPPORTED_ROUTE(11),
    INVALID_RESPONSE(12),
    UNKNOWN_OUTCOME(13),
    NOT_FOUND(14),
    IDEMPOTENCY_CONFLICT(15),
    SERVER(16),
    NOTE_STORAGE(17),
    WATCH_UNAVAILABLE(18),
}

enum class WireItemKind(val value: Int) {
    NONE(0),
    REQUEST(1),
    NOTE(2),
}

data class WireMessage(
    val kind: WireMessageKind,
    val transferId: Long,
    val captureId: Long = 0,
    val correlationId: Long = 0,
    val itemId: Long = 0,
    val status: Int = 0,
    val errorCode: Int = 0,
    val itemState: Int = 0,
    val itemKind: WireItemKind = WireItemKind.NONE,
    val pageOffset: Long = 0,
    val pageCount: Int = 0,
    val totalBytes: Long = 0,
    val generation: Long = 0,
    val flags: Int = 0,
    val payload: ByteArray,
) {
    fun hasSameMetadata(other: WireMessage): Boolean =
        kind == other.kind &&
            transferId == other.transferId &&
            captureId == other.captureId &&
            correlationId == other.correlationId &&
            itemId == other.itemId &&
            status == other.status &&
            errorCode == other.errorCode &&
            itemState == other.itemState &&
            itemKind == other.itemKind &&
            pageOffset == other.pageOffset &&
            pageCount == other.pageCount &&
            totalBytes == other.totalBytes &&
            generation == other.generation &&
            flags == other.flags
}

sealed interface DecodedChunkResult {
    data class Pending(val transferId: Long) : DecodedChunkResult
    data class Complete(val message: WireMessage) : DecodedChunkResult
    data class Error(val code: WireError, val message: String) : DecodedChunkResult
}

class IncomingTransferAssembler(private val clock: () -> Long = System::currentTimeMillis) {
    private var expectedCount = 0
    private var lastChunkAt = 0L
    private var completed: WireMessage? = null
    private var kind: WireMessageKind? = null
    private var transferId = 0L
    private var metadata: WireMessage? = null
    private val chunks = arrayOfNulls<ByteArray>(WireProtocol.MAX_CHUNKS)
    private val received = BooleanArray(WireProtocol.MAX_CHUNKS)
    private var receivedCount = 0
    private var totalBytes = 0

    @Synchronized
    fun accept(data: PebbleDictionary): DecodedChunkResult {
        if (clock() - lastChunkAt > 30_000L) reset()
        lastChunkAt = clock()
        val version = data.uint8(WireProtocol.KEY_PROTOCOL_VERSION)
            ?: return DecodedChunkResult.Error(WireError.MALFORMED, "Protocol version is missing or invalid")
        if (version != WireProtocol.VERSION) {
            reset()
            return DecodedChunkResult.Error(
                WireError.PROTOCOL_VERSION,
                "The watch and phone protocol versions differ. Update both apps.",
            )
        }
        val kindValue = data.uint8(WireProtocol.KEY_MESSAGE_KIND)
            ?: return DecodedChunkResult.Error(WireError.MALFORMED, "Message kind is missing or invalid")
        val kind = WireMessageKind.from(kindValue)
            ?: return DecodedChunkResult.Error(WireError.UNSUPPORTED_KIND, "The watch sent an unsupported message kind")
        val transfer = data.uint32(WireProtocol.KEY_TRANSFER_ID)
            ?: return DecodedChunkResult.Error(WireError.MALFORMED, "Transfer ID is missing or invalid")
        if (transfer == 0L) {
            return DecodedChunkResult.Error(WireError.MALFORMED, "Transfer ID must be non-zero")
        }
        val index = data.uint8(WireProtocol.KEY_CHUNK_INDEX)
            ?: return DecodedChunkResult.Error(WireError.MALFORMED, "Chunk index is missing or invalid")
        val count = data.uint8(WireProtocol.KEY_CHUNK_COUNT)
            ?: return DecodedChunkResult.Error(WireError.MALFORMED, "Chunk count is missing or invalid")
        if (count !in 1..WireProtocol.MAX_CHUNKS || index >= count) {
            return DecodedChunkResult.Error(WireError.TRANSFER_BOUND, "Chunk range exceeds the protocol bound")
        }
        val payload = data.bytes(WireProtocol.KEY_PAYLOAD)
            ?: return DecodedChunkResult.Error(WireError.MALFORMED, "Payload is missing or has the wrong type")
        if (payload.size > WireProtocol.MAX_CHUNK_PAYLOAD_BYTES) {
            return DecodedChunkResult.Error(WireError.TRANSFER_BOUND, "A payload chunk exceeds 192 bytes")
        }
        val parsed = parseMetadata(data, kind, transfer)
            ?: return DecodedChunkResult.Error(WireError.MALFORMED, "A required metadata field is invalid")
        if (completed != null && transfer != this.transferId) reset()
        val activeKind = this.kind
        if (activeKind == null) {
            if (index != 0) {
                return DecodedChunkResult.Error(WireError.MALFORMED, "A transfer must begin with chunk zero")
            }
            expectedCount = count
            this.kind = kind
            this.transferId = transfer
            this.metadata = parsed
        } else if (
            expectedCount != count ||
            activeKind != parsed.kind ||
            this.transferId != transfer ||
            !requireNotNull(metadata).hasSameMetadata(parsed)
        ) {
            reset()
            return DecodedChunkResult.Error(WireError.DUPLICATE_CHUNK, "Chunks carried conflicting metadata")
        }
        if (received[index]) {
            val previous = chunks[index]
            if (previous == null || !previous.contentEquals(payload)) {
                reset()
                return DecodedChunkResult.Error(WireError.DUPLICATE_CHUNK, "A duplicate chunk did not match its original bytes")
            }
            return completed?.let { DecodedChunkResult.Complete(it) } ?: DecodedChunkResult.Pending(transfer)
        }
        if (totalBytes + payload.size > WireProtocol.MAX_TRANSFER_BYTES) {
            reset()
            return DecodedChunkResult.Error(WireError.TRANSFER_BOUND, "The logical transfer exceeds 1024 bytes")
        }
        chunks[index] = payload.copyOf()
        received[index] = true
        receivedCount += 1
        totalBytes += payload.size
        if (receivedCount != count) return DecodedChunkResult.Pending(transfer)
        val bytes = ByteArray(totalBytes)
        var offset = 0
        for (chunkIndex in 0 until count) {
            val chunk = chunks[chunkIndex] ?: return DecodedChunkResult.Error(WireError.MALFORMED, "A transfer chunk is missing")
            chunk.copyInto(bytes, offset)
            offset += chunk.size
        }
        try {
            decodeUtf8(bytes)
        } catch (_: Exception) {
            reset()
            return DecodedChunkResult.Error(WireError.MALFORMED, "The transfer is not valid UTF-8")
        }
        val completed = requireNotNull(metadata).copy(payload = bytes)
        this.completed = completed
        return DecodedChunkResult.Complete(completed)
    }

    @Synchronized
    fun reset() {
        completed = null
        expectedCount = 0
        kind = null
        transferId = 0
        metadata = null
        chunks.fill(null)
        received.fill(false)
        receivedCount = 0
        totalBytes = 0
    }

    private fun parseMetadata(data: PebbleDictionary, kind: WireMessageKind, transferId: Long): WireMessage? {
        val captureId = data.uint32(WireProtocol.KEY_CAPTURE_ID) ?: return null
        val correlationId = data.uint32(WireProtocol.KEY_CORRELATION_ID) ?: return null
        val itemId = data.uint32(WireProtocol.KEY_ITEM_ID) ?: return null
        val pageOffset = data.uint32(WireProtocol.KEY_PAGE_OFFSET) ?: return null
        val totalBytes = data.uint32(WireProtocol.KEY_TOTAL_BYTES) ?: return null
        val generation = data.uint32(WireProtocol.KEY_CONVERSATION_GENERATION) ?: return null
        val status = data.uint8(WireProtocol.KEY_STATUS) ?: return null
        val errorCode = data.uint8(WireProtocol.KEY_ERROR_CODE) ?: return null
        val itemState = data.uint8(WireProtocol.KEY_ITEM_STATE) ?: return null
        val itemKind = data.uint8(WireProtocol.KEY_ITEM_KIND) ?: return null
        val pageCount = data.uint8(WireProtocol.KEY_PAGE_COUNT) ?: return null
        val flags = data.uint8(WireProtocol.KEY_FLAGS) ?: return null
        return WireMessage(
            kind = kind,
            transferId = transferId,
            captureId = captureId,
            correlationId = correlationId,
            itemId = itemId,
            status = status,
            errorCode = errorCode,
            itemState = itemState,
            itemKind = WireItemKind.entries.firstOrNull { it.value == itemKind } ?: return null,
            pageOffset = pageOffset,
            pageCount = pageCount,
            totalBytes = totalBytes,
            generation = generation,
            flags = flags,
            payload = ByteArray(0),
        )
    }
}

object OutgoingProtocolCodec {
    fun encode(
        kind: WireMessageKind,
        transferId: Long,
        captureId: Long = 0,
        correlationId: Long = 0,
        itemId: Long = 0,
        status: Int = 0,
        errorCode: Int = 0,
        itemState: Int = 0,
        itemKind: WireItemKind = WireItemKind.NONE,
        pageOffset: Long = 0,
        pageCount: Int = 0,
        totalBytes: Long = 0,
        generation: Long = 0,
        flags: Int = 0,
        payload: ByteArray = ByteArray(0),
    ): List<PebbleDictionary> {
        require(payload.size <= WireProtocol.MAX_TRANSFER_BYTES)
        require(transferId in 1..UInt.MAX_VALUE.toLong())
        require(captureId in 0..UInt.MAX_VALUE.toLong())
        require(correlationId in 0..UInt.MAX_VALUE.toLong())
        require(itemId in 0..UInt.MAX_VALUE.toLong())
        require(pageOffset in 0..UInt.MAX_VALUE.toLong())
        require(totalBytes in 0..UInt.MAX_VALUE.toLong())
        require(generation in 0..UInt.MAX_VALUE.toLong())
        val boundaries = utf8ChunkBoundaries(payload)
        if (boundaries.size > WireProtocol.MAX_CHUNKS) {
            throw IllegalArgumentException("Transfer needs too many chunks")
        }
        return boundaries.mapIndexed { index, range ->
            mapOf(
                WireProtocol.KEY_PROTOCOL_VERSION to PebbleDictionaryItem.UInt8(WireProtocol.VERSION.toUByte()),
                WireProtocol.KEY_MESSAGE_KIND to PebbleDictionaryItem.UInt8(kind.value.toUByte()),
                WireProtocol.KEY_TRANSFER_ID to PebbleDictionaryItem.UInt32(transferId.toUInt()),
                WireProtocol.KEY_CAPTURE_ID to PebbleDictionaryItem.UInt32(captureId.toUInt()),
                WireProtocol.KEY_CHUNK_INDEX to PebbleDictionaryItem.UInt8(index.toUByte()),
                WireProtocol.KEY_CHUNK_COUNT to PebbleDictionaryItem.UInt8(boundaries.size.toUByte()),
                WireProtocol.KEY_PAYLOAD to PebbleDictionaryItem.Bytes(
                    if (payload.isEmpty()) ByteArray(0) else payload.copyOfRange(range.first, range.last + 1),
                ),
                WireProtocol.KEY_STATUS to PebbleDictionaryItem.UInt8(status.coerceIn(0, 255).toUByte()),
                WireProtocol.KEY_ERROR_CODE to PebbleDictionaryItem.UInt8(errorCode.coerceIn(0, 255).toUByte()),
                WireProtocol.KEY_ITEM_ID to PebbleDictionaryItem.UInt32(itemId.toUInt()),
                WireProtocol.KEY_ITEM_STATE to PebbleDictionaryItem.UInt8(itemState.coerceIn(0, 255).toUByte()),
                WireProtocol.KEY_ITEM_KIND to PebbleDictionaryItem.UInt8(itemKind.value.toUByte()),
                WireProtocol.KEY_PAGE_OFFSET to PebbleDictionaryItem.UInt32(pageOffset.toUInt()),
                WireProtocol.KEY_PAGE_COUNT to PebbleDictionaryItem.UInt8(pageCount.coerceIn(0, 255).toUByte()),
                WireProtocol.KEY_TOTAL_BYTES to PebbleDictionaryItem.UInt32(totalBytes.toUInt()),
                WireProtocol.KEY_CONVERSATION_GENERATION to PebbleDictionaryItem.UInt32(generation.toUInt()),
                WireProtocol.KEY_FLAGS to PebbleDictionaryItem.UInt8(flags.coerceIn(0, 255).toUByte()),
                WireProtocol.KEY_CORRELATION_ID to PebbleDictionaryItem.UInt32(correlationId.toUInt()),
            )
        }
    }

    private fun utf8ChunkBoundaries(payload: ByteArray): List<IntRange> {
        if (payload.isEmpty()) return listOf(0..0)
        val ranges = mutableListOf<IntRange>()
        var start = 0
        while (start < payload.size) {
            var endExclusive = minOf(start + WireProtocol.MAX_CHUNK_PAYLOAD_BYTES, payload.size)
            while (endExclusive < payload.size && isContinuation(payload[endExclusive])) {
                endExclusive--
            }
            if (endExclusive == start) {
                throw IllegalArgumentException("A UTF-8 code point exceeds the chunk limit")
            }
            val lastIncluded = endExclusive - 1
            ranges += start..lastIncluded
            start = endExclusive
        }
        return ranges
    }

    private fun isContinuation(value: Byte): Boolean = (value.toInt() and 0xc0) == 0x80
}

fun decodeUtf8(bytes: ByteArray): String = Charsets.UTF_8.newDecoder()
    .onMalformedInput(CodingErrorAction.REPORT)
    .onUnmappableCharacter(CodingErrorAction.REPORT)
    .decode(ByteBuffer.wrap(bytes))
    .toString()

private fun PebbleDictionary.uint8(key: UInt): Int? = when (val item = this[key]) {
    is PebbleDictionaryItem.UInt8 -> item.value.toInt()
    is PebbleDictionaryItem.UInt16 -> item.value.toInt().takeIf { it <= 0xff }
    is PebbleDictionaryItem.UInt32 -> item.value.toLong().takeIf { it <= 0xff }?.toInt()
    is PebbleDictionaryItem.Int32 -> item.value.takeIf { it in 0..0xff }
    else -> null
}

private fun PebbleDictionary.uint32(key: UInt): Long? = when (val item = this[key]) {
    is PebbleDictionaryItem.UInt32 -> item.value.toLong()
    is PebbleDictionaryItem.UInt16 -> item.value.toLong()
    is PebbleDictionaryItem.UInt8 -> item.value.toLong()
    is PebbleDictionaryItem.Int32 -> item.value.toLong().takeIf { it >= 0 }
    else -> null
}

private fun PebbleDictionary.bytes(key: UInt): ByteArray? = (this[key] as? PebbleDictionaryItem.Bytes)?.value
