package dev.hermespebble.companion.network

import java.io.IOException
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest
import java.time.ZonedDateTime
import java.time.format.DateTimeFormatter
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.longOrNull
import okhttp3.CookieJar
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response

fun interface HermesConfigurationProvider {
    suspend fun resolve(expectedProfileId: String?): HermesRequestConfiguration
}

class HermesClient internal constructor(
    private val configurationProvider: HermesConfigurationProvider,
    private val httpClient: OkHttpClient = defaultHttpClient,
    private val diagnostic: (String) -> Unit = {},
) {
    private val json = Json {
        explicitNulls = false
        ignoreUnknownKeys = true
        isLenient = false
    }

    suspend fun getCapabilities(expectedProfileId: String? = null): HermesCapabilities {
        val configuration = configurationProvider.resolve(expectedProfileId)
        val response = send(
            configuration = configuration,
            url = configuration.httpUrl().endpoint("v1", "capabilities"),
            method = "GET",
        )
        if (response.status == 404) {
            throw unsupported("Hermes does not expose the required capabilities route")
        }
        response.requireSuccess(unsupportedCategory = true)
        return try {
            parseCapabilities(response.requireJsonObject()).also {
                diagnostic("Capabilities parsed: required routes ${it.supportsRequiredV1Api}; idempotency supported ${it.runsIdempotencySupported}, durable ${it.runsIdempotencyDurable}")
            }
        } catch (error: HermesApiException) {
            diagnostic("Capabilities schema: ${error.message}")
            throw error
        }
    }

    suspend fun createOrReconcileSession(sessionId: String, expectedProfileId: String): HermesSession {
        validateIdentifier(sessionId, "session ID")
        val configuration = configurationProvider.resolve(expectedProfileId)
        val body = json.encodeToString(CreateSessionRequest(sessionId))
            .toRequestBody(JSON_MEDIA_TYPE)
        val response = send(
            configuration = configuration,
            url = configuration.httpUrl().endpoint("api", "sessions"),
            method = "POST",
            body = body,
        )
        if (response.status == 409 && response.errorCode() == SESSION_EXISTS) {
            return reconcileSession(configuration, sessionId)
        }
        if (response.status != 201) throw response.failure()
        val root = response.requireJsonObject()
        if (root.requiredString("object") != "hermes.session") throw invalidResponse()
        val session = root.requiredObject("session")
        val confirmedId = session.requiredString("id")
        validateIdentifier(confirmedId, "session ID")
        if (confirmedId != sessionId) {
            throw invalidResponse("Hermes confirmed a different session ID")
        }
        return HermesSession(confirmedId)
    }

    suspend fun reconcileSession(sessionId: String, expectedProfileId: String): HermesSession {
        val configuration = configurationProvider.resolve(expectedProfileId)
        return reconcileSession(configuration, sessionId)
    }

    private suspend fun reconcileSession(
        configuration: HermesRequestConfiguration,
        sessionId: String,
    ): HermesSession {
        validateIdentifier(sessionId, "session ID")
        val response = send(
            configuration = configuration,
            url = configuration.httpUrl().endpoint("api", "sessions", sessionId),
            method = "GET",
        )
        if (response.status == 404) throw HermesApiException(
            HermesErrorCategory.NOT_FOUND,
            "Hermes session was not found",
            404,
            response.retryAfterMillis,
        )
        response.requireSuccess()
        val root = response.requireJsonObject()
        if (root.requiredString("object") != "hermes.session") throw invalidResponse()
        val confirmedId = root.requiredObject("session").requiredString("id")
        validateIdentifier(confirmedId, "session ID")
        if (confirmedId != sessionId) {
            throw invalidResponse("Hermes returned a different session ID")
        }
        return HermesSession(confirmedId)
    }

    suspend fun submitRun(
        submission: HermesRunSubmission,
        idempotencyKey: String,
        expectedProfileId: String,
    ): RunAcceptance = submitRunPayload(
        payloadJson = json.encodeToString(submission),
        sessionId = submission.sessionId,
        idempotencyKey = idempotencyKey,
        expectedProfileId = expectedProfileId,
    )

    suspend fun submitRunPayload(
        payloadJson: String,
        sessionId: String,
        idempotencyKey: String,
        expectedProfileId: String,
    ): RunAcceptance {
        validateIdentifier(idempotencyKey, "idempotency key")
        validateIdentifier(sessionId, "session ID")
        val configuration = configurationProvider.resolve(expectedProfileId)
        val response = send(
            configuration = configuration,
            url = configuration.httpUrl().endpoint("v1", "runs"),
            method = "POST",
            body = payloadJson.toRequestBody(JSON_MEDIA_TYPE),
            additionalHeaders = mapOf("Idempotency-Key" to idempotencyKey),
        )
        if (response.status == 409) {
            throw HermesApiException(
                HermesErrorCategory.IDEMPOTENCY_CONFLICT,
                "Hermes reported an idempotency conflict",
                409,
                response.retryAfterMillis,
            )
        }
        if (response.status != 202) throw response.failure()
        val root = response.requireJsonObject()
        val runId = root.requiredString("run_id")
        validateIdentifier(runId, "run ID")
        val rawStatus = root.requiredString("status")
        val status = RunServerStatus.parse(rawStatus)
        // Pre-idempotency servers do not report replay metadata.
        val replayed = if ("replayed" in root) root.requiredBoolean("replayed") else false
        if (status == RunServerStatus.UNKNOWN || (!replayed && status !in setOf(RunServerStatus.QUEUED, RunServerStatus.STARTED))) {
            throw invalidResponse("Hermes returned an invalid run acceptance status")
        }
        return RunAcceptance(
            runId = runId,
            status = status,
            replayed = replayed,
        )
    }

    suspend fun getRun(runId: String, expectedProfileId: String): HermesRun {
        validateIdentifier(runId, "run ID")
        val configuration = configurationProvider.resolve(expectedProfileId)
        val response = send(
            configuration = configuration,
            url = configuration.httpUrl().endpoint("v1", "runs", runId),
            method = "GET",
        )
        if (response.status == 404 || response.status == 410) throw HermesApiException(
            HermesErrorCategory.NOT_FOUND,
            "Hermes run was not found",
            response.status,
            response.retryAfterMillis,
        )
        response.requireSuccess()
        return parseRun(response.requireJsonObject(), runId)
    }

    suspend fun stopRun(runId: String, expectedProfileId: String): HermesRun {
        validateIdentifier(runId, "run ID")
        val configuration = configurationProvider.resolve(expectedProfileId)
        val response = send(
            configuration = configuration,
            url = configuration.httpUrl().endpoint("v1", "runs", runId, "stop"),
            method = "POST",
            body = ByteArray(0).toRequestBody(null),
        )
        if (response.status != 200 && response.status != 202) throw response.failure()
        return parseStopResponse(response.requireJsonObject(), runId)
    }

    suspend fun getHistory(
        sessionId: String,
        limit: Int = HISTORY_LIMIT,
        offset: Int = 0,
        expectedProfileId: String,
    ): ConversationHistoryPage {
        validateIdentifier(sessionId, "session ID")
        if (limit !in 1..HISTORY_LIMIT || offset < 0) {
            throw HermesApiException(HermesErrorCategory.INVALID_SETTINGS, "History pagination is invalid")
        }
        val configuration = configurationProvider.resolve(expectedProfileId)
        val url = configuration.httpUrl()
            .endpoint("api", "sessions", sessionId, "messages")
            .newBuilder()
            .addQueryParameter("limit", limit.toString())
            .addQueryParameter("offset", offset.toString())
            .addQueryParameter("order", "latest")
            .build()
        val response = send(configuration, url, "GET")
        if (response.status == 404) throw HermesApiException(
            HermesErrorCategory.NOT_FOUND,
            "Hermes session history was not found",
            404,
            response.retryAfterMillis,
        )
        response.requireSuccess()
        val root = response.requireJsonObject()
        if (root.requiredString("object") != "list") throw invalidResponse()
        val confirmedSessionId = root.requiredString("session_id")
        validateIdentifier(confirmedSessionId, "session ID")
        // Hermes may resolve a compacted conversation to its successor session.
        val data = root.requiredArray("data")
        val pagination = root.requiredObject("pagination")
        val responseLimit = pagination.requiredInt("limit")
        val responseOffset = pagination.requiredInt("offset")
        val order = pagination.requiredString("order")
        val returned = pagination.requiredInt("returned")
        if (
            responseLimit != limit ||
            responseOffset != offset ||
            order != "latest" ||
            returned != data.size ||
            returned > responseLimit
        ) {
            throw invalidResponse("Hermes history pagination is invalid")
        }
        val messages = data.mapIndexed { index, element ->
            parseHistoryMessage(confirmedSessionId, index, element)
        }
        return ConversationHistoryPage(
            sessionId = confirmedSessionId,
            messages = messages,
            limit = responseLimit,
            offset = responseOffset,
            order = order,
            returned = returned,
        )
    }

    private suspend fun send(
        configuration: HermesRequestConfiguration,
        url: HttpUrl,
        method: String,
        body: RequestBody? = null,
        additionalHeaders: Map<String, String> = emptyMap(),
    ): RawResponse {
        val requestBuilder = Request.Builder()
            .url(url)
            .header("Authorization", configuration.authorization)
            .header("Accept", "application/json")
            .header("Cache-Control", "no-store")
        configuration.accessHeaderName?.let { name ->
            configuration.accessHeaderValue?.let { value -> requestBuilder.header(name, value) }
        }
        additionalHeaders.forEach { (name, value) -> requestBuilder.header(name, value) }
        when (method) {
            "GET" -> requestBuilder.get()
            "POST" -> requestBuilder.post(requireNotNull(body))
            else -> throw invalidResponse("Unsupported internal HTTP method")
        }
        return withContext(Dispatchers.IO) {
            val started = System.nanoTime()
            // No URL prefix, identifiers, headers or payloads enter the diagnostic log.
            val route = when {
                "capabilities" in url.pathSegments -> "capabilities"
                "runs" in url.pathSegments -> "runs"
                else -> "sessions"
            }
            diagnostic("$method $route: request started; bearer configured; access header ${configuration.accessHeaderName != null}")
            try {
                httpClient.newCall(requestBuilder.build()).execute().use { response ->
                    val media = response.body.contentType()
                    val type = when {
                        media.isJson() -> "JSON"
                        media?.subtype == "html" -> "HTML"
                        media == null -> "missing Content-Type"
                        else -> "non-JSON"
                    }
                    diagnostic("$method $route: HTTP ${response.code}, $type, length ${response.body.contentLength()}, ${(System.nanoTime() - started) / 1_000_000} ms")
                    try {
                        readResponse(response, requireJson = response.isSuccessful)
                    } catch (error: HermesApiException) {
                        diagnostic("$route: ${error.message}")
                        throw error
                    }
                }
            } catch (_: SSLException) {
                diagnostic("$route: TLS certificate/handshake failure")
                throw HermesApiException(HermesErrorCategory.TLS, "Hermes TLS validation failed")
            } catch (error: IOException) {
                val reason = when (error) {
                    is java.net.UnknownHostException -> "DNS lookup failed"
                    is java.net.SocketTimeoutException -> "Request timed out"
                    is java.net.ConnectException -> "Connection refused or unreachable"
                    else -> "Network request failed"
                }
                diagnostic("$route: $reason")
                throw HermesApiException(HermesErrorCategory.NETWORK, reason)
            }
        }
    }

    private fun readResponse(response: Response, requireJson: Boolean): RawResponse {
        val retryAfterMillis = parseRetryAfter(response.header("Retry-After"))
        val declaredLength = response.body.contentLength()
        if (declaredLength > MAX_RESPONSE_BYTES) {
            if (requireJson) throw invalidResponse("Hermes response is too large")
            return RawResponse(response.code, retryAfterMillis, null)
        }
        val contentType = response.body.contentType()
        if (requireJson && !contentType.isJson()) throw invalidResponse(
            "HTTP ${response.code}: expected JSON but received ${if (contentType?.subtype == "html") "HTML (web dashboard or proxy login page)" else "a non-JSON response"}. Check the API root and access header.",
        )
        val bytes = if (declaredLength == 0L) {
            ByteArray(0)
        } else {
            response.body.source().let { source ->
                val buffer = okio.Buffer()
                while (buffer.size <= MAX_RESPONSE_BYTES) {
                    val read = source.read(buffer, minOf(8192L, MAX_RESPONSE_BYTES + 1L - buffer.size))
                    if (read == -1L) break
                }
                buffer.readByteArray()
            }
        }
        if (bytes.size > MAX_RESPONSE_BYTES) {
            if (requireJson) throw invalidResponse("Hermes response is too large")
            return RawResponse(response.code, retryAfterMillis, null)
        }
        val text = if (requireJson) {
            decodeUtf8(bytes)
        } else {
            runCatching { decodeUtf8(bytes) }.getOrNull()
        }
        return RawResponse(response.code, retryAfterMillis, text)
    }

    private fun parseCapabilities(root: JsonObject): HermesCapabilities {
        if (root.requiredString("object") != "hermes.api_server.capabilities") throw unsupported("JSON is not Hermes Agent API capabilities. Check the API root and server version.")
        val features = root.requiredObject("features")
        // Older servers may omit optional idempotency support. Never invent replay guarantees.
        val idempotency = if ("runs_idempotency" in features) features.requiredObject("runs_idempotency") else null
        val retention = idempotency?.get("retention_seconds")?.let { value ->
            if (value is JsonNull) null else idempotency.requiredLong("retention_seconds")
        }
        val endpointObject = root.requiredObject("endpoints")
        if (endpointObject.size !in 1..MAX_ENDPOINTS) throw invalidResponse()
        val endpoints = endpointObject.values.map { value ->
            val endpoint = value as? JsonObject ?: throw invalidResponse()
            val method = endpoint.requiredString("method")
            val path = endpoint.requiredString("path")
            if (
                method !in setOf("GET", "POST", "PUT", "PATCH", "DELETE") ||
                !path.startsWith("/") ||
                path.length > MAX_ENDPOINT_PATH_CHARS
            ) {
                throw unsupported("Hermes advertised an invalid API route")
            }
            HermesEndpoint(method, path)
        }
        return HermesCapabilities(
            runSubmission = features.requiredBoolean("run_submission"),
            runsIdempotencySupported = idempotency?.requiredBoolean("supported") ?: false,
            runsIdempotencyDurable = idempotency?.requiredBoolean("durable") ?: false,
            runsIdempotencyRetentionSeconds = retention,
            endpoints = endpoints,
        )
    }

    private fun parseRun(root: JsonObject, expectedRunId: String): HermesRun {
        if (root.requiredString("object") != "hermes.run") throw invalidResponse()
        val runId = root.requiredString("run_id")
        validateIdentifier(runId, "run ID")
        if (runId != expectedRunId) throw invalidResponse("Hermes returned a different run ID")
        val rawStatus = root.requiredString("status")
        val status = RunServerStatus.parse(rawStatus)
        val output = root.optionalString("output")
        if (status == RunServerStatus.COMPLETED && output == null) throw invalidResponse()
        if (output != null && output.length > MAX_OUTPUT_CHARS) {
            throw invalidResponse("Hermes run output is too large")
        }
        val sessionId = root.optionalString("session_id")
        sessionId?.let { validateIdentifier(it, "session ID") }
        return HermesRun(
            runId = runId,
            status = status,
            rawStatus = rawStatus,
            sessionId = sessionId,
            output = output,
            error = parseRunError(root["error"]),
        )
    }

    private fun parseStopResponse(root: JsonObject, expectedRunId: String): HermesRun {
        root.optionalString("object")?.let { objectType ->
            if (objectType != "hermes.run") throw invalidResponse()
        }
        val returnedRunId = root.optionalString("run_id") ?: expectedRunId
        validateIdentifier(returnedRunId, "run ID")
        if (returnedRunId != expectedRunId) throw invalidResponse("Hermes returned a different run ID")
        val rawStatus = root.requiredString("status")
        val status = RunServerStatus.parse(rawStatus)
        val output = root.optionalString("output")
        if (output != null && output.length > MAX_OUTPUT_CHARS) {
            throw invalidResponse("Hermes run output is too large")
        }
        val sessionId = root.optionalString("session_id")
        sessionId?.let { validateIdentifier(it, "session ID") }
        return HermesRun(
            runId = expectedRunId,
            status = status,
            rawStatus = rawStatus,
            sessionId = sessionId,
            output = output,
            error = parseRunError(root["error"]),
        )
    }

    private fun parseHistoryMessage(
        sessionId: String,
        index: Int,
        element: JsonElement,
    ): ConversationMessage {
        val message = element as? JsonObject ?: throw invalidResponse()
        val role = message.optionalString("role")?.lowercase()
        if (role != null && role !in setOf("user", "assistant", "system", "tool")) {
            throw invalidResponse("Hermes history contains an invalid role")
        }
        val createdAt = message.optionalScalar("timestamp") ?: message.optionalScalar("created_at")
        val content = parseMessageContent(message["content"])
        if (content.length > MAX_HISTORY_CONTENT_CHARS) throw invalidResponse()
        val serverId = message.optionalScalar("id")
        val digest = MessageDigest.getInstance("SHA-256")
        digest.update(sessionId.toByteArray(Charsets.UTF_8))
        role?.let {
            digest.update(0)
            digest.update(it.toByteArray(Charsets.UTF_8))
        }
        digest.update(0)
        digest.update(createdAt?.toByteArray(Charsets.UTF_8) ?: byteArrayOf())
        digest.update(0)
        digest.update(content.toByteArray(Charsets.UTF_8))
        digest.update(0)
        digest.update((serverId ?: "").toByteArray(Charsets.UTF_8))
        return ConversationMessage(
            fingerprint = digest.digest().joinToString("") { byte -> "%02x".format(byte) },
            serverId = serverId,
            role = role,
            content = content,
            createdAt = createdAt,
        )
    }

    private fun parseMessageContent(element: JsonElement?): String {
        if (element == null || element is JsonNull) return "[No text content; inspect this tool event in Hermes]"
        val primitive = element as? JsonPrimitive
        if (primitive != null && primitive.isString) {
            return primitive.contentOrNull ?: throw invalidResponse()
        }
        val blocks = element as? JsonArray ?: throw invalidResponse()
        val text = buildString {
            blocks.forEachIndexed { index, block ->
                val blockObject = block as? JsonObject ?: throw invalidResponse()
                if (index > 0) append('\n')
                if (blockObject.requiredString("type") == "text") append(blockObject.requiredString("text"))
                else append("[Non-text content; inspect in Hermes]")
                if (length > MAX_HISTORY_CONTENT_CHARS) throw invalidResponse()
            }
        }
        return text
    }

    private fun parseRunError(element: JsonElement?): String? {
        if (element == null || element is JsonNull) return null
        val primitive = element as? JsonPrimitive ?: return "Hermes reported an error"
        val text = primitive.contentOrNull ?: return "Hermes reported an error"
        return text
            .map { if (Character.isISOControl(it.code) || it.code in 0x7f..0x9f) ' ' else it }
            .joinToString("")
            .take(MAX_RUN_ERROR_CHARS)
    }

    private fun JsonObject.requiredObject(name: String): JsonObject =
        this[name] as? JsonObject ?: throw invalidResponse("Expected JSON object field: $name")

    private fun JsonObject.requiredArray(name: String): JsonArray =
        this[name] as? JsonArray ?: throw invalidResponse("Expected JSON array field: $name")

    private fun JsonObject.requiredString(name: String): String {
        val primitive = this[name] as? JsonPrimitive ?: throw invalidResponse("Missing or invalid JSON field: $name")
        if (!primitive.isString) throw invalidResponse("Missing or invalid JSON field: $name")
        return primitive.contentOrNull ?: throw invalidResponse("Missing or invalid JSON field: $name")
    }

    private fun JsonObject.optionalString(name: String): String? {
        val value = this[name] ?: return null
        if (value is JsonNull) return null
        val primitive = value as? JsonPrimitive ?: throw invalidResponse("Missing or invalid JSON field: $name")
        if (!primitive.isString) throw invalidResponse("Missing or invalid JSON field: $name")
        return primitive.contentOrNull ?: throw invalidResponse("Missing or invalid JSON field: $name")
    }

    private fun JsonObject.optionalScalar(name: String): String? {
        val value = this[name] ?: return null
        if (value is JsonNull) return null
        val primitive = value as? JsonPrimitive ?: throw invalidResponse("Missing or invalid JSON field: $name")
        return primitive.contentOrNull
    }

    private fun JsonObject.requiredBoolean(name: String): Boolean {
        val primitive = this[name] as? JsonPrimitive ?: throw invalidResponse("Missing or invalid JSON field: $name")
        if (primitive.isString) throw invalidResponse("Missing or invalid JSON field: $name")
        return primitive.booleanOrNull ?: throw invalidResponse("Missing or invalid JSON field: $name")
    }

    private fun JsonObject.requiredLong(name: String): Long {
        val primitive = this[name] as? JsonPrimitive ?: throw invalidResponse("Missing or invalid JSON field: $name")
        if (primitive.isString) throw invalidResponse("Missing or invalid JSON field: $name")
        return primitive.longOrNull ?: throw invalidResponse("Missing or invalid JSON field: $name")
    }

    private fun JsonObject.requiredInt(name: String): Int {
        val primitive = this[name] as? JsonPrimitive ?: throw invalidResponse("Missing or invalid JSON field: $name")
        val value = primitive.longOrNull ?: throw invalidResponse("Missing or invalid JSON field: $name")
        if (value !in Int.MIN_VALUE.toLong()..Int.MAX_VALUE.toLong()) throw invalidResponse("Missing or invalid JSON field: $name")
        return value.toInt()
    }

    private fun HermesRequestConfiguration.httpUrl(): HttpUrl =
        baseUrl.toHttpUrlOrNull() ?: throw invalidSettings("Hermes server URL is invalid")

    private fun HttpUrl.endpoint(vararg segments: String): HttpUrl = newBuilder()
        .apply { segments.forEach { segment -> addPathSegment(segment) } }
        .build()

    private fun RawResponse.requireSuccess(unsupportedCategory: Boolean = false) {
        if (status in 200..299) return
        if (unsupportedCategory && status in 404..405) {
            throw unsupported("Hermes does not expose the required API routes")
        }
        throw failure()
    }

    private fun RawResponse.requireJsonObject(): JsonObject {
        if (status !in 200..299) throw failure()
        val element = try {
            json.parseToJsonElement(bodyText.orEmpty())
        } catch (_: Exception) {
            throw invalidResponse("HTTP $status: response body is not valid JSON")
        }
        return element as? JsonObject ?: throw invalidResponse("HTTP $status: expected a JSON object")
    }

    private fun RawResponse.failure(): HermesApiException {
        val category = when (status) {
            in 300..399 -> HermesErrorCategory.REDIRECT
            401, 403 -> HermesErrorCategory.AUTHENTICATION
            404 -> HermesErrorCategory.NOT_FOUND
            409 -> if (errorCode() == IDEMPOTENCY_CONFLICT) {
                HermesErrorCategory.IDEMPOTENCY_CONFLICT
            } else {
                HermesErrorCategory.REQUEST_REJECTED
            }
            429 -> HermesErrorCategory.RATE_LIMITED
            in 400..499 -> HermesErrorCategory.REQUEST_REJECTED
            in 500..599 -> HermesErrorCategory.SERVER
            else -> HermesErrorCategory.INVALID_RESPONSE
        }
        val message = when (category) {
            HermesErrorCategory.REDIRECT -> "Hermes redirected an authenticated request"
            HermesErrorCategory.AUTHENTICATION -> "Authentication rejected; Hermes versus access proxy is indeterminate"
            HermesErrorCategory.NOT_FOUND -> "The requested Hermes resource was not found"
            HermesErrorCategory.IDEMPOTENCY_CONFLICT -> "Hermes reported an idempotency conflict"
            HermesErrorCategory.RATE_LIMITED -> "Hermes rate-limited the request"
            HermesErrorCategory.REQUEST_REJECTED -> "Hermes rejected the request"
            HermesErrorCategory.SERVER -> "Hermes returned a server error"
            else -> "Hermes returned an unexpected HTTP response"
        }
        return HermesApiException(category, "HTTP $status: $message", status, retryAfterMillis)
    }

    private fun RawResponse.errorCode(): String? {
        val root = try {
            json.parseToJsonElement(bodyText.orEmpty()) as? JsonObject
        } catch (_: Exception) {
            null
        } ?: return null
        val error = root["error"] as? JsonObject ?: return null
        return (error["code"] as? JsonPrimitive)?.contentOrNull
    }

    private fun decodeUtf8(bytes: ByteArray): String = try {
        Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(bytes))
            .toString()
    } catch (_: Exception) {
        throw invalidResponse("Hermes response is not valid UTF-8")
    }

    private fun parseRetryAfter(value: String?): Long? {
        val raw = value?.trim()?.takeIf { it.isNotEmpty() } ?: return null
        raw.toLongOrNull()?.let { seconds ->
            return seconds.coerceIn(0L, MAX_RETRY_AFTER_SECONDS) * 1000L
        }
        val retryAt = try {
            ZonedDateTime.parse(raw, DateTimeFormatter.RFC_1123_DATE_TIME).toInstant().toEpochMilli()
        } catch (_: Exception) {
            return null
        }
        return (retryAt - System.currentTimeMillis()).coerceIn(0L, MAX_RETRY_AFTER_SECONDS * 1000L)
    }

    private fun validateIdentifier(value: String, label: String) {
        if (
            value.isEmpty() ||
            value.length > MAX_IDENTIFIER_CHARS ||
            value.any { Character.isISOControl(it.code) || it.code in 0x7f..0x9f }
        ) {
            throw invalidResponse("Hermes $label is invalid")
        }
    }

    private fun invalidResponse(message: String = "Hermes returned an invalid response") =
        HermesApiException(HermesErrorCategory.INVALID_RESPONSE, message)

    private fun unsupported(message: String) =
        HermesApiException(HermesErrorCategory.UNSUPPORTED_API, message)

    private fun invalidSettings(message: String) =
        HermesApiException(HermesErrorCategory.INVALID_SETTINGS, message)

    private data class RawResponse(
        val status: Int,
        val retryAfterMillis: Long?,
        val bodyText: String?,
    )

    @Serializable
    private data class CreateSessionRequest(
        @SerialName("id") val id: String,
    )

    companion object {
        const val HISTORY_LIMIT = 50
        private const val MAX_RESPONSE_BYTES = 2 * 1024 * 1024
        private const val MAX_OUTPUT_CHARS = 1_500_000
        private const val MAX_HISTORY_CONTENT_CHARS = 100_000
        private const val MAX_RUN_ERROR_CHARS = 2048
        private const val MAX_IDENTIFIER_CHARS = 256
        private const val MAX_ENDPOINT_PATH_CHARS = 512
        private const val MAX_ENDPOINTS = 100
        private const val MAX_RETRY_AFTER_SECONDS = 24L * 60L * 60L
        private const val SESSION_EXISTS = "session_exists"
        private const val IDEMPOTENCY_CONFLICT = "idempotency_conflict"
        private val JSON_MEDIA_TYPE = "application/json; charset=utf-8".toMediaType()
        private val defaultHttpClient = OkHttpClient.Builder()
            .connectTimeout(20, TimeUnit.SECONDS)
            .readTimeout(45, TimeUnit.SECONDS)
            .writeTimeout(45, TimeUnit.SECONDS)
            .callTimeout(60, TimeUnit.SECONDS)
            .followRedirects(false)
            .followSslRedirects(false)
            .cookieJar(CookieJar.NO_COOKIES)
            .retryOnConnectionFailure(false)
            .build()
    }
}

private fun okhttp3.MediaType?.isJson(): Boolean {
    if (this == null) return false
    return type.equals("application", ignoreCase = true) &&
        (subtype.equals("json", ignoreCase = true) || subtype.endsWith("+json", ignoreCase = true))
}
