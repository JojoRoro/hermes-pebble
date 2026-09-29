package dev.hermespebble.companion.pebble

import java.util.zip.CRC32

data class InkPoint(val x: Int, val y: Int)
data class InkCell(val strokes: List<List<InkPoint>>, val space: Boolean = false)
data class InkDrawing(val createdAt: Long, val cells: List<InkCell>)
data class InkBlock(val captureId: Long, val total: Int, val offset: Int, val checksum: Long, val bytes: ByteArray)

object InkCodec {
    const val MAX_BYTES = 2048
    const val HEADER_BYTES = 16
    const val BLOCK_BYTES = 192
    const val LOG_TAG = 0x48494e31L
    const val LOG_ITEM_BYTES = 212
    fun checksum(bytes: ByteArray): Long = CRC32().apply { update(bytes) }.value
    private fun u8(bytes: ByteArray, i: Int) = bytes[i].toInt() and 255
    private fun u16(bytes: ByteArray, i: Int) = u8(bytes, i) or (u8(bytes, i + 1) shl 8)
    private fun u32(bytes: ByteArray, i: Int) = (0..3).fold(0L) { n, b -> n or (u8(bytes, i + b).toLong() shl (8 * b)) }

    fun decode(bytes: ByteArray): InkDrawing {
        require(bytes.size in 20..MAX_BYTES && bytes.size % 2 == 0) { "Invalid handwriting size" }
        require(bytes.copyOfRange(0, 4).contentEquals("HIN1".toByteArray())) { "Unsupported handwriting format" }
        require(u8(bytes, 4) == 160 && u8(bytes, 5) == 144 && u16(bytes, 6) == bytes.size - HEADER_BYTES)
        require(u32(bytes, 12) == checksum(bytes.copyOfRange(HEADER_BYTES, bytes.size))) { "Handwriting checksum mismatch" }
        val cells = mutableListOf<InkCell>()
        val strokes = mutableListOf<List<InkPoint>>()
        var stroke = mutableListOf<InkPoint>()
        fun finishCell() {
            if (strokes.isNotEmpty()) { cells += InkCell(strokes.toList()); strokes.clear() }
        }
        for (i in HEADER_BYTES until bytes.size step 2) {
            val x = u8(bytes, i); val y = u8(bytes, i + 1)
            if (x == 255) {
                require(y in 0..2 && (stroke.isEmpty() || y == 0)) { "Invalid stroke boundary" }
                when (y) {
                    0 -> if (stroke.isNotEmpty()) { strokes += stroke; stroke = mutableListOf() }
                    1 -> finishCell()
                    2 -> { finishCell(); cells += InkCell(emptyList(), space = true) }
                }
            } else {
                require(x < 160 && y < 144) { "Handwriting point outside canvas" }
                stroke += InkPoint(x, y)
            }
        }
        require(stroke.isEmpty()) { "Unfinished handwriting stroke" }
        finishCell()
        require(cells.any { it.strokes.isNotEmpty() }) { "Empty handwriting" }
        return InkDrawing(u32(bytes, 8) * 1000L, cells)
    }

    fun validateBlock(block: InkBlock) {
        require(block.captureId in 1..UInt.MAX_VALUE.toLong())
        require(block.total in 20..MAX_BYTES && block.total % 2 == 0)
        require(block.offset in 0 until block.total && block.offset % BLOCK_BYTES == 0)
        require(block.bytes.size == minOf(BLOCK_BYTES, block.total - block.offset))
        require(block.checksum in 0..UInt.MAX_VALUE.toLong())
    }

    fun logBlock(record: ByteArray): InkBlock {
        require(record.size == LOG_ITEM_BYTES && record.copyOfRange(0, 4).contentEquals("IHC1".toByteArray()))
        val count = u16(record, 12)
        require(count in 1..BLOCK_BYTES && u16(record, 14) == 0)
        return InkBlock(u32(record, 4), u16(record, 8), u16(record, 10), u32(record, 16), record.copyOfRange(20, 20 + count))
            .also(::validateBlock)
    }

    /** Every block is independently durable; duplicates must have identical bytes. */
    fun merge(existing: ByteArray, mask: Int, block: InkBlock): Pair<ByteArray, Int> {
        validateBlock(block)
        require(existing.size == block.total)
        val bit = 1 shl (block.offset / BLOCK_BYTES)
        if (mask and bit != 0) require(existing.copyOfRange(block.offset, block.offset + block.bytes.size).contentEquals(block.bytes)) {
            "Conflicting handwriting block"
        }
        val next = existing.copyOf()
        block.bytes.copyInto(next, block.offset)
        val nextMask = mask or bit
        if (complete(block.total, nextMask)) {
            require(checksum(next) == block.checksum) { "Handwriting transfer checksum mismatch" }
            decode(next)
        }
        return next to nextMask
    }

    fun complete(total: Int, mask: Int): Boolean = mask == (1 shl ((total + BLOCK_BYTES - 1) / BLOCK_BYTES)) - 1
}
