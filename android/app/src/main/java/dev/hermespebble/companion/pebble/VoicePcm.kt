package dev.hermespebble.companion.pebble

import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.roundToInt

/** Converts Android TTS PCM to the watch's 8 kHz mono signed 8-bit format. */
object VoicePcm {
    const val MAX_TEXT = 400
    const val MAX_SECONDS = 60
    const val MAX_SOURCE_BYTES = 24 * 1024 * 1024

    fun spokenText(text: String): String {
        val plain = text.replace(Regex("[`#*]"), "").replace(Regex("\\s+"), " ").trim()
        if (plain.codePointCount(0, plain.length) <= MAX_TEXT) return plain
        val prefix = plain.substring(0, plain.offsetByCodePoints(0, MAX_TEXT))
        return prefix.substringBeforeLast(' ', prefix) + ". Read the rest on your watch."
    }

    fun convert(bytes: ByteArray, rate: Int, channels: Int, encoding: Int): ByteArray {
        require(rate in 8_000..192_000 && channels in 1..2) { "Unsupported speech audio format." }
        val width = when (encoding) { 3 -> 1; 2 -> 2; 4 -> 4; else -> error("Unsupported speech PCM encoding.") }
        val frameSize = width * channels
        require(bytes.isNotEmpty() && bytes.size <= MAX_SOURCE_BYTES && bytes.size % frameSize == 0) {
            "Speech engine returned invalid audio."
        }
        val frames = bytes.size / frameSize
        require(frames.toLong() <= rate.toLong() * MAX_SECONDS) { "Spoken reply exceeds one minute." }
        val source = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        fun sample(frame: Int): Float {
            var sum = 0f
            repeat(channels) { channel ->
                val offset = frame * frameSize + channel * width
                sum += when (encoding) {
                    3 -> ((bytes[offset].toInt() and 255) - 128) / 128f
                    2 -> source.getShort(offset) / 32768f
                    else -> source.getFloat(offset).let { if (it.isFinite()) it.coerceIn(-1f, 1f) else 0f }
                }
            }
            return sum / channels
        }
        // Average each source interval to reduce aliasing when downsampling.
        val count = (frames.toLong() * 8_000 / rate).toInt()
        require(count > 0) { "Speech engine returned empty audio." }
        return ByteArray(count) { index ->
            val first = (index.toLong() * rate / 8_000).toInt()
            val end = ((index + 1L) * rate / 8_000).toInt().coerceAtMost(frames)
            var sum = 0f
            for (frame in first until end) sum += sample(frame)
            (sum / (end - first) * 128f).roundToInt().coerceIn(-128, 127).toByte()
        }
    }
}
