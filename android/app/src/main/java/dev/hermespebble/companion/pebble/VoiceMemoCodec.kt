package dev.hermespebble.companion.pebble

import java.io.ByteArrayOutputStream
import java.util.zip.CRC32
import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.roundToInt
import kotlin.math.sign
import kotlin.math.sqrt
import kotlin.math.tanh

/** One AppMessage of a watch voice memo: [payload] belongs at [offset] of a [total]-byte HVM1 file. */
data class VoiceMemoBlock(
    val transferId: Long,
    val captureId: Long,
    val offset: Long,
    val total: Long,
    val checksum: Long,
    val payload: ByteArray,
) {
    val isFinal: Boolean get() = offset + payload.size == total
}

/**
 * A piece of a memo whose ADPCM left the watch while it was recorded (stream mode, kind 12, or
 * watch-flash mode through Data Logging): [payload] belongs at data [offset], counted from the
 * first ADPCM byte. The last piece carries [end] instead: the HVM1 header, data length and CRC.
 */
class VoiceMemoPart(val captureId: Long, val offset: Long, val payload: ByteArray, val end: MemoPartEnd? = null)

class MemoPartEnd(val header: ByteArray, val total: Long, val checksum: Long)

/** Loudness of a rendered memo for the diagnostic log: [speechDbfs] is the speech RMS before gain. */
data class MemoLevels(val speechDbfs: Double, val peakDbfs: Double, val gain: Float, val dcOffset: Int)

class RenderedMemo(val wav: ByteArray, val levels: MemoLevels)

/**
 * HVM1, written by the watch's memo_core.h: a 16-byte header ("HVM1", u16 sample rate,
 * u8 codec, u8 capture coverage percent (0 = not reported), u32 sample count, u32 watch
 * time) and 4-bit IMA ADPCM, low nibble first, starting from predictor 0 / step index 0.
 */
object VoiceMemoCodec {
    const val HEADER_BYTES = 16
    const val SAMPLE_RATE = 8000
    const val CODEC_IMA_ADPCM = 1
    /** Above the longest memo the watch makes (10 min in stream mode), but bounds a hostile sender. */
    const val MAX_BYTES = HEADER_BYTES + 610 * SAMPLE_RATE / 2
    /** Watch-flash Data Logging session: tag "HVL1", 240-byte items (see memo_watch.h). */
    const val LOG_TAG = 0x48564c31L
    const val LOG_ITEM_BYTES = 240
    private const val LOG_HEAD_BYTES = 16
    const val MAX_BLOCK_BYTES = 1024

    private val steps = intArrayOf(
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
    )
    private val indexSteps = intArrayOf(-1, -1, -1, -1, 2, 4, 6, 8)

    fun crc32(bytes: ByteArray): Long = CRC32().apply { update(bytes) }.value

    fun validateBlock(block: VoiceMemoBlock) {
        require(block.captureId in 1..UInt.MAX_VALUE.toLong()) { "Capture identifier is outside the protocol range" }
        require(block.total in (HEADER_BYTES + 1).toLong()..MAX_BYTES.toLong()) { "Memo size is out of range" }
        require(block.payload.isNotEmpty() && block.payload.size <= MAX_BLOCK_BYTES) { "Memo block size is invalid" }
        require(block.offset >= 0 && block.offset + block.payload.size <= block.total) { "Memo block is outside the memo" }
    }

    fun validatePart(part: VoiceMemoPart) {
        require(part.captureId in 1..UInt.MAX_VALUE.toLong()) { "Capture identifier is outside the protocol range" }
        require(part.payload.size <= MAX_BLOCK_BYTES) { "Memo piece is too large" }
        require(part.offset >= 0 && part.offset + part.payload.size <= MAX_BYTES - HEADER_BYTES) { "Memo piece is outside the memo" }
        val end = part.end
        if (end == null) {
            require(part.payload.isNotEmpty()) { "Memo piece is empty" }
        } else {
            require(end.header.size == HEADER_BYTES && end.total == part.offset && end.total > 0) { "Memo end is invalid" }
        }
    }

