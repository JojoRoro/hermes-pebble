package dev.hermespebble.companion.dispatch

import android.content.Context
import androidx.work.CoroutineWorker
import androidx.work.WorkerParameters
import dev.hermespebble.companion.data.local.CommandKind
import dev.hermespebble.companion.data.local.CommandState
import dev.hermespebble.companion.data.local.CommandValidationException
import dev.hermespebble.companion.data.local.ConcurrentCommandMutationException
import dev.hermespebble.companion.data.local.FrozenPayloadConflictException
import dev.hermespebble.companion.data.local.ImmutableCommandConflictException
import dev.hermespebble.companion.network.HermesApiException
import dev.hermespebble.companion.network.HermesErrorCategory
import dev.hermespebble.companion.network.HermesRunSubmission
import dev.hermespebble.companion.network.RunConversationContext
import dev.hermespebble.companion.network.RunServerStatus
import dev.hermespebble.companion.network.RetryDecision
import dev.hermespebble.companion.network.retryDecision
import java.util.UUID
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay

class CommandSubmissionWorker(
    appContext: Context,
    workerParameters: WorkerParameters,
) : CoroutineWorker(appContext, workerParameters) {
    private val dependencies = workerDependencies(appContext)

    override suspend fun doWork(): Result {
        val commandId = inputData.getLong(COMMAND_ID, -1L)
        if (commandId < 0) return Result.failure()
        val initial = dependencies.commandRepository.get(commandId) ?: return Result.success()
        if (
            initial.state.terminal ||
            initial.state == CommandState.PAUSED ||
            initial.state == CommandState.OUTCOME_UNKNOWN ||
            initial.kind == CommandKind.WATCH_NOTE
        ) {
            return Result.success()
        }
        val settings = dependencies.settingsRepository.current()
        if (initial.targetProfileId != settings.profileId) {
            dependencies.commandRepository.markPaused(
                commandId,
                HermesErrorCategory.PROFILE_CHANGED,
                "Waiting for the previous connection profile",
                requiresReconciliation = initial.runId != null || initial.submissionAttemptCount > 0,
            )
            return Result.success()
        }
        initial.nextAttemptAt?.let { nextAttemptAt ->
            if (nextAttemptAt > dependencies.clock()) {
                val wait = nextAttemptAt - dependencies.clock()
                if (wait > CommandDispatcher.MAX_WORK_DELAY_MILLIS) return Result.retry()
                delay(wait)
            }
        }
        val owner = UUID.randomUUID().toString()
        val command = dependencies.commandRepository.acquireSubmission(
            commandId = commandId,
            profileId = settings.profileId,
            owner = owner,
            nowMillis = dependencies.clock(),
            leaseMillis = SUBMISSION_LEASE_MILLIS,
        )
        if (command == null) {
            val latest = dependencies.commandRepository.get(commandId) ?: return Result.success()
            return if (
                latest.state == CommandState.SUBMITTING &&
                !dependencies.commandRepository.hasEarlierBlockingCommand(
                    latest.id,
                    latest.targetProfileId,
                    latest.conversationGeneration,
                )
            ) {
                Result.retry()
            } else {
                Result.success()
            }
        }
        return try {
            val capabilities = ensureCapabilities(commandId, settings.profileId)
            val beforeSession = dependencies.settingsRepository.current()
            if (beforeSession.profileId != command.targetProfileId) {
                dependencies.commandRepository.markPaused(
                    commandId,
                    HermesErrorCategory.PROFILE_CHANGED,
                    "Waiting for the previous connection profile",
                    requiresReconciliation = false,
                )
                return Result.success()
            }
            val prepared = RunConversationContext.prepare(command.frozenPayloadJson, command.submittedSessionId) {
                val session = dependencies.commandRepository.getOrCreateSession(
                    command.targetProfileId,
                    command.conversationGeneration,
                )
                val confirmedSession = if (session.serverConfirmed) {
                    session
                } else {
                    val remote = dependencies.hermesClient.createOrReconcileSession(session.sessionId, command.targetProfileId)
                    dependencies.commandRepository.confirmSession(
                        command.targetProfileId,
                        command.conversationGeneration,
                        remote.id,
                    ) ?: throw HermesApiException(
                        HermesErrorCategory.INVALID_RESPONSE,
                        "Hermes session mapping changed unexpectedly",
                    )
                }
                val history = if (capabilities.needsExplicitRunHistory) {
                    dependencies.commandRepository.replyHistory(command).takeIf { it.isNotEmpty() }
                } else null
                HermesRunSubmission(command.input, confirmedSession.sessionId, history)
            }
            val frozen = dependencies.commandRepository.ensureFrozenPayload(
                commandId = commandId,
                owner = owner,
                sessionId = prepared.sessionId,
                payloadJson = prepared.json,
            )
            val persistedPayload = frozen.frozenPayloadJson
                ?: throw FrozenPayloadConflictException()
            val beforeSubmission = dependencies.settingsRepository.current()
            if (beforeSubmission.profileId != frozen.targetProfileId) {
                dependencies.commandRepository.markPaused(
                    commandId,
                    HermesErrorCategory.PROFILE_CHANGED,
                    "Waiting for the previous connection profile",
                    requiresReconciliation = false,
                )
                return Result.success()
            }
            // WorkManager may wake after the server's idempotency retention expired.
            // Recheck immediately before every repeat, not merely when scheduling it.
            if (frozen.firstSubmissionAttemptAt != null &&
                beforeSubmission.capabilities?.retryDecision(frozen.firstSubmissionAttemptAt, dependencies.clock()) != RetryDecision.SAFE
            ) {
                dependencies.commandRepository.markOutcomeUnknown(commandId)
                return Result.success()
            }
            val attempting = dependencies.commandRepository.markSubmissionAttempt(
                commandId,
                owner,
                dependencies.clock(),
            )
            if (attempting.state == CommandState.CANCELLED) {
                dependencies.dispatcher.enqueueFollowers(attempting.targetProfileId, attempting.conversationGeneration)
                return Result.success()
            }
            val acceptance = dependencies.hermesClient.submitRunPayload(
                payloadJson = persistedPayload,
                sessionId = prepared.sessionId,
                idempotencyKey = frozen.idempotencyKey,
                expectedProfileId = frozen.targetProfileId,
            )
            val accepted = dependencies.commandRepository.markAccepted(
                commandId = commandId,
                runId = acceptance.runId,
                serverState = acceptance.status.name.lowercase(),
                replayed = acceptance.replayed,
            )
            if (accepted.targetProfileId == dependencies.settingsRepository.current().profileId) {
                if (accepted.stopRequested) dependencies.dispatcher.enqueueStopNow(commandId)
                dependencies.dispatcher.enqueueAccepted(commandId)
            } else {
                dependencies.commandRepository.markPaused(
                    commandId,
                    HermesErrorCategory.PROFILE_CHANGED,
                    "Run was accepted under the previous connection profile",
                    requiresReconciliation = true,
                )
            }
            Result.success()
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            handleSubmissionFailure(commandId, owner, error)
        }
    }

    private suspend fun ensureCapabilities(
        commandId: Long,
        profileId: String,
    ): dev.hermespebble.companion.network.HermesCapabilities {
        val settings = dependencies.settingsRepository.current()
        if (settings.profileId != profileId) {
            throw HermesApiException(
                HermesErrorCategory.PROFILE_CHANGED,
                "Connection settings changed during submission",
            )
        }
        val cached = settings.capabilities
        // Re-probe pre-upgrade caches once; refresh version after a server update.
        val fresh = settings.capabilitiesCheckedAtMillis?.let { dependencies.clock() - it in 0 until CAPABILITY_CACHE_MILLIS } == true
        if (cached != null && cached.serverVersion != null && fresh) {
            if (!cached.supportsRequiredV1Api) {
                throw HermesApiException(
                    HermesErrorCategory.UNSUPPORTED_API,
                    "Hermes does not support the required v1 asynchronous run API",
                )
            }
            return cached
        }
        val capabilities = dependencies.hermesClient.getCapabilities(profileId)
        val after = dependencies.settingsRepository.current()
        if (after.profileId != profileId) {
            throw HermesApiException(
                HermesErrorCategory.PROFILE_CHANGED,
                "Connection settings changed during capability inspection",
            )
        }
        dependencies.settingsRepository.recordCapabilities(profileId, capabilities, dependencies.clock())
        if (!capabilities.supportsRequiredV1Api) {
            dependencies.commandRepository.markPaused(
                commandId,
                HermesErrorCategory.UNSUPPORTED_API,
                "Hermes does not support the required v1 asynchronous run API",
                requiresReconciliation = false,
            )
            throw HermesApiException(
                HermesErrorCategory.UNSUPPORTED_API,
                "Hermes does not support the required v1 asynchronous run API",
            )
        }
        return capabilities
    }

    private suspend fun handleSubmissionFailure(
        commandId: Long,
        owner: String,
        error: Exception,
    ): Result {
        val command = dependencies.commandRepository.get(commandId) ?: return Result.success()
        if (command.state == CommandState.PAUSED || command.state.terminal) return Result.success()
        if (error is CommandValidationException ||
            error is FrozenPayloadConflictException ||
            error is ConcurrentCommandMutationException ||
            error is ImmutableCommandConflictException
        ) {
            dependencies.commandRepository.markFailed(
                commandId,
                HermesErrorCategory.INTERNAL,
                "Local durable submission state is inconsistent",
            )
            dependencies.dispatcher.enqueueFollowers(command.targetProfileId, command.conversationGeneration)
            return Result.success()
        }
        val apiError = error as? HermesApiException
        val category = apiError?.category ?: HermesErrorCategory.INTERNAL
        val message = apiError?.message ?: "Hermes submission failed"
        if (command.firstSubmissionAttemptAt == null) {
            return when (category) {
                HermesErrorCategory.AUTHENTICATION,
                HermesErrorCategory.INVALID_SETTINGS,
                HermesErrorCategory.REDIRECT,
                HermesErrorCategory.UNSUPPORTED_API,
                -> {
                    val stopped = dependencies.commandRepository.markPaused(
                        commandId,
                        category,
                        message,
                        requiresReconciliation = false,
                    )
                    if (stopped.state.terminal) {
                        dependencies.dispatcher.enqueueFollowers(
                            stopped.targetProfileId,
                            stopped.conversationGeneration,
                        )
                    }
                    return Result.success()
                }
                HermesErrorCategory.INTERNAL -> {
                    dependencies.commandRepository.markFailed(
                        commandId,
                        HermesErrorCategory.INTERNAL,
                        "Local submission preparation failed",
                    )
                    dependencies.dispatcher.enqueueFollowers(command.targetProfileId, command.conversationGeneration)
                    return Result.success()
                }
                else -> {
                    val released = dependencies.commandRepository.releaseSubmission(
                        commandId,
                        owner,
                        nextAttemptAt = dependencies.clock() + PREFLIGHT_RETRY_DELAY_MILLIS,
                        category = category,
                        message = message,
                    )
                    if (released.state == CommandState.CANCELLED) {
                        dependencies.dispatcher.enqueueFollowers(
                            released.targetProfileId,
                            released.conversationGeneration,
                        )
                        Result.success()
                    } else {
                        Result.retry()
                    }
                }
            }
        }
        val currentProfileId = try {
            dependencies.settingsRepository.current().profileId
        } catch (error: CancellationException) {
            throw error
        } catch (_: Exception) {
            null
        }
        if (currentProfileId != command.targetProfileId) {
            dependencies.commandRepository.markPaused(
                commandId,
                HermesErrorCategory.PROFILE_CHANGED,
                "Submission outcome belongs to the previous connection profile",
                requiresReconciliation = true,
            )
            return Result.success()
        }
        if (command.submissionAttemptCount > 1 && category in setOf(
                HermesErrorCategory.REQUEST_REJECTED, HermesErrorCategory.NOT_FOUND,
                HermesErrorCategory.AUTHENTICATION, HermesErrorCategory.INVALID_SETTINGS,
                HermesErrorCategory.REDIRECT, HermesErrorCategory.UNSUPPORTED_API,
            )) {
            dependencies.commandRepository.markOutcomeUnknown(commandId)
            return Result.success()
        }
        when (category) {
            HermesErrorCategory.IDEMPOTENCY_CONFLICT -> {
                dependencies.commandRepository.markOutcomeUnknown(
                    commandId,
                    category,
                    "Idempotency state conflict; review before creating a new request",
                )
                return Result.success()
            }
            HermesErrorCategory.REQUEST_REJECTED,
            HermesErrorCategory.NOT_FOUND,
            -> {
                dependencies.commandRepository.markFailed(commandId, category, message, apiError?.httpStatus)
                dependencies.dispatcher.enqueueFollowers(command.targetProfileId, command.conversationGeneration)
                return Result.success()
            }
            HermesErrorCategory.AUTHENTICATION,
            HermesErrorCategory.INVALID_SETTINGS,
            HermesErrorCategory.REDIRECT,
            HermesErrorCategory.UNSUPPORTED_API,
            -> {
                val stopped = dependencies.commandRepository.markPaused(commandId, category, message, false)
                if (stopped.state.terminal) {
                    dependencies.dispatcher.enqueueFollowers(
                        stopped.targetProfileId,
                        stopped.conversationGeneration,
                    )
                }
                return Result.success()
            }
            else -> Unit
        }
        val firstAttemptAt = command.firstSubmissionAttemptAt
        val capabilities = dependencies.settingsRepository.current().capabilities
        val retryDecision = if (capabilities != null && firstAttemptAt != null) {
            capabilities.retryDecision(firstAttemptAt, dependencies.clock())
        } else {
            RetryDecision.UNSUPPORTED
        }
        return when (retryDecision) {
            RetryDecision.SAFE -> {
                val nextDelay = apiError?.retryAfterMillis?.coerceAtMost(MAX_RETRY_AFTER_MILLIS)
                    ?: retryBackoff(command.submissionAttemptCount)
                dependencies.commandRepository.markSubmissionRetry(
                    commandId,
                    owner,
                    category,
                    message,
                    apiError?.httpStatus,
                    dependencies.clock() + nextDelay,
                )
                Result.retry()
            }
            RetryDecision.DEADLINE_PASSED,
            RetryDecision.UNSUPPORTED,
            -> {
                dependencies.commandRepository.markOutcomeUnknown(
                    commandId,
                    category,
                    "Outcome unknown — review before retrying",
                )
                Result.success()
            }
        }
    }

    companion object {
        const val COMMAND_ID = "command_id"
        private const val CAPABILITY_CACHE_MILLIS = 24L * 60L * 60L * 1000L
        private const val SUBMISSION_LEASE_MILLIS = 2L * 60L * 1000L
        private const val PREFLIGHT_RETRY_DELAY_MILLIS = 30_000L
        private const val MAX_RETRY_AFTER_MILLIS = 15L * 60L * 1000L
        private const val MAX_EXPLICIT_RETRY_WINDOW_MILLIS = 23L * 60L * 60L * 1000L
    }
}

