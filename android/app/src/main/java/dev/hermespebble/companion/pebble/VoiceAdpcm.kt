package dev.hermespebble.companion.pebble

/** IMA ADPCM with independently decodable packets; PCM offsets count 8 kHz samples.
 * Reference: https://www.cs.columbia.edu/~hgs/audio/dvi/IMA_ADPCM.pdf
 * Header: signed LE16 initial sample, step index, reserved zero. Low nibble first.
 */
object VoiceAdpcm {
    const val SAMPLES_PER_BLOCK = 1529
    private val steps = intArrayOf(7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,
        66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,
        658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,
        3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,
        16818,18500,20350,22385,24623,27086,29794,32767)
    private val indices = intArrayOf(-1,-1,-1,-1,2,4,6,8)

    fun encode(pcm: ByteArray): List<ByteArray> {
        var index = 0
        return pcm.indices.step(SAMPLES_PER_BLOCK).map { offset ->
            val count = minOf(SAMPLES_PER_BLOCK, pcm.size - offset)
            var predictor = pcm[offset].toInt() * 256
            val packet = ByteArray(4 + count / 2)
            packet[0] = predictor.toByte()
            packet[1] = (predictor shr 8).toByte()
            packet[2] = index.toByte()
            for (i in 1 until count) {
                var difference = pcm[offset + i].toInt() * 256 - predictor
                var code = if (difference < 0) 8 else 0
                if (difference < 0) difference = -difference
                var step = steps[index]
                var delta = step shr 3
                var bit = 4
                while (bit != 0) {
                    if (difference >= step) { code = code or bit; difference -= step; delta += step }
                    step = step shr 1
                    bit = bit shr 1
                }
                predictor = (predictor + if (code and 8 != 0) -delta else delta).coerceIn(-32768, 32767)
                index = (index + indices[code and 7]).coerceIn(0, 88)
                val pos = 4 + (i - 1) / 2
                packet[pos] = (packet[pos].toInt() or (code shl (((i - 1) % 2) * 4))).toByte()
            }
            packet
        }
    }
}
