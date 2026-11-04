package dev.hermespebble.companion.pebble

import dev.hermespebble.companion.HermesPt2Application
import dev.hermespebble.companion.data.local.CommandState
import dev.hermespebble.companion.data.local.CommandValidationException
import dev.hermespebble.companion.data.local.DuplicateCaptureConflictException
import dev.hermespebble.companion.data.local.MemoAcceptResult
import io.rebble.pebblekit2.client.BasePebbleListenerService
import io.rebble.pebblekit2.common.model.PebbleDictionary
import io.rebble.pebblekit2.common.model.ReceiveResult
import io.rebble.pebblekit2.common.model.WatchIdentifier
import java.util.UUID
import dev.hermespebble.companion.diagnostics.DiagnosticLog
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class PebbleListenerService : BasePebbleListenerService() {
    private val serviceJob = SupervisorJob()
    private val hermesApplication by lazy { applicationContext as HermesPt2Application }
    private val container by lazy { hermesApplication.requireContainer() }
    private val bridge by lazy { container.pebbleBridge }
    private val assemblers = java.util.concurrent.ConcurrentHashMap<String, IncomingTransferAssembler>()

    override val coroutineScope: CoroutineScope = CoroutineScope(serviceJob + Dispatchers.IO)

    override fun onCreate() {
        super.onCreate()
        DiagnosticLog.record("Pebble", "Listener service created")
    }

    override fun onBind(intent: android.content.Intent?): android.os.IBinder? {
        DiagnosticLog.record("Pebble", "Host bound listener; awaiting selected-host validation")
        return super.onBind(intent)
    }

    override suspend fun onMessageReceived(
        watchappUUID: UUID,
        data: PebbleDictionary,
        watch: WatchIdentifier,
    ): ReceiveResult {
        DiagnosticLog.record("Pebble", "RX dictionary: ${data.size} fields; UUID match ${watchappUUID == WireProtocol.APP_UUID}")
        if (watchappUUID != WireProtocol.APP_UUID) {
            bridge.launch {
                bridge.sendProtocolError(
                    watch = watch.value,
                    code = WireError.MALFORMED,
                    message = "This companion does not serve the watch app UUID that sent the message.",
                )
            }
            return ReceiveResult.Ack
        }
        bridge.markActive(watch.value)
        when (val stream = decodeMemoStream(data)) {
            MemoStreamDecode.NotStream -> Unit
            is MemoStreamDecode.Invalid -> {
                DiagnosticLog.record("Memo", "Stream RX rejected: ${stream.reason}")
                return ReceiveResult.Nack
            }
            is MemoStreamDecode.Part -> return handleMemoPart(watch.value, stream.transferId, stream.part)
        }
        when (val memo = decodeMemoBlock(data)) {
            MemoBlockDecode.NotMemo -> Unit
            is MemoBlockDecode.Invalid -> {
                DiagnosticLog.record("Memo", "RX rejected: ${memo.reason}")
                return ReceiveResult.Nack
            }
            is MemoBlockDecode.Block -> return handleMemoBlock(watch.value, memo.block)
        }
        val assembler = assemblers.getOrPut(watch.value) { IncomingTransferAssembler() }
        return when (val decoded = assembler.accept(data)) {
            is DecodedChunkResult.Pending -> ReceiveResult.Ack
            is DecodedChunkResult.Error -> {
                DiagnosticLog.record("Pebble", "RX rejected: ${decoded.code}")
                bridge.launch {
                    bridge.sendProtocolError(
                        watch = watch.value,
                        code = decoded.code,
                        message = decoded.message,
                    )
                }
                ReceiveResult.Ack
            }
            is DecodedChunkResult.Complete -> handleComplete(watch.value, decoded.message)
        }
    }

    override fun onAppOpened(watchappUUID: UUID, watch: WatchIdentifier) {
        if (watchappUUID == WireProtocol.APP_UUID) bridge.onAppOpened(watch.value)
    }

    override suspend fun onDataLogReceived(
        watchappUUID: UUID,
        session: io.rebble.pebblekit2.common.model.DataLogSession,
        data: ByteArray,
        itemsLeft: Long,
        watch: WatchIdentifier,
    ): ReceiveResult {
        if (watchappUUID == WireProtocol.APP_UUID && session.tag == VoiceMemoCodec.LOG_TAG) return handleMemoLog(watch.value, session, data)
        if (watchappUUID != WireProtocol.APP_UUID || session.tag != InkCodec.LOG_TAG ||
            session.itemSize != InkCodec.LOG_ITEM_BYTES || data.isEmpty() || data.size % session.itemSize != 0) return ReceiveResult.Nack
        return try {
            for (offset in data.indices step session.itemSize) {
                container.inkRepository.accept(watch.value, InkCodec.logBlock(data.copyOfRange(offset, offset + session.itemSize)))
            }
            bridge.launch { container.inkNotifier.notifySaved() }
            ReceiveResult.Ack
        } catch (error: CancellationException) { throw error }
        catch (_: Exception) { ReceiveResult.Nack }
    }

    override suspend fun onDataLogSessionFinished(
        watchappUUID: UUID,
        session: io.rebble.pebblekit2.common.model.DataLogSession,
        watch: WatchIdentifier,
    ): ReceiveResult {
        if (watchappUUID == WireProtocol.APP_UUID && session.tag == VoiceMemoCodec.LOG_TAG) {
            // A session that finished without its end item (Hermes closed while saving): keep what
            // arrived. Without the session's capture (process restarted), take idle partial files.
            val capture = memoLogSessions.remove(session.timestamp.toString())
            try {
                withContext(Dispatchers.IO) {
                    container.voiceMemoRepository.finishIncomplete(watch.value, logged = true, captureId = capture,
                        idleMillis = if (capture == null) 120_000L else 0L)
                }.forEach { DiagnosticLog.record("Memo", "Saved an incomplete watch-flash memo: ${it.memo.durationMillis / 1000.0}s") }
            } catch (error: CancellationException) { throw error }
            catch (error: Exception) { DiagnosticLog.record("Memo", "Incomplete memo not saved: ${error.javaClass.simpleName}") }
            return ReceiveResult.Ack
        }
        return if (watchappUUID == WireProtocol.APP_UUID && session.tag == InkCodec.LOG_TAG &&
            session.itemSize == InkCodec.LOG_ITEM_BYTES) ReceiveResult.Ack else ReceiveResult.Nack
    }

    override fun onAppClosed(watchappUUID: UUID, watch: WatchIdentifier) {
        if (watchappUUID == WireProtocol.APP_UUID) {
            assemblers.remove(watch.value)
            bridge.onAppClosed(watch.value)
            // A stream cut off by closing Hermes keeps what arrived (watch flash keeps uploading).
            coroutineScope.launch {
                runCatching { container.voiceMemoRepository.finishIncomplete(watch.value, logged = false) }
                    .getOrNull()?.forEach { DiagnosticLog.record("Memo", "Saved an incomplete streamed memo: ${it.memo.durationMillis / 1000.0}s") }
            }
        }
    }

    override fun onDestroy() {
        assemblers.clear()
        coroutineScope.cancel()
        super.onDestroy()
    }

    private suspend fun handleComplete(watch: String, message: WireMessage): ReceiveResult {
        DiagnosticLog.record("Pebble", "RX complete: ${message.kind.name}; transfer ${message.transferId}")
        return try {
            when (message.kind) {
                WireMessageKind.AUDIO_STATUS -> {
                    bridge.onAudioStatus(watch, message)
                    ReceiveResult.Ack
                }
                WireMessageKind.INK_BLOCK -> {
                    val block = InkBlock(message.captureId, message.totalBytes.toInt(), message.pageOffset.toInt(), message.generation, message.payload)
                    val note = container.inkRepository.accept(watch, block)
                    bridge.launch {
                        bridge.sendInkReceipt(watch, message, note.completedAt != null)
                        container.inkNotifier.notifySaved()
                    }
                    ReceiveResult.Ack
                }
                WireMessageKind.SUBMIT_REQUEST,
                WireMessageKind.SAVE_NOTE,
                -> {
                    val receipt = acceptDurableCommand(watch, message)
                    bridge.launch {
                        bridge.sendDurableReceipt(watch, message, receipt)
                        container.commandRepository.get(receipt.commandId)?.let { bridge.sendStatus(watch, it) }
                    }
                    ReceiveResult.Ack
                }
                WireMessageKind.HANDSHAKE -> {
                    bridge.launch {
                        container.startNewConversationIfIdle(syncWatches = false)
                        bridge.onHandshake(watch, message)
                    }
                    ReceiveResult.Ack
                }
                WireMessageKind.DISCARD_CAPTURE -> {
                    bridge.launch { handleDiscard(watch, message) }
                    ReceiveResult.Ack
                }
                WireMessageKind.FETCH_RECENT -> {
                    bridge.launch { bridge.sendRecentPage(watch, message, message.pageOffset.toInt(), message.pageCount) }
                    ReceiveResult.Ack
                }
                WireMessageKind.FETCH_RESULT -> {
                    bridge.launch { bridge.sendResultPage(watch, message) }
                    ReceiveResult.Ack
                }
                WireMessageKind.START_CONVERSATION -> {
                    bridge.launch { handleNewConversation(watch, message) }
                    ReceiveResult.Ack
                }
                WireMessageKind.STOP_REQUEST -> {
                    bridge.launch { handleStop(watch, message) }
                    ReceiveResult.Ack
                }
                else -> {
                    bridge.launch {
                        bridge.sendProtocolError(
                            watch = watch,
                            code = WireError.UNSUPPORTED_KIND,
                            message = "The watch sent an operation the phone does not accept.",
                        )
                    }
                    ReceiveResult.Ack
                }
            }
        } catch (error: DuplicateCaptureConflictException) {
            bridge.launch {
                bridge.sendStructuredError(
                    watch = watch,
                    request = message,
                    code = WireError.IDEMPOTENCY_CONFLICT,
                    message = "This capture identifier was already used for different content.",
                )
            }
            ReceiveResult.Ack
        } catch (error: CommandValidationException) {
            bridge.launch {
                bridge.sendStructuredError(
                    watch = watch,
                    request = message,
                    code = WireError.MALFORMED,
                    message = error.message ?: "The watch command failed validation.",
                )
            }
            ReceiveResult.Ack
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            DiagnosticLog.record("Pebble", "RX failed; NACK: ${error.javaClass.simpleName}")
            ReceiveResult.Nack
        }
    }

    private val memoLogSessions = java.util.concurrent.ConcurrentHashMap<String, Long>()

    /** Stream mode: written before the ACK (the watch's flow control), then played live if enabled. */
    private suspend fun handleMemoPart(watch: String, transferId: Long, part: VoiceMemoPart): ReceiveResult = try {
        val result = withContext(Dispatchers.IO) { container.voiceMemoRepository.acceptPart(watch, part) }
        if (result !is MemoAcceptResult.Rejected) {
            try { container.liveMemoPlayer.onPart(watch, part) }
            catch (error: CancellationException) { throw error }
            catch (error: Exception) { DiagnosticLog.record("Memo", "Live playback error: ${error.javaClass.simpleName}") }
        }
        val end = part.end
        fun receipt(saved: Boolean) {
            if (end == null) return
            val block = VoiceMemoBlock(transferId, part.captureId, end.total, end.total, end.checksum, ByteArray(0))
            bridge.launch { bridge.sendMemoReceipt(watch, block, saved) }
        }
        when (result) {
            MemoAcceptResult.Accepted -> ReceiveResult.Ack
            is MemoAcceptResult.Complete -> {
                if (!result.duplicate) logSaved("streamed", result, end?.total ?: 0L)
                receipt(saved = true)
                ReceiveResult.Ack
            }
            is MemoAcceptResult.Corrupt -> {
                DiagnosticLog.record("Memo", "Discarded streamed memo: ${result.reason}")
                receipt(saved = false)
                ReceiveResult.Ack
            }
            is MemoAcceptResult.Rejected -> ReceiveResult.Nack
        }
    } catch (error: CancellationException) {
        throw error
    } catch (error: Exception) {
        DiagnosticLog.record("Memo", "Stream piece at ${part.offset} failed; NACK: ${error.javaClass.simpleName}")
        ReceiveResult.Nack
    }

    /** Watch flash: Data Logging items, in order, possibly long after recording. */
    private suspend fun handleMemoLog(
        watch: String,
        session: io.rebble.pebblekit2.common.model.DataLogSession,
        data: ByteArray,
    ): ReceiveResult {
        if (session.itemSize != VoiceMemoCodec.LOG_ITEM_BYTES || data.isEmpty() || data.size % session.itemSize != 0) return ReceiveResult.Nack
        return try {
            for (offset in data.indices step session.itemSize) {
                val part = try {
                    VoiceMemoCodec.logPart(data.copyOfRange(offset, offset + session.itemSize))
                } catch (error: IllegalArgumentException) {
                    DiagnosticLog.record("Memo", "Skipped a bad watch-flash item: ${error.message}")
                    continue
                }
                memoLogSessions[session.timestamp.toString()] = part.captureId
                when (val result = withContext(Dispatchers.IO) { container.voiceMemoRepository.acceptPart(watch, part, logged = true) }) {
                    is MemoAcceptResult.Complete -> if (!result.duplicate) logSaved("watch-flash", result, part.end?.total ?: 0L)
                    is MemoAcceptResult.Corrupt -> DiagnosticLog.record("Memo", "Discarded watch-flash memo: ${result.reason}")
                    else -> Unit
                }
            }
            ReceiveResult.Ack
        } catch (error: CancellationException) { throw error }
        catch (error: Exception) {
            DiagnosticLog.record("Memo", "Watch-flash items failed; NACK: ${error.javaClass.simpleName}")
            ReceiveResult.Nack
        }
    }

    private fun logSaved(kind: String, result: MemoAcceptResult.Complete, bytes: Long) {
        val levels = result.levels?.let {
            ", speech %.0f dBFS, peak %.0f dBFS, gain %.1fx, DC %d".format(it.speechDbfs, it.peakDbfs, it.gain, it.dcOffset)
        }.orEmpty()
        val coverage = result.coveragePercent?.let { ", watch heard $it% of the recording time" }.orEmpty()
        DiagnosticLog.record("Memo", "Saved $kind voice memo: ${result.memo.durationMillis / 1000.0}s, $bytes bytes from watch$coverage$levels")
    }

    /** The ACK is the watch's flow control: it is returned only once the block is on disk. */
    private suspend fun handleMemoBlock(watch: String, block: VoiceMemoBlock): ReceiveResult = try {
        when (val result = withContext(Dispatchers.IO) { container.voiceMemoRepository.accept(watch, block) }) {
            MemoAcceptResult.Accepted -> ReceiveResult.Ack
            is MemoAcceptResult.Complete -> {
                if (!result.duplicate) {
                    logSaved("RAM", result, block.total)
                }
                bridge.launch { bridge.sendMemoReceipt(watch, block, saved = true) }
                ReceiveResult.Ack
            }
            is MemoAcceptResult.Corrupt -> {
                DiagnosticLog.record("Memo", "Discarded voice memo: ${result.reason}")
                bridge.launch { bridge.sendMemoReceipt(watch, block, saved = false) }
                ReceiveResult.Ack
            }
            is MemoAcceptResult.Rejected -> {
                DiagnosticLog.record("Memo", "Block at ${block.offset} refused: ${result.reason}")
                ReceiveResult.Nack
            }
        }
    } catch (error: CancellationException) {
        throw error
    } catch (error: Exception) {
        DiagnosticLog.record("Memo", "Block at ${block.offset} failed; NACK: ${error.javaClass.simpleName}")
        ReceiveResult.Nack
    }

    private suspend fun acceptDurableCommand(
        watch: String,
        message: WireMessage,
    ) = withContext(Dispatchers.IO) {
        if (message.captureId !in 1..UInt.MAX_VALUE.toLong()) {
            throw CommandValidationException("Capture identifier is outside the protocol range")
        }
        if (message.payload.isEmpty() || message.payload.size > WireProtocol.MAX_WATCH_TEXT_BYTES) {
            throw CommandValidationException("Transcript is empty or larger than 1024 UTF-8 bytes")
        }
        val text = decodeUtf8(message.payload)
        val kind = if (message.kind == WireMessageKind.SAVE_NOTE) {
            dev.hermespebble.companion.data.local.CommandKind.WATCH_NOTE
        } else {
            dev.hermespebble.companion.data.local.CommandKind.WATCH_REQUEST
        }
        container.acceptWatchCommand(
            watchIdentifier = watch,
            captureId = message.captureId,
            input = text,
            kind = kind,
            conversationGeneration = message.generation,
        )
    }

    private suspend fun handleNewConversation(watch: String, message: WireMessage) {
        val newGeneration = container.settingsRepository.newConversation(message.generation)
        bridge.sendNewConversationAck(watch, message, newGeneration)
    }

    private suspend fun handleStop(watch: String, message: WireMessage) {
        val command = container.commandRepository.getByCapture(watch, message.captureId)
        if (command == null) {
            bridge.sendStructuredError(
                watch = watch,
                request = message,
                code = WireError.NOT_FOUND,
                message = "The submitted request is not available for this watch.",
            )
            return
        }
        container.dispatcher.requestStop(command.id)
        container.commandRepository.get(command.id)?.let { bridge.sendStatus(watch, it) }
    }

    private suspend fun handleDiscard(watch: String, message: WireMessage) {
        val settings = container.settingsRepository.current()
        val discarded = container.commandRepository.discardWatchCapture(
            watch, message.captureId, settings.profileId, message.generation,
        )
        if (discarded) {
            container.commandRepository.getByCapture(watch, message.captureId)?.let {
                container.dispatcher.enqueueFollowers(it.targetProfileId, it.conversationGeneration)
            }
            bridge.sendCaptureDiscarded(watch, message)
        } else {
            bridge.sendStructuredError(watch, message, WireError.UNKNOWN_OUTCOME,
                "The request may already have reached Hermes. Stop or review it instead of discarding.")
        }
    }
}
