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
    fun pcmOf(wav: ByteArray) = (0 until (wav.size - 44) / 2).map { ((wav[44 + 2 * it].toInt() and 0xff) or (wav[45 + 2 * it].toInt() shl 8)) }
    val samples = pcmOf(wav)
    check(samples.drop(2700).dropLast(1900).maxOf { abs(it) } in 7_000..10_000) // Level set to the target.
    check(abs(samples.drop(4000).average()) < 400) // DC offset removed.
    check(samples.take(2400).all { it == 0 } && samples.takeLast(1600).all { it == 0 }) // Edges muted.
    check(VoiceMemoCodec.toWav(ShortArray(100)).size == 244) // Silence: no division by zero.

    // Very quiet speech on a DC offset, with full-scale clicks from the button at both ends and
    // one in the middle: the clicks must not set the gain, and the middle one is turned down.
    val second = 8000
    val clickAt = 2 * second
    val quiet = ShortArray(4 * second) { i ->
        val voice = 150 * sin(2 * PI * 220 * i / 8000.0) * (0.6 + 0.4 * sin(2 * PI * 3 * i / 8000.0))
        val click = when {
            i < 2000 -> if (i % 2 == 0) 30_000 else -30_000 // Start press and release.
            i >= 4 * second - 1200 && i % 3 == 0 -> 28_000 // Stop press.
            i in clickAt until clickAt + 240 -> if (i % 4 < 2) 25_000 else -25_000
            else -> 0
        }
        (2500 + voice + click).toInt().coerceIn(-32768, 32767).toShort()
    }
    val rendered = VoiceMemoCodec.render(quiet)
    val quietOut = pcmOf(rendered.wav)
    check(quietOut.size == quiet.size)
    val speech = quietOut.subList(second / 2, clickAt - 400).maxOf { abs(it) }
    check(speech in 6_000..12_000) { "Quiet speech reached $speech" }
    check(rendered.levels.gain > 40f && rendered.levels.dcOffset in 2400..2600) { rendered.levels.toString() }
    val clickOut = quietOut.subList(clickAt, clickAt + 240).maxOf { abs(it) }
    check(clickOut <= 16_000) { "Middle click reached $clickOut" }
    check(quietOut.take(2400).all { it == 0 } && quietOut.takeLast(1600).all { it == 0 })
    check(quietOut.subList(clickAt + 800, 3 * second).maxOf { abs(it) } in 6_000..12_000) // Speech recovers.
    // A memo that is mostly pause takes its gain from the words, not from the hiss.
    var noise = 1
    val sparse = ShortArray(6 * second) { i ->
        noise = noise * 1103515245 + 12345
        val hiss = ((noise ushr 16) % 21) - 10
        val word = if (i in 3 * second until 3 * second + 2400) 600 * sin(2 * PI * 200 * i / 8000.0) else 0.0
        (hiss + word).toInt().toShort()
    }
    val sparseOut = pcmOf(VoiceMemoCodec.render(sparse).wav)
    val wordPeak = sparseOut.subList(3 * second + 400, 3 * second + 2000).maxOf { abs(it) }
    val hissPeak = sparseOut.subList(second / 2, 2 * second).maxOf { abs(it) }
    check(wordPeak > 8 * hissPeak) { "Word $wordPeak against hiss $hissPeak" }
    // A clip that is loud from start to end is never boosted into the limiter.
    val loud = VoiceMemoCodec.render(ShortArray(second) { (30_000 * sin(2 * PI * 300 * it / 8000.0)).toInt().toShort() })
    check(loud.levels.gain < 1f && pcmOf(loud.wav).maxOf { abs(it) } in 7_000..10_000)

    // The watch reports how much of the recording time reached the encoder.
    check(VoiceMemoCodec.coveragePercent(memo) == null)
    check(VoiceMemoCodec.coveragePercent(memo.copyOf().also { it[7] = 37 }) == 37)
    check(VoiceMemoCodec.decode(memo.copyOf().also { it[7] = 37 }).size == tone.size)

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
        check(complete.levels != null && complete.coveragePercent == null)
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
    liveModes(memo)
    println("Voice memo phone checks: C fixture decode, ADPCM/WAV, validation, dictionary decode, repository append/duplicate/gap/conflict/corrupt/cleanup/delete, " +
        "stream decode, watch-flash items, piece reassembly, incomplete recovery and live level passed")
}