    /** One watch-flash Data Logging item: capture, data offset, type (0 data, 1 end), length, payload. */
    fun logPart(item: ByteArray): VoiceMemoPart {
        require(item.size == LOG_ITEM_BYTES) { "Memo log item has the wrong size" }
        val length = (item[10].toInt() and 0xff) or ((item[11].toInt() and 0xff) shl 8)
        require(length in 1..LOG_ITEM_BYTES - LOG_HEAD_BYTES) { "Memo log item length is invalid" }
        val captureId = u32(item, 0)
        val offset = u32(item, 4)
        val part = when (item[8].toInt()) {
            0 -> VoiceMemoPart(captureId, offset, item.copyOfRange(LOG_HEAD_BYTES, LOG_HEAD_BYTES + length))
            1 -> {
                require(length == HEADER_BYTES + 8) { "Memo log end has the wrong length" }
                val end = MemoPartEnd(item.copyOfRange(LOG_HEAD_BYTES, LOG_HEAD_BYTES + HEADER_BYTES),
                    total = u32(item, LOG_HEAD_BYTES + HEADER_BYTES + 4), checksum = u32(item, LOG_HEAD_BYTES + HEADER_BYTES))
                VoiceMemoPart(captureId, offset, ByteArray(0), end)
            }
            else -> throw IllegalArgumentException("Unknown memo log item type")
        }
        validatePart(part)
        return part
    }

    /** Decodes one 4-bit code; [state] holds predictor and step index between calls. */
    fun decodeNibble(state: IntArray, code: Int): Short {
        val step = steps[state[1]]
        var delta = step shr 3
        if (code and 4 != 0) delta += step
        if (code and 2 != 0) delta += step shr 1
        if (code and 1 != 0) delta += step shr 2
        state[0] = (if (code and 8 != 0) state[0] - delta else state[0] + delta).coerceIn(-32768, 32767)
        state[1] = (state[1] + indexSteps[code and 7]).coerceIn(0, 88)
        return state[0].toShort()
    }

    /** Header for a memo whose end never arrived (the watch app closed mid-way); coverage unknown. */
    fun headerFor(dataBytes: Int): ByteArray = ByteArray(HEADER_BYTES).also { b ->
        "HVM1".toByteArray().copyInto(b)
        b[4] = (SAMPLE_RATE and 0xff).toByte(); b[5] = (SAMPLE_RATE shr 8).toByte(); b[6] = CODEC_IMA_ADPCM.toByte()
        val samples = dataBytes.toLong() * 2
        repeat(4) { b[8 + it] = (samples shr (8 * it)).toByte() }
    }

    /** Decodes a complete HVM1 file to 16-bit PCM, or throws if the header and length disagree. */
    fun decode(file: ByteArray): ShortArray {
        require(file.size > HEADER_BYTES && file.size <= MAX_BYTES) { "Memo size is out of range" }
        require(file[0] == 'H'.code.toByte() && file[1] == 'V'.code.toByte() &&
            file[2] == 'M'.code.toByte() && file[3] == '1'.code.toByte()) { "Not an HVM1 memo" }
        val rate = (file[4].toInt() and 0xff) or ((file[5].toInt() and 0xff) shl 8)
        require(rate == SAMPLE_RATE) { "Unsupported memo sample rate $rate" }
        require(file[6].toInt() == CODEC_IMA_ADPCM) { "Unsupported memo codec" }
        val samples = u32(file, 8)
        require(samples > 0 && (samples + 1) / 2 == (file.size - HEADER_BYTES).toLong()) { "Memo length does not match its sample count" }
        val out = ShortArray(samples.toInt())
        var predictor = 0
        var index = 0
        for (i in out.indices) {
            val byte = file[HEADER_BYTES + i / 2].toInt() and 0xff
            val code = if (i % 2 == 0) byte and 0x0f else byte shr 4
            val step = steps[index]
            var delta = step shr 3
            if (code and 4 != 0) delta += step
            if (code and 2 != 0) delta += step shr 1
            if (code and 1 != 0) delta += step shr 2
            predictor = (if (code and 8 != 0) predictor - delta else predictor + delta).coerceIn(-32768, 32767)
            index = (index + indexSteps[code and 7]).coerceIn(0, 88)
            out[i] = predictor.toShort()
        }
        return out
    }

