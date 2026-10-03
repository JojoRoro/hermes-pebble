package dev.hermespebble.companion.pebble

import dev.hermespebble.companion.data.local.CommandItem
import dev.hermespebble.companion.data.local.CommandKind
import dev.hermespebble.companion.data.local.CommandRepository
import dev.hermespebble.companion.data.local.CommandState
import dev.hermespebble.companion.data.local.DurableCommandReceipt
import dev.hermespebble.companion.data.preferences.SettingsRepository
import dev.hermespebble.companion.dispatch.CommandBridge
import dev.hermespebble.companion.network.HermesErrorCategory
import io.rebble.pebblekit2.client.DefaultPebbleAndroidAppPicker
import io.rebble.pebblekit2.client.DefaultPebbleSender
import io.rebble.pebblekit2.client.PebbleSender
import io.rebble.pebblekit2.common.model.TransmissionResult
import io.rebble.pebblekit2.common.model.WatchIdentifier
import dev.hermespebble.companion.diagnostics.DiagnosticLog
import java.io.Closeable
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

class PebbleBridge(
    context: android.content.Context,
    private val commandRepository: CommandRepository,
    private val settingsRepository: SettingsRepository,
) : CommandBridge, Closeable {
    private val applicationContext = context.applicationContext
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val sendMutex = Mutex()
    private var sender: PebbleSender = DefaultPebbleSender(applicationContext)
    private val picker = DefaultPebbleAndroidAppPicker.getInstance(applicationContext)
    private val json = Json {
        encodeDefaults = true
        explicitNulls = false
        ignoreUnknownKeys = true
    }
    private val activeWatchState = MutableStateFlow<Set<String>>(emptySet())
    private val linkProbes = java.util.concurrent.ConcurrentHashMap<Long, CompletableDeferred<String>>()
    private val probeMutex = Mutex()
    private val lastStatus = java.util.concurrent.ConcurrentHashMap<String, String>()
    private val voiceJobs = java.util.concurrent.ConcurrentHashMap<String, Job>()
    private val voiceRequests = VoiceReplyRequests()
    private val voiceMutex = Mutex()
    private val audioTransfer = WatchAudioTransfer(
        send = { watch, message -> sendTransfer(
            watch = watch, kind = message.kind, captureId = message.captureId,
            generation = message.generation, pageOffset = message.pageOffset,
            totalBytes = message.totalBytes, flags = message.flags, payload = message.payload,
            messageId = message.transferId, itemId = message.itemId,
        ) },
        nextId = ::transferId,
    )

    val activeWatches: StateFlow<Set<String>> = activeWatchState.asStateFlow()

    fun launch(block: suspend CoroutineScope.() -> Unit) {
        scope.launch {
            try { block() } catch (error: CancellationException) { throw error } catch (error: Exception) {
                DiagnosticLog.record("Pebble", "Reply failed: ${error.javaClass.simpleName}")
            }
        }
    }

    fun markActive(watch: String) {
        if (watch.isNotBlank()) activeWatchState.update { it + watch }
    }

    fun onAppOpened(watch: String) {
        DiagnosticLog.record("Pebble", "Watch app opened")
        if (watch.isBlank()) return
        markActive(watch)
        lastStatus.keys.retainAll(activeWatchState.value)
        // The watch initiates the handshake. An unsolicited ACK used to race its request.
    }

    fun onAppClosed(watch: String) {
        DiagnosticLog.record("Pebble", "Watch app closed")
        activeWatchState.update { it - watch }
        voiceJobs.remove(watch)?.cancel()
        lastStatus.remove(watch)
    }

    suspend fun eligibleHosts(): List<String> = picker.getAllEligibleApps()

    suspend fun selectedHost(): String? = picker.getCurrentlySelectedApp()

    suspend fun selectHost(packageName: String?) = sendMutex.withLock {
        picker.selectApp(packageName)
        DiagnosticLog.record("Pebble", "Host selection saved; reopen the watch app")
        voiceJobs.values.forEach { it.cancel() }
        voiceJobs.clear()
        resetSender()
        activeWatchState.value = emptySet()
        lastStatus.clear()
    }

    override suspend fun onCommandChanged(commandId: Long, state: CommandState) {
        val command = commandRepository.get(commandId) ?: return
        val watches = if (command.watchIdentifier != null) {
            if (command.watchIdentifier in activeWatchState.value) setOf(command.watchIdentifier) else emptySet()
        } else {
            activeWatchState.value
        }
        if (watches.isEmpty()) return
        watches.forEach { watch ->
            val cacheKey = "$watch:${command.captureId ?: command.id}:${command.state}:${command.errorCategory}"
            if (lastStatus[watch] == cacheKey) return@forEach
            if (sendStatus(watch, command)) lastStatus[watch] = cacheKey
        }
    }

    suspend fun onHandshake(watch: String, message: WireMessage) {
        if (sendHandshake(watch, message.transferId)) {
            DiagnosticLog.record("Pebble", "Handshake reply delivered; probe correlation ${message.correlationId}")
            linkProbes[message.correlationId]?.complete(watch)
        }
    }

    suspend fun sendHandshake(watch: String, correlationId: Long): Boolean {
        val settings = settingsRepository.current()
        val payload = json.encodeToString(HandshakePayload(WireProtocol.VERSION))
        return sendTransfer(
            watch = watch,
            kind = WireMessageKind.HANDSHAKE_ACK,
            correlationId = correlationId,
            generation = settings.conversationGeneration,
            payload = payload.toByteArray(Charsets.UTF_8),
        )
    }

    suspend fun sendDurableReceipt(watch: String, request: WireMessage, receipt: DurableCommandReceipt) {
        val status = if (receipt.kind == CommandKind.WATCH_NOTE) {
            WireStatus.NOTE_SAVED.value
        } else {
            WireStatus.SAVED_QUEUED.value
        }
        sendTransfer(
            watch = watch,
            kind = WireMessageKind.DURABLE_RECEIPT,
            captureId = requireNotNull(receipt.captureId),
            correlationId = request.transferId,
            itemId = receipt.commandId,
            status = status,
            itemState = status,
            itemKind = wireItemKind(receipt.kind),
            generation = request.generation,
            flags = WireProtocol.FLAG_DURABLE_COMMIT,
        )
    }

    suspend fun sendInkReceipt(watch: String, request: WireMessage, complete: Boolean) {
        sendTransfer(watch = watch, kind = WireMessageKind.INK_RECEIPT,
            captureId = request.captureId, correlationId = request.transferId,
            pageOffset = request.pageOffset + request.payload.size, totalBytes = request.totalBytes,
            generation = request.generation, flags = WireProtocol.FLAG_DURABLE_COMMIT,
            status = if (complete) WireStatus.NOTE_SAVED.value else WireStatus.WAITING_FOR_PHONE.value)
    }

    fun onAudioStatus(watch: String, message: WireMessage) {
        DiagnosticLog.record("Audio", "Watch status ${message.status}; confirmed ${message.pageOffset}/${message.totalBytes} bytes")
        audioTransfer.onStatus(watch, message)
    }

    suspend fun testWatchAudio(progress: (String) -> Unit): String = probeMutex.withLock {
        check(selectedHost() != null) { "Select the Pebble phone host first." }
        progress("Checking for an open Hermes watch app…")
        val probeId = transferId()
        val reply = CompletableDeferred<String>()
        linkProbes[probeId] = reply
        val watch = try {
            // Discover an already-open app even after Android process death. Never launch it.
            val probe = OutgoingProtocolCodec.encode(WireMessageKind.HANDSHAKE_ACK, probeId,
                generation = settingsRepository.current().conversationGeneration).single()
            val sent = sendMutex.withLock {
                callHost("Audio app check") { sender.sendDataToPebble(WireProtocol.APP_UUID, probe, watches = null) }
            }
            check(sent?.values?.any { it is TransmissionResult.Success } == true) {
                "Open Hermes on the watch, then press Play test sound again. Check the selected Pebble host if Hermes is already open."
            }
            withTimeoutOrNull(25_000L) { reply.await() }
                ?: error("The watch did not answer. Return to the Hermes watch menu and install the matching PBW.")
        } finally {
            linkProbes.remove(probeId)
            reply.cancel()
        }
        val pcm = applicationContext.assets.open("watch_test.s8").use { it.readBytes() }
        val started = android.os.SystemClock.elapsedRealtime()
        try {
            audioTransfer.play(watch, pcm) { stage ->
                DiagnosticLog.record("Audio", stage)
                progress(stage)
            }.also {
                DiagnosticLog.record("Audio", "Test completed: ${pcm.size} bytes; ${android.os.SystemClock.elapsedRealtime() - started} ms including playback")
            }
        } catch (error: Exception) {
            DiagnosticLog.record("Audio", "Test stopped: ${error.javaClass.simpleName}")
            throw error
        }
    }

    suspend fun sendStatus(watch: String, command: CommandItem): Boolean {
        val status = if (command.kind == CommandKind.WATCH_NOTE) WireStatus.NOTE_SAVED else command.state.toWireStatus()
        val error = command.errorCategory.toWireError()
        var flags = 0
        if (command.stopRequested) flags = flags or WireProtocol.FLAG_STOP_REQUESTED
        if (command.replayed) flags = flags or WireProtocol.FLAG_REPLAYED
        return sendTransfer(
            watch = watch,
            kind = WireMessageKind.STATUS_UPDATE,
            captureId = command.captureId ?: return false,
            itemId = command.id,
            status = status.value,
            errorCode = error.value,
            itemState = status.value,
            itemKind = wireItemKind(command.kind),
            flags = flags,
        )
    }

    suspend fun sendRecentPage(
        watch: String,
        request: WireMessage,
        requestedOffset: Int,
        requestedCount: Int,
    ) {
        val settings = settingsRepository.current()
        val offset = requestedOffset.coerceAtLeast(0)
        val count = requestedCount.coerceIn(0, WireProtocol.MAX_RECENT_ITEMS)
        val recent = commandRepository.observeRecent(settings.profileId, 50).first()
            .filter { it.watchIdentifier == watch && it.captureId != null }
        val page = recent.drop(offset).take(count)
        val payload = RecentPagePayload(
            items = page.map { item ->
                RecentItemPayload(
                    captureId = requireNotNull(item.captureId),
                    itemId = item.id,
                    kind = wireItemKind(item.kind).value,
                    state = item.state.toWireStatus().value,
                    preview = boundedJsonPreview(item.output ?: item.input),
                )
            },
        )
        sendTransfer(
            watch = watch,
            kind = WireMessageKind.RECENT_PAGE,
            correlationId = request.transferId,
            pageOffset = offset.toLong(),
            pageCount = page.size,
            totalBytes = recent.size.toLong(),
            generation = settings.conversationGeneration,
            payload = json.encodeToString(payload).toByteArray(Charsets.UTF_8),
        )
    }

    suspend fun sendResultPage(watch: String, request: WireMessage) {
        val command = commandRepository.getByCapture(watch, request.captureId)
        if (command == null) {
            sendStructuredError(
                watch = watch,
                request = request,
                code = WireError.NOT_FOUND,
                message = "The requested item is not available for this watch.",
            )
            return
        }
        val output = (if (command.kind == CommandKind.WATCH_NOTE) command.input else command.output.orEmpty()).replace('\u0000', ' ')
        val outputBytes = output.toByteArray(Charsets.UTF_8)
        val total = outputBytes.size
        val offset = request.pageOffset.coerceIn(0, total.toLong()).toInt()
        if (offset < total && (outputBytes[offset].toInt() and 0xc0) == 0x80) {
            sendStructuredError(watch, request, WireError.MALFORMED, "Page offset splits a UTF-8 character")
            return
        }
        var pageText = boundedUtf8Page(outputBytes, offset, MAX_RESULT_TEXT_BYTES)
        var payload = ResultPagePayload(
            captureId = requireNotNull(command.captureId),
            itemId = command.id,
            state = command.state.toWireStatus().value,
            output = pageText,
            more = offset + pageText.toByteArray(Charsets.UTF_8).size < total,
        )
        while (json.encodeToString(payload).toByteArray(Charsets.UTF_8).size > WireProtocol.MAX_RESULT_PAGE_BYTES) {
            pageText = pageText.dropLast(Character.charCount(pageText.codePointBefore(pageText.length)))
            payload = payload.copy(output = pageText, more = true)
        }
        val more = payload.more
        val delivered = sendTransfer(
            watch = watch,
            kind = WireMessageKind.RESULT_PAGE,
            generation = command.conversationGeneration,
            captureId = requireNotNull(command.captureId),
            correlationId = request.transferId,
            itemId = command.id,
            status = command.state.toWireStatus().value,
            itemState = command.state.toWireStatus().value,
            itemKind = wireItemKind(command.kind),
            pageOffset = offset.toLong(),
            pageCount = 1,
            totalBytes = total.toLong(),
            flags = if (more) WireProtocol.FLAG_MORE else 0,
            payload = json.encodeToString(payload).toByteArray(Charsets.UTF_8),
        )
        if (delivered && command.kind == CommandKind.WATCH_REQUEST && command.state == CommandState.COMPLETED &&
            output.isNotBlank() && voiceRequests.accept(watch, request)) {
            startVoiceReply(watch, request.captureId, output)
        }
    }

    private fun startVoiceReply(watch: String, captureId: Long, text: String) {
        voiceJobs.remove(watch)?.cancel()
        voiceJobs[watch] = scope.launch {
            suspend fun status(message: String) {
                DiagnosticLog.record("Voice", message)
                sendTransfer(watch = watch, kind = WireMessageKind.VOICE_STATUS, captureId = captureId,
                    payload = message.take(120).toByteArray(Charsets.UTF_8))
            }
            try {
                withTimeout(10 * 60_000L) {
                    voiceMutex.withLock {
                        status("Preparing voice on phone")
                        val pcm = WatchSpeech(applicationContext).synthesize(text)
                        DiagnosticLog.record("Voice", "Normal-speed speech: ${pcm.size} bytes at 8000 Hz; ${pcm.size / 8} ms")
                        val count = (pcm.size + WatchAudioTransfer.MAX_BYTES - 1) / WatchAudioTransfer.MAX_BYTES
                        for ((index, offset) in pcm.indices.step(WatchAudioTransfer.MAX_BYTES).withIndex()) {
                            check(watch in activeWatchState.value) { "Watch app closed. Voice stopped." }
                            status("Loading voice ${index + 1}/$count")
                            audioTransfer.play(watch, pcm.copyOfRange(offset, minOf(offset + WatchAudioTransfer.MAX_BYTES, pcm.size)),
                                replyCaptureId = captureId,
                                onPlayback = { status("Playing ${index + 1}/$count - BACK stops") }) { }
                        }
                        status("Voice reply finished")
                    }
                }
            } catch (_: TimeoutCancellationException) {
                status("Voice timed out. Read the answer below.")
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                status(error.message ?: "Voice unavailable. Read the answer below.")
            }
        }
    }

    suspend fun sendNewConversationAck(watch: String, request: WireMessage, newGeneration: Long) {
        val previous = request.generation
        sendTransfer(
            watch = watch,
            kind = WireMessageKind.NEW_CONVERSATION_ACK,
            correlationId = request.transferId,
            pageOffset = previous,
            totalBytes = newGeneration,
            generation = newGeneration,
            payload = json.encodeToString(NewConversationPayload(previous, newGeneration))
                .toByteArray(Charsets.UTF_8),
        )
    }

    suspend fun sendCaptureDiscarded(watch: String, request: WireMessage) {
        sendTransfer(
            watch = watch,
            kind = WireMessageKind.CAPTURE_DISCARDED,
            captureId = request.captureId,
            correlationId = request.transferId,
            status = WireStatus.DISCARDED.value,
            itemState = WireStatus.DISCARDED.value,
        )
    }

    suspend fun sendStructuredError(watch: String, request: WireMessage, code: WireError, message: String) {
        sendProtocolError(
            watch = watch,
            correlationId = request.transferId,
            captureId = request.captureId,
            itemId = request.itemId,
            code = code,
            message = message,
        )
    }

    suspend fun sendProtocolError(
        watch: String,
        code: WireError,
        message: String,
        correlationId: Long = 0,
        captureId: Long = 0,
        itemId: Long = 0,
    ) {
        val bounded = message.take(200).toByteArray(Charsets.UTF_8)
        sendTransfer(
            watch = watch,
            kind = WireMessageKind.STRUCTURED_ERROR,
            captureId = captureId,
            correlationId = correlationId,
            itemId = itemId,
            errorCode = code.value,
            payload = bounded,
        )
    }

    suspend fun testWatchLink(): String = probeMutex.withLock {
        if (selectedHost() == null) return@withLock "Select the Pebble phone host first."
        val probeId = transferId()
        val reply = CompletableDeferred<String>()
        linkProbes[probeId] = reply
        try {
            // Start can discover watches even if no listener callback has arrived yet.
            val started = sendMutex.withLock {
                callHost("Start watch app") { sender.startAppOnTheWatch(WireProtocol.APP_UUID, watches = null) }
            }
            if (started == null) return@withLock "Pebble host did not answer. Open the selected Pebble phone app and retry."
            if (started.isEmpty()) return@withLock "The Pebble host reports no connected watches. Check its Bluetooth connection."
            val watches = started.filterValues { it is TransmissionResult.Success }.keys
            if (watches.isEmpty()) return@withLock "Could not open Hermes on the watch. Check the Start watch app result in the log and install the current PBW."
            val configuration = settingsRepository.current()
            val probe = OutgoingProtocolCodec.encode(
                kind = WireMessageKind.HANDSHAKE_ACK,
                transferId = probeId,
                correlationId = 0,
                generation = configuration.conversationGeneration,
            ).single()
            // Start Success acknowledges the launch request, not that the watch app
            // is already foreground. Give it time without blocking inbound replies.
            DiagnosticLog.record("Pebble", "Launch accepted; waiting for watch readiness before probing")
            delay(1_000L)
            var awaitingLaunch = watches.toList()
            var delivered = false
            for (attempt in 1..8) {
                val sent = sendMutex.withLock {
                    callHost("Link probe $attempt/8") {
                        sender.sendDataToPebble(WireProtocol.APP_UUID, probe, awaitingLaunch)
                    }
                }
                if (sent?.values?.any { it is TransmissionResult.Success } == true) {
                    delivered = true
                    break
                }
                // Retry only the launch race. Permission/disconnection failures
                // need a different remedy and must not be hidden by retries.
                awaitingLaunch = awaitingLaunch.filter { sent?.get(it) is TransmissionResult.FailedDifferentAppOpen }
                if (awaitingLaunch.isEmpty() || attempt == 8) break
                DiagnosticLog.record("Pebble", "Host still reports another app open; waiting 1 second ($attempt/8)")
                delay(1_000L)
            }
            if (!delivered) {
                return@withLock "Probe delivery failed after waiting for launch. Check the Link probe events. If DifferentAppOpen persists while Hermes is visibly open, the Pebble host has not recognized it; close/reopen Hermes and reconnect the watch in the Pebble phone app. NoPermissions means companion access/PBW metadata."
            }
            // Receiving a transport ACK is insufficient: wait for the watch's correlated request
            // AND successful delivery of our handshake reply. Never hold sendMutex while waiting.
            val confirmed = withTimeoutOrNull(25_000L) { reply.await() }
            if (confirmed == null) {
                DiagnosticLog.record("Pebble", "Probe $probeId timed out: delivered, but no completed round trip")
                "Probe reached the watch, but no round trip completed. Install the matching 0.1.2-or-newer PBW, return to its menu, and retry. Check RX and handshake TX events."
            } else {
                DiagnosticLog.record("Pebble", "Probe $probeId: round trip verified")
                "Watch link verified in both directions. The watch received the handshake reply."
            }
        } finally {
            linkProbes.remove(probeId)
            reply.cancel()
        }
    }

    /** PebbleKit close can throw if its service was never bound. Still replace the sender. */
    private fun resetSender() {
        try { sender.close() } catch (_: IllegalArgumentException) { }
        sender = DefaultPebbleSender(applicationContext)
    }

    private suspend fun callHost(
        label: String,
        block: suspend () -> Map<WatchIdentifier, TransmissionResult>?,
    ): Map<WatchIdentifier, TransmissionResult>? {
        return try {
            val results = withTimeoutOrNull(10_000L) { block() }
            if (results == null) {
                DiagnosticLog.record("Pebble", "$label: host unavailable or timed out; resetting binding")
                resetSender()
            } else if (results.isEmpty()) {
                DiagnosticLog.record("Pebble", "$label: no connected watches")
            } else {
                results.values.forEach { DiagnosticLog.record("Pebble", "$label: ${it.javaClass.simpleName}") }
            }
            results
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            DiagnosticLog.record("Pebble", "$label: ${error.javaClass.simpleName}; resetting binding")
            resetSender()
            null
        }
    }

    override fun close() {
        scope.cancel()
        try { sender.close() } catch (_: IllegalArgumentException) { }
    }

    private suspend fun sendTransfer(
        watch: String,
        kind: WireMessageKind,
        captureId: Long = 0,
        correlationId: Long = 0,
        itemId: Long = 0,
        status: Int = 0,
        errorCode: Int = 0,
        itemState: Int = 0,
        itemKind: WireItemKind = WireItemKind.NONE,
        pageOffset: Long = 0,
        pageCount: Int = 0,
        totalBytes: Long = 0,
        generation: Long = 0,
        flags: Int = 0,
        payload: ByteArray = ByteArray(0),
        messageId: Long = transferId(),
    ): Boolean {
        if (watch !in activeWatchState.value) return false
        val dictionaries = OutgoingProtocolCodec.encode(
            kind = kind,
            transferId = messageId,
            captureId = captureId,
            correlationId = correlationId,
            itemId = itemId,
            status = status,
            errorCode = errorCode,
            itemState = itemState,
            itemKind = itemKind,
            pageOffset = pageOffset,
            pageCount = pageCount,
            totalBytes = totalBytes,
            generation = generation,
            flags = flags,
            payload = payload,
        )
        sendMutex.withLock {
            for (dictionary in dictionaries) {
                var delivered = false
                var attempt = 0
                while (!delivered && attempt < MAX_TRANSMISSION_ATTEMPTS) {
                    try {
                        val results = callHost("TX ${kind.name}") {
                            sender.sendDataToPebble(WireProtocol.APP_UUID, dictionary, listOf(WatchIdentifier(watch)))
                        }
                        val result = results?.get(WatchIdentifier(watch))
                        DiagnosticLog.record("Pebble", "TX ${kind.name}, attempt ${attempt + 1}: ${result?.javaClass?.simpleName ?: "no host response"}")
                        if (result is TransmissionResult.FailedNoPermissions ||
                            result is TransmissionResult.FailedDifferentAppOpen ||
                            result is TransmissionResult.FailedWatchNotConnected) return false
                        delivered = when (result) {
                            is TransmissionResult.Success -> true
                            null,
                            is TransmissionResult.FailedWatchNotConnected,
                            is TransmissionResult.FailedTimeout,
                            is TransmissionResult.FailedWatchNacked,
                            is TransmissionResult.FailedDifferentAppOpen,
                            is TransmissionResult.FailedNoPermissions,
                            is TransmissionResult.Unknown,
                            -> false
                        }
                    } catch (error: CancellationException) {
                        throw error
                    } catch (error: Exception) {
                        DiagnosticLog.record("Pebble", "TX ${kind.name}: ${error.javaClass.simpleName}")
                        delivered = false
                    }
                    if (!delivered) {
                        delay(RETRY_DELAYS_MILLIS[attempt])
                        attempt += 1
                    }
                }
                if (!delivered) return false
            }
        }
        return true
    }

    private fun boundedJsonPreview(text: String): String {
        var preview = text.replace('\u0000', ' ').take(96)
        if (preview.lastOrNull()?.isHighSurrogate() == true) preview = preview.dropLast(1)
        while (json.encodeToString(preview).toByteArray(Charsets.UTF_8).size > 120) {
            preview = preview.dropLast(Character.charCount(preview.codePointBefore(preview.length)))
        }
        return preview
    }

    private fun transferId(): Long = java.util.UUID.randomUUID().let { random ->
        (random.mostSignificantBits and 0xffff_ffffL).coerceAtLeast(1L)
    }

    private fun boundedUtf8Page(bytes: ByteArray, offset: Int, maxBytes: Int): String {
        if (offset >= bytes.size) return ""
        var endExclusive = minOf(bytes.size, offset + maxBytes)
        while (endExclusive < bytes.size && (bytes[endExclusive].toInt() and 0xc0) == 0x80) {
            endExclusive -= 1
        }
        if (endExclusive <= offset) return ""
        return decodeUtf8(bytes.copyOfRange(offset, endExclusive))
    }

    private fun wireItemKind(kind: CommandKind): WireItemKind = when (kind) {
        CommandKind.WATCH_NOTE -> WireItemKind.NOTE
        CommandKind.WATCH_REQUEST,
        CommandKind.PHONE_REQUEST,
        -> WireItemKind.REQUEST
    }

    private fun CommandState.toWireStatus(): WireStatus = when (this) {
        CommandState.QUEUED -> WireStatus.SAVED_QUEUED
        CommandState.SUBMITTING -> WireStatus.SUBMITTING
        CommandState.ACCEPTED -> WireStatus.ACCEPTED
        CommandState.RUNNING -> WireStatus.WORKING
        CommandState.NEEDS_APPROVAL -> WireStatus.APPROVAL_NEEDED
        CommandState.COMPLETED -> WireStatus.COMPLETED
        CommandState.FAILED -> WireStatus.FAILED
        CommandState.STOPPING -> WireStatus.STOPPING
        CommandState.CANCELLED -> WireStatus.CANCELLED
        CommandState.INTERRUPTED -> WireStatus.INTERRUPTED
        CommandState.OUTCOME_UNKNOWN -> WireStatus.OUTCOME_UNKNOWN
        CommandState.PAUSED -> WireStatus.WAITING_FOR_PROFILE
    }

    private fun HermesErrorCategory?.toWireError(): WireError = when (this) {
        HermesErrorCategory.INVALID_SETTINGS -> WireError.CONNECTION_SETTINGS
        HermesErrorCategory.AUTHENTICATION -> WireError.AUTHENTICATION
        HermesErrorCategory.REDIRECT -> WireError.REDIRECT
        HermesErrorCategory.NETWORK,
        HermesErrorCategory.TLS,
        -> WireError.NETWORK_TLS
        HermesErrorCategory.UNSUPPORTED_API -> WireError.UNSUPPORTED_ROUTE
        HermesErrorCategory.INVALID_RESPONSE -> WireError.INVALID_RESPONSE
        HermesErrorCategory.SERVER,
        HermesErrorCategory.RATE_LIMITED,
        HermesErrorCategory.REQUEST_REJECTED,
        HermesErrorCategory.INTERNAL,
        -> WireError.SERVER
        HermesErrorCategory.SUBMISSION_OUTCOME_UNKNOWN -> WireError.UNKNOWN_OUTCOME
        HermesErrorCategory.IDEMPOTENCY_CONFLICT -> WireError.IDEMPOTENCY_CONFLICT
        HermesErrorCategory.NOT_FOUND -> WireError.NOT_FOUND
        HermesErrorCategory.PROFILE_CHANGED -> WireError.CONNECTION_SETTINGS
        null -> WireError.NONE
    }

    private companion object {
        const val MAX_TRANSMISSION_ATTEMPTS = 4
        const val MAX_RESULT_TEXT_BYTES = 520
        val RETRY_DELAYS_MILLIS = longArrayOf(250, 500, 1_000, 2_000)
    }
}

@Serializable
private data class HandshakePayload(val protocolVersion: Int)

@Serializable
private data class RecentPagePayload(val items: List<RecentItemPayload>)

@Serializable
private data class RecentItemPayload(
    val captureId: Long,
    val itemId: Long,
    val kind: Int,
    val state: Int,
    val preview: String,
)

@Serializable
private data class ResultPagePayload(
    val captureId: Long,
    val itemId: Long,
    val state: Int,
    val output: String,
    val more: Boolean,
)

@Serializable
private data class NewConversationPayload(val previous: Long, val new: Long)
