package dev.hermespebble.companion.data.local

import androidx.room.withTransaction
import dev.hermespebble.companion.network.ConversationHistoryPage
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

class ConversationRepository internal constructor(
    private val database: HermesDatabase,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    private val sessionDao = database.conversationSessionDao()
    private val messageDao = database.conversationMessageDao()

    fun observeRecent(
        profileId: String,
        sessionId: String,
        limit: Int = DEFAULT_MESSAGE_LIMIT,
    ): Flow<List<ConversationMessageItem>> = messageDao.observeRecent(
        profileId,
        sessionId,
        limit.coerceIn(1, MAX_MESSAGE_LIMIT),
    ).map { messages -> messages.map(::mapMessage) }

    suspend fun currentSession(
        profileId: String,
        conversationGeneration: Long,
    ): ConversationSessionEntity? = sessionDao.get(profileId, conversationGeneration)

    suspend fun storeHistory(
        profileId: String,
        conversationGeneration: Long,
        page: ConversationHistoryPage,
    ): Int {
        val session = sessionDao.get(profileId, conversationGeneration)
            ?: throw UnknownConversationException()
        if (session.sessionId != page.sessionId) throw HistorySessionMismatchException()
        return database.withTransaction {
            var inserted = 0
            page.messages.forEach { message ->
                val id = messageDao.insert(
                    ConversationMessageEntity(
                        profileId = profileId,
                        conversationGeneration = conversationGeneration,
                        sessionId = page.sessionId,
                        fingerprint = message.fingerprint,
                        serverId = message.serverId,
                        role = message.role,
                        content = message.content,
                        serverCreatedAt = message.createdAt,
                        createdAt = clock(),
                    ),
                )
                if (id > 0) inserted++
            }
            inserted
        }
    }

    private fun mapMessage(message: ConversationMessageEntity) = ConversationMessageItem(
        id = message.id,
        profileId = message.profileId,
        conversationGeneration = message.conversationGeneration,
        sessionId = message.sessionId,
        role = message.role,
        content = message.content,
        serverCreatedAt = message.serverCreatedAt,
        createdAt = message.createdAt,
    )

    companion object {
        const val DEFAULT_MESSAGE_LIMIT = 50
        const val MAX_MESSAGE_LIMIT = 200
    }
}

data class ConversationMessageItem(
    val id: Long,
    val profileId: String,
    val conversationGeneration: Long,
    val sessionId: String,
    val role: String?,
    val content: String,
    val serverCreatedAt: String?,
    val createdAt: Long,
)

class UnknownConversationException : Exception("Conversation mapping was not found")
class HistorySessionMismatchException : Exception("History session does not match the conversation")
