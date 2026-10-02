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

    suspend fun play(watch: String, pcm: ByteArray, replyCaptureId: Long = 0, progress: (String) -> Unit): String = mutex.withLock {
        require(pcm.isNotEmpty() && pcm.size <= MAX_BYTES) { "The audio clip exceeds the watch audio limit." }
        val session = nextId()
        val checksum = checksum(pcm)
        var finished = false
        fun request(kind: WireMessageKind, offset: Int = 0, payload: ByteArray = ByteArray(0)) = WireMessage(
            kind = kind, transferId = nextId(), captureId = session, generation = checksum,
            totalBytes = pcm.size.toLong(), pageOffset = offset.toLong(), flags = FORMAT, payload = payload, itemId = replyCaptureId,
        )
        try {
            withTimeout(90_000L) {
                progress("Checking the watch speaker…")
                exchange(watch, request(WireMessageKind.AUDIO_BEGIN), READY, 0)
                for (offset in pcm.indices.step(WireProtocol.MAX_TRANSFER_BYTES)) {
                    val end = minOf(offset + WireProtocol.MAX_TRANSFER_BYTES, pcm.size)
                    exchange(watch, request(WireMessageKind.AUDIO_BLOCK, offset, pcm.copyOfRange(offset, end)), BUFFERED, end)
                    progress("Sending sound: ${end * 100 / pcm.size}%")
                }
                progress("Waiting for watch playback…")
                exchange(watch, request(WireMessageKind.AUDIO_PLAY), COMPLETE, pcm.size)
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

    private suspend fun exchange(watch: String, request: WireMessage, expected: Int, received: Int) {
        val waiter = Pending(watch, request.captureId, CompletableDeferred())
        pending[request.transferId] = waiter
        try {
            check(send(watch, request)) { "Could not deliver audio. Open Hermes on the watch and check its connection in the Pebble phone app." }
            val reply = withTimeout(replyTimeoutMillis) { waiter.reply.await() }
            check(reply.status == expected) {
                when (reply.status) {
                    QUIET_TIME -> "Quiet Time is on. Turn it off on the watch, then try again."
                    MUTED -> "The watch speaker is muted. Check Sounds & Haptics and Quiet Time on the watch."
                    BUSY -> "The watch speaker or dictation is busy. Finish that activity and try again."
                    INVALID -> "The watch rejected incomplete or damaged audio. Try again with the matching PBW."
                    CANCELLED -> "Watch audio playback was stopped. Keep Hermes open until the sound finishes."
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
        const val FORMAT = 1 // 8 kHz, mono, signed 8-bit PCM; no WAV header.
        const val READY = 1
        const val BUFFERED = 2
        const val COMPLETE = 3
        const val MUTED = 4
        const val BUSY = 5
        const val INVALID = 6
        const val CANCELLED = 8
        const val QUIET_TIME = 9

        fun checksum(bytes: ByteArray): Long {
            var hash = 0x811c9dc5u
            bytes.forEach { hash = (hash xor it.toUByte().toUInt()) * 0x01000193u }
            return hash.toLong()
        }
    }
}
