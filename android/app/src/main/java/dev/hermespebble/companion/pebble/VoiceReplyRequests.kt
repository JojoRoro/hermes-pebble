package dev.hermespebble.companion.pebble

/** Deduplicate transport retries while allowing a new explicit playback request. */
class VoiceReplyRequests {
    private data class Key(val watch: String, val capture: Long, val transfer: Long)
    private val accepted = LinkedHashSet<Key>()

    @Synchronized
    fun accept(watch: String, request: WireMessage): Boolean {
        if (request.kind != WireMessageKind.FETCH_RESULT || request.pageOffset != 0L ||
            request.captureId == 0L || request.flags and WireProtocol.FLAG_VOICE_REPLY == 0) return false
        if (!accepted.add(Key(watch, request.captureId, request.transferId))) return false
        if (accepted.size > 128) accepted.remove(accepted.first())
        return true
    }
}
