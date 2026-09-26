package dev.hermespebble.companion.data.local

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import androidx.room.Update
import kotlinx.coroutines.flow.Flow

@Dao
interface CommandDao {
    @Insert(onConflict = OnConflictStrategy.ABORT)
    suspend fun insert(command: CommandEntity): Long

    @Update
    suspend fun update(command: CommandEntity)

    @Query("SELECT * FROM commands WHERE visible = 1 ORDER BY createdAt DESC, id DESC LIMIT :limit")
    fun observeAllRecent(limit: Int): Flow<List<CommandEntity>>

    @Query("SELECT * FROM commands WHERE id = :commandId")
    suspend fun get(commandId: Long): CommandEntity?

    @Query("SELECT * FROM commands WHERE id = :commandId")
    fun observe(commandId: Long): Flow<CommandEntity?>

    @Query("SELECT * FROM commands WHERE watchIdentifier = :watchIdentifier AND captureId = :captureId")
    suspend fun getByCapture(watchIdentifier: String, captureId: Long): CommandEntity?

    @Query(
        """
        SELECT * FROM commands
        WHERE targetProfileId = :profileId
          AND conversationGeneration = :conversationGeneration
          AND kind IN ('WATCH_REQUEST', 'PHONE_REQUEST')
          AND state = 'QUEUED'
          AND (nextAttemptAt IS NULL OR nextAttemptAt <= :nowMillis)
        ORDER BY id ASC
        LIMIT 1
        """,
    )
    suspend fun nextQueued(
        profileId: String,
        conversationGeneration: Long,
        nowMillis: Long,
    ): CommandEntity?

    @Query(
        """
        SELECT * FROM commands
        WHERE targetProfileId = :profileId
          AND conversationGeneration = :conversationGeneration
          AND id < :commandId
          AND state IN (
            'QUEUED', 'SUBMITTING', 'ACCEPTED', 'RUNNING',
            'NEEDS_APPROVAL', 'STOPPING', 'PAUSED', 'OUTCOME_UNKNOWN'
          )
        LIMIT 1
        """,
    )
    suspend fun earlierBlockingCommand(
        profileId: String,
        conversationGeneration: Long,
        commandId: Long,
    ): CommandEntity?

    @Query("SELECT * FROM commands WHERE targetProfileId = :profileId AND state IN (:states) ORDER BY id ASC")
    suspend fun byProfileAndStates(profileId: String, states: List<String>): List<CommandEntity>

    @Query("SELECT * FROM commands WHERE state IN (:states) ORDER BY id ASC")
    suspend fun byStates(states: List<String>): List<CommandEntity>

    @Query("SELECT * FROM commands WHERE targetProfileId = :profileId AND state IN (:states) ORDER BY id ASC")
    fun observeByProfileAndStates(profileId: String, states: List<String>): Flow<List<CommandEntity>>

    @Query("SELECT * FROM commands WHERE runId IS NOT NULL AND state IN (:states) ORDER BY id ASC")
    suspend fun withRunByStates(states: List<String>): List<CommandEntity>

    @Query("SELECT * FROM commands WHERE targetProfileId = :profileId ORDER BY id DESC LIMIT 1")
    suspend fun newestForProfile(profileId: String): CommandEntity?

    @Query(
        """
        SELECT * FROM commands
        WHERE visible = 1 AND targetProfileId = :profileId
        ORDER BY createdAt DESC, id DESC
        LIMIT :limit
        """,
    )
    fun observeRecent(profileId: String, limit: Int): Flow<List<CommandEntity>>

    @Query(
        """
        SELECT * FROM commands
        WHERE targetProfileId = :profileId
          AND state IN ('COMPLETED', 'FAILED', 'CANCELLED', 'INTERRUPTED')
          AND completedAt IS NOT NULL
          AND completedAt < :cutoffMillis
        ORDER BY completedAt ASC
        """,
    )
    suspend fun terminalBefore(profileId: String, cutoffMillis: Long): List<CommandEntity>

    @Query("SELECT DISTINCT targetProfileId FROM commands")
    suspend fun profileIds(): List<String>

    @Query("SELECT COUNT(*) FROM commands")
    suspend fun count(): Int
}

@Dao
interface CaptureReceiptDao {
    @Insert(onConflict = OnConflictStrategy.ABORT)
    suspend fun insert(receipt: CaptureReceiptEntity): Long

    @Update
    suspend fun update(receipt: CaptureReceiptEntity)

    @Query("SELECT * FROM capture_receipts WHERE watchIdentifier = :watchIdentifier AND captureId = :captureId")
    suspend fun get(watchIdentifier: String, captureId: Long): CaptureReceiptEntity?
}

@Dao
interface ConversationSessionDao {
    @Insert(onConflict = OnConflictStrategy.ABORT)
    suspend fun insert(session: ConversationSessionEntity)

    @Update
    suspend fun update(session: ConversationSessionEntity)

    @Query(
        """
        SELECT * FROM conversation_sessions
        WHERE profileId = :profileId AND conversationGeneration = :conversationGeneration
        LIMIT 1
        """,
    )
    suspend fun get(profileId: String, conversationGeneration: Long): ConversationSessionEntity?

    @Query(
        """
        UPDATE conversation_sessions
        SET sessionId = :sessionId, serverConfirmed = :serverConfirmed, updatedAt = :updatedAt
        WHERE profileId = :profileId
          AND conversationGeneration = :conversationGeneration
          AND sessionId = :expectedSessionId
        """,
    )
    suspend fun updateSessionId(
        profileId: String,
        conversationGeneration: Long,
        expectedSessionId: String,
        sessionId: String,
        serverConfirmed: Boolean,
        updatedAt: Long,
    ): Int
}

@Dao
interface ConversationMessageDao {
    @Insert(onConflict = OnConflictStrategy.IGNORE)
    suspend fun insert(message: ConversationMessageEntity): Long

    @Query(
        """
        SELECT * FROM conversation_messages
        WHERE profileId = :profileId AND sessionId = :sessionId
        ORDER BY createdAt DESC, id DESC
        LIMIT :limit
        """,
    )
    fun observeRecent(profileId: String, sessionId: String, limit: Int): Flow<List<ConversationMessageEntity>>

    @Query(
        """
        DELETE FROM conversation_messages
        WHERE profileId = :profileId
          AND createdAt < :cutoffMillis
        """,
    )
    suspend fun deleteBefore(profileId: String, cutoffMillis: Long): Int

    @Query("DELETE FROM conversation_messages WHERE profileId = :profileId")
    suspend fun deleteForProfile(profileId: String): Int
}
