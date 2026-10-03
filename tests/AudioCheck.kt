package dev.hermespebble.companion.pebble

import kotlinx.coroutines.async
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking

fun main() = runBlocking {
    val requests = VoiceReplyRequests()
    val voiceFetch = WireMessage(kind = WireMessageKind.FETCH_RESULT, transferId = 1,
        captureId = 42, flags = WireProtocol.FLAG_VOICE_REPLY, payload = ByteArray(0))
    check(!requests.accept("watch", voiceFetch.copy(flags = 0))) // Ordinary open/refresh stays silent.
    check(!requests.accept("watch", voiceFetch.copy(pageOffset = 520)))
    check(!requests.accept("watch", voiceFetch.copy(kind = WireMessageKind.SAVE_NOTE)))
    check(requests.accept("watch", voiceFetch))
    check(!requests.accept("watch", voiceFetch)) // Transport retry cannot speak twice.
    check(requests.accept("watch", voiceFetch.copy(transferId = 2))) // Explicit replay of same capture.
    check(!requests.accept("watch", voiceFetch.copy(transferId = 2)))
    check(requests.accept("another watch", voiceFetch))

    check(VoicePcm.spokenText("**Hello**\nworld") == "Hello world")
    check(VoicePcm.spokenText("a ".repeat(300)).endsWith("Read the rest on your watch."))
    check(VoicePcm.spokenText("🚀".repeat(401)).contains("🚀".repeat(400)))
    check(VoicePcm.convert(byteArrayOf(0, 128.toByte(), 255.toByte()), 8000, 1, 3)
        .contentEquals(byteArrayOf((-128).toByte(), 0, 127)))
    check(VoicePcm.convert(byteArrayOf(0, 128.toByte(), 0, 0, 255.toByte(), 127), 8000, 1, 2)
        .contentEquals(byteArrayOf((-128).toByte(), 0, 127)))
    val floats = java.nio.ByteBuffer.allocate(16).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        .putFloat(-1f).putFloat(1f).putFloat(Float.NaN).putFloat(0f).array()
    check(VoicePcm.convert(floats, 8000, 2, 4).contentEquals(byteArrayOf(0, 0)))
    check(VoicePcm.convert(ByteArray(44100 * 2) { 128.toByte() }, 44100, 2, 3).size == 8000)
    check(VoicePcm.convert(byteArrayOf(0, 255.toByte()), 16000, 1, 3).contentEquals(byteArrayOf(0)))
    // Common Android speech rates must preserve both duration and pitch.
    for (rate in listOf(16000, 22050, 24000, 44100, 48000)) {
        val source = java.nio.ByteBuffer.allocate(rate * 2 * 2).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        repeat(rate * 2) { frame ->
            source.putShort((kotlin.math.sin(2 * Math.PI * 440 * frame / rate) * 16000).toInt().toShort())
        }
        val converted = VoicePcm.convert(source.array(), rate, 1, 2)
        check(converted.size == 16000) // Two seconds at 8 kHz.
        val crossings = (1 until converted.size).count { converted[it - 1] <= 0 && converted[it] > 0 }
        check(crossings in 879..881) // 440 Hz remains 440 Hz, not double speed.
    }
    for (invalid in listOf(byteArrayOf(), byteArrayOf(1))) {
        check(runCatching { VoicePcm.convert(invalid, 8000, 1, 2) }.isFailure)
    }
    check(runCatching { VoicePcm.convert(byteArrayOf(0), 0, 1, 3) }.isFailure)
    check(runCatching { VoicePcm.convert(ByteArray(8000 * 61), 8000, 1, 3) }.isFailure)
    val pcm = ByteArray(16000) { (it * 71).toByte() }
    val binary = ByteArray(1024) { 0x80.toByte() } // Never treat PCM as UTF-8.
    val chunks = OutgoingProtocolCodec.encode(WireMessageKind.AUDIO_BLOCK, 1, payload = binary)
    check(chunks.size == 2) // Previously six round trips for every 1 KiB block.
    chunks.forEach { dictionary ->
        check(1 + dictionary.values.sumOf { 7 + it.size } <= 1024) // Actual tuple encoding fits inbox.
    }
    val oversized = chunks.first().toMutableMap().apply {
        put(WireProtocol.KEY_PAYLOAD, io.rebble.pebblekit2.common.model.PebbleDictionaryItem.Bytes(ByteArray(769)))
    }
    check(IncomingTransferAssembler().accept(oversized) is DecodedChunkResult.Error)
    val oversizedText = chunks.first().toMutableMap().apply {
        put(WireProtocol.KEY_MESSAGE_KIND, io.rebble.pebblekit2.common.model.PebbleDictionaryItem.UInt8(WireMessageKind.RESULT_PAGE.value))
    }
    check(IncomingTransferAssembler().accept(oversizedText) is DecodedChunkResult.Error)
    val assembler = IncomingTransferAssembler()
    chunks.dropLast(1).forEach { check(assembler.accept(it) is DecodedChunkResult.Pending) }
    check((assembler.accept(chunks.last()) as DecodedChunkResult.Complete).message.payload.contentEquals(binary))
    var id = 100L
    var received = 0
    var play: WireMessage? = null
    var receiver: WatchAudioTransfer? = null
    val buffer = ByteArray(16000)
    val transfer = WatchAudioTransfer(send = { watch, request ->
        check(request.itemId == 42L) // Every clip operation stays bound to the opted-in capture.
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
    var playbackAnnounced = false
    val result = async { transfer.play("watch", pcm, replyCaptureId = 42, onPlayback = {
        check(received == pcm.size && play == null)
        playbackAnnounced = true
    }) {} }
    while (play == null) delay(1)
    check(playbackAnnounced && received == pcm.size && buffer.contentEquals(pcm))
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
    var quietReceiver: WatchAudioTransfer? = null
    var quietCancel = false
    var quietBlocks = 0
    val quiet = WatchAudioTransfer(send = { watch, request ->
        when (request.kind) {
            WireMessageKind.AUDIO_BEGIN -> quietReceiver!!.onStatus(watch, request.copy(
                kind = WireMessageKind.AUDIO_STATUS, correlationId = request.transferId,
                status = WatchAudioTransfer.QUIET_TIME))
            WireMessageKind.AUDIO_BLOCK -> quietBlocks++
            WireMessageKind.AUDIO_CANCEL -> quietCancel = true
            else -> Unit
        }
        true
    }, nextId = { ++id })
    quietReceiver = quiet
    val quietError = runCatching { quiet.play("watch", pcm) {} }.exceptionOrNull()
    check(quietError?.message?.contains("Quiet Time is on") == true)
    check(quietBlocks == 0 && quietCancel)
    check(WatchAudioTransfer.checksum("hello".toByteArray()) == 0x4f9f2cabL)
    println("Audio binary transfer, correlated completion, and timeout checks passed")
}
