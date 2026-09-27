package dev.hermespebble.companion.diagnostics

import java.time.Instant
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

/** Process-local, bounded metadata only. Never pass credentials, bodies or exception messages. */
object DiagnosticLog {
    private val entries = MutableStateFlow<List<String>>(emptyList())
    val events = entries.asStateFlow()
    fun record(component: String, detail: String) {
        android.util.Log.i("HermesLink", "[$component] $detail")
        entries.update { (it + "${Instant.now()} [$component] $detail").takeLast(150) }
    }
    fun clear() { entries.value = emptyList() }
}
