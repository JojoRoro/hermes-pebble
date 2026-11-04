package dev.hermespebble.companion.pebble

import java.io.ByteArrayOutputStream
import java.util.zip.CRC32
import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.roundToInt
import kotlin.math.sign
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

/** Loudness of a rendered memo for the diagnostic log: [speechDbfs] is the 99.5th percentile. */
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
    /** Far above what the watch can hold (about 30 s), but bounds a hostile sender. */
    const val MAX_BYTES = HEADER_BYTES + 180 * SAMPLE_RATE / 2
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
     * The watch microphone is quiet, and its first frames carry a start-up transient while the
     * stop press adds a click at the end. Scaling to the single loudest sample let those
     * transients set the gain and left speech near the noise floor. Instead: mute the first
     * [LEAD_MUTE_MS] and last [TAIL_MUTE_MS] with short fades, remove the DC offset, set the gain
     * so the 99.5th percentile of the remaining audio reaches [TARGET_LEVEL] (at most
     * [MAX_GAIN]x), and soft-limit the rare peaks above [LIMIT_KNEE] instead of clipping them.
     */
    fun render(pcm: ShortArray, sampleRate: Int = SAMPLE_RATE): RenderedMemo {
        val n = pcm.size
        var lead = sampleRate * LEAD_MUTE_MS / 1000
        var tail = sampleRate * TAIL_MUTE_MS / 1000
        var fade = sampleRate * FADE_MS / 1000
        if (n < lead + tail + 4 * fade) { lead = 0; tail = 0; fade = 0 } // Too short to trim.
        val end = n - tail

        var sum = 0.0
        for (i in lead until end) sum += pcm[i]
        val dc = if (end > lead) (sum / (end - lead)).toFloat() else 0f

        // Remove the offset, then a gentle high-pass for drift. Starting at the first kept sample
        // means the muted transient never passes through the filter.
        val filtered = FloatArray(n)
        var previousIn = 0f
        var previousOut = 0f
        for (i in lead until end) {
            val x = pcm[i] - dc
            val y = x - previousIn + 0.995f * previousOut
            previousIn = x
            previousOut = y
            filtered[i] = y * when {
                fade > 0 && i < lead + fade -> (i - lead + 1).toFloat() / fade
                fade > 0 && i >= end - fade -> (end - i).toFloat() / fade
                else -> 1f
            }
        }

        val magnitudes = FloatArray(end - lead) { abs(filtered[lead + it]) }
        magnitudes.sort()
        val level = if (magnitudes.isEmpty()) 0f else magnitudes[((magnitudes.size - 1) * 0.995).toInt()]
        val peak = magnitudes.lastOrNull() ?: 0f
        val gain = if (level < 1f) 1f else minOf(TARGET_LEVEL / level, MAX_GAIN)

        val data = ByteArray(n * 2)
        for (i in 0 until n) {
            val v = limit(filtered[i] * gain).toInt().coerceIn(-32768, 32767)
            data[2 * i] = v.toByte()
            data[2 * i + 1] = (v shr 8).toByte()
        }
        val out = ByteArrayOutputStream(44 + data.size)
        fun int(value: Int) { repeat(4) { out.write((value shr (8 * it)) and 0xff) } }
        fun short(value: Int) { repeat(2) { out.write((value shr (8 * it)) and 0xff) } }
        out.write("RIFF".toByteArray()); int(36 + data.size); out.write("WAVE".toByteArray())
        out.write("fmt ".toByteArray()); int(16); short(1); short(1); int(sampleRate); int(sampleRate * 2); short(2); short(16)
        out.write("data".toByteArray()); int(data.size); out.write(data)
        return RenderedMemo(out.toByteArray(), MemoLevels(dbfs(level), dbfs(peak), gain, dc.roundToInt()))
    }

    private fun limit(x: Float): Float {
        val magnitude = abs(x)
        if (magnitude <= LIMIT_KNEE) return x
        val room = LIMIT_CEILING - LIMIT_KNEE
        return sign(x) * (LIMIT_KNEE + room * tanh((magnitude - LIMIT_KNEE) / room))
    }

    private fun dbfs(value: Float): Double = if (value < 1f) -96.0 else 20 * log10(value / 32768.0)

    private const val LEAD_MUTE_MS = 100
    private const val TAIL_MUTE_MS = 80
    private const val FADE_MS = 20
    private const val TARGET_LEVEL = 12_000f
    private const val MAX_GAIN = 32f
    private const val LIMIT_KNEE = 24_000f
    private const val LIMIT_CEILING = 32_000f

    /** Duration of a 16-bit mono WAV produced by [toWav]. */
    fun wavDurationMillis(wavBytes: Long, sampleRate: Int = SAMPLE_RATE): Long =
        ((wavBytes - 44).coerceAtLeast(0) / 2) * 1000 / sampleRate

    private fun u32(bytes: ByteArray, offset: Int): Long =
        (0 until 4).fold(0L) { acc, i -> acc or ((bytes[offset + i].toLong() and 0xff) shl (8 * i)) }
}