class RunMonitorWorker(
    appContext: Context,
    workerParameters: WorkerParameters,
) : CoroutineWorker(appContext, workerParameters) {
    private val dependencies = workerDependencies(appContext)

    override suspend fun doWork(): Result {
        val commandId = inputData.getLong(COMMAND_ID, -1L)
        if (commandId < 0) return Result.failure()
        val initial = dependencies.commandRepository.get(commandId) ?: return Result.success()
        val runId = initial.runId
        if (
            runId == null ||
            initial.state.terminal ||
            initial.state == CommandState.PAUSED ||
            initial.state == CommandState.OUTCOME_UNKNOWN
        ) {
            return Result.success()
        }
        val settings = dependencies.settingsRepository.current()
        if (initial.targetProfileId != settings.profileId) {
            dependencies.commandRepository.markPaused(
                commandId,
                HermesErrorCategory.PROFILE_CHANGED,
                "Accepted run belongs to the previous connection profile",
                requiresReconciliation = true,
            )
            return Result.success()
        }
        if (!inputData.getBoolean(FORCE_REFRESH, false)) {
            initial.nextAttemptAt?.let { nextAttemptAt ->
                val wait = nextAttemptAt - dependencies.clock()
                if (wait > 0) {
                    if (wait > CommandDispatcher.MAX_WORK_DELAY_MILLIS) {
                        dependencies.dispatcher.enqueueMonitorNow(commandId)
                        return Result.success()
                    }
                    delay(wait)
                }
            }
        }
        return try {
            val command = dependencies.commandRepository.get(commandId) ?: return Result.success()
            if (
                command.state == CommandState.PAUSED ||
                command.state == CommandState.OUTCOME_UNKNOWN ||
                command.state.terminal
            ) {
                return Result.success()
            }
            val run = dependencies.hermesClient.getRun(runId, command.targetProfileId)
            val after = dependencies.settingsRepository.current()
            if (after.profileId != command.targetProfileId) {
                dependencies.commandRepository.markPaused(commandId, HermesErrorCategory.PROFILE_CHANGED,
                    "Accepted run belongs to the previous connection profile", true)
                return Result.success()
            }
            if (run.sessionId != null) {
                val expectedSessionId = command.sessionId
                if (expectedSessionId != null && expectedSessionId != run.sessionId) {
                    dependencies.commandRepository.rotateSession(
                        command.targetProfileId,
                        command.conversationGeneration,
                        expectedSessionId,
                        run.sessionId,
                    )
                }
            }
            val localState = when (run.status) {
                RunServerStatus.WAITING_FOR_APPROVAL -> CommandState.NEEDS_APPROVAL
                RunServerStatus.RUNNING -> CommandState.RUNNING
                else -> CommandState.ACCEPTED
            }
            val nextPollAt = if (run.terminal) {
                null
            } else {
                dependencies.clock() + dependencies.dispatcher.monitorDelayMillis(
                    localState,
                    dependencies.dispatcher.isForegroundActive(),
                )
            }
            val priorUpdatedAt = command.updatedAt
            val updated = dependencies.commandRepository.applyRun(commandId, run, nextPollAt)
            if (updated.state == CommandState.COMPLETED && updated.updatedAt != priorUpdatedAt) {
                try {
                    dependencies.dispatcher.notifier?.notifyCompleted(updated.id, updated.captureId)
                } catch (error: CancellationException) {
                    throw error
                } catch (_: Exception) {
                }
            }
            if (updated.state.terminal || updated.state == CommandState.OUTCOME_UNKNOWN) {
                dependencies.dispatcher.enqueueFollowers(updated.targetProfileId, updated.conversationGeneration)
            } else {
                dependencies.dispatcher.enqueueMonitorNow(commandId)
            }
            Result.success()
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            handleMonitorFailure(commandId, error)
        }
    }

    private suspend fun handleMonitorFailure(commandId: Long, error: Exception): Result {
        val command = dependencies.commandRepository.get(commandId) ?: return Result.success()
        if (command.state.terminal || command.state == CommandState.PAUSED) return Result.success()
        val apiError = error as? HermesApiException
        val category = apiError?.category ?: HermesErrorCategory.INTERNAL
        val message = apiError?.message ?: "Hermes run monitoring failed"
        if (category == HermesErrorCategory.NOT_FOUND) {
            try {
                dependencies.dispatcher.reconcileHistory(commandId)
            } catch (error: CancellationException) {
                throw error
            } catch (_: Exception) {
            }
            dependencies.commandRepository.markOutcomeUnknown(
                commandId,
                HermesErrorCategory.NOT_FOUND,
                "Run status is unavailable; review conversation history before retrying",
            )
            val updated = dependencies.commandRepository.get(commandId) ?: return Result.success()
            dependencies.dispatcher.enqueueFollowers(updated.targetProfileId, updated.conversationGeneration)
            return Result.success()
        }
        if (
            category == HermesErrorCategory.AUTHENTICATION ||
            category == HermesErrorCategory.INVALID_SETTINGS ||
            category == HermesErrorCategory.REDIRECT ||
            category == HermesErrorCategory.UNSUPPORTED_API
        ) {
            dependencies.commandRepository.markPaused(commandId, category, message, true)
            return Result.success()
        }
        if (category == HermesErrorCategory.INVALID_RESPONSE && command.monitorFailureCount >= MAX_FORMAT_FAILURES) {
            dependencies.commandRepository.markOutcomeUnknown(
                commandId,
                HermesErrorCategory.INVALID_RESPONSE,
                "Run status could not be validated; review before retrying",
            )
            val updated = dependencies.commandRepository.get(commandId) ?: return Result.success()
            dependencies.dispatcher.enqueueFollowers(updated.targetProfileId, updated.conversationGeneration)
            return Result.success()
        }
        val retryDelay = apiError?.retryAfterMillis?.coerceAtMost(CommandDispatcher.MAX_WORK_DELAY_MILLIS)
            ?: retryBackoff(command.monitorFailureCount)
        dependencies.commandRepository.recordMonitorFailure(
            commandId = commandId,
            category = category,
            message = message,
            httpStatus = apiError?.httpStatus,
            retryable = true,
            nextAttemptAt = dependencies.clock() + retryDelay,
        )
        dependencies.dispatcher.enqueueMonitorNow(commandId)
        return Result.success()
    }

    companion object {
        const val COMMAND_ID = "command_id"
        const val FORCE_REFRESH = "force_refresh"
        private const val MAX_FORMAT_FAILURES = 4
    }
}

