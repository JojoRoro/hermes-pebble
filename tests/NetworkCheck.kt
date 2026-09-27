package dev.hermespebble.companion.network

import kotlinx.coroutines.runBlocking
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import okhttp3.MediaType.Companion.toMediaType

fun main() = runBlocking {
    var responseText = ""
    var status = 200
    var contentType = "application/json"
    var calls = 0
    val transport = OkHttpClient.Builder().addInterceptor { chain ->
        calls++
        val request = chain.request()
        check(request.url.encodedPath.startsWith("/hermes/"))
        check(request.header("Authorization") == "Bearer test-only")
        check(request.header("X-NetBird-Access") == "test-access")
        Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(status).message("fixture")
            .body(responseText.toResponseBody(contentType.toMediaType())).build()
    }.build()
    val diagnostics = mutableListOf<String>()
    val client = HermesClient(HermesConfigurationProvider { expected ->
        check(expected == "profile")
        HermesRequestConfiguration("profile", "https://example.invalid/hermes/", "Bearer test-only", "X-NetBird-Access", "test-access")
    }, transport, diagnostics::add)
    responseText = """{"object":"hermes.run","run_id":"run-1","status":"completed","session_id":"session-1","output":"Hello 🚀","error":null} """
    val run = client.getRun("run-1", "profile")
    check(run.output == "Hello 🚀" && run.error == null)
    // Replayed acceptance may already be terminal; keep the run ID and monitor it.
    status = 202
    responseText = """{"run_id":"run-1","status":"completed","replayed":true}"""
    check(client.submitRunPayload("{}", "session-1", "key", "profile").runId == "run-1")
    status = 200
    responseText = """{"object":"list","session_id":"session-1","data":[{"id":1,"role":"assistant","content":null,"timestamp":123.5}],"pagination":{"limit":50,"offset":0,"order":"latest","returned":1}}"""
    check(client.getHistory("session-1", expectedProfileId = "profile").messages.single().serverId == "1")
    responseText = "<html>login</html>"
    contentType = "text/html"
    try {
        client.getRun("run-1", "profile")
        error("HTML must be rejected")
    } catch (e: HermesApiException) {
        check(e.category == HermesErrorCategory.INVALID_RESPONSE)
        check(e.message.contains("HTML") && e.message.contains("HTTP 200"))
    }
    check(diagnostics.any { "HTTP 200, HTML" in it })
    status = 302
    try {
        client.getRun("run-1", "profile")
        error("Redirect must be rejected")
    } catch (e: HermesApiException) { check(e.category == HermesErrorCategory.REDIRECT) }
    status = 200
    contentType = "application/json"
    responseText = "x".repeat(2 * 1024 * 1024 + 1)
    try {
        client.getRun("run-1", "profile")
        error("Oversized body must be rejected")
    } catch (e: HermesApiException) { check(e.category == HermesErrorCategory.INVALID_RESPONSE) }
    check(calls == 6)
    contentType = "application/json"
    responseText = """{"object":"hermes.api_server.capabilities","features":{"run_submission":true},"endpoints":{
        "create":{"method":"POST","path":"/api/sessions"},
        "session":{"method":"GET","path":"/api/sessions/{session_id}"},
        "run":{"method":"POST","path":"/v1/runs"},
        "status":{"method":"GET","path":"/v1/runs/{run_id}"},
        "stop":{"method":"POST","path":"/v1/runs/{run_id}/stop"},
        "history":{"method":"GET","path":"/api/sessions/{session_id}/messages"}}}"""
    val legacy = client.getCapabilities("profile")
    check(legacy.supportsRequiredV1Api)
    check(!legacy.runsIdempotencySupported && !legacy.runsIdempotencyDurable && legacy.safeRetentionSeconds == null)
    val legacyJson = responseText
    responseText = legacyJson.replace("\"run_submission\":true", "\"run_submission\":true,\"runs_idempotency\":{\"supported\":true,\"durable\":true,\"retention_seconds\":86400}")
    val modern = client.getCapabilities("profile")
    check(modern.supportsRequiredV1Api && modern.safeRetentionSeconds != null)
    responseText = legacyJson.replace("\"run_submission\":true", "\"run_submission\":true,\"runs_idempotency\":false")
    try {
        client.getCapabilities("profile")
        error("Present malformed optional field must not imply safe retries")
    } catch (e: HermesApiException) { check(e.message.contains("runs_idempotency")) }
    responseText = "{invalid-json"
    try {
        client.getCapabilities("profile")
        error("Malformed JSON must be diagnosed")
    } catch (e: HermesApiException) { check(e.message.contains("not valid JSON")) }
    responseText = legacyJson.replace("hermes.api_server.capabilities", "some.other.api")
    try {
        client.getCapabilities("profile")
        error("Wrong server must be diagnosed")
    } catch (e: HermesApiException) { check(e.category == HermesErrorCategory.UNSUPPORTED_API) }
    status = 202
    responseText = """{"run_id":"run-legacy","status":"queued"}"""
    check(!client.submitRunPayload("{}", "session-1", "legacy-key", "profile").replayed)
    responseText = """{"run_id":"run-legacy","status":"queued","replayed":"yes"}"""
    try {
        client.submitRunPayload("{}", "session-1", "legacy-key", "profile")
        error("Malformed replay flag must not be accepted")
    } catch (e: HermesApiException) { check(e.message.contains("replayed")) }
    check(diagnostics.none { "test-only" in it || "test-access" in it || "Hello" in it || "login</html>" in it })
    println("HTTP response, authentication, prefix, replay, and bounds checks passed (no network calls)")
}
