package dev.hermespebble.companion.data.preferences

import android.content.Context
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.longPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import dev.hermespebble.companion.network.HermesCapabilities
import dev.hermespebble.companion.security.SecretStore
import java.net.URI
import java.util.UUID
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onStart
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

private val Context.hermesSettingsDataStore by preferencesDataStore(name = "hermes_settings")

class SettingsRepository internal constructor(
    context: Context,
    private val secretStore: SecretStore,
    private val onProfileRotated: suspend (HermesSettings, HermesSettings) -> Unit = { _, _ -> },
) {
    private val dataStore = context.hermesSettingsDataStore
    private val updateMutex = Mutex()
    private val initializationMutex = Mutex()
    private val json = Json {
        encodeDefaults = true
        explicitNulls = false
        ignoreUnknownKeys = true
    }

    val settings: Flow<HermesSettings> = dataStore.data
        .map(::mapSettings)
        .onStart { ensureInitialized() }

    suspend fun current(): HermesSettings = ensureInitialized().let { mapSettings(it) }

    suspend fun update(update: SettingsUpdate): SettingsUpdateResult = updateMutex.withLock {
        updateLocked(update)
    }

    private suspend fun updateLocked(update: SettingsUpdate): SettingsUpdateResult {
        val oldSettings = ensureInitialized().let(::mapSettings)
        val finalUrl = ConnectionSettingsValidator.validateServerUrl(update.serverUrl)
        val headerRequired = update.accessHeaderEnabled
        ConnectionSettingsValidator.validateHeaderName(
            name = update.accessHeaderName,
            required = headerRequired,
        )
        val oldHermesKey = if (
            update.hermesKey is SecretChange.Keep &&
            oldSettings.hermesKeyReference.isNotEmpty()
        ) {
            secretStore.readHermesKey(oldSettings.hermesKeyReference)
        } else {
            null
        }
        val oldAccessHeaderValue = if (update.accessHeaderValue is SecretChange.Keep) {
            oldSettings.accessHeaderReference?.let { secretStore.readAccessHeaderValue(it) }
        } else {
            null
        }
        val hermesChanged = update.hermesKey is SecretChange.Set
        val newHermesKey = when (val change = update.hermesKey) {
            SecretChange.Keep -> oldHermesKey
            is SecretChange.Set -> change.value
            SecretChange.Clear -> throw SettingsValidationException("Hermes API key cannot be cleared")
        }
        val newAccessHeaderValue = when (val change = update.accessHeaderValue) {
            SecretChange.Keep -> oldAccessHeaderValue
            is SecretChange.Set -> change.value
            SecretChange.Clear -> null
        }
        val accessChanged = update.accessHeaderValue !is SecretChange.Keep
        if (headerRequired && newAccessHeaderValue == null) {
            throw SettingsValidationException("An access header value is required when the header is enabled")
        }
        if (newAccessHeaderValue != null) {
            SecretStore.validateAccessHeaderValue(newAccessHeaderValue)
        }
        val configurationChanged = finalUrl != oldSettings.serverUrl ||
            update.accessHeaderEnabled != oldSettings.accessHeaderEnabled ||
            update.accessHeaderName != oldSettings.accessHeaderName
        val secretsChanged = hermesChanged || accessChanged
        if (!configurationChanged && !secretsChanged) {
            return SettingsUpdateResult(oldSettings, false)
        }
        val secrets = if (secretsChanged) {
            secretStore.writeBundle(
                hermesKey = requireNotNull(newHermesKey) {
                    "Hermes API key is required before storing another secret"
                },
                accessHeaderValue = newAccessHeaderValue,
            )
        } else {
            null
        }
        val newProfileId = UUID.randomUUID().toString()
        val newProfileGeneration = oldSettings.profileGeneration + 1L
        var committed = false
        try {
            dataStore.edit { preferences ->
                preferences[SERVER_URL] = finalUrl
                preferences[ACCESS_HEADER_ENABLED] = update.accessHeaderEnabled
                preferences[ACCESS_HEADER_NAME] = update.accessHeaderName
                if (secrets != null) {
                    preferences[HERMES_KEY_REFERENCE] = secrets.hermesKeyReference
                    if (secrets.accessHeaderReference == null) {
                        preferences.remove(ACCESS_HEADER_REFERENCE)
                    } else {
                        preferences[ACCESS_HEADER_REFERENCE] = secrets.accessHeaderReference
                    }
                }
                preferences[PROFILE_ID] = newProfileId
                preferences[PROFILE_GENERATION] = newProfileGeneration
                preferences.remove(CAPABILITIES)
                preferences.remove(CAPABILITIES_CHECKED_AT)
            }
            committed = true
        } catch (error: Exception) {
            if (mapSettings(dataStore.data.first()).profileId == newProfileId) {
                committed = true
            }
            if (!committed) {
                if (secrets != null) secretStore.deleteBundle(secrets)
                throw error
            }
        }
        val newSettings = current()
        onProfileRotated(oldSettings, newSettings)
        if (secrets != null && oldSettings.hermesKeyReference.isNotEmpty()) {
            secretStore.deleteBundle(
                SecretStore.Bundle(
                    oldSettings.hermesKeyReference,
                    oldSettings.accessHeaderReference,
                ),
            )
        }
        return SettingsUpdateResult(newSettings, true)
    }

    suspend fun setHermesKey(value: String): SettingsUpdateResult = updateMutex.withLock {
        val current = current()
        updateLocked(
            SettingsUpdate(
                serverUrl = current.serverUrl,
                accessHeaderEnabled = current.accessHeaderEnabled,
                accessHeaderName = current.accessHeaderName,
                hermesKey = SecretChange.Set(value),
            ),
        )
    }

    suspend fun setAccessHeaderValue(value: String): SettingsUpdateResult = updateMutex.withLock {
        val current = current()
        updateLocked(
            SettingsUpdate(
                serverUrl = current.serverUrl,
                accessHeaderEnabled = current.accessHeaderEnabled,
                accessHeaderName = current.accessHeaderName,
                accessHeaderValue = SecretChange.Set(value),
            ),
        )
    }

    suspend fun clearAccessHeaderValue(): SettingsUpdateResult = updateMutex.withLock {
        val current = current()
        updateLocked(
            SettingsUpdate(
                serverUrl = current.serverUrl,
                accessHeaderEnabled = false,
                accessHeaderName = current.accessHeaderName,
                accessHeaderValue = SecretChange.Clear,
            ),
        )
    }

    suspend fun setAccessHeaderEnabled(enabled: Boolean): SettingsUpdateResult = updateMutex.withLock {
        val current = current()
        updateLocked(
            SettingsUpdate(
                serverUrl = current.serverUrl,
                accessHeaderEnabled = enabled,
                accessHeaderName = current.accessHeaderName,
            ),
        )
    }

    suspend fun setAccessHeaderName(name: String): SettingsUpdateResult = updateMutex.withLock {
        val current = current()
        updateLocked(
            SettingsUpdate(
                serverUrl = current.serverUrl,
                accessHeaderEnabled = current.accessHeaderEnabled,
                accessHeaderName = name,
            ),
        )
    }

    suspend fun newConversation(expectedGeneration: Long? = null): Long = updateMutex.withLock {
        ensureInitialized()
        var generation = 0L
        dataStore.edit { preferences ->
            val current = preferences[CONVERSATION_GENERATION] ?: 0L
            require(expectedGeneration == null || expectedGeneration <= current) { "Unknown conversation generation" }
            generation = if (expectedGeneration != null && expectedGeneration < current) current else current + 1L
            preferences[CONVERSATION_GENERATION] = generation
        }
        generation
    }

    suspend fun setResultNotificationsEnabled(enabled: Boolean) {
        updateMutex.withLock {
            ensureInitialized()
            dataStore.edit { preferences ->
                preferences[RESULT_NOTIFICATIONS_ENABLED] = enabled
            }
        }
    }

    suspend fun recordCapabilities(
        profileId: String,
        capabilities: HermesCapabilities,
        checkedAtMillis: Long = System.currentTimeMillis(),
    ): Boolean {
        ensureInitialized()
        var recorded = false
        val encoded = json.encodeToString(capabilities)
        dataStore.edit { preferences ->
            if (preferences[PROFILE_ID] == profileId) {
                preferences[CAPABILITIES] = encoded
                preferences[CAPABILITIES_CHECKED_AT] = checkedAtMillis
                recorded = true
            }
        }
        return recorded
    }

    private suspend fun ensureInitialized(): Preferences {
        return initializationMutex.withLock {
            val preferences = dataStore.data.first()
            if (preferences[PROFILE_ID] == null) {
                dataStore.edit {
                    it[PROFILE_ID] = UUID.randomUUID().toString()
                    it[PROFILE_GENERATION] = 0L
                    it[CONVERSATION_GENERATION] = 0L
                    it[SERVER_URL] = ""
                    it[ACCESS_HEADER_ENABLED] = false
                    it[ACCESS_HEADER_NAME] = "X-NetBird-Access"
                    it[RESULT_NOTIFICATIONS_ENABLED] = false
                }
            }
            return@withLock dataStore.data.first()
        }
    }

    private fun mapSettings(preferences: Preferences): HermesSettings {
        val storedCapabilities = preferences[CAPABILITIES]?.let { encoded ->
            runCatching { StoredCapabilities(json.decodeFromString<HermesCapabilities>(encoded), preferences[CAPABILITIES_CHECKED_AT] ?: 0L) }.getOrNull()
        }
        return HermesSettings(
            serverUrl = preferences[SERVER_URL] ?: "",
            accessHeaderEnabled = preferences[ACCESS_HEADER_ENABLED] ?: false,
            accessHeaderName = preferences[ACCESS_HEADER_NAME] ?: "X-NetBird-Access",
            hermesKeyReference = preferences[HERMES_KEY_REFERENCE].orEmpty(),
            accessHeaderReference = preferences[ACCESS_HEADER_REFERENCE],
            profileId = preferences[PROFILE_ID].orEmpty(),
            profileGeneration = preferences[PROFILE_GENERATION] ?: 0L,
            conversationGeneration = preferences[CONVERSATION_GENERATION] ?: 0L,
            capabilities = storedCapabilities?.capabilities,
            capabilitiesCheckedAtMillis = storedCapabilities?.checkedAtMillis,
            resultNotificationsEnabled = preferences[RESULT_NOTIFICATIONS_ENABLED] ?: false,
        )
    }

    private companion object {
        val SERVER_URL = stringPreferencesKey("server_url")
        val ACCESS_HEADER_ENABLED = booleanPreferencesKey("access_header_enabled")
        val ACCESS_HEADER_NAME = stringPreferencesKey("access_header_name")
        val HERMES_KEY_REFERENCE = stringPreferencesKey("hermes_key_reference")
        val ACCESS_HEADER_REFERENCE = stringPreferencesKey("access_header_reference")
        val PROFILE_ID = stringPreferencesKey("profile_id")
        val PROFILE_GENERATION = longPreferencesKey("profile_generation")
        val CONVERSATION_GENERATION = longPreferencesKey("conversation_generation")
        val CAPABILITIES = stringPreferencesKey("capabilities")
        val CAPABILITIES_CHECKED_AT = longPreferencesKey("capabilities_checked_at")
        val RESULT_NOTIFICATIONS_ENABLED = booleanPreferencesKey("result_notifications_enabled")
    }
}

