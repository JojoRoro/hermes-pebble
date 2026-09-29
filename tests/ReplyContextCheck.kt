package dev.hermespebble.companion.network

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.*
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import okhttp3.MediaType.Companion.toMediaType
import okio.Buffer

fun main() = runBlocking {
    var version = "0.19.0"
    var healthStatus = 200
    val requests = mutableListOf<String>()
    val keys = mutableListOf<String?>()
    val session = "pt2_existing-session"
    val first = "Remind me to collect the boots tomorrow at 8 am"
    val answer = "Reminder set for tomorrow at 8 am"
    val followup = "Please remind me on Matrix instead"
    val responses = mutableMapOf<String, String>()
    val transport = OkHttpClient.Builder().addInterceptor { chain ->
        val request = chain.request()
        check(request.url.encodedPath.startsWith("/prefix/"))
        check(request.header("Authorization") == "Bearer fixture")
        check(request.header("X-NetBird-Access") == "fixture-access")
        var status = 200
        val response = when (request.url.encodedPath) {
            "/prefix/v1/capabilities" -> """{"object":"hermes.api_server.capabilities","features":{"run_submission":true},"endpoints":{"health":{"method":"GET","path":"/health"}}}"""
            "/prefix/health" -> { status = healthStatus; """{"platform":"hermes-agent","version":"$version"}""" }
            "/prefix/v1/runs" -> {
                val buffer = Buffer(); request.body!!.writeTo(buffer)
                val bytes = buffer.readUtf8(); requests += bytes; keys += request.header("Idempotency-Key")
                val body = Json.parseToJsonElement(bytes).jsonObject
                check(body["session_id"]!!.jsonPrimitive.content == session)
                val run = "run-${requests.size}"
                // 0.19.0 reads only explicit history, even when session_id already exists.
                val history = body["conversation_history"]?.jsonArray ?: JsonArray(emptyList())
                val sawContext = history.map { it.jsonObject["content"]!!.jsonPrimitive.content }.containsAll(listOf(first, answer))
                responses[run] = if (body["input"]!!.jsonPrimitive.content == first) answer else if (sawContext) "Same reminder, on Matrix" else "Context missing"
                status = 202
                """{"run_id":"$run","status":"queued"}"""
            }
            else -> {
                val run = request.url.pathSegments.last()
                buildJsonObject {
                    put("object", "hermes.run"); put("run_id", run); put("status", "completed")
                    put("session_id", session); put("output", responses.getValue(run))
                }.toString()
            }
        }
        Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(status).message("fixture")
            .body(response.toResponseBody("application/json".toMediaType())).build()
    }.build()
    val client = HermesClient(HermesConfigurationProvider {
        check(it == "profile")
        HermesRequestConfiguration("profile", "https://example.invalid/prefix/", "Bearer fixture", "X-NetBird-Access", "fixture-access")
    }, transport)
    val capabilities = client.getCapabilities("profile")
    check(capabilities.needsExplicitRunHistory && !capabilities.runsIdempotencySupported)
    val initial = RunConversationContext.prepare(null, null) { HermesRunSubmission(first, session) }
    val initialRun = client.submitRunPayload(initial.json, initial.sessionId, "first", "profile")
    check(client.getRun(initialRun.runId, "profile").output == answer)
    check("conversation_history" !in Json.parseToJsonElement(initial.json).jsonObject)
    val broken = client.submitRun(HermesRunSubmission(followup, session), "before-fix", "profile")
    check(client.getRun(broken.runId, "profile").output == "Context missing")
    val reply = RunConversationContext.prepare(null, null) {
        HermesRunSubmission(followup, session, RunConversationContext.recent(listOf(CompletedTurn(first, answer))))
    }
    val fixed = client.submitRunPayload(reply.json, reply.sessionId, "reply", "profile")
    check(client.getRun(fixed.runId, "profile").output == "Same reminder, on Matrix")
    val retry = RunConversationContext.prepare(reply.json, session) { error("Retry must not re-read history or session") }
    client.submitRunPayload(retry.json, retry.sessionId, "reply", "profile")
    check(requests[2] == requests[3] && keys[2] == keys[3])
    // Existing frozen requests from before the upgrade also stay byte-identical.
    check(RunConversationContext.prepare(initial.json, session) { error("Must preserve old payload") } == initial)
    try { RunConversationContext.prepare(initial.json, null) { error("Must not rebuild") }; error("Missing session accepted") }
    catch (_: IllegalArgumentException) { }
    version = "0.20.0"; check(!client.getCapabilities("profile").needsExplicitRunHistory)
    version = "dev"; check(!client.getCapabilities("profile").needsExplicitRunHistory)
    version = "0.19.0"; healthStatus = 404
    check(client.getCapabilities("profile").serverVersion == null)
    val oldCache = """{"run_submission":true,"runs_idempotency_supported":false,"runs_idempotency_durable":false,"runs_idempotency_retention_seconds":null,"endpoints":[]}"""
    check(Json.decodeFromString<HermesCapabilities>(oldCache).serverVersion == null)

    val turns = (30 downTo 1).map { CompletedTurn("User $it", "Answer $it") }
    val context = RunConversationContext.recent(turns)
    check(context.size == 40 && context.first().content == "User 11" && context.last().content == "Answer 30")
    check(context.chunked(2).all { it.map { m -> m.role } == listOf("user", "assistant") })
    val big = RunConversationContext.recent(listOf(CompletedTurn("🚀".repeat(40_000), "ü".repeat(70_000))))
    check(big.size == 2 && big.sumOf { it.content.toByteArray().size } <= RunConversationContext.MAX_BYTES)
    check(big.all { '\uFFFD' !in it.content && it.content.endsWith("[Earlier message shortened]") })
    check(RunConversationContext.recent(listOf(CompletedTurn("a".repeat(40_000), "b"))).first().content.length == 40_000)
    val bounded = RunConversationContext.recent(listOf(CompletedTurn("new", "answer"), CompletedTurn("old", "x".repeat(100_000))))
    check(bounded.size == 2 && bounded.first().content == "new")
    println("Reply context: reproduced 0.19.0 missing history; fixed follow-up, version detection, unchanged retries, UTF-8 and context bounds passed")
}
