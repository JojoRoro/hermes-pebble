package dev.hermespebble.companion.ui

import android.os.Build
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.clickable
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.platform.LocalContext
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.PowerManager
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import dev.hermespebble.companion.data.local.CommandItem
import dev.hermespebble.companion.data.local.CommandKind
import dev.hermespebble.companion.data.local.CommandState
import dev.hermespebble.companion.data.preferences.HermesSettings

@Composable
@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
fun HermesApp(viewModel: AppViewModel, onRequestNotificationPermission: () -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Hermes Pebble") },
                actions = {
                    TextButton(onClick = viewModel::showHome, enabled = state.settings?.hasHermesKey == true) {
                        Text("Home")
                    }
                    TextButton(onClick = viewModel::showSetup) { Text("Setup") }
                    TextButton(onClick = viewModel::showDiagnostics) { Text("Diagnostics") }
                },
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 12.dp),
        ) {
            if (state.busy) {
                CircularProgressIndicator(modifier = Modifier.padding(vertical = 8.dp))
            }
            state.message?.let { message ->
                Card(modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp)) {
                    Row(
                        modifier = Modifier.fillMaxWidth().padding(10.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            text = message.text,
                            color = if (message.isError) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface,
                            modifier = Modifier.weight(1f),
                        )
                        TextButton(onClick = viewModel::clearMessage) { Text("Dismiss") }
                    }
                }
            }
            when (state.section) {
                AppSection.SETUP -> SetupScreen(state, viewModel)
                AppSection.HOME -> HomeScreen(state, viewModel)
                AppSection.DETAIL -> DetailScreen(state, viewModel)
                AppSection.NOTES -> NotesScreen(state, viewModel, onRequestNotificationPermission)
                AppSection.DIAGNOSTICS -> DiagnosticsScreen(
                    state = state,
                    viewModel = viewModel,
                    onRequestNotificationPermission = onRequestNotificationPermission,
                )
            }
        }
    }
}

