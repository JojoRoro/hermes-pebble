package dev.hermespebble.companion.data.local

import androidx.room.Entity
import androidx.room.Index
import androidx.room.PrimaryKey
import androidx.room.TypeConverter
import dev.hermespebble.companion.network.HermesErrorCategory

@Entity(
    tableName = "commands",
    indices = [
        Index(value = ["watchIdentifier", "captureId"], unique = true),
        Index(value = ["idempotencyKey"], unique = true),
        Index(value = ["targetProfileId", "conversationGeneration", "id"]),
        Index(value = ["targetProfileId", "state", "nextAttemptAt"]),
        Index(value = ["runId"]),
        Index(value = ["visible", "createdAt"]),
    ],
)
data class CommandEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val kind: CommandKind,
    val watchIdentifier: String?,
    val captureId: Long?,
    val targetProfileId: String,
    val conversationGeneration: Long,
    val input: String,
    val visible: Boolean = true,
    val state: CommandState,
    val serverState: String? = null,
    val serverStatusRaw: String? = null,
    val submittedSessionId: String? = null,
    val currentSessionId: String? = null,
    val runId: String? = null,
    val frozenPayloadJson: String? = null,
    val idempotencyKey: String,
    val output: String? = null,
    val errorCategory: HermesErrorCategory? = null,
    val errorCode: String? = null,
    val errorMessage: String? = null,
    val httpStatus: Int? = null,
    val retryable: Boolean = false,
    val requiresReconciliation: Boolean = false,
    val firstSubmissionAttemptAt: Long? = null,
    val lastSubmissionAttemptAt: Long? = null,
    val nextAttemptAt: Long? = null,
    val submissionAttemptCount: Int = 0,
    val lastPolledAt: Long? = null,
    val monitorFailureCount: Int = 0,
    val stopRequested: Boolean = false,
    val stopRequestedAt: Long? = null,
    val replayed: Boolean = false,
    val leaseOwner: String? = null,
    val leaseUntil: Long? = null,
    val createdAt: Long,
    val updatedAt: Long,
    val completedAt: Long? = null,
)

@Entity(
    tableName = "capture_receipts",
    indices = [
        Index(value = ["watchIdentifier", "captureId"], unique = true),
        Index(value = ["commandId"], unique = true),
    ],
)
data class CaptureReceiptEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val watchIdentifier: String,
    val captureId: Long,
    val commandId: Long,
    val itemKind: CommandKind,
    val state: CommandState,
    val inputDigest: String,
    val durableAt: Long,
    val updatedAt: Long,
)

@Entity(
    tableName = "conversation_sessions",
    primaryKeys = ["profileId", "conversationGeneration"],
    indices = [Index(value = ["sessionId"])],
)
data class ConversationSessionEntity(
    val profileId: String,
    val conversationGeneration: Long,
    val sessionId: String,
    val serverConfirmed: Boolean,
    val createdAt: Long,
    val updatedAt: Long,
)

@Entity(
    tableName = "conversation_messages",
    indices = [
        Index(value = ["profileId", "sessionId", "fingerprint"], unique = true),
        Index(value = ["profileId", "sessionId", "createdAt"]),
    ],
)
data class ConversationMessageEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val profileId: String,
    val conversationGeneration: Long,
    val sessionId: String,
    val fingerprint: String,
    val serverId: String?,
    val role: String?,
    val content: String,
    val serverCreatedAt: String?,
    val createdAt: Long,
)

enum class CommandKind {
    WATCH_REQUEST,
    WATCH_NOTE,
    PHONE_REQUEST,
}

enum class CommandState {
    QUEUED,
    SUBMITTING,
    ACCEPTED,
    RUNNING,
    NEEDS_APPROVAL,
    COMPLETED,
    FAILED,
    STOPPING,
    CANCELLED,
    INTERRUPTED,
    OUTCOME_UNKNOWN,
    PAUSED;

    val terminal: Boolean
        get() = this == COMPLETED || this == FAILED || this == CANCELLED || this == INTERRUPTED
}

class HermesDatabaseConverters {
    @TypeConverter
    fun commandKindToString(value: CommandKind): String = value.name

    @TypeConverter
    fun stringToCommandKind(value: String): CommandKind = CommandKind.valueOf(value)

    @TypeConverter
    fun commandStateToString(value: CommandState): String = value.name

    @TypeConverter
    fun stringToCommandState(value: String): CommandState = CommandState.valueOf(value)

    @TypeConverter
    fun errorCategoryToString(value: HermesErrorCategory): String = value.name

    @TypeConverter
    fun stringToErrorCategory(value: String): HermesErrorCategory = HermesErrorCategory.valueOf(value)
}
