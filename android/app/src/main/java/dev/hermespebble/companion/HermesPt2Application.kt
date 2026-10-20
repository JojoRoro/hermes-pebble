package dev.hermespebble.companion

import android.app.Application
import android.content.Context
import dev.hermespebble.companion.data.local.CommandKind
import dev.hermespebble.companion.data.local.CommandRepository
import dev.hermespebble.companion.data.local.ConversationRepository
import dev.hermespebble.companion.data.local.DurableCommandReceipt
import dev.hermespebble.companion.data.local.HermesDatabase
import dev.hermespebble.companion.data.preferences.SettingsRepository
import dev.hermespebble.companion.diagnostics.DiagnosticLog
import dev.hermespebble.companion.dispatch.CommandBridge
import dev.hermespebble.companion.dispatch.CommandDispatcher
import dev.hermespebble.companion.dispatch.ResultNotifier
import dev.hermespebble.companion.network.HermesClient
import dev.hermespebble.companion.network.HermesRequestConfigurationProvider
import dev.hermespebble.companion.pebble.AndroidResultNotifier
import dev.hermespebble.companion.pebble.PebbleBridge
import dev.hermespebble.companion.security.SecretStore
import io.rebble.pebblekit2.client.DefaultPebbleAndroidAppPicker
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch

class HermesPt2Application : Application() {
    private val containerLock = Any()
    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        requireContainer().start()
    }

    override fun onTerminate() {
        requireContainer().pebbleBridge.close()
        super.onTerminate()
    }

    fun requireContainer(): AppContainer = synchronized(containerLock) {
        if (!::container.isInitialized) container = AppContainer(this)
        container
    }
}

class AppContainer(context: Context) {
    private val applicationContext = context.applicationContext
    private val applicationScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val started = AtomicBoolean(false)

    val database: HermesDatabase = HermesDatabase.create(applicationContext)
    val inkRepository = dev.hermespebble.companion.data.local.InkRepository(database)
    val inkNotifier = dev.hermespebble.companion.pebble.InkNotifier(applicationContext, database)
    val secretStore = SecretStore(applicationContext)
    val commandRepository = CommandRepository(database)
    val conversationRepository = ConversationRepository(database)
    val settingsRepository = SettingsRepository(
        context = applicationContext,
        secretStore = secretStore,
        onProfileRotated = { _, newSettings ->
            commandRepository.pauseForProfileChange(newSettings.profileId)
        },
    )
    val pebbleBridge = PebbleBridge(
        context = applicationContext,
        commandRepository = commandRepository,
        settingsRepository = settingsRepository,
    )
    val hermesClient = HermesClient(
        HermesRequestConfigurationProvider(settingsRepository, secretStore),
        diagnostic = { dev.hermespebble.companion.diagnostics.DiagnosticLog.record("HTTP", it) },
    )
    val dispatcher = CommandDispatcher(
        context = applicationContext,
        commandRepository = commandRepository,
        conversationRepository = conversationRepository,
        settingsRepository = settingsRepository,
        hermesClient = hermesClient,
    )

    @Volatile
    var bridge: CommandBridge? = null

    var notifier: ResultNotifier?
        get() = dispatcher.notifier
        set(value) {
            dispatcher.notifier = value
        }

    init {
        DefaultPebbleAndroidAppPicker.getInstance(applicationContext).enableAutoSelect = false
        bridge = pebbleBridge
        notifier = AndroidResultNotifier(
            context = applicationContext,
            settingsRepository = settingsRepository,
            commandRepository = commandRepository,
        )
        applicationScope.launch {
            commandRepository.changes.collect { change ->
                val currentBridge = bridge
                if (currentBridge != null) {
                    try {
                        currentBridge.onCommandChanged(change.commandId, change.state)
                    } catch (error: CancellationException) {
                        throw error
                    } catch (_: Exception) {
                    }
                }
            }
        }
    }

