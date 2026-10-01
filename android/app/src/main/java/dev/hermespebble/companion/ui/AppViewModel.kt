package dev.hermespebble.companion.ui

import android.app.Application
import dev.hermespebble.companion.diagnostics.DiagnosticLog
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import dev.hermespebble.companion.HermesPt2Application
import dev.hermespebble.companion.data.local.CommandItem
import dev.hermespebble.companion.data.local.CommandRepository
import dev.hermespebble.companion.data.local.StopDisposition
import dev.hermespebble.companion.data.preferences.HermesSettings
import dev.hermespebble.companion.data.preferences.SecretChange
import dev.hermespebble.companion.data.preferences.SettingsUpdate
import dev.hermespebble.companion.dispatch.ConnectionTestResult
import dev.hermespebble.companion.pebble.WireProtocol
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

enum class AppSection {
    HOME,
    SETUP,
    DETAIL,
    NOTES,
    DIAGNOSTICS,
}

data class HostSelectionState(
    val eligible: List<String> = emptyList(),
    val selected: String? = null,
    val loading: Boolean = false,
)

data class UiMessage(val text: String, val isError: Boolean = false)

data class HermesUiState(
    val settings: HermesSettings? = null,
    val recent: List<CommandItem> = emptyList(),
    val selectedCommand: CommandItem? = null,
    val conversationMessages: List<dev.hermespebble.companion.data.local.ConversationMessageItem> = emptyList(),
    val section: AppSection = AppSection.SETUP,
    val host: HostSelectionState = HostSelectionState(),
    val activeWatches: Set<String> = emptySet(),
    val busy: Boolean = false,
    val message: UiMessage? = null,
    val connectionTest: String? = null,
) {
    val connectionReady: Boolean
        get() = settings?.let { it.hasHermesKey && it.serverUrl.isNotBlank() &&
            (!it.accessHeaderEnabled || it.hasAccessHeaderValue) } == true

    val protocolVersion: Int = WireProtocol.VERSION
}

@OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
class AppViewModel(application: Application) : AndroidViewModel(application) {
    private val container = (application as HermesPt2Application).requireContainer()
    private val commandRepository: CommandRepository = container.commandRepository
    val inkNotes = container.inkRepository.notes.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), emptyList())
    val selectedInkId = MutableStateFlow<Long?>(null)
    fun openInkNote(id: Long) { selectedInkId.value = id; setSection(AppSection.NOTES) }

    private val selectedId = MutableStateFlow<Long?>(null)
    private val control = MutableStateFlow(ControlState())
    private val hostState = MutableStateFlow(HostSelectionState(loading = true))
    private val settings = container.settingsRepository.settings.stateIn(
        viewModelScope,
        SharingStarted.Eagerly,
        null,
    )
    private val recent = commandRepository.observeAllRecent(100)
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), emptyList())
    private val selected = selectedId.flatMapLatest { id ->
        if (id == null) flowOf(null) else commandRepository.observe(id)
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), null)

    private val history = selected.flatMapLatest { item ->
        val sessionId = item?.sessionId
        if (item == null || sessionId == null) flowOf(emptyList())
        else container.conversationRepository.observeRecent(item.targetProfileId, sessionId)
    }

    val state: StateFlow<HermesUiState> = combine(
        settings,
        recent,
        combine(selected, history) { item, messages -> item to messages },
        control,
        combine(hostState, container.pebbleBridge.activeWatches) { host, watches -> host to watches },
    ) { currentSettings, currentRecent, currentSelected, currentControl, hostPair ->
        HermesUiState(
            settings = currentSettings,
            recent = currentRecent,
            selectedCommand = currentSelected.first,
            conversationMessages = currentSelected.second,
            section = if ((currentSettings == null || !currentSettings.hasHermesKey || currentSettings.serverUrl.isBlank()) &&
                currentControl.section != AppSection.DIAGNOSTICS && currentControl.section != AppSection.NOTES) {
                AppSection.SETUP
            } else if (currentControl.section == AppSection.SETUP && currentControl.initialized) {
                AppSection.SETUP
            } else {
                currentControl.section
            },
            host = hostPair.first,
            activeWatches = hostPair.second,
            busy = currentControl.busy,
            message = currentControl.message,
            connectionTest = currentControl.connectionTest,
        )
    }.stateIn(
        viewModelScope,
        SharingStarted.WhileSubscribed(5_000),
        HermesUiState(),
    )

    init {
        refreshHosts()

    }

    val diagnosticEvents = DiagnosticLog.events

    fun clearDiagnostics() = DiagnosticLog.clear()

    suspend fun loadCredential(accessHeader: Boolean): String {
        val current = container.settingsRepository.current()
        return if (accessHeader) {
            current.accessHeaderReference?.let { container.secretStore.readAccessHeaderValue(it) }.orEmpty()
        } else {
            if (current.hasHermesKey) container.secretStore.readHermesKey(current.hermesKeyReference) else ""
        }
    }

    private suspend fun secretChange(value: String, accessHeader: Boolean): SecretChange {
        if (value.isEmpty()) return SecretChange.Keep
        val stored = try {
            loadCredential(accessHeader)
        } catch (error: CancellationException) {
            throw error
        } catch (_: Exception) {
            null // Allow replacing an unreadable credential.
        }
        return if (value == stored) SecretChange.Keep else SecretChange.Set(value)
    }

    fun testWatchLink() {
        launchBusy { setMessage(container.pebbleBridge.testWatchLink()) }
    }

    val audioTestProgress = MutableStateFlow<String?>(null)

    fun testWatchAudio() {
        if (control.value.busy) return
        launchBusy {
            try {
                val result = container.pebbleBridge.testWatchAudio { audioTestProgress.value = it }
                audioTestProgress.value = result
            } catch (error: CancellationException) {
                audioTestProgress.value = "Audio test cancelled."
                throw error
            } catch (error: Exception) {
                audioTestProgress.value = error.message ?: "The audio test could not be completed."
                throw error
            }
        }
    }

    fun setSection(section: AppSection) {
        control.value = control.value.copy(section = section, initialized = true)
    }

    fun showHome() {
        selectedId.value = null
        setSection(AppSection.HOME)
    }

    fun showSetup() {
        setSection(AppSection.SETUP)
    }

    fun showNotes() {
        selectedInkId.value = null
        setSection(AppSection.NOTES)
    }

    fun showDiagnostics() {
        refreshHosts()
        setSection(AppSection.DIAGNOSTICS)
    }

    fun openCommand(id: Long) {
        selectedId.value = id
        setSection(AppSection.DETAIL)
    }

    fun openCommandIfPresent(id: Long) {
        launchBusy { if (commandRepository.get(id) != null) openCommand(id) }
    }

    fun saveConnection(
        serverUrl: String,
        accessHeaderEnabled: Boolean,
        accessHeaderName: String,
        hermesKey: String,
        accessHeaderValue: String,
    ) {
        launchBusy {
            val result = container.settingsRepository.update(
                SettingsUpdate(
                    serverUrl = serverUrl,
                    accessHeaderEnabled = accessHeaderEnabled,
                    accessHeaderName = accessHeaderName,
                    hermesKey = secretChange(hermesKey, accessHeader = false),
                    accessHeaderValue = secretChange(accessHeaderValue, accessHeader = true),
                ),
            )
            setMessage(
                if (result.profileRotated) {
                    "Connection profile saved. Unresolved requests for the previous profile are paused."
                } else {
                    "Connection settings are unchanged."
                },
            )
            control.value = control.value.copy(connectionTest = null, section = AppSection.HOME, initialized = true)
        }
    }

    fun testConnection() {
        launchBusy {
            when (val result = container.dispatcher.testConnection()) {
                is ConnectionTestResult.Succeeded -> {
                    val message = "Hermes ${result.capabilities.serverVersion ?: "capabilities"} verified. Durable run idempotency: " +
                        (if (result.capabilities.runsIdempotencyDurable) "supported" else "not advertised") +
                        (if (result.capabilities.needsExplicitRunHistory) ". Reply context compatibility enabled." else "")
                    control.value = control.value.copy(connectionTest = message)
                    setMessage("Connection check succeeded.")
                }
                is ConnectionTestResult.Failed -> {
                    control.value = control.value.copy(connectionTest = result.message)
                    setMessage(result.message, isError = true)
                }
            }
        }
    }

    fun clearAccessHeaderSecret() {
        launchBusy {
            container.settingsRepository.clearAccessHeaderValue()
            setMessage("Stored access-header value removed. Hermes authentication remains configured.")
        }
    }

    fun submitPhoneRequest(input: String) {
        if (!state.value.connectionReady) {
            setMessage("Configure the Hermes API root and key before sending a request.", isError = true)
            setSection(AppSection.SETUP)
            return
        }
        launchBusy {
            val receipt = container.submitPhoneRequest(input.trim())
            setMessage("Request stored durably on the phone.")
            openCommand(receipt.commandId)
        }
    }

    fun repeatRequest(item: CommandItem) {
        launchBusy {
            if (!item.state.terminal) container.startNewConversation()
            val receipt = container.submitPhoneRequest(item.input)
            openCommand(receipt.commandId)
        }
    }

    fun startNewConversation() {
        launchBusy {
            val generation = container.startNewConversation()
            setMessage("New Hermes conversation generation $generation is active. Queued requests keep their intended conversation.")
        }
    }

    fun refreshCommand(id: Long) {
        launchBusy { container.dispatcher.refresh(id) }
    }

    fun stopCommand(id: Long) {
        launchBusy {
            val disposition = container.dispatcher.requestStop(id)
            val message = when (disposition) {
                StopDisposition.CANCELLED_UNSENT -> "Unsent queued request removed before dispatch."
                StopDisposition.STOP_REQUESTED -> "Stop requested. Hermes may already have performed an external action."
                StopDisposition.WAITING_FOR_ACCEPTANCE -> "Stop will be applied if the run is accepted."
                StopDisposition.RUN_ID_UNAVAILABLE -> "No accepted run ID is available; review the request instead of assuming undo."
                StopDisposition.ALREADY_TERMINAL -> "The request is already finished."
                StopDisposition.NOT_ACCEPTED -> "The request cannot be stopped in its current state."
            }
            setMessage(message, isError = disposition == StopDisposition.RUN_ID_UNAVAILABLE)
        }
    }

    fun reconcileCommand(id: Long) {
        launchBusy {
            val count = container.dispatcher.reconcileHistory(id)
            setMessage(
                if (count > 0) {
                    "Recovered $count conversation history item(s) for review. No request was re-executed."
                } else {
                    "No new conversation history was available. The request was not re-executed."
                },
            )
        }
    }

    fun clearHistory() {
        val profile = state.value.settings?.profileId ?: return
        launchBusy {
            val count = commandRepository.clearCurrentHistory(profile)
            setMessage("Cleared $count finished item(s). Unresolved work and duplicate-prevention records were retained.")
        }
    }

    fun refreshHosts() {
        viewModelScope.launch {
            hostState.value = hostState.value.copy(loading = true)
            try {
                val eligible = container.pebbleBridge.eligibleHosts().sorted()
                var selected = container.pebbleBridge.selectedHost()
                if (selected == null && eligible == listOf("coredevices.coreapp")) {
                    container.pebbleBridge.selectHost(eligible.single())
                    selected = eligible.single()
                    DiagnosticLog.record("Pebble", "Official Pebble host selected automatically")
                }
                hostState.value = HostSelectionState(eligible = eligible, selected = selected, loading = false)
                DiagnosticLog.record("Pebble", "Host scan: ${eligible.size} eligible; selected ${selected ?: "NONE — watch messages will be rejected"}")
                if (selected == null && eligible.isNotEmpty()) {
                    setMessage("Select the official Pebble phone host before using the watch app.", isError = true)
                }
            } catch (error: CancellationException) {
                throw error
            } catch (_: Exception) {
                hostState.value = HostSelectionState(loading = false)
                setMessage("The Pebble phone host list could not be read.", isError = true)
            }
        }
    }

    fun selectHost(packageName: String?) {
        launchBusy {
            container.pebbleBridge.selectHost(packageName)
            refreshHosts()
            setMessage("Pebble host saved. Close and reopen Hermes on the watch, or choose Reconnect.")
        }
    }

    fun setResultNotificationsEnabled(enabled: Boolean) {
        launchBusy { container.settingsRepository.setResultNotificationsEnabled(enabled) }
    }

    fun clearMessage() {
        control.value = control.value.copy(message = null)
    }

    private fun launchBusy(block: suspend () -> Unit) {
        viewModelScope.launch {
            control.value = control.value.copy(busy = true, message = null)
            try {
                block()
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                setMessage(error.message?.take(300) ?: "The action could not be completed.", isError = true)
            } finally {
                control.value = control.value.copy(busy = false)
            }
        }
    }

    private fun setMessage(text: String, isError: Boolean = false) {
        control.value = control.value.copy(message = UiMessage(text, isError))
    }

    private data class ControlState(
        val section: AppSection = AppSection.SETUP,
        val busy: Boolean = false,
        val message: UiMessage? = null,
        val connectionTest: String? = null,
        val initialized: Boolean = false,
    )
}