@Composable
private fun SetupScreen(state: HermesUiState, viewModel: AppViewModel) {
    val settings = state.settings
    var serverUrl by rememberSaveable(settings?.serverUrl) { mutableStateOf(settings?.serverUrl.orEmpty()) }
    var accessEnabled by rememberSaveable(settings?.accessHeaderEnabled) { mutableStateOf(settings?.accessHeaderEnabled == true) }
    var headerName by rememberSaveable(settings?.accessHeaderName) {
        mutableStateOf(settings?.accessHeaderName ?: "X-NetBird-Access")
    }
    var hermesKey by remember(settings?.hermesKeyReference) { mutableStateOf("") }
    var headerValue by remember(settings?.accessHeaderReference) { mutableStateOf("") }
    var keyFocused by remember { mutableStateOf(false) }
    var headerFocused by remember { mutableStateOf(false) }
    var credentialsLoaded by remember(settings?.hermesKeyReference, settings?.accessHeaderReference) { mutableStateOf(false) }
    var credentialError by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(settings?.hermesKeyReference, settings?.accessHeaderReference) {
        if (settings != null) {
            try {
                hermesKey = viewModel.loadCredential(false)
                headerValue = viewModel.loadCredential(true)
            } catch (error: kotlinx.coroutines.CancellationException) {
                throw error
            } catch (_: Exception) {
                credentialError = "Stored credentials could not be read. Enter replacement values."
            }
            credentialsLoaded = true
        }
    }
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        HostPicker(state, viewModel)
        OutlinedButton(onClick = viewModel::showNotes) { Text("Open local notes") }
        HorizontalDivider()
        Text("Hermes connection", style = MaterialTheme.typography.headlineSmall)
        credentialError?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        Text("Tap a credential field to reveal and edit its saved value. It is masked again when you leave the field.")
        Text(
            "Enter the HTTPS API root, not a chat-completions endpoint. A path prefix is preserved.",
            style = MaterialTheme.typography.bodyMedium,
        )
        OutlinedTextField(
            value = serverUrl,
            onValueChange = { serverUrl = it },
            label = { Text("Hermes server URL") },
            placeholder = { Text("https://host.example/hermes/") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = hermesKey,
            onValueChange = { hermesKey = it },
            label = { Text("Hermes API key") },
            placeholder = { Text("Required") },
            enabled = credentialsLoaded,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
            singleLine = true,
            visualTransformation = if (keyFocused) VisualTransformation.None else PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth().onFocusChanged { keyFocused = it.isFocused },
        )
        HorizontalDivider()
        Text("Additional service access header", style = MaterialTheme.typography.titleMedium)
        Text(
            "Usable for NetBird Custom Header authentication. The value is sent exactly as entered, without a Bearer prefix.",
            style = MaterialTheme.typography.bodySmall,
        )
        Row(verticalAlignment = Alignment.CenterVertically) {
            Switch(checked = accessEnabled, onCheckedChange = { accessEnabled = it })
            Text("Send an additional access header", modifier = Modifier.padding(start = 8.dp))
        }
        OutlinedTextField(
            value = headerName,
            onValueChange = { headerName = it },
            label = { Text("Header name") },
            singleLine = true,
            enabled = accessEnabled,
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = headerValue,
            onValueChange = { headerValue = it },
            label = { Text("Header value") },
            placeholder = { Text("NetBird service secret") },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
            singleLine = true,
            enabled = accessEnabled && credentialsLoaded,
            visualTransformation = if (headerFocused) VisualTransformation.None else PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth().onFocusChanged { headerFocused = it.isFocused },
        )
        Button(
            onClick = {
                viewModel.saveConnection(serverUrl, accessEnabled, headerName, hermesKey, headerValue)
            },
            enabled = credentialsLoaded && !state.busy && serverUrl.isNotBlank() && hermesKey.isNotEmpty() && (!accessEnabled || (headerName.isNotBlank() && headerValue.isNotEmpty())),
            modifier = Modifier.fillMaxWidth(),
        ) { Text("Save connection") }
        OutlinedButton(
            onClick = viewModel::testConnection,
            enabled = state.connectionReady && !state.busy,
            modifier = Modifier.fillMaxWidth(),
        ) { Text("Test saved Hermes connection") }
        Text("Save any edits before testing. The test uses the saved API key and access header.", style = MaterialTheme.typography.bodySmall)
        OutlinedButton(
            onClick = viewModel::clearAccessHeaderSecret,
            enabled = settings?.hasAccessHeaderValue == true,
            modifier = Modifier.fillMaxWidth(),
        ) { Text("Remove stored access-header value") }
        state.connectionTest?.let { Card(Modifier.fillMaxWidth()) { Text(it, Modifier.padding(12.dp)) } }
        Text(
            "First run: configure and test the connection, then send only a harmless test request. No request is sent from the watch during setup.",
            style = MaterialTheme.typography.bodyMedium,
        )
        Button(onClick = viewModel::showHome, enabled = state.connectionReady) { Text("Open home") }
    }
}

@Composable
private fun HomeScreen(state: HermesUiState, viewModel: AppViewModel) {
    var request by rememberSaveable { mutableStateOf("") }
    Column(modifier = Modifier.fillMaxSize()) {
        ConnectionCard(state)
        if (state.host.selected == null) {
            Button(onClick = viewModel::showSetup, modifier = Modifier.fillMaxWidth()) {
                Text("Connect Pebble phone host")
            }
        }
        Row(
            modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Button(
                onClick = { viewModel.submitPhoneRequest(request) },
                enabled = state.connectionReady && request.isNotBlank() && !state.busy,
            ) { Text("Send request") }
            OutlinedButton(onClick = viewModel::startNewConversation, enabled = !state.busy) {
                Text("New conversation")
            }
        }
        OutlinedTextField(
            value = request,
            onValueChange = { request = it },
            label = { Text("Typed request (phone fallback)") },
            minLines = 2,
            modifier = Modifier.fillMaxWidth().padding(bottom = 8.dp),
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = viewModel::showNotes) { Text("Notes") }
            TextButton(onClick = viewModel::clearHistory) { Text("Clear finished history") }
        }
        HorizontalDivider()
        Text("Recent requests and notes", style = MaterialTheme.typography.titleMedium)
        RecentList(
            items = state.recent,
            onOpen = viewModel::openCommand,
            modifier = Modifier.fillMaxSize(),
        )
    }
}

