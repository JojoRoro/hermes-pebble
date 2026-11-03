package dev.hermespebble.companion.pebble

import java.io.ByteArrayOutputStream
import java.util.zip.CRC32
import kotlin.math.abs

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
 * HVM1, written by the watch's memo_core.h: a 16-byte header ("HVM1", u16 sample rate,
 * u8 codec, u8 reserved, u32 sample count, u32 watch time) and 4-bit IMA ADPCM, low
 * nibble first, starting from predictor 0 / step index 0.
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

    /**
     * Removes the microphone's DC offset and normalizes the peak to about -1 dBFS (at most 12x
     * gain, so silence is not amplified into noise), then wraps the result as a PCM WAV file.
     */
    fun toWav(pcm: ShortArray, sampleRate: Int = SAMPLE_RATE): ByteArray {
        val filtered = FloatArray(pcm.size)
        var previousIn = 0f
        var previousOut = 0f
        for (i in pcm.indices) {
            val x = pcm[i].toFloat()
            val y = x - previousIn + 0.995f * previousOut
            previousIn = x
            previousOut = y
            filtered[i] = y
        }
        val peak = filtered.maxOfOrNull { abs(it) } ?: 0f
        val gain = if (peak < 1f) 1f else minOf(29000f / peak, 12f)
        val data = ByteArray(pcm.size * 2)
        for (i in filtered.indices) {
            val v = (filtered[i] * gain).toInt().coerceIn(-32768, 32767)
            data[2 * i] = v.toByte()
            data[2 * i + 1] = (v shr 8).toByte()
        }
        val out = ByteArrayOutputStream(44 + data.size)
        fun int(value: Int) { repeat(4) { out.write((value shr (8 * it)) and 0xff) } }
        fun short(value: Int) { repeat(2) { out.write((value shr (8 * it)) and 0xff) } }
        out.write("RIFF".toByteArray()); int(36 + data.size); out.write("WAVE".toByteArray())
        out.write("fmt ".toByteArray()); int(16); short(1); short(1); int(sampleRate); int(sampleRate * 2); short(2); short(16)
        out.write("data".toByteArray()); int(data.size); out.write(data)
        return out.toByteArray()
    }

    /** Duration of a 16-bit mono WAV produced by [toWav]. */
    fun wavDurationMillis(wavBytes: Long, sampleRate: Int = SAMPLE_RATE): Long =
        ((wavBytes - 44).coerceAtLeast(0) / 2) * 1000 / sampleRate

    private fun u32(bytes: ByteArray, offset: Int): Long =
        (0 until 4).fold(0L) { acc, i -> acc or ((bytes[offset + i].toLong() and 0xff) shl (8 * i)) }
}
