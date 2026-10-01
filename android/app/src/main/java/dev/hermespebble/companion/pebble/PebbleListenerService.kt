package dev.hermespebble.companion.pebble

import dev.hermespebble.companion.HermesPt2Application
import dev.hermespebble.companion.data.local.CommandState
import dev.hermespebble.companion.data.local.CommandValidationException
import dev.hermespebble.companion.data.local.DuplicateCaptureConflictException
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
    ): ReceiveResult = if (watchappUUID == WireProtocol.APP_UUID && session.tag == InkCodec.LOG_TAG &&
        session.itemSize == InkCodec.LOG_ITEM_BYTES) ReceiveResult.Ack else ReceiveResult.Nack

    override fun onAppClosed(watchappUUID: UUID, watch: WatchIdentifier) {
        if (watchappUUID == WireProtocol.APP_UUID) {
            assemblers.remove(watch.value)
            bridge.onAppClosed(watch.value)
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
                    bridge.launch { bridge.onHandshake(watch, message) }
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
