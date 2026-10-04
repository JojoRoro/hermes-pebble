package dev.hermespebble.companion.pebble

import android.content.Context
import android.os.Bundle
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import dev.hermespebble.companion.diagnostics.DiagnosticLog
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.Locale
import java.util.UUID
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout

/** Synthesize using an installed offline voice; never play on the phone. */
class WatchSpeech(private val context: Context) {
    suspend fun synthesize(text: String): ByteArray {
        val initialized = CompletableDeferred<Int>()
        var engine: TextToSpeech? = null
        var file: File? = null
        try {
            return withTimeout(45_000L) {
                withContext(Dispatchers.Main) {
                    engine = TextToSpeech(context.applicationContext) { initialized.complete(it) }
                }
                check(initialized.await() == TextToSpeech.SUCCESS) {
                    "Set up text-to-speech on your phone."
                }
                val tts = requireNotNull(engine)
                val preferred = tts.defaultVoice
                val locale = preferred?.locale ?: Locale.getDefault()
                val voice = preferred?.takeUnless { it.isNetworkConnectionRequired }
                    ?: tts.voices.orEmpty().filter { !it.isNetworkConnectionRequired && it.locale.language == locale.language }
                        .sortedBy { it.name }.firstOrNull()
                    ?: error("Install an offline speech voice on your phone.")
                check(tts.setVoice(voice) == TextToSpeech.SUCCESS) { "Phone speech voice is unavailable." }
                check(tts.setSpeechRate(1.5f) == TextToSpeech.SUCCESS &&
                    tts.setPitch(1.0f) == TextToSpeech.SUCCESS) { "Could not set speech speed." }
                val complete = CompletableDeferred<ByteArray>()
                val bytes = ByteArrayOutputStream()
                val id = UUID.randomUUID().toString()
                var rate = 0
                var channels = 0
                var encoding = 0
                check(tts.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
                    override fun onStart(utteranceId: String?) = Unit
                    override fun onBeginSynthesis(utteranceId: String?, sampleRateInHz: Int, audioFormat: Int, channelCount: Int) {
                        if (utteranceId != id) return
                        synchronized(bytes) { rate = sampleRateInHz; encoding = audioFormat; channels = channelCount }
                    }
                    override fun onAudioAvailable(utteranceId: String?, audio: ByteArray?) {
                        if (utteranceId != id || audio == null || complete.isCompleted) return
                        synchronized(bytes) {
                            if (audio.size > VoicePcm.MAX_SOURCE_BYTES - bytes.size()) {
                                complete.completeExceptionally(IllegalStateException("Speech audio exceeds the size limit."))
                            } else bytes.write(audio)
                        }
                    }
                    override fun onDone(utteranceId: String?) {
                        if (utteranceId != id || complete.isCompleted) return
                        synchronized(bytes) {
                            DiagnosticLog.record("Voice", "TTS PCM: $rate Hz, $channels channels, encoding $encoding, ${bytes.size()} bytes")
                            try { complete.complete(VoicePcm.convert(bytes.toByteArray(), rate, channels, encoding)) }
                            catch (error: Exception) { complete.completeExceptionally(error) }
                        }
                    }
                    @Deprecated("Required by Android")
                    override fun onError(utteranceId: String?) {
                        if (utteranceId == id) complete.completeExceptionally(IllegalStateException("Phone speech synthesis failed. Check the installed voice."))
                    }
                }) == TextToSpeech.SUCCESS) { "Could not start phone speech synthesis." }
                file = File.createTempFile("watch-voice-", ".wav", context.cacheDir)
                check(tts.synthesizeToFile(VoicePcm.spokenText(text), Bundle(), requireNotNull(file), id) == TextToSpeech.SUCCESS) {
                    "Phone speech synthesis could not start."
                }
                complete.await()
            }
        } finally {
            withContext(NonCancellable + Dispatchers.Main) { engine?.stop(); engine?.shutdown() }
            file?.delete()
        }
    }
}
