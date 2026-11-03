package dev.hermespebble.companion.pebble

import dev.hermespebble.companion.data.local.MemoAcceptResult
import dev.hermespebble.companion.data.local.VoiceMemoRepository
import io.rebble.pebblekit2.common.model.PebbleDictionaryItem
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.sin

/** Encodes like memo_core.h (without its 16 kHz filter) so the check needs no fixture. */
private fun encode(pcm: ShortArray): ByteArray {
    val steps = intArrayOf(
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
    )
    val indexSteps = intArrayOf(-1, -1, -1, -1, 2, 4, 6, 8)
    val out = ByteArray(16 + (pcm.size + 1) / 2)
    "HVM1".toByteArray().copyInto(out); out[4] = 0x40; out[5] = 0x1f; out[6] = 1
    repeat(4) { out[8 + it] = (pcm.size shr (8 * it)).toByte() }
    var predictor = 0; var index = 0
    pcm.forEachIndexed { i, sample ->
        var step = steps[index]; var diff = sample - predictor; var delta = step shr 3; var code = 0
        if (diff < 0) { code = 8; diff = -diff }
        if (diff >= step) { code = code or 4; diff -= step; delta += step }; step = step shr 1
        if (diff >= step) { code = code or 2; diff -= step; delta += step }; step = step shr 1
        if (diff >= step) { code = code or 1; delta += step }
        predictor = (if (code and 8 != 0) predictor - delta else predictor + delta).coerceIn(-32768, 32767)
        index = (index + indexSteps[code and 7]).coerceIn(0, 88)
        val at = 16 + i / 2
        out[at] = (if (i % 2 == 0) code else (out[at].toInt() and 0x0f) or (code shl 4)).toByte()
    }
    return out
}

private fun blocks(memo: ByteArray, capture: Long, size: Int = 768): List<VoiceMemoBlock> {
    val crc = VoiceMemoCodec.crc32(memo)
    return memo.indices.step(size).mapIndexed { n, offset ->
        VoiceMemoBlock(1000L + n, capture, offset.toLong(), memo.size.toLong(), crc,
            memo.copyOfRange(offset, minOf(offset + size, memo.size)))
    }
}

private fun rejected(block: () -> Unit) { check(runCatching(block).isFailure) }

