package dev.hermespebble.companion.network

import dev.hermespebble.companion.data.preferences.ConnectionSettingsValidator
import dev.hermespebble.companion.data.preferences.SettingsRepository
import dev.hermespebble.companion.security.SecretStore
import dev.hermespebble.companion.security.SecretStoreException
import kotlinx.coroutines.CancellationException
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull

class HermesRequestConfigurationProvider internal constructor(
    private val settingsRepository: SettingsRepository,
    private val secretStore: SecretStore,
) : HermesConfigurationProvider {
    override suspend fun resolve(expectedProfileId: String?): HermesRequestConfiguration {
        val settings = try {
            settingsRepository.current()
        } catch (error: CancellationException) {
            throw error
        } catch (_: Exception) {
            throw invalidSettings("Connection settings are unavailable")
        }
        if (expectedProfileId != null && settings.profileId != expectedProfileId) {
            throw HermesApiException(HermesErrorCategory.PROFILE_CHANGED, "Connection profile changed")
        }
        val serverUrl = try {
            ConnectionSettingsValidator.validateServerUrl(settings.serverUrl)
        } catch (_: Exception) {
            throw invalidSettings("Hermes server URL is invalid or incomplete")
        }
        if (!settings.hasHermesKey) {
            throw invalidSettings("Hermes API key is required")
        }
        if (settings.accessHeaderEnabled) {
            if (settings.accessHeaderReference == null) {
                throw invalidSettings("Access header value is required")
            }
            try {
                ConnectionSettingsValidator.validateHeaderName(settings.accessHeaderName, required = true)
            } catch (error: CancellationException) {
                throw error
            } catch (_: Exception) {
                throw invalidSettings("Access header configuration is invalid")
            }
        }
        val secrets = try {
            secretStore.readBundle(
                settings.hermesKeyReference,
                settings.accessHeaderReference.takeIf { settings.accessHeaderEnabled },
            )
        } catch (error: CancellationException) {
            throw error
        } catch (_: SecretStoreException) {
            throw invalidSettings("Stored credentials are unavailable; enter them again")
        } catch (_: Exception) {
            throw invalidSettings("Stored credentials could not be read")
        }
        if (settings.accessHeaderEnabled && secrets.accessHeaderValue == null) {
            throw invalidSettings("Access header value is required")
        }
        val baseUrl = serverUrl.toHttpUrlOrNull()
            ?.takeIf { it.username.isEmpty() && it.password.isEmpty() && it.querySize == 0 && it.fragment == null }
            ?: throw invalidSettings("Hermes server URL is invalid")
        if (baseUrl.scheme != "https" || baseUrl.host.isEmpty()) {
            throw invalidSettings("Hermes server URL is invalid")
        }
        val accessHeaderName = settings.accessHeaderName.takeIf {
            settings.accessHeaderEnabled && it.isNotEmpty()
        }
        return HermesRequestConfiguration(
            profileId = settings.profileId,
            baseUrl = baseUrl.toString(),
            authorization = "Bearer ${secrets.hermesKey}",
            accessHeaderName = accessHeaderName,
            accessHeaderValue = secrets.accessHeaderValue.takeIf { settings.accessHeaderEnabled },
        )
    }

    private fun invalidSettings(message: String) = HermesApiException(
        HermesErrorCategory.INVALID_SETTINGS,
        message,
    )
}

