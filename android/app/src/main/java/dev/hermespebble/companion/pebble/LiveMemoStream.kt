package dev.hermespebble.companion.pebble

import kotlin.math.abs
import kotlin.math.sign
import kotlin.math.sqrt
import kotlin.math.tanh

/**
 * Turns a stream-mode memo's ADPCM into playable PCM as it arrives. Unlike
 * [VoiceMemoCodec.render] it cannot look at the whole memo, so the level is tracked as it goes:
 * the first [LEAD_MUTE_MS] are muted (button press and release), DC and rumble are removed, and
 * each 20 ms window gets a gain from the speech level of the last [HISTORY_WINDOWS] windows,
 * measured as the saved memo is (90th percentile of window RMS, at least a third of the 98th).
 * Clicks are too short to move that level, and a window far louder than it is turned down.
 * Pure Kotlin, so host checks can run it.
 */
class LiveMemoStream(private val sampleRate: Int = VoiceMemoCodec.SAMPLE_RATE) {
    private val adpcm = IntArray(2)
    private var nextOffset = 0L
    private var produced = 0L
    private var previousIn = 0f
    private var previousOut = 0f
    private val history = FloatArray(HISTORY_WINDOWS)
    private var heard = 0
    private var gain = 1f
    private val window = FloatArray(sampleRate * WINDOW_MS / 1000)
    private var filled = 0

    /** Data bytes decoded so far; pieces must arrive in order (repeats are skipped). */
    val decodedBytes: Long get() = nextOffset

    /** Decodes [payload] at data [offset] and returns the PCM for every completed 20 ms window. */
    fun feed(offset: Long, payload: ByteArray): ShortArray {
        val skip = (nextOffset - offset).coerceAtLeast(0)
        if (skip >= payload.size) return ShortArray(0)
        val out = ShortArrayBuilder()
        for (i in skip.toInt() until payload.size) {
            val byte = payload[i].toInt() and 0xff
            push(VoiceMemoCodec.decodeNibble(adpcm, byte and 0x0f), out)
            push(VoiceMemoCodec.decodeNibble(adpcm, byte shr 4), out)
        }
        nextOffset = maxOf(nextOffset, offset) + payload.size - skip
        return out.toArray()
    }

    /** Whatever is left in the current window, at the current gain. */
    fun flush(): ShortArray {
        val out = ShortArrayBuilder()
        if (filled > 0) emitWindow(out)
        return out.toArray()
    }

    private fun push(sample: Short, out: ShortArrayBuilder) {
        val x = sample.toFloat()
        val y = x - previousIn + 0.94f * previousOut
        previousIn = x
        previousOut = y
        val muted = produced < sampleRate.toLong() * LEAD_MUTE_MS / 1000
        produced++
        window[filled++] = if (muted) 0f else y
        if (filled == window.size) emitWindow(out)
    }

    private fun emitWindow(out: ShortArrayBuilder) {
        var energy = 0.0
        var peak = 0f
        for (i in 0 until filled) {
            energy += window[i].toDouble() * window[i]
            peak = maxOf(peak, abs(window[i]))
        }
        val rms = sqrt(energy / filled).toFloat()
        if (rms > 0f) history[heard++ % HISTORY_WINDOWS] = rms // Muted windows do not count.
        val level = speechLevel()
        val target = (TARGET_RMS / maxOf(level, LEVEL_FLOOR)).coerceIn(1f, MAX_GAIN)
        gain += (target - gain) * GAIN_SMOOTHING
        var g = gain
        if (rms * g > MAX_WINDOW_RMS) g = MAX_WINDOW_RMS / rms
        if (peak * g > MAX_WINDOW_PEAK) g = MAX_WINDOW_PEAK / peak // A click that only fills part of a window.
        val quiet = level / PAUSE_RATIO
        if (rms < quiet && quiet > 0f) g *= maxOf(PAUSE_FLOOR, sqrt(rms / quiet))
        for (i in 0 until filled) out.add(limit(window[i] * g).toInt().coerceIn(-32768, 32767).toShort())
        filled = 0
    }

    private fun speechLevel(): Float {
        val count = minOf(heard, HISTORY_WINDOWS)
        if (count == 0) return 0f
        val sorted = history.copyOf(count).also { it.sort() }
        return maxOf(sorted[((count - 1) * 0.9).toInt()], sorted[((count - 1) * 0.98).toInt()] / 3f)
    }

    private fun limit(x: Float): Float {
        val magnitude = abs(x)
        if (magnitude <= KNEE) return x
        return sign(x) * (KNEE + (CEILING - KNEE) * tanh((magnitude - KNEE) / (CEILING - KNEE)))
    }

    private class ShortArrayBuilder {
        private var data = ShortArray(512)
        private var size = 0
        fun add(value: Short) {
            if (size == data.size) data = data.copyOf(size * 2)
            data[size++] = value
        }
        fun toArray(): ShortArray = data.copyOf(size)
    }

    private companion object {
        const val LEAD_MUTE_MS = 300
        const val WINDOW_MS = 20
        const val TARGET_RMS = 6_000f
        const val MAX_WINDOW_RMS = 9_000f
        const val MAX_WINDOW_PEAK = 16_000f
        const val MAX_GAIN = 128f
        const val LEVEL_FLOOR = 50f
        const val HISTORY_WINDOWS = 250 // 5 s
        const val GAIN_SMOOTHING = 0.3f
        const val PAUSE_RATIO = 10f
        const val PAUSE_FLOOR = 0.35f
        const val KNEE = 24_000f
        const val CEILING = 32_000f
    }
}
