package dev.hermespebble.companion.network

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class HermesEndpoint(
    val method: String,
    val path: String,
)

@Serializable
data class HermesCapabilities(
    @SerialName("run_submission") val runSubmission: Boolean,
    @SerialName("runs_idempotency_supported") val runsIdempotencySupported: Boolean,
    @SerialName("runs_idempotency_durable") val runsIdempotencyDurable: Boolean,
    @SerialName("runs_idempotency_retention_seconds") val runsIdempotencyRetentionSeconds: Long?,
    val endpoints: List<HermesEndpoint>,
    val serverVersion: String? = null,
) {
    // The published 0.19.0 wheel never restores SessionDB history in /v1/runs.
    // Restrict the workaround to the verified affected release.
    val needsExplicitRunHistory: Boolean get() = serverVersion == "0.19.0"

    val safeRetentionSeconds: Long?
        get() {
            val retention = runsIdempotencyRetentionSeconds ?: return null
            if (!runsIdempotencySupported || !runsIdempotencyDurable) return null
            if (retention <= RETRY_SAFETY_MARGIN_SECONDS || retention > MAX_PLAUSIBLE_RETENTION_SECONDS) {
                return null
            }
            return minOf(MAX_SAFE_RETRY_WINDOW_SECONDS, retention - RETRY_SAFETY_MARGIN_SECONDS)
        }

    fun supports(method: String, path: String): Boolean = endpoints.any {
        it.method.equals(method, ignoreCase = true) && it.path == path
    }

    val supportsRequiredV1Api: Boolean
        get() = runSubmission &&
            supports("POST", "/api/sessions") &&
            supports("GET", "/api/sessions/{session_id}") &&
            supports("POST", "/v1/runs") &&
            supports("GET", "/v1/runs/{run_id}") &&
            supports("POST", "/v1/runs/{run_id}/stop") &&
            supports("GET", "/api/sessions/{session_id}/messages")

    companion object {
        const val MAX_SAFE_RETRY_WINDOW_SECONDS = 23L * 60L * 60L
        const val RETRY_SAFETY_MARGIN_SECONDS = 5L * 60L
        const val MAX_PLAUSIBLE_RETENTION_SECONDS = 7L * 24L * 60L * 60L
    }
}

data class HermesSession(
    val id: String,
)

data class RunAcceptance(
    val runId: String,
    val status: RunServerStatus,
    val replayed: Boolean,
)

data class HermesRun(
    val runId: String,
    val status: RunServerStatus,
    val rawStatus: String,
    val sessionId: String?,
    val output: String?,
    val error: String?,
) {
    val terminal: Boolean
        get() = status in setOf(
            RunServerStatus.COMPLETED,
            RunServerStatus.FAILED,
            RunServerStatus.CANCELLED,
            RunServerStatus.INTERRUPTED,
            RunServerStatus.UNKNOWN,
        )
}

enum class RunServerStatus {
    QUEUED,
    STARTED,
    RUNNING,
    WAITING_FOR_APPROVAL,
    STOPPING,
    COMPLETED,
    FAILED,
    CANCELLED,
    INTERRUPTED,
    UNKNOWN;

    companion object {
        fun parse(value: String): RunServerStatus = when (value) {
            "queued" -> QUEUED
            "started" -> STARTED
            "running" -> RUNNING
            "waiting_for_approval" -> WAITING_FOR_APPROVAL
            "stopping" -> STOPPING
            "completed" -> COMPLETED
            "failed" -> FAILED
            "cancelled" -> CANCELLED
            "interrupted" -> INTERRUPTED
            else -> UNKNOWN
        }
    }
}

data class ConversationHistoryPage(
    val sessionId: String,
    val messages: List<ConversationMessage>,
    val limit: Int,
    val offset: Int,
    val order: String,
    val returned: Int,
)

data class ConversationMessage(
    val fingerprint: String,
    val serverId: String?,
    val role: String?,
    val content: String,
    val createdAt: String?,
)

enum class HermesErrorCategory {
    INVALID_SETTINGS,
    AUTHENTICATION,
    REDIRECT,
    NETWORK,
    TLS,
    UNSUPPORTED_API,
    INVALID_RESPONSE,
    SERVER,
    RATE_LIMITED,
    REQUEST_REJECTED,
    IDEMPOTENCY_CONFLICT,
    SUBMISSION_OUTCOME_UNKNOWN,
    NOT_FOUND,
    PROFILE_CHANGED,
    INTERNAL,
}

class HermesApiException(
    val category: HermesErrorCategory,
    override val message: String,
    val httpStatus: Int? = null,
    val retryAfterMillis: Long? = null,
) : Exception(message) {
    val submissionOutcomeMayBeUnknown: Boolean
        get() = category in ambiguousSubmissionCategories

    private companion object {
        val ambiguousSubmissionCategories = setOf(
            HermesErrorCategory.NETWORK,
            HermesErrorCategory.TLS,
            HermesErrorCategory.INVALID_RESPONSE,
            HermesErrorCategory.SERVER,
        )
    }
}

data class HermesRequestConfiguration(
    val profileId: String,
    val baseUrl: String,
    val authorization: String,
    val accessHeaderName: String?,
    val accessHeaderValue: String?,
)

@Serializable
data class HermesRunSubmission(
    val input: String,
    @SerialName("session_id") val sessionId: String,
    @SerialName("conversation_history") val conversationHistory: List<RunHistoryMessage>? = null,
    // Hermes applies this as an ephemeral system prompt for the run only.
    val instructions: String? = null,
)

/** Shapes replies for text-to-speech and the small watch screen. */
const val BRIEF_WATCH_REPLY_INSTRUCTIONS =
    "This message was sent from a Pebble smartwatch. Reply briefly in short, simple sentences. " +
        "Use plain text only: no Markdown, no lists, no tables, no headings, no code blocks, and no emoji. " +
        "The reply may be read aloud by text-to-speech, so write it the way you would say it."

@Serializable
data class RunHistoryMessage(val role: String, val content: String)

enum class RetryDecision {
    SAFE,
    DEADLINE_PASSED,
    UNSUPPORTED,
}

fun HermesCapabilities.retryDecision(
    firstAttemptAtMillis: Long,
    nowMillis: Long = System.currentTimeMillis(),
): RetryDecision {
    val safeWindow = safeRetentionSeconds ?: return RetryDecision.UNSUPPORTED
    val deadline = firstAttemptAtMillis + safeWindow * 1000L
    return if (nowMillis < deadline) RetryDecision.SAFE else RetryDecision.DEADLINE_PASSED
}