    fun start() {
        if (!started.compareAndSet(false, true)) return
        pebbleBridge.launch { inkNotifier.notifySaved() }
        applicationScope.launch {
            repeat(MAX_STARTUP_RECOVERY_ATTEMPTS) { attempt ->
                try {
                    dispatcher.recoverOnce()
                    return@launch
                } catch (error: CancellationException) {
                    throw error
                } catch (_: Exception) {
                    delay(STARTUP_RECOVERY_RETRY_DELAY_MILLIS * (attempt + 1L))
                }
            }
        }
        applicationScope.launch {
            // Rotates while the watch app stays open, so its next message is already in the new conversation.
            while (true) {
                startNewConversationIfIdle()
                delay(IDLE_CHECK_INTERVAL_MILLIS)
            }
        }
    }

    suspend fun acceptWatchCommand(
        watchIdentifier: String,
        captureId: Long,
        input: String,
        kind: CommandKind,
        conversationGeneration: Long,
    ): DurableCommandReceipt {
        require(kind == CommandKind.WATCH_REQUEST || kind == CommandKind.WATCH_NOTE)
        val rotated = kind == CommandKind.WATCH_REQUEST && startNewConversationIfIdle()
        val settings = settingsRepository.current()
        require(conversationGeneration in 0..settings.conversationGeneration) { "Unknown conversation generation" }
        // The watch still named the conversation that just went idle; its message opens the new one.
        val generation = if (rotated && conversationGeneration == settings.conversationGeneration - 1L) {
            settings.conversationGeneration
        } else {
            conversationGeneration
        }
        val receipt = commandRepository.acceptWatchCommand(
            watchIdentifier = watchIdentifier,
            captureId = captureId,
            input = input,
            kind = kind,
            targetProfileId = settings.profileId,
            conversationGeneration = generation,
        )
        if (kind == CommandKind.WATCH_REQUEST) dispatcher.enqueue(receipt.commandId)
        return receipt
    }

    suspend fun submitPhoneRequest(input: String): DurableCommandReceipt {
        startNewConversationIfIdle()
        val settings = settingsRepository.current()
        val receipt = commandRepository.acceptPhoneRequest(
            input = input,
            targetProfileId = settings.profileId,
            conversationGeneration = settings.conversationGeneration,
        )
        dispatcher.enqueue(receipt.commandId)
        return receipt
    }

    suspend fun startNewConversation(): Long =
        settingsRepository.newConversation().also { pebbleBridge.launch { pebbleBridge.requestHandshakes() } }

    /**
     * Starts a new conversation after the configured period without requests.
     *
     * Runs periodically, when the watch app connects and before each request. The watch
     * adopts the generation from a handshake reply; [syncWatches] asks open watch apps to
     * handshake again. A failure keeps the current conversation. Returns whether it rotated.
     */
    suspend fun startNewConversationIfIdle(syncWatches: Boolean = true): Boolean {
        try {
            val settings = settingsRepository.current()
            val hours = settings.autoNewConversationHours
            if (hours <= 0) return false
            val current = settings.conversationGeneration
            if (!commandRepository.conversationIdle(settings.profileId, current, hours * HOUR_MILLIS)) return false
            // Passing the observed generation keeps a concurrent rotation from advancing twice.
            val generation = settingsRepository.newConversation(expectedGeneration = current)
            if (generation != current + 1L) return false
            DiagnosticLog.record("Conversation", "Started conversation $generation after $hours h without requests")
            if (syncWatches) pebbleBridge.launch { pebbleBridge.requestHandshakes() }
            return true
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            DiagnosticLog.record("Conversation", "Idle check failed: ${error.javaClass.simpleName}")
            return false
        }
    }

    private companion object {
        const val MAX_STARTUP_RECOVERY_ATTEMPTS = 3
        const val HOUR_MILLIS = 60L * 60L * 1000L
        const val IDLE_CHECK_INTERVAL_MILLIS = 60_000L
        const val STARTUP_RECOVERY_RETRY_DELAY_MILLIS = 1_000L
    }
}
