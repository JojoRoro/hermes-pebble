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
    val client = HermesClient(HermesConfigurationProvider { expected ->
        check(expected == "profile")
        HermesRequestConfiguration("profile", "https://example.invalid/hermes/", "Bearer test-only", "X-NetBird-Access", "test-access")
    }, transport)
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
    } catch (e: HermesApiException) { check(e.category == HermesErrorCategory.INVALID_RESPONSE) }
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
    println("HTTP response, authentication, prefix, replay, and bounds checks passed (no network calls)")
}
