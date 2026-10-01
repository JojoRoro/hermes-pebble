package dev.hermespebble.companion.pebble

import kotlinx.coroutines.async
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking

fun main() = runBlocking {
    val pcm = ByteArray(16000) { (it * 71).toByte() }
    val binary = ByteArray(1024) { 0x80.toByte() } // Never treat PCM as UTF-8.
    val chunks = OutgoingProtocolCodec.encode(WireMessageKind.AUDIO_BLOCK, 1, payload = binary)
    val assembler = IncomingTransferAssembler()
    chunks.dropLast(1).forEach { check(assembler.accept(it) is DecodedChunkResult.Pending) }
    check((assembler.accept(chunks.last()) as DecodedChunkResult.Complete).message.payload.contentEquals(binary))
    var id = 100L
    var received = 0
    var play: WireMessage? = null
    var receiver: WatchAudioTransfer? = null
    val buffer = ByteArray(16000)
    val transfer = WatchAudioTransfer(send = { watch, request ->
        val status = when (request.kind) {
            WireMessageKind.AUDIO_BEGIN -> WatchAudioTransfer.READY
            WireMessageKind.AUDIO_BLOCK -> {
                check(request.pageOffset == received.toLong())
                request.payload.copyInto(buffer, received)
                received += request.payload.size
                WatchAudioTransfer.BUFFERED
            }
            WireMessageKind.AUDIO_PLAY -> { play = request; 0 }
            else -> 0
        }
        if (status != 0) receiver!!.onStatus(watch, request.copy(kind = WireMessageKind.AUDIO_STATUS,
            correlationId = request.transferId, status = status, pageOffset = received.toLong(), payload = ByteArray(0)))
        true
    }, nextId = { ++id })
    receiver = transfer
    val result = async { transfer.play("watch", pcm) {} }
    while (play == null) delay(1)
    check(received == pcm.size && buffer.contentEquals(pcm))
    check(!result.isCompleted) // A transport ACK / full buffer cannot report success.
    val done = play!!.copy(kind = WireMessageKind.AUDIO_STATUS, correlationId = play!!.transferId,
        status = WatchAudioTransfer.COMPLETE, pageOffset = received.toLong())
    transfer.onStatus("another watch", done)
    transfer.onStatus("watch", done.copy(captureId = done.captureId + 1))
    transfer.onStatus("watch", done.copy(correlationId = done.correlationId + 1))
    delay(5)
    check(!result.isCompleted)
    transfer.onStatus("watch", done)
    check(result.await().contains("playback complete"))

    var cancelled = false
    val silent = WatchAudioTransfer(send = { _, request ->
        if (request.kind == WireMessageKind.AUDIO_CANCEL) cancelled = true
        true
    }, nextId = { ++id }, replyTimeoutMillis = 10)
    val failure = runCatching { silent.play("watch", byteArrayOf(1, 2)) {} }.exceptionOrNull()
    check(failure?.message?.contains("timed out") == true && cancelled)
    check(WatchAudioTransfer.checksum("hello".toByteArray()) == 0x4f9f2cabL)
    println("Audio binary transfer, correlated completion, and timeout checks passed")
}
