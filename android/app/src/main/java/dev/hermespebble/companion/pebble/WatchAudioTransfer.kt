package dev.hermespebble.companion.pebble

import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull

/** A bounded, prebuffered audio transfer. Transport ACKs alone never mean playback succeeded. */
class WatchAudioTransfer(
    private val send: suspend (String, WireMessage) -> Boolean,
    private val nextId: () -> Long,
    private val replyTimeoutMillis: Long = 20_000L,
) {
    private data class Pending(val watch: String, val session: Long, val reply: CompletableDeferred<WireMessage>)
    private val pending = ConcurrentHashMap<Long, Pending>()
    private val mutex = Mutex()

    fun onStatus(watch: String, message: WireMessage) {
        if (message.kind != WireMessageKind.AUDIO_STATUS) return
        val waiter = pending[message.correlationId] ?: return
        if (waiter.watch == watch && waiter.session == message.captureId) waiter.reply.complete(message)
    }

    suspend fun play(watch: String, pcm: ByteArray, replyCaptureId: Long = 0,
        format: Int = FORMAT, onUpload: suspend (Int) -> Unit = {},
        onPlayback: suspend () -> Unit = {}, progress: (String) -> Unit): String = mutex.withLock {
        require(format == FORMAT || format == REPLY_FORMAT) { "Unsupported watch audio format." }
        require(pcm.isNotEmpty() && pcm.size <= if (format == REPLY_FORMAT) MAX_REPLY_BYTES else MAX_BYTES) {
            "The audio clip exceeds the watch audio limit."
        }
        val session = nextId()
        val checksum = checksum(pcm)
        var finished = false
        fun request(kind: WireMessageKind, offset: Int = 0, payload: ByteArray = ByteArray(0)) = WireMessage(
            kind = kind, transferId = nextId(), captureId = session, generation = checksum,
            totalBytes = pcm.size.toLong(), pageOffset = offset.toLong(), flags = format, payload = payload, itemId = replyCaptureId,
        )
        try {
            withTimeout(if (format == REPLY_FORMAT) 9 * 60_000L else 90_000L) {
                progress("Checking the watch speaker…")
                exchange(watch, request(WireMessageKind.AUDIO_BEGIN), READY, 0)
                var lastPercent = -5
                // One dictionary per block avoids a second Bluetooth round trip.
                for (offset in pcm.indices.step(WireProtocol.MAX_AUDIO_CHUNK_PAYLOAD_BYTES)) {
                    val end = minOf(offset + WireProtocol.MAX_AUDIO_CHUNK_PAYLOAD_BYTES, pcm.size)
                    exchange(watch, request(WireMessageKind.AUDIO_BLOCK, offset, pcm.copyOfRange(offset, end)), BUFFERED, end)
                    val percent = end * 100 / pcm.size
                    if (percent >= lastPercent + 5 || end == pcm.size) {
                        progress("Sending sound: $percent%")
                        onUpload(percent)
                        lastPercent = percent
                    }
                }
                progress("Waiting for watch playback…")
                onPlayback()
                val playbackTimeout = if (format == REPLY_FORMAT) maxOf(replyTimeoutMillis, pcm.size / 8L + 10_000L)
                    else replyTimeoutMillis
                exchange(watch, request(WireMessageKind.AUDIO_PLAY), COMPLETE, pcm.size, playbackTimeout)
                finished = true
                "Watch reported playback complete."
            }
        } catch (_: TimeoutCancellationException) {
            throw IllegalStateException("Audio transfer timed out. Keep Hermes open on the watch and install the matching PBW. Check Diagnostics for the last confirmed stage.")
        } finally {
            if (!finished) withContext(NonCancellable) {
                // No queued playback after cancellation; watch also expires incomplete clips.
                withTimeoutOrNull(2_000L) {
                    try { send(watch, request(WireMessageKind.AUDIO_CANCEL)) }
                    catch (error: CancellationException) { throw error }
                    catch (_: Exception) { }
                }
            }
        }
    }

    private suspend fun exchange(watch: String, request: WireMessage, expected: Int, received: Int,
        timeoutMillis: Long = replyTimeoutMillis) {
        val waiter = Pending(watch, request.captureId, CompletableDeferred())
        pending[request.transferId] = waiter
        try {
            check(send(watch, request)) { "Could not deliver audio. Open Hermes on the watch and check its connection in the Pebble phone app." }
            val reply = withTimeout(timeoutMillis) { waiter.reply.await() }
            check(reply.status == expected) {
                when (reply.status) {
                    QUIET_TIME -> "Quiet Time is on. Turn it off on the watch, then try again."
                    MUTED -> "The watch speaker is muted. Check Sounds & Haptics and Quiet Time on the watch."
                    BUSY -> "The watch speaker or dictation is busy. Finish that activity and try again."
                    INVALID -> "The watch rejected incomplete or damaged audio. Try again with the matching PBW."
                    CANCELLED -> "Watch audio playback was stopped. Keep Hermes open until the sound finishes."
                    STORAGE -> "The watch could not buffer the reply. Check its free storage and update its firmware."
                    else -> "The watch could not play the sound. Check its speaker support and firmware."
                }
            }
            check(reply.pageOffset == received.toLong() && reply.totalBytes == request.totalBytes &&
                reply.generation == request.generation) { "The watch audio receipt did not match the transferred clip." }
        } finally {
            pending.remove(request.transferId)
            waiter.reply.cancel()
        }
    }

    companion object {
        const val MAX_BYTES = 16_000
        const val MAX_REPLY_BYTES = 8_000 * VoicePcm.MAX_SECONDS
        const val FORMAT = 1 // 8 kHz, mono, signed 8-bit PCM; no WAV header.
        const val REPLY_FORMAT = 2 // Same PCM, fully cached before continuous playback.
        const val READY = 1
        const val BUFFERED = 2
        const val COMPLETE = 3
        const val MUTED = 4
        const val BUSY = 5
        const val INVALID = 6
        const val CANCELLED = 8
        const val QUIET_TIME = 9
        const val STORAGE = 10

        fun checksum(bytes: ByteArray): Long {
            var hash = 0x811c9dc5u
            bytes.forEach { hash = (hash xor it.toUByte().toUInt()) * 0x01000193u }
            return hash.toLong()
        }
    }
}