private fun le32(value: Long) = ByteArray(4) { (value shr (8 * it)).toByte() }

/** A watch-flash item laid out like memo_log_drain() in memo_watch.h. */
private fun logItem(capture: Long, offset: Long, payload: ByteArray, end: Boolean = false): ByteArray {
    val item = ByteArray(VoiceMemoCodec.LOG_ITEM_BYTES)
    le32(capture).copyInto(item, 0); le32(offset).copyInto(item, 4)
    item[8] = if (end) 1 else 0
    item[10] = payload.size.toByte(); item[11] = (payload.size shr 8).toByte()
    payload.copyInto(item, 16)
    return item
}

private fun liveModes(memo: ByteArray) {
    val header = memo.copyOfRange(0, 16)
    val data = memo.copyOfRange(16, memo.size)
    val crc = VoiceMemoCodec.crc32(data)

    // Kind 12 dictionaries: data pieces and the end message.
    fun stream(offset: Long, payload: ByteArray, end: Boolean) = mapOf(
        WireProtocol.KEY_PROTOCOL_VERSION to PebbleDictionaryItem.UInt8(1u),
        WireProtocol.KEY_MESSAGE_KIND to PebbleDictionaryItem.UInt8(WireMessageKind.MEMO_STREAM.value.toUByte()),
        WireProtocol.KEY_TRANSFER_ID to PebbleDictionaryItem.UInt32(9u),
        WireProtocol.KEY_CAPTURE_ID to PebbleDictionaryItem.UInt32(500u),
        WireProtocol.KEY_PAGE_OFFSET to PebbleDictionaryItem.UInt32(offset.toUInt()),
        WireProtocol.KEY_TOTAL_BYTES to PebbleDictionaryItem.UInt32(if (end) data.size.toUInt() else 0u),
        WireProtocol.KEY_CONVERSATION_GENERATION to PebbleDictionaryItem.UInt32(if (end) crc.toUInt() else 0u),
        WireProtocol.KEY_FLAGS to PebbleDictionaryItem.UInt8(if (end) 0x20u else 1u),
        WireProtocol.KEY_PAYLOAD to PebbleDictionaryItem.Bytes(payload),
    )
    val first = (decodeMemoStream(stream(0, data.copyOfRange(0, 700), false)) as MemoStreamDecode.Part).part
    check(first.captureId == 500L && first.offset == 0L && first.payload.size == 700 && first.end == null)
    val endPart = (decodeMemoStream(stream(data.size.toLong(), header, true)) as MemoStreamDecode.Part).part
    check(endPart.end!!.total == data.size.toLong() && endPart.end!!.checksum == crc && endPart.payload.isEmpty())
    check(decodeMemoStream(stream(0, header, true)) is MemoStreamDecode.Invalid) // End offset must equal total.
    check(decodeMemoBlock(stream(0, header, false)) == MemoBlockDecode.NotMemo)
    check(!first.stored && !endPart.stored)
    val storedPart = (decodeMemoStream(stream(0, data.copyOfRange(0, 700), false) + (WireProtocol.KEY_FLAGS to PebbleDictionaryItem.UInt8(0x41u))) as MemoStreamDecode.Part).part
    check(storedPart.stored && storedPart.end == null)

    // Watch-flash items.
    val logFirst = VoiceMemoCodec.logPart(logItem(501, 0, data.copyOfRange(0, 224)))
    check(logFirst.captureId == 501L && logFirst.payload.contentEquals(data.copyOfRange(0, 224)))
    val logEnd = VoiceMemoCodec.logPart(logItem(501, data.size.toLong(), header + le32(crc) + le32(data.size.toLong()), end = true))
    check(logEnd.end!!.header.contentEquals(header) && logEnd.end!!.checksum == crc && logEnd.end!!.total == data.size.toLong())
    rejected { VoiceMemoCodec.logPart(ByteArray(239)) }
    rejected { VoiceMemoCodec.logPart(logItem(501, 0, ByteArray(0))) }
    rejected { VoiceMemoCodec.logPart(logItem(501, 0, ByteArray(30), end = true)) }
    rejected { VoiceMemoCodec.logPart(logItem(0, 0, ByteArray(10))) }
    rejected { VoiceMemoCodec.logPart(logItem(501, 0, ByteArray(10)).also { it[8] = 7 }) }

    val directory = java.nio.file.Files.createTempDirectory("live-memo-check").toFile()
    try {
        var now = 1_700_000_000_000L
        val repository = VoiceMemoRepository(directory) { now }
        // Stream pieces in order, with one repeat: the saved WAV matches the RAM-mode memo exactly.
        val pieces = (data.indices step 300).map { VoiceMemoPart(500, it.toLong(), data.copyOfRange(it, minOf(it + 300, data.size))) }
        pieces.forEach { check(repository.acceptPart("watch", it) == MemoAcceptResult.Accepted) }
        check(repository.acceptPart("watch", pieces[1]) == MemoAcceptResult.Accepted)
        val streamed = repository.acceptPart("watch", endPart) as MemoAcceptResult.Complete
        check(!streamed.duplicate && streamed.memo.file.readBytes().contentEquals(VoiceMemoCodec.toWav(VoiceMemoCodec.decode(memo))))
        check((repository.acceptPart("watch", endPart) as MemoAcceptResult.Complete).duplicate) // Repeated end.
        check(repository.acceptPart("watch", pieces[2]) == MemoAcceptResult.Accepted) // Late repeat: nothing new.
        check(repository.memos.value.size == 1)

        // Watch flash: the same memo as 224-byte items, saved under its own capture.
        (data.indices step 224).forEach {
            val item = logItem(501, it.toLong(), data.copyOfRange(it, minOf(it + 224, data.size)))
            check(repository.acceptPart("watch", VoiceMemoCodec.logPart(item), logged = true) == MemoAcceptResult.Accepted)
        }
        val logged = repository.acceptPart("watch", logEnd, logged = true) as MemoAcceptResult.Complete
        check(logged.memo.file.readBytes().contentEquals(streamed.memo.file.readBytes()) && repository.memos.value.size == 2)

        // Wrong CRC at the end: discarded.
        pieces.forEach { repository.acceptPart("watch", VoiceMemoPart(502, it.offset, it.payload)) }
        val badEnd = VoiceMemoPart(502, data.size.toLong(), ByteArray(0), MemoPartEnd(header, data.size.toLong(), crc xor 1))
        check(repository.acceptPart("watch", badEnd) is MemoAcceptResult.Corrupt)
        check(repository.acceptPart("watch", VoiceMemoPart(503, 4, ByteArray(0), MemoPartEnd(header, 4, 0))) is MemoAcceptResult.Corrupt)

        // A lost item leaves silence-sized zeros, not a failure; the length still has to match.
        (data.indices step 224).filter { it != 448 }.forEach {
            repository.acceptPart("watch", VoiceMemoCodec.logPart(logItem(504, it.toLong(), data.copyOfRange(it, minOf(it + 224, data.size)))), logged = true)
        }
        val gapEnd = VoiceMemoPart(504, data.size.toLong(), ByteArray(0),
            MemoPartEnd(header, data.size.toLong(), VoiceMemoCodec.crc32(data.copyOf().also { java.util.Arrays.fill(it, 448, 672, 0) })))
        check(repository.acceptPart("watch", gapEnd, logged = true) is MemoAcceptResult.Complete)

        // Sent from watch storage: never saved incomplete by either cleanup (the watch still holds
        // it and starts again), a restart overwrites the partial copy, and the end saves it once.
        val stored = pieces.map { VoiceMemoPart(507, it.offset, it.payload, stored = true) }
        stored.take(4).forEach { check(repository.acceptPart("watch", it) == MemoAcceptResult.Accepted) }
        check(repository.finishIncomplete("watch", logged = false).isEmpty())
        check(repository.finishIncomplete("watch", logged = true, idleMillis = 0).isEmpty())
        stored.forEach { check(repository.acceptPart("watch", it) == MemoAcceptResult.Accepted) }
        val storedEnd = VoiceMemoPart(507, data.size.toLong(), ByteArray(0), MemoPartEnd(header, data.size.toLong(), crc), stored = true)
        val storedSaved = repository.acceptPart("watch", storedEnd) as MemoAcceptResult.Complete
        check(!storedSaved.duplicate && storedSaved.memo.file.readBytes().contentEquals(streamed.memo.file.readBytes()))
        check((repository.acceptPart("watch", storedEnd) as MemoAcceptResult.Complete).duplicate)
        check(repository.memos.value.size == 4)

        // Cut off without an end: streams are saved when the app closes, flash only when its
        // session finishes, and an idle filter protects partial files still being written.
        pieces.take(10).forEach { repository.acceptPart("watch", VoiceMemoPart(505, it.offset, it.payload)) }
        pieces.take(5).forEach { repository.acceptPart("watch", VoiceMemoPart(506, it.offset, it.payload), logged = true) }
        check(repository.finishIncomplete("watch", logged = true, idleMillis = 60_000).isEmpty())
        val cut = repository.finishIncomplete("watch", logged = false).single()
        check(cut.memo.durationMillis == 750L) // 3,000 bytes = 6,000 samples.
        check(repository.finishIncomplete("watch", logged = true, captureId = 506).single().memo.durationMillis == 375L)
        check(java.io.File(directory, "partial").listFiles().orEmpty().isEmpty())
        check(repository.memos.value.size == 6)
    } finally {
        directory.deleteRecursively()
    }

    // Live playback level: very quiet speech after a full-scale start click and with one click
    // mid-way. After the muted start, speech is raised near the target and the click is held down.
    val second = 8000
    val clickAt = 3 * second
    val speech = ShortArray(6 * second) { i ->
        val voice = 120 * kotlin.math.sin(2 * PI * 220 * i / 8000.0) * (0.6 + 0.4 * kotlin.math.sin(2 * PI * 3 * i / 8000.0))
        val click = when {
            i < 2000 -> if (i % 2 == 0) 30_000 else -30_000
            i in clickAt until clickAt + 200 -> if (i % 4 < 2) 25_000 else -25_000
            else -> 0
        }
        (1800 + voice + click).toInt().coerceIn(-32768, 32767).toShort()
    }
    val encoded = encode(speech)
    val live = LiveMemoStream()
    val out = ArrayList<Short>()
    val adpcm = encoded.copyOfRange(16, encoded.size)
    for (offset in adpcm.indices step 333) {
        live.feed(offset.toLong(), adpcm.copyOfRange(offset, minOf(offset + 333, adpcm.size))).forEach { out.add(it) }
    }
    check(live.feed(0, adpcm.copyOfRange(0, 100)).isEmpty()) // A repeat plays nothing twice.
    live.flush().forEach { out.add(it) }
    check(out.size == speech.size) { "Live output ${out.size} of ${speech.size}" }
    check(out.take(2400).all { it.toInt() == 0 }) // First 300 ms muted.
    val voiced = out.subList(2 * second, clickAt - 400).maxOf { abs(it.toInt()) }
    check(voiced in 5_000..14_000) { "Live speech reached $voiced" }
    val clicked = out.subList(clickAt, clickAt + 200).maxOf { abs(it.toInt()) }
    check(clicked <= 16_000) { "Live click reached $clicked" }
    check(out.subList(clickAt + 1600, 5 * second).maxOf { abs(it.toInt()) } in 5_000..14_000) // Recovers at once.
}
