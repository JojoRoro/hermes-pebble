package dev.hermespebble.companion.data.local

import androidx.room.Dao
import androidx.room.Entity
import androidx.room.Index
import androidx.room.Insert
import androidx.room.PrimaryKey
import androidx.room.Query
import androidx.room.Update
import androidx.room.withTransaction
import dev.hermespebble.companion.pebble.InkBlock
import dev.hermespebble.companion.pebble.InkCodec
import kotlinx.coroutines.flow.Flow

@Entity(tableName = "ink_notes", indices = [Index(value = ["watchIdentifier", "captureId"], unique = true)])
data class InkNoteEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val watchIdentifier: String,
    val captureId: Long,
    val total: Int,
    val checksum: Long,
    val bytes: ByteArray,
    val receivedMask: Int,
    val receivedAt: Long,
    val completedAt: Long? = null,
    val notified: Boolean = false,
)

@Dao
interface InkNoteDao {
    @Query("SELECT * FROM ink_notes WHERE completedAt IS NOT NULL ORDER BY receivedAt DESC")
    fun observeNotes(): Flow<List<InkNoteEntity>>
    @Query("SELECT * FROM ink_notes WHERE watchIdentifier = :watch AND captureId = :capture")
    suspend fun find(watch: String, capture: Long): InkNoteEntity?
    @Insert suspend fun insert(note: InkNoteEntity): Long
    @Update suspend fun update(note: InkNoteEntity)
    @Query("UPDATE ink_notes SET notified = 1 WHERE id = :id") suspend fun markNotified(id: Long)
    @Query("SELECT * FROM ink_notes WHERE completedAt IS NOT NULL AND notified = 0")
    suspend fun unnotified(): List<InkNoteEntity>
    @Query("DELETE FROM ink_notes WHERE completedAt IS NULL AND receivedAt < :before")
    suspend fun removeAbandoned(before: Long)
}

class InkRepository(private val database: HermesDatabase) {
    val notes = database.inkNoteDao().observeNotes()
    suspend fun accept(watch: String, block: InkBlock): InkNoteEntity = database.withTransaction {
        InkCodec.validateBlock(block)
        require(watch.isNotBlank())
        val dao = database.inkNoteDao()
        dao.removeAbandoned(System.currentTimeMillis() - 30L * 24 * 60 * 60 * 1000)
        val old = dao.find(watch, block.captureId)
        require(old == null || (old.total == block.total && old.checksum == block.checksum)) { "Conflicting handwriting capture" }
        val (bytes, mask) = InkCodec.merge(old?.bytes ?: ByteArray(block.total), old?.receivedMask ?: 0, block)
        val note = InkNoteEntity(
            id = old?.id ?: 0, watchIdentifier = watch, captureId = block.captureId,
            total = block.total, checksum = block.checksum, bytes = bytes, receivedMask = mask,
            receivedAt = old?.receivedAt ?: System.currentTimeMillis(),
            completedAt = old?.completedAt ?: if (InkCodec.complete(block.total, mask)) System.currentTimeMillis() else null,
            notified = old?.notified ?: false,
        )
        if (old == null) note.copy(id = dao.insert(note)) else note.also { dao.update(it) }
    }
}
