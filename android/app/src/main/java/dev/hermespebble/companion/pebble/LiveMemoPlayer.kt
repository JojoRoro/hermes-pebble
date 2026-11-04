package dev.hermespebble.companion.pebble

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.SystemClock
import dev.hermespebble.companion.diagnostics.DiagnosticLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * Plays a stream-mode memo on the phone speaker while it is being recorded. One memo plays at a
 * time; the saved WAV is produced separately by the repository, so playback problems never
 * affect what is kept. Writes never block the Pebble listener: if the speaker falls behind by
 * more than the track buffer, the oldest audio is simply not played.
 */
class LiveMemoPlayer(
    private val scope: CoroutineScope,
    private val enabled: suspend () -> Boolean,
) {
    private class Session(val key: String, val track: AudioTrack, val stream: LiveMemoStream) {
        var written = 0L
        var playing = false
        var lastData = SystemClock.elapsedRealtime()
    }

    private val lock = Any()
    private var session: Session? = null
    private var watchdog: Job? = null

    suspend fun onPart(watch: String, part: VoiceMemoPart) {
        val key = "$watch/${part.captureId}"
        val current = synchronized(lock) { session?.takeIf { it.key == key } }
        if (part.end != null) {
            current?.let { finish(it) }
            return
        }
        val active = current ?: (if (part.offset == 0L && enabled()) start(key) else null) ?: return
        synchronized(lock) {
            if (session !== active) return
            active.lastData = SystemClock.elapsedRealtime()
            write(active, active.stream.feed(part.offset, part.payload))
        }
    }

    private fun start(key: String): Session? = synchronized(lock) {
        session?.let { stopNow(it) }
        val rate = VoiceMemoCodec.SAMPLE_RATE
        val minimum = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val track = try {
            AudioTrack.Builder()
                .setAudioAttributes(AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build())
                .setAudioFormat(AudioFormat.Builder()
                    .setSampleRate(rate)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build())
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setBufferSizeInBytes(maxOf(minimum, rate * 2 * BUFFER_SECONDS))
                .build()
        } catch (error: Exception) {
            DiagnosticLog.record("Memo", "Live playback unavailable: ${error.javaClass.simpleName}")
            return null
        }
        DiagnosticLog.record("Memo", "Live playback started")
        Session(key, track, LiveMemoStream()).also {
            session = it
            startWatchdog()
        }
    }

    private fun write(active: Session, pcm: ShortArray) {
        if (pcm.isEmpty()) return
        val accepted = active.track.write(pcm, 0, pcm.size, AudioTrack.WRITE_NON_BLOCKING)
        if (accepted > 0) active.written += accepted
        // Start once a little audio is queued, so Bluetooth jitter does not cause gaps at once.
        if (!active.playing && active.written >= VoiceMemoCodec.SAMPLE_RATE * PREBUFFER_MS / 1000) {
            active.track.play()
            active.playing = true
        }
    }

    private fun finish(active: Session) {
        synchronized(lock) { if (session !== active) return; session = null }
        write(active, active.stream.flush())
        if (!active.playing && active.written > 0) { active.track.play(); active.playing = true }
        // In streaming mode stop() plays out what is queued first; release once that has played.
        runCatching { active.track.stop() }
        val track = active.track
        scope.launch {
            delay(BUFFER_SECONDS * 1000L + 500L)
            runCatching { track.release() }
        }
    }

    private fun stopNow(active: Session) {
        runCatching { active.track.pause(); active.track.flush(); active.track.release() }
        if (session === active) session = null
    }

    /** Ends playback when a stream stops without its end message (watch closed, link lost). */
    private fun startWatchdog() {
        watchdog?.cancel()
        watchdog = scope.launch {
            while (true) {
                delay(1_000L)
                val idle = synchronized(lock) {
                    val active = session ?: return@launch
                    if (SystemClock.elapsedRealtime() - active.lastData > IDLE_TIMEOUT_MS) active else null
                }
                if (idle != null) {
                    finish(idle)
                    return@launch
                }
            }
        }
    }

    private companion object {
        const val BUFFER_SECONDS = 4
        const val PREBUFFER_MS = 500
        const val IDLE_TIMEOUT_MS = 6_000L
    }
}