@Composable
private fun ConnectionCard(state: HermesUiState) {
    Card(Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(
                if (state.connectionReady) "Hermes credentials configured" else "Hermes setup required",
                style = MaterialTheme.typography.titleMedium,
            )
            Text(
                if (state.settings?.capabilities?.supportsRequiredV1Api == true) {
                    "Required asynchronous run API verified"
                } else {
                    "Capabilities not yet verified"
                },
            )
            Text(
                if (state.host.selected != null) {
                    "Pebble host: ${state.host.selected}"
                } else {
                    "Select an official Pebble host in Diagnostics"
                },
            )
            Text("Active watch apps: ${state.activeWatches.size}")
        }
    }
}

@Composable
private fun RecentList(items: List<CommandItem>, onOpen: (Long) -> Unit, modifier: Modifier = Modifier) {
    if (items.isEmpty()) {
        Text("No requests or notes yet.", modifier = modifier.padding(12.dp))
        return
    }
    LazyColumn(modifier = modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        items(items, key = { it.id }) { item ->
            Card(
                modifier = Modifier.fillMaxWidth().clickableCard { onOpen(item.id) },
            ) {
                Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                    Text(
                        text = item.input.ifBlank { "Finished item content cleared" },
                        maxLines = 2,
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Text(
                        text = "${if (item.kind == CommandKind.WATCH_NOTE) "Note" else "Request"} • ${item.state.display()}",
                        style = MaterialTheme.typography.labelMedium,
                    )
                    item.errorMessage?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
                }
            }
        }
    }
}

@Composable
private fun DetailScreen(state: HermesUiState, viewModel: AppViewModel) {
    val command = state.selectedCommand
    if (command == null) {
        Text("Select a request or note.", modifier = Modifier.padding(12.dp))
        return
    }
    Column(
        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(if (command.kind == CommandKind.WATCH_NOTE) "Local note" else "Hermes request", style = MaterialTheme.typography.headlineSmall)
        Text("Status: ${command.state.display()}")
        Text("Origin: ${command.kind.name}")
        command.sessionId?.let { Text("Conversation: $it") }
        command.runId?.let { Text("Run: $it") }
        command.errorMessage?.let {
            Text(it, color = MaterialTheme.colorScheme.error)
        }
        if (command.requiresReconciliation) {
            Text(
                "Review before retrying. The app will not automatically repeat a potentially executed request.",
                color = MaterialTheme.colorScheme.error,
            )
        }
        HorizontalDivider()
        Text("Transcript", style = MaterialTheme.typography.titleMedium)
        SelectionContainer { Text(command.input.ifBlank { "Content cleared" }, fontFamily = FontFamily.Default) }
        command.output?.let {
            HorizontalDivider()
            Text("Hermes answer", style = MaterialTheme.typography.titleMedium)
            SelectionContainer { Text(it) }
            Text("A finished run is not independent proof of an external delivery.", style = MaterialTheme.typography.bodySmall)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { viewModel.refreshCommand(command.id) }, enabled = !state.busy) {
                Text("Refresh")
            }
            if (command.state !in TERMINAL_STATES && command.kind != CommandKind.WATCH_NOTE) {
                OutlinedButton(onClick = { viewModel.stopCommand(command.id) }, enabled = !state.busy) {
                    Text("Stop")
                }
            }
            if (command.requiresReconciliation || command.state == CommandState.PAUSED) {
                OutlinedButton(onClick = { viewModel.reconcileCommand(command.id) }, enabled = !state.busy) {
                    Text("Reconcile history")
                }
            }
        }
        if (command.kind != CommandKind.WATCH_NOTE && command.input.isNotBlank() &&
            (command.state.terminal || command.requiresReconciliation || command.state == CommandState.PAUSED)) {
            Button(
                onClick = { viewModel.repeatRequest(command) },
                enabled = state.connectionReady && !state.busy,
            ) { Text("Send again as an explicit new request") }
        }
        if (command.targetProfileId != state.settings?.profileId) {
            Text("This item belongs to a previous connection profile. Review it in the original Hermes server; it will not use the new connection.")
        }
        if (state.conversationMessages.isNotEmpty()) {
            HorizontalDivider()
            Text("Recovered conversation history", style = MaterialTheme.typography.titleMedium)
            state.conversationMessages.asReversed().forEach { message ->
                Text(message.role ?: "Message", style = MaterialTheme.typography.labelMedium)
                SelectionContainer { Text(message.content) }
            }
        }
        OutlinedButton(onClick = viewModel::showHome) { Text("Back to recent items") }
    }
}