    /** Share of the recording time for which the watch received microphone audio, or null if not reported. */
    fun coveragePercent(file: ByteArray): Int? =
        if (file.size > HEADER_BYTES) (file[7].toInt() and 0xff).takeIf { it in 1..100 } else null

    /** Wraps decoded memo audio as a PCM WAV file; see [render]. */
    fun toWav(pcm: ShortArray, sampleRate: Int = SAMPLE_RATE): ByteArray = render(pcm, sampleRate).wav

    /**
     * Makes the memo listenable and wraps it as a 16-bit mono WAV of the same length.
     *
     * The watch microphone is quiet, while pressing and releasing the button makes clicks far
     * louder than speech. So: mute the first [LEAD_MUTE_MS] (start press and release) and last
     * [TAIL_MUTE_MS] (stop press) with short fades, remove DC and low rumble, and set one gain
     * from the speech level (roughly the 90th percentile of [WINDOW_MS] window RMS) towards [TARGET_RMS],
     * at most [MAX_GAIN]x. Windows that would still be far louder than speech, such as a click,
     * are turned down to [MAX_WINDOW_RMS] (and [MAX_WINDOW_PEAK] for clicks shorter than a
     * window); quiet pauses are gently lowered so the gain does
     * not bring up hiss; rare peaks are soft-limited instead of clipped.
     */
    fun render(pcm: ShortArray, sampleRate: Int = SAMPLE_RATE): RenderedMemo {
        val n = pcm.size
        var lead = sampleRate * LEAD_MUTE_MS / 1000
        var tail = sampleRate * TAIL_MUTE_MS / 1000
        var fade = sampleRate * FADE_MS / 1000
        if (n < lead + tail + 8 * fade) { lead = 0; tail = 0; fade = 0 } // Too short to trim.
        val end = n - tail

        var sum = 0.0
        for (i in lead until end) sum += pcm[i]
        val dc = if (end > lead) (sum / (end - lead)).toFloat() else 0f

        // Offset removal, then a high-pass near 75 Hz against drift and handling rumble. It starts
        // at the first kept sample, so the muted clicks never pass through it.
        val filtered = FloatArray(n)
        var previousIn = 0f
        var previousOut = 0f
        for (i in lead until end) {
            val x = pcm[i] - dc
            val y = x - previousIn + HIGH_PASS_POLE * previousOut
            previousIn = x
            previousOut = y
            filtered[i] = y * when {
                fade > 0 && i < lead + fade -> (i - lead + 1).toFloat() / fade
                fade > 0 && i >= end - fade -> (end - i).toFloat() / fade
                else -> 1f
            }
        }

        // Window RMS over the kept part.
        val window = maxOf(1, sampleRate * WINDOW_MS / 1000)
        val windows = maxOf(1, (end - lead + window - 1) / window)
        val windowPeak = FloatArray(windows)
        val rms = FloatArray(windows) { w ->
            val from = lead + w * window
            val to = minOf(end, from + window)
            var energy = 0.0
            for (i in from until to) {
                energy += filtered[i].toDouble() * filtered[i]
                windowPeak[w] = maxOf(windowPeak[w], abs(filtered[i]))
            }
            if (to > from) sqrt(energy / (to - from)).toFloat() else 0f
        }
        // Speech level: the 90th percentile of window RMS, but at least a third of the 98th, so a
        // memo that is mostly pause still takes its gain from the words, not from the hiss.
        val speech = rms.sortedArray().let { maxOf(it[((it.size - 1) * 0.9).toInt()], it[((it.size - 1) * 0.98).toInt()] / 3f) }
        var peak = 0f
        for (i in lead until end) peak = maxOf(peak, abs(filtered[i]))
        val gain = if (speech < 1f) 1f else minOf(TARGET_RMS / speech, MAX_GAIN)

        // Per-window gain: the overall gain, lowered for windows much louder than speech (a
        // neighbour's lower gain is taken too, so the reduction starts before a click), and
        // lowered gently for pauses.
        val own = FloatArray(windows) { w ->
            val r = rms[w]
            var g = gain
            if (r * g > MAX_WINDOW_RMS) g = MAX_WINDOW_RMS / r
            if (windowPeak[w] * g > MAX_WINDOW_PEAK) g = MAX_WINDOW_PEAK / windowPeak[w] // Part-window clicks.
            val quiet = speech / PAUSE_RATIO
            if (r < quiet && quiet > 0f) g *= maxOf(PAUSE_FLOOR, sqrt(r / quiet))
            g
        }
        val windowGain = FloatArray(windows) { w ->
            minOf(own[w], own[maxOf(0, w - 1)], own[minOf(windows - 1, w + 1)])
        }

        val data = ByteArray(n * 2)
        for (i in lead until end) {
            // Interpolate between window centres so the gain never steps.
            val position = (i - lead).toFloat() / window - 0.5f
            val w0 = position.toInt().coerceIn(0, windows - 1)
            val w1 = minOf(windows - 1, w0 + 1)
            val t = (position - w0).coerceIn(0f, 1f)
            val g = windowGain[w0] + (windowGain[w1] - windowGain[w0]) * t
            val v = limit(filtered[i] * g).toInt().coerceIn(-32768, 32767)
            data[2 * i] = v.toByte()
            data[2 * i + 1] = (v shr 8).toByte()
        }
        val out = ByteArrayOutputStream(44 + data.size)
        fun int(value: Int) { repeat(4) { out.write((value shr (8 * it)) and 0xff) } }
        fun short(value: Int) { repeat(2) { out.write((value shr (8 * it)) and 0xff) } }
        out.write("RIFF".toByteArray()); int(36 + data.size); out.write("WAVE".toByteArray())
        out.write("fmt ".toByteArray()); int(16); short(1); short(1); int(sampleRate); int(sampleRate * 2); short(2); short(16)
        out.write("data".toByteArray()); int(data.size); out.write(data)
        return RenderedMemo(out.toByteArray(), MemoLevels(dbfs(speech), dbfs(peak), gain, dc.roundToInt()))
    }