data class HermesSettings(
    val serverUrl: String,
    val accessHeaderEnabled: Boolean,
    val accessHeaderName: String,
    val hermesKeyReference: String,
    val accessHeaderReference: String?,
    val profileId: String,
    val profileGeneration: Long,
    val conversationGeneration: Long,
    val capabilities: HermesCapabilities?,
    val capabilitiesCheckedAtMillis: Long?,
    val resultNotificationsEnabled: Boolean = false,
) {
    val hasHermesKey: Boolean
        get() = hermesKeyReference.isNotEmpty()

    val hasAccessHeaderValue: Boolean
        get() = accessHeaderReference != null
}

data class SettingsUpdate(
    val serverUrl: String,
    val accessHeaderEnabled: Boolean,
    val accessHeaderName: String,
    val hermesKey: SecretChange = SecretChange.Keep,
    val accessHeaderValue: SecretChange = SecretChange.Keep,
)

data class SettingsUpdateResult(
    val settings: HermesSettings,
    val profileRotated: Boolean,
)

sealed interface SecretChange {
    data object Keep : SecretChange
    data class Set(val value: String) : SecretChange
    data object Clear : SecretChange
}

private data class StoredCapabilities(
    val capabilities: HermesCapabilities,
    val checkedAtMillis: Long,
)