@Composable
private fun NotesScreen(state: HermesUiState, viewModel: AppViewModel, onRequestNotificationPermission: () -> Unit) {
    val notes = state.recent.filter { it.kind == CommandKind.WATCH_NOTE }
    val inkNotes by viewModel.inkNotes.collectAsStateWithLifecycle()
    val selectedInk by viewModel.selectedInkId.collectAsStateWithLifecycle()
    val selected = inkNotes.firstOrNull { it.id == selectedInk }
    if (selected != null) {
        HandwritingDetail(selected, viewModel::showNotes)
        return
    }
    Column(Modifier.fillMaxSize()) {
        Text("Local notes", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.padding(vertical = 8.dp))
        Text("Handwriting stays on your phone for you to review.", style = MaterialTheme.typography.bodySmall)
        TextButton(onClick = onRequestNotificationPermission) { Text("Allow note sync notifications") }
        LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            items(inkNotes, key = { "ink-${it.id}" }) { note ->
                Card(Modifier.fillMaxWidth().clickable { viewModel.openInkNote(note.id) }) {
                    Column(Modifier.padding(12.dp)) {
                        Text("Handwritten note", style = MaterialTheme.typography.titleMedium)
                        Text(java.text.DateFormat.getDateTimeInstance().format(java.util.Date(note.receivedAt)))
                        HandwritingCanvas(note.bytes, 48, maxCells = 12)
                    }
                }
            }
            items(notes, key = { "text-${it.id}" }) { note ->
                Card(Modifier.fillMaxWidth().clickable { viewModel.openCommand(note.id) }) {
                    Text(note.input, Modifier.padding(12.dp), maxLines = 3)
                }
            }
            if (notes.isEmpty() && inkNotes.isEmpty()) item { Text("No notes yet. Choose Handwritten note on your watch.", Modifier.padding(12.dp)) }
        }
    }
}

@Composable
private fun DiagnosticsScreen(
    state: HermesUiState,
    viewModel: AppViewModel,
    onRequestNotificationPermission: () -> Unit,
) {
    Column(
        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        HostPicker(state, viewModel)
        OutlinedButton(onClick = viewModel::testWatchLink, enabled = !state.busy) { Text("Open watch app & test link") }
        Text("This opens Hermes on connected watches and checks a complete round trip. Use the matching PBW from this release. RX means the phone received a message; TX Success alone is only a transport acknowledgment.")
        OutlinedButton(onClick = viewModel::testWatchAudio, enabled = !state.busy) {
            Text("Play test sound on watch")
        }
        Text("Keep Hermes open on your watch. Sends a short “Hello from your Pebble” voice clip to its speaker. No Hermes server or speech API is needed.")
        val audioProgress by viewModel.audioTestProgress.collectAsStateWithLifecycle()
        audioProgress?.let { Text(it) }
        OutlinedButton(onClick = viewModel::testConnection, enabled = state.connectionReady && !state.busy) {
            Text("Test Hermes API (read only)")
        }
        state.connectionTest?.let { SelectionContainer { Text(it) } }
        DiagnosticPanel(viewModel)
        HorizontalDivider()
        Text("Protocol", style = MaterialTheme.typography.titleMedium)
        Text("Version: ${state.protocolVersion}")
        Text("Watch UUID: ${dev.hermespebble.companion.pebble.WireProtocol.APP_UUID}")
        Text("Active watch apps: ${state.activeWatches.joinToString().ifBlank { "none" }}")
        HorizontalDivider()
        Text("Hermes capabilities", style = MaterialTheme.typography.titleMedium)
        Text("API root: ${state.settings?.serverUrl.orEmpty()}")
        Text("Bearer key stored: ${state.settings?.hasHermesKey == true}")
        Text("Access header: ${if (state.settings?.accessHeaderEnabled == true) state.settings.accessHeaderName else "disabled"}; value stored: ${state.settings?.hasAccessHeaderValue == true}")
        val capabilities = state.settings?.capabilities
        if (capabilities == null) {
            Text("Not verified")
        } else {
            Text("Required API routes available: ${capabilities.supportsRequiredV1Api}")
            Text("Run submission: ${capabilities.runSubmission}")
            Text("Idempotency supported/durable: ${capabilities.runsIdempotencySupported}/${capabilities.runsIdempotencyDurable}")
            Text("Advertised retention seconds: ${capabilities.runsIdempotencyRetentionSeconds ?: "unspecified"}")
        }
        HorizontalDivider()
        Text("Optional result notifications", style = MaterialTheme.typography.titleMedium)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Switch(
                checked = state.settings?.resultNotificationsEnabled == true,
                onCheckedChange = { enabled ->
                    if (enabled && Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        onRequestNotificationPermission()
                    }
                    viewModel.setResultNotificationsEnabled(enabled)
                },
            )
            Text("Show completed Hermes results", modifier = Modifier.padding(start = 8.dp))
        }
        Text("Notifications are not required to send. Lock-screen content is private.")
        Spacer(Modifier.height(12.dp))
    }
}