class RunStopWorker(
    appContext: Context,
    workerParameters: WorkerParameters,
) : CoroutineWorker(appContext, workerParameters) {
    private val dependencies = workerDependencies(appContext)

    override suspend fun doWork(): Result {
        val commandId = inputData.getLong(COMMAND_ID, -1L)
        if (commandId < 0) return Result.failure()
        val command = dependencies.commandRepository.get(commandId) ?: return Result.success()
        val runId = command.runId ?: return Result.success()
        if (
            command.state.terminal ||
            command.state == CommandState.PAUSED ||
            command.state == CommandState.OUTCOME_UNKNOWN
        ) {
            return Result.success()
        }
        val settings = dependencies.settingsRepository.current()
        if (command.targetProfileId != settings.profileId) {
            dependencies.commandRepository.markPaused(
                commandId,
                HermesErrorCategory.PROFILE_CHANGED,
                "Run belongs to the previous connection profile",
                requiresReconciliation = true,
            )
            return Result.success()
        }
        return try {
            val run = dependencies.hermesClient.stopRun(runId, command.targetProfileId)
            if (run.status == RunServerStatus.UNKNOWN) {
                dependencies.commandRepository.recordMonitorFailure(
                    commandId = commandId,
                    category = HermesErrorCategory.INVALID_RESPONSE,
                    message = "Hermes returned an unknown stop status; reconciling the run",
                    httpStatus = null,
                    retryable = true,
                    nextAttemptAt = dependencies.clock() + BACKGROUND_STOP_RECONCILE_DELAY_MILLIS,
                )
                dependencies.dispatcher.enqueueMonitorNow(commandId)
                return Result.success()
            }
            dependencies.dispatcher.enqueueMonitorNow(commandId)
            Result.success()
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            val apiError = error as? HermesApiException
            val category = apiError?.category ?: HermesErrorCategory.INTERNAL
            if (
                category == HermesErrorCategory.AUTHENTICATION ||
                category == HermesErrorCategory.INVALID_SETTINGS ||
                category == HermesErrorCategory.REDIRECT ||
                category == HermesErrorCategory.UNSUPPORTED_API
            ) {
                dependencies.commandRepository.markPaused(
                    commandId,
                    category,
                    apiError?.message ?: "Hermes stop request failed",
                    requiresReconciliation = true,
                )
            } else {
                dependencies.commandRepository.recordMonitorFailure(
                    commandId,
                    category,
                    apiError?.message ?: "Hermes stop request failed; reconciling run status",
                    apiError?.httpStatus,
                    retryable = true,
                    nextAttemptAt = dependencies.clock() + retryBackoff(command.monitorFailureCount),
                )
                dependencies.dispatcher.enqueueMonitorNow(commandId)
            }
            Result.success()
        }
    }

    companion object {
        const val COMMAND_ID = "command_id"
        private const val BACKGROUND_STOP_RECONCILE_DELAY_MILLIS = 5_000L
    }
}

private fun retryBackoff(attemptCount: Int): Long {
    val exponent = attemptCount.coerceIn(0, 7)
    return (30_000L * (1L shl exponent)).coerceAtMost(CommandDispatcher.MAX_WORK_DELAY_MILLIS)
}
