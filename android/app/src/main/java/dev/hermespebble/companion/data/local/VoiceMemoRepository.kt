package dev.hermespebble.companion.data.local

import dev.hermespebble.companion.pebble.VoiceMemoBlock
import dev.hermespebble.companion.pebble.VoiceMemoCodec
import java.io.File
import java.io.RandomAccessFile
import java.security.MessageDigest
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

data class VoiceMemo(
    val id: String,
    val file: File,
    val receivedAt: Long,
    val durationMillis: Long,
)

sealed interface MemoAcceptResult {
    /** Stored (or already stored) and not yet complete: acknowledge the block. */
    data object Accepted : MemoAcceptResult
    /** The memo is complete and saved as WAV; [duplicate] when this repeats an earlier completion. */
    data class Complete(val memo: VoiceMemo, val duplicate: Boolean) : MemoAcceptResult
    /** All bytes arrived but the checksum or format is wrong; the partial file was discarded. */
    data class Corrupt(val reason: String) : MemoAcceptResult
    /** The block does not continue the stored partial file (gap or conflicting bytes). */
    data class Rejected(val reason: String) : MemoAcceptResult
}

/**
 * Voice memos are kept as files, not in Room: blocks append to `partial/<key>.part` and a
 * finished memo becomes `<receivedAt>-<key>.wav`. The key ties a file to one watch, capture
 * and checksum, so repeated or resent blocks are recognised. App backups exclude these files.
 */
class VoiceMemoRepository(private val directory: File, private val clock: () -> Long = System::currentTimeMillis) {
    private val partials = File(directory, "partial")
    private val state = MutableStateFlow(scan())
    val memos: StateFlow<List<VoiceMemo>> = state.asStateFlow()

    @Synchronized
    fun accept(watch: String, block: VoiceMemoBlock): MemoAcceptResult {
        VoiceMemoCodec.validateBlock(block)
        require(watch.isNotBlank())
        val key = "${watchKey(watch)}-${block.captureId}-${"%08x".format(block.checksum)}"
        completed(key)?.let { memo ->
            return if (block.isFinal) MemoAcceptResult.Complete(memo, duplicate = true) else MemoAcceptResult.Accepted
        }
        partials.mkdirs()
        val part = File(partials, "$key.part")
        if (block.offset == 0L) {
            removeAbandoned()
            part.writeBytes(block.payload) // A restart from zero replaces any earlier partial copy.
        } else {
            if (!part.exists()) return MemoAcceptResult.Rejected("No partial memo to continue")
            val length = part.length()
            when {
                block.offset == length -> RandomAccessFile(part, "rw").use { it.seek(length); it.write(block.payload) }
                block.offset + block.payload.size <= length -> {
                    val existing = ByteArray(block.payload.size)
                    RandomAccessFile(part, "r").use { it.seek(block.offset); it.readFully(existing) }
                    if (!existing.contentEquals(block.payload)) return MemoAcceptResult.Rejected("Resent memo bytes differ")
                }
                else -> return MemoAcceptResult.Rejected("Memo block skips ahead of the stored data")
            }
        }
        if (part.length() < block.total) return MemoAcceptResult.Accepted
        val bytes = part.readBytes()
        part.delete()
        if (bytes.size.toLong() != block.total || VoiceMemoCodec.crc32(bytes) != block.checksum) {
            return MemoAcceptResult.Corrupt("Memo checksum mismatch")
        }
        val wav = try {
            VoiceMemoCodec.toWav(VoiceMemoCodec.decode(bytes))
        } catch (error: IllegalArgumentException) {
            return MemoAcceptResult.Corrupt(error.message ?: "Memo format is invalid")
        }
        directory.mkdirs()
        val receivedAt = clock()
        val target = File(directory, "$receivedAt-$key.wav")
        val temporary = File(directory, "$receivedAt-$key.tmp")
        temporary.writeBytes(wav)
        check(temporary.renameTo(target)) { "Could not save the memo" }
        refresh()
        return MemoAcceptResult.Complete(requireNotNull(completed(key)), duplicate = false)
    }

    @Synchronized
    fun delete(id: String): Boolean {
        val memo = state.value.firstOrNull { it.id == id } ?: return false
        val deleted = memo.file.delete() || !memo.file.exists()
        refresh()
        return deleted
    }

    @Synchronized
    fun refresh() {
        state.value = scan()
    }

    private fun completed(key: String): VoiceMemo? = state.value.firstOrNull { it.id.endsWith("-$key") }

    private fun removeAbandoned() {
        val cutoff = clock() - ABANDONED_AFTER_MILLIS
        partials.listFiles()?.filter { it.lastModified() < cutoff }?.forEach { it.delete() }
        directory.listFiles()?.filter { it.name.endsWith(".tmp") }?.forEach { it.delete() }
    }

    private fun scan(): List<VoiceMemo> = directory.listFiles()
        .orEmpty()
        .filter { it.isFile && it.name.endsWith(".wav") }
        .map { file ->
            val id = file.name.removeSuffix(".wav")
            VoiceMemo(
                id = id,
                file = file,
                receivedAt = id.substringBefore('-').toLongOrNull() ?: file.lastModified(),
                durationMillis = VoiceMemoCodec.wavDurationMillis(file.length()),
            )
        }
        .sortedByDescending { it.receivedAt }

    private fun watchKey(watch: String): String = MessageDigest.getInstance("SHA-256")
        .digest(watch.toByteArray())
        .take(4)
        .joinToString("") { "%02x".format(it) }

    private companion object {
        const val ABANDONED_AFTER_MILLIS = 24L * 60 * 60 * 1000
    }
}