private val TERMINAL_STATES = setOf(
    CommandState.COMPLETED,
    CommandState.FAILED,
    CommandState.CANCELLED,
    CommandState.INTERRUPTED,
)

private fun CommandState.display(): String = when (this) {
    CommandState.NEEDS_APPROVAL -> "Approval needed in Hermes"
    CommandState.OUTCOME_UNKNOWN -> "Outcome unknown — review before retrying"
    CommandState.PAUSED -> "Waiting for the previous connection profile"
    else -> name.lowercase().replace('_', ' ').replaceFirstChar { it.uppercase() }
}

private fun Modifier.clickableCard(onClick: () -> Unit): Modifier = clickable(onClick = onClick)


@Composable
private fun HostPicker(state: HermesUiState, viewModel: AppViewModel) {
    Text("1. Connect your Pebble phone app", style = MaterialTheme.typography.titleMedium)
    Text(if (state.host.selected == null) "Required: choose the host below so watch messages can reach this app." else "Host selected. Reopen Hermes on the watch after changing it.")
    if (!state.host.loading && state.host.eligible.isEmpty()) {
        Text("No compatible PebbleKit 2 host found. Open or update the Pebble phone app, then refresh.")
    }
    state.host.eligible.forEach { packageName ->
        Row(
            modifier = Modifier.fillMaxWidth().selectable(
                selected = state.host.selected == packageName,
                onClick = { viewModel.selectHost(packageName) },
                enabled = !state.busy,
            ),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            RadioButton(selected = state.host.selected == packageName, onClick = null)
            Text(packageName, modifier = Modifier.padding(start = 8.dp))
        }
    }
    TextButton(onClick = viewModel::refreshHosts, enabled = !state.host.loading) { Text("Refresh Pebble hosts") }
}

@Composable
private fun DiagnosticPanel(viewModel: AppViewModel) {
    val events by viewModel.diagnosticEvents.collectAsStateWithLifecycle()
    val context = LocalContext.current
    var deviceStatus by remember { mutableStateOf("") }
    fun refreshDeviceStatus() {
        val connectivity = context.getSystemService(ConnectivityManager::class.java)
        val network = connectivity.getNetworkCapabilities(connectivity.activeNetwork)
        val power = context.getSystemService(PowerManager::class.java)
        deviceStatus = "Android ${Build.VERSION.RELEASE} / API ${Build.VERSION.SDK_INT}\n" +
            "Network present: ${network != null}; internet validated: ${network?.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED) == true}\n" +
            "VPN transport: ${network?.hasTransport(NetworkCapabilities.TRANSPORT_VPN) == true} (a reverse proxy does not require VPN)\n" +
            "Battery saver: ${power.isPowerSaveMode}; device idle: ${power.isDeviceIdleMode}"
    }
    LaunchedEffect(Unit) { refreshDeviceStatus() }
    HorizontalDivider()
    Text("Device and connection diagnostics", style = MaterialTheme.typography.titleMedium)
    SelectionContainer { Text(deviceStatus) }
    TextButton(onClick = { refreshDeviceStatus() }) { Text("Refresh device status") }
    Text("Last 150 events, kept in memory until the app process exits. Credentials and message bodies are excluded.")
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        OutlinedButton(onClick = {
            val report = "Hermes Pebble diagnostics\n$deviceStatus\n" + events.joinToString("\n")
            (context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
                .setPrimaryClip(ClipData.newPlainText("Hermes diagnostics", report))
        }) { Text("Copy report") }
        TextButton(onClick = viewModel::clearDiagnostics) { Text("Clear log") }
    }
    SelectionContainer {
        Text(events.asReversed().joinToString("\n\n").ifBlank { "No events yet. Run a test or reconnect the watch." },
            style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
    }
}
