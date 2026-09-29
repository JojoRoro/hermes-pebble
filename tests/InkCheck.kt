package dev.hermespebble.companion.pebble

import io.rebble.pebblekit2.common.model.PebbleDictionaryItem

private fun rejected(block: () -> Unit) { check(runCatching(block).isFailure) }
fun main(args: Array<String>) {
    check(InkCodec.checksum("123456789".toByteArray()) == 0xcbf43926L)
    fun note(size: Int): ByteArray {
        val b = ByteArray(size)
        "HIN1".toByteArray().copyInto(b); b[4] = 160.toByte(); b[5] = 144.toByte()
        b[6] = (size - 16).toByte(); b[7] = ((size - 16) shr 8).toByte()
        for (i in 16 until size step 2) { b[i] = (i % 160).toByte(); b[i + 1] = 130.toByte() }
        b[size - 2] = 255.toByte(); b[size - 1] = 0
        val crc = InkCodec.checksum(b.copyOfRange(16, size))
        repeat(4) { b[12 + it] = (crc shr (it * 8)).toByte() }
        return b
    }
    if (args.isNotEmpty()) {
        val cFixture = InkCodec.decode(java.io.File(args[0]).readBytes())
        check(cFixture.cells.size == 1 && cFixture.cells.single().strokes.size == 2)
        check(cFixture.cells.single().strokes.first() == listOf(InkPoint(20, 20), InkPoint(130, 130)))
        check(cFixture.createdAt == 1700000000000L)
    }
    val source = note(InkCodec.MAX_BYTES)
    val checksum = InkCodec.checksum(source)
    val blocks = source.toList().chunked(192).mapIndexed { i, bytes -> InkBlock(5, source.size, i * 192, checksum, bytes.toByteArray()) }
    var received = ByteArray(source.size); var mask = 0
    // Reordered datalog delivery, repeated blocks and process recovery from saved state.
    for (block in blocks.reversed()) {
        val merged = InkCodec.merge(received, mask, block); received = merged.first; mask = merged.second
        val duplicate = InkCodec.merge(received.copyOf(), mask, block)
        check(duplicate.second == mask && duplicate.first.contentEquals(received))
    }
    check(InkCodec.complete(source.size, mask) && received.contentEquals(source))
    rejected { InkCodec.merge(received, mask, blocks[0].copy(bytes = blocks[0].bytes.copyOf().also { it[0]++ })) }
    rejected { InkCodec.validateBlock(blocks[0].copy(offset = 1)) }
    rejected { InkCodec.validateBlock(blocks[0].copy(total = 4096)) }
    rejected { InkCodec.decode(source.copyOf().also { it[25]++ }) }
    rejected { InkCodec.decode(source.copyOf(2047)) }
    rejected { InkCodec.merge(received, mask, blocks[0].copy(checksum = 1)) }
    val binaryMessage = OutgoingProtocolCodec.encode(WireMessageKind.INK_BLOCK, 700, captureId = 5,
        pageOffset = 0, totalBytes = source.size.toLong(), generation = checksum).single() +
        (WireProtocol.KEY_PAYLOAD to PebbleDictionaryItem.Bytes(blocks[0].bytes))
    val result = IncomingTransferAssembler().accept(binaryMessage) as DecodedChunkResult.Complete
    check(result.message.payload.contentEquals(blocks[0].bytes))
    val textMessage = binaryMessage + (WireProtocol.KEY_MESSAGE_KIND to PebbleDictionaryItem.UInt8(WireMessageKind.SAVE_NOTE.value.toUByte()))
    check(IncomingTransferAssembler().accept(textMessage) is DecodedChunkResult.Error)
    val record = ByteArray(InkCodec.LOG_ITEM_BYTES)
    "IHC1".toByteArray().copyInto(record); record[4] = 5; record[9] = 8; record[12] = 192.toByte()
    repeat(4) { record[16 + it] = (checksum shr (it * 8)).toByte() }
    blocks[0].bytes.copyInto(record, 20)
    check(InkCodec.logBlock(record).bytes.contentEquals(blocks[0].bytes))
    rejected { InkCodec.logBlock(record.copyOf(211)) }
    println("Ink binary format, C interoperability, durable reassembly, duplicates and bounds checks passed")
}
