package dev.hermespebble.companion.network

import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

data class CompletedTurn(val input: String, val output: String)
data class PreparedRunPayload(val sessionId: String, val json: String)

object RunConversationContext {
    const val MAX_BYTES = 64 * 1024
    private const val MAX_TURNS = 20
    private const val SHORTENED = "\n[Earlier message shortened]"
    private val json = Json { encodeDefaults = true; explicitNulls = false }

    /** Input is newest first; output is chronological, preserving complete role pairs. */
    fun recent(newestFirst: List<CompletedTurn>): List<RunHistoryMessage> {
        val turns = mutableListOf<List<RunHistoryMessage>>()
        var remaining = MAX_BYTES
        for (turn in newestFirst.take(MAX_TURNS)) {
            val inputBytes = turn.input.toByteArray(Charsets.UTF_8).size
            val outputBytes = turn.output.toByteArray(Charsets.UTF_8).size
            if (inputBytes + outputBytes > remaining && turns.isNotEmpty()) break
            val inputLimit = if (inputBytes + outputBytes <= remaining) inputBytes else minOf(inputBytes, remaining / 2)
            val input = bounded(turn.input, inputLimit)
            val output = bounded(turn.output, remaining - input.toByteArray(Charsets.UTF_8).size)
            if (input.isBlank() || output.isBlank()) break
            val pair = listOf(RunHistoryMessage("user", input), RunHistoryMessage("assistant", output))
            turns += pair
            remaining -= pair.sumOf { it.content.toByteArray(Charsets.UTF_8).size }
            if (input != turn.input || output != turn.output || remaining == 0) break
        }
        return turns.asReversed().flatten()
    }

    /** A retry must not rebuild history or adopt a later session mapping. */
    suspend fun prepare(
        frozenJson: String?,
        submittedSessionId: String?,
        create: suspend () -> HermesRunSubmission,
    ): PreparedRunPayload {
        if (frozenJson != null) {
            require(!submittedSessionId.isNullOrBlank()) { "Frozen submission has no session" }
            return PreparedRunPayload(submittedSessionId, frozenJson)
        }
        val submission = create()
        return PreparedRunPayload(submission.sessionId, json.encodeToString(submission))
    }

    private fun bounded(text: String, limit: Int): String {
        val bytes = text.toByteArray(Charsets.UTF_8)
        if (bytes.size <= limit) return text
        val suffix = SHORTENED.toByteArray(Charsets.UTF_8)
        if (limit <= suffix.size) return ""
        var end = limit - suffix.size
        while (end > 0 && (bytes[end].toInt() and 0xc0) == 0x80) end--
        return String(bytes, 0, end, Charsets.UTF_8) + SHORTENED
    }
}