class SettingsValidationException(message: String) : Exception(message)

object ConnectionSettingsValidator {
    private val tokenPattern = Regex("[!#$%&'*+.^_`|~0-9A-Za-z-]+")
    private val encodedControlPattern = Regex("(?i)%(?:0[0-9a-f]|1[0-9a-f]|7f|8[0-9a-f]|9[0-9a-f])")
    private val encodedPathSeparatorPattern = Regex("(?i)%(?:2e|2f|5c)")
    private val reservedHeaderNames = setOf(
        "authorization",
        "host",
        "content-length",
        "content-type",
        "accept",
        "connection",
        "transfer-encoding",
        "idempotency-key",
        "x-netbird-user",
        "x-netbird-groups",
        "x-hermes",
        "x-hermes-request-id",
        "x-hermes-session-id",
        "x-hermes-run-id",
    )

    fun validateServerUrl(value: String): String {
        if (value.isEmpty() || value.length > MAX_URL_CHARS) {
            throw SettingsValidationException("Hermes server URL is empty or too long")
        }
        if (value.any { it.isWhitespace() || Character.isISOControl(it.code) || it.code in 0x7f..0x9f }) {
            throw SettingsValidationException("Hermes server URL contains whitespace or a control character")
        }
        if (encodedControlPattern.containsMatchIn(value)) {
            throw SettingsValidationException("Hermes server URL contains an encoded control character")
        }
        if (encodedPathSeparatorPattern.containsMatchIn(value)) {
            throw SettingsValidationException("Hermes server URL contains an ambiguous encoded path character")
        }
        val uri = try {
            URI(value)
        } catch (_: Exception) {
            throw SettingsValidationException("Hermes server URL is not a valid URI")
        }
        if (uri.scheme != "https" || uri.host.isNullOrBlank() || !uri.isAbsolute) {
            throw SettingsValidationException("Hermes server URL must be an absolute HTTPS URL")
        }
        if (uri.userInfo != null || uri.rawQuery != null || uri.rawFragment != null) {
            throw SettingsValidationException("Hermes server URL cannot contain credentials, a query, or a fragment")
        }
        if (uri.port !in -1..65535 || uri.port == 0) {
            throw SettingsValidationException("Hermes server URL contains an invalid port")
        }
        if (uri.rawPath.split('/').any { it == "." || it == ".." }) {
            throw SettingsValidationException("Hermes server URL cannot contain dot path segments")
        }
        return value
    }

    fun validateHeaderName(name: String, required: Boolean) {
        if (name.isEmpty()) {
            if (required) {
                throw SettingsValidationException("Access header name is required")
            }
            return
        }
        if (name.length > MAX_HEADER_NAME_CHARS || !tokenPattern.matches(name)) {
            throw SettingsValidationException("Access header name is not a valid HTTP token")
        }
        val lower = name.lowercase()
        if (lower in reservedHeaderNames || lower.startsWith("x-hermes-")) {
            throw SettingsValidationException("Access header name collides with a reserved HTTP or Hermes header")
        }
    }

    private const val MAX_URL_CHARS = 2048
    private const val MAX_HEADER_NAME_CHARS = 256
}
