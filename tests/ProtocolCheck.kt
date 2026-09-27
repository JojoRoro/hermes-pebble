package dev.hermespebble.companion.pebble

import dev.hermespebble.companion.network.*
import io.rebble.pebblekit2.common.model.PebbleDictionaryItem

fun main() {
    var now = 1_000L
    val assembler = IncomingTransferAssembler { now }
    val text = "🚀".repeat(256).toByteArray()
    val chunks = OutgoingProtocolCodec.encode(WireMessageKind.SUBMIT_REQUEST, 42, captureId = 7, payload = text)
    check(chunks.size == 6)
    chunks.dropLast(1).forEach { check(assembler.accept(it) is DecodedChunkResult.Pending) }
    val result = assembler.accept(chunks.last()) as DecodedChunkResult.Complete
    check(result.message.payload.contentEquals(text))
    // A lost transport ACK for the final chunk must return the completed command again.
    check(assembler.accept(chunks.last()) is DecodedChunkResult.Complete)
    // A complete application retry keeps the same capture and transfer identifiers.
    chunks.forEach { assembler.accept(it) }
    val conflicting = chunks.first() + (WireProtocol.KEY_CHUNK_COUNT to PebbleDictionaryItem.UInt8(5))
    check(assembler.accept(conflicting) is DecodedChunkResult.Error)
    check(assembler.accept(chunks.first()) is DecodedChunkResult.Pending)
    now += 31_000
    check(assembler.accept(chunks[1]) is DecodedChunkResult.Error)
    check(assembler.accept(chunks.first()) is DecodedChunkResult.Pending)
    val invalidText = OutgoingProtocolCodec.encode(WireMessageKind.SUBMIT_REQUEST, 9, payload = byteArrayOf(0xc0.toByte(), 0x80.toByte()))
    assembler.reset()
    check(assembler.accept(invalidText.single()) is DecodedChunkResult.Error)
    // Round-trip probe correlation survives the actual wire codec and assembler.
    assembler.reset()
    val greeting = OutgoingProtocolCodec.encode(WireMessageKind.HANDSHAKE_ACK, 501).single()
    val phoneProbe = (assembler.accept(greeting) as DecodedChunkResult.Complete).message
    check(phoneProbe.correlationId == 0L)
    val watchReply = OutgoingProtocolCodec.encode(WireMessageKind.HANDSHAKE, 502, correlationId = phoneProbe.transferId).single()
    val handshake = (assembler.accept(watchReply) as DecodedChunkResult.Complete).message
    check(handshake.correlationId == 501L && handshake.transferId == 502L)
    val ack = OutgoingProtocolCodec.encode(WireMessageKind.HANDSHAKE_ACK, 503, correlationId = handshake.transferId).single()
    check((assembler.accept(ack) as DecodedChunkResult.Complete).message.correlationId == 502L)
    val capabilities = HermesCapabilities(true, true, true, 86400, emptyList())
    check(capabilities.retryDecision(1_000, 2_000) == RetryDecision.SAFE)
    check(capabilities.retryDecision(1_000, 1_000 + 23 * 60 * 60 * 1000L) == RetryDecision.DEADLINE_PASSED)
    check(capabilities.copy(runsIdempotencyDurable = false).retryDecision(1_000, 2_000) == RetryDecision.UNSUPPORTED)
    println("Kotlin protocol and retry-deadline checks passed")
}