fun main(args: Array<String>) {
    check(VoiceMemoCodec.crc32("123456789".toByteArray()) == 0xcbf43926L)

    // A memo produced by the C encoder decodes to a clean 440 Hz tone.
    if (args.isNotEmpty()) {
        val fixture = java.io.File(args[0]).readBytes()
        val pcm = VoiceMemoCodec.decode(fixture)
        check(pcm.size in 11_990..12_002 && (pcm.size + 1) / 2 + 16 == fixture.size)
        // Zero crossings over the settled part give the frequency.
        val crossings = (401 until pcm.size).count { (pcm[it - 1] < 0) != (pcm[it] < 0) }
        val hz = crossings / 2.0 / ((pcm.size - 401) / 8000.0)
        check(abs(hz - 440.0) < 5.0) { "C fixture decoded at $hz Hz" }
        check(pcm.drop(400).maxOf { abs(it.toInt()) } in 9_000..14_000)
    }

    // Round trip through the Kotlin encoder model, and the WAV writer.
    val tone = ShortArray(8001) { (8000 * sin(2 * PI * 300 * it / 8000.0) + 1500).toInt().toShort() }
    val memo = encode(tone)
    val decoded = VoiceMemoCodec.decode(memo)
    check(decoded.size == tone.size)
    val error = tone.indices.drop(200).maxOf { abs(tone[it] - decoded[it]) }
    check(error < 1200) { "ADPCM error $error" }
    val wav = VoiceMemoCodec.toWav(decoded)
    check(String(wav, 0, 4) == "RIFF" && String(wav, 8, 4) == "WAVE" && wav.size == 44 + 2 * tone.size)
    check(VoiceMemoCodec.wavDurationMillis(wav.size.toLong()) == 1000L)
    val samples = (0 until tone.size).map { ((wav[44 + 2 * it].toInt() and 0xff) or (wav[45 + 2 * it].toInt() shl 8)) }
    check(samples.maxOf { abs(it) } in 26_000..32_767) // Normalized near full scale.
    check(abs(samples.drop(4000).average()) < 400) // DC offset removed.
    check(VoiceMemoCodec.toWav(ShortArray(100)).size == 244) // Silence: no division by zero.

    // Header validation.
    rejected { VoiceMemoCodec.decode(memo.copyOf(memo.size - 1)) }
    rejected { VoiceMemoCodec.decode(memo.copyOf().also { it[0] = 'X'.code.toByte() }) }
    rejected { VoiceMemoCodec.decode(memo.copyOf().also { it[4] = 0x7d }) }
    rejected { VoiceMemoCodec.decode(memo.copyOf().also { it[6] = 2 }) }
    rejected { VoiceMemoCodec.validateBlock(VoiceMemoBlock(1, 0, 0, 100, 1, ByteArray(10))) }
    rejected { VoiceMemoCodec.validateBlock(VoiceMemoBlock(1, 5, 95, 100, 1, ByteArray(10))) }
    rejected { VoiceMemoCodec.validateBlock(VoiceMemoBlock(1, 5, 0, VoiceMemoCodec.MAX_BYTES + 1L, 1, ByteArray(10))) }
    rejected { VoiceMemoCodec.validateBlock(VoiceMemoBlock(1, 5, 0, 5000, 1, ByteArray(1025))) }

    // Dictionary decoding bypasses the 192-byte text chunk limit.
    val parts = blocks(memo, 77)
    fun dictionary(block: VoiceMemoBlock, kind: Int = WireMessageKind.MEMO_BLOCK.value) = mapOf(
        WireProtocol.KEY_PROTOCOL_VERSION to PebbleDictionaryItem.UInt8(1u),
        WireProtocol.KEY_MESSAGE_KIND to PebbleDictionaryItem.UInt8(kind.toUByte()),
        WireProtocol.KEY_TRANSFER_ID to PebbleDictionaryItem.UInt32(block.transferId.toUInt()),
        WireProtocol.KEY_CAPTURE_ID to PebbleDictionaryItem.UInt32(block.captureId.toUInt()),
        WireProtocol.KEY_PAGE_OFFSET to PebbleDictionaryItem.UInt32(block.offset.toUInt()),
        WireProtocol.KEY_TOTAL_BYTES to PebbleDictionaryItem.UInt32(block.total.toUInt()),
        WireProtocol.KEY_CONVERSATION_GENERATION to PebbleDictionaryItem.UInt32(block.checksum.toUInt()),
        WireProtocol.KEY_FLAGS to PebbleDictionaryItem.UInt8(if (block.isFinal) 0u else 1u),
        WireProtocol.KEY_PAYLOAD to PebbleDictionaryItem.Bytes(block.payload),
    )
    val decodedBlock = (decodeMemoBlock(dictionary(parts[0])) as MemoBlockDecode.Block).block
    check(decodedBlock.payload.contentEquals(parts[0].payload) && decodedBlock.payload.size == 768 && decodedBlock.checksum == parts[0].checksum)
    check(decodeMemoBlock(dictionary(parts[0], WireMessageKind.INK_BLOCK.value)) == MemoBlockDecode.NotMemo)
    check(decodeMemoBlock(dictionary(parts[0]) - WireProtocol.KEY_PAYLOAD) is MemoBlockDecode.Invalid)
    check(decodeMemoBlock(dictionary(parts[0].copy(captureId = 0))) is MemoBlockDecode.Invalid)

    // Repository: in-order append, duplicates, gaps, conflicts, completion, idempotent finals, delete.
    val directory = java.nio.file.Files.createTempDirectory("memo-check").toFile()
    try {
        var now = 1_700_000_000_000L
        val repository = VoiceMemoRepository(directory) { now }
        check(repository.memos.value.isEmpty())
        rejected { repository.accept(" ", parts[0]) }
        check(repository.accept("watch", parts[1]) is MemoAcceptResult.Rejected) // Nothing to continue.
        check(repository.accept("watch", parts[0]) == MemoAcceptResult.Accepted)
        check(repository.accept("watch", parts[0]) == MemoAcceptResult.Accepted) // Resent block.
        check(repository.accept("watch", parts[2]) is MemoAcceptResult.Rejected) // Gap.
        check(repository.accept("watch", parts[0].copy(payload = parts[0].payload.copyOf().also { it[20] = 9 })) == MemoAcceptResult.Accepted) // Restart from zero.
        check(repository.accept("watch", parts[0]) == MemoAcceptResult.Accepted) // Restart again with the real bytes.
        check(repository.accept("watch", parts[1].copy(payload = parts[1].payload.copyOf().also { it[0] = (it[0] + 1).toByte() })) == MemoAcceptResult.Accepted)
        check(repository.accept("watch", parts[1]) is MemoAcceptResult.Rejected) // Conflicting resend.
        check(repository.accept("watch", parts[0]) == MemoAcceptResult.Accepted)
        for (part in parts.drop(1).dropLast(1)) check(repository.accept("watch", part) == MemoAcceptResult.Accepted)
        val complete = repository.accept("watch", parts.last()) as MemoAcceptResult.Complete
        check(!complete.duplicate && complete.memo.durationMillis == 1000L && complete.memo.receivedAt == now)
        check(complete.memo.file.readBytes().contentEquals(VoiceMemoCodec.toWav(VoiceMemoCodec.decode(memo))))
        check(repository.memos.value.single().id == complete.memo.id)
        check(java.io.File(directory, "partial").listFiles().orEmpty().isEmpty())
        // Resent blocks after completion: ACK, and the final one repeats the receipt.
        check(repository.accept("watch", parts[0]) == MemoAcceptResult.Accepted)
        check((repository.accept("watch", parts.last()) as MemoAcceptResult.Complete).duplicate)
        check(repository.memos.value.size == 1)
        // The same capture from another watch is a separate memo.
        now += 1000
        for (part in parts) repository.accept("other watch", part)
        check(repository.memos.value.size == 2 && repository.memos.value.first().receivedAt == now)

        // Wrong checksum: discarded and reported, never listed.
        val corrupt = blocks(memo, 78).map { it.copy(checksum = it.checksum xor 1) }
        corrupt.dropLast(1).forEach { check(repository.accept("watch", it) == MemoAcceptResult.Accepted) }
        check(repository.accept("watch", corrupt.last()) is MemoAcceptResult.Corrupt)
        // Right checksum but an inconsistent header.
        val badHeader = memo.copyOf().also { it[8] = (it[8] + 2).toByte() }
        check(blocks(badHeader, 79).map { repository.accept("watch", it) }.last() is MemoAcceptResult.Corrupt)
        check(repository.memos.value.size == 2)

        // Abandoned partial files are removed when a new memo starts.
        repository.accept("watch", blocks(memo, 80)[0])
        java.io.File(directory, "partial").listFiles()!!.forEach { it.setLastModified(now - 2L * 24 * 60 * 60 * 1000) }
        now += 25L * 60 * 60 * 1000
        repository.accept("watch", blocks(memo, 81)[0])
        check(java.io.File(directory, "partial").listFiles()!!.size == 1)

        // A new repository instance lists what is on disk; delete removes it.
        val reopened = VoiceMemoRepository(directory)
        check(reopened.memos.value.map { it.id } == repository.memos.value.map { it.id })
        check(reopened.delete(complete.memo.id) && !complete.memo.file.exists())
        check(!reopened.delete(complete.memo.id) && reopened.memos.value.size == 1)
    } finally {
        directory.deleteRecursively()
    }
    println("Voice memo phone checks: C fixture decode, ADPCM/WAV, validation, dictionary decode, repository append/duplicate/gap/conflict/corrupt/cleanup/delete passed")
}