    private fun limit(x: Float): Float {
        val magnitude = abs(x)
        if (magnitude <= LIMIT_KNEE) return x
        val room = LIMIT_CEILING - LIMIT_KNEE
        return sign(x) * (LIMIT_KNEE + room * tanh((magnitude - LIMIT_KNEE) / room))
    }

    private fun dbfs(value: Float): Double = if (value < 1f) -96.0 else 20 * log10(value / 32768.0)

    private const val LEAD_MUTE_MS = 300
    private const val TAIL_MUTE_MS = 200
    private const val FADE_MS = 30
    private const val HIGH_PASS_POLE = 0.94f
    private const val WINDOW_MS = 20
    private const val TARGET_RMS = 6_000f
    private const val MAX_WINDOW_RMS = 9_000f
    private const val MAX_WINDOW_PEAK = 16_000f
    private const val MAX_GAIN = 128f
    private const val PAUSE_RATIO = 10f
    private const val PAUSE_FLOOR = 0.35f
    private const val LIMIT_KNEE = 24_000f
    private const val LIMIT_CEILING = 32_000f

    /** Duration of a 16-bit mono WAV produced by [toWav]. */
    fun wavDurationMillis(wavBytes: Long, sampleRate: Int = SAMPLE_RATE): Long =
        ((wavBytes - 44).coerceAtLeast(0) / 2) * 1000 / sampleRate

    private fun u32(bytes: ByteArray, offset: Int): Long =
        (0 until 4).fold(0L) { acc, i -> acc or ((bytes[offset + i].toLong() and 0xff) shl (8 * i)) }
}
