package dev.hermespebble.companion.dispatch

import android.content.Context
import androidx.work.BackoffPolicy
import androidx.work.Constraints
import androidx.work.ExistingWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.workDataOf
import dev.hermespebble.companion.HermesPt2Application
import dev.hermespebble.companion.data.local.CommandRepository
import dev.hermespebble.companion.data.local.CommandState
import dev.hermespebble.companion.data.local.ConversationRepository
import dev.hermespebble.companion.data.local.CommandRepository.Companion.DEFAULT_RETENTION_MILLIS
import dev.hermespebble.companion.data.local.StopDisposition
import dev.hermespebble.companion.data.preferences.SettingsRepository
import dev.hermespebble.companion.network.HermesApiException
import dev.hermespebble.companion.network.HermesCapabilities
import dev.hermespebble.companion.network.HermesClient
import dev.hermespebble.companion.network.HermesErrorCategory
import dev.hermespebble.companion.network.RetryDecision
import dev.hermespebble.companion.network.retryDecision
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CancellationException

class CommandDispatcher internal constructor(
    private val context: Context,
    private val commandRepository: CommandRepository,
    private val conversationRepository: ConversationRepository,
    private val settingsRepository: SettingsRepository,
    private val hermesClient: HermesClient,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    private val workManager = WorkManager.getInstance(context.applicationContext)
    private val foregroundActive = AtomicBoolean(false)
    private val recoveryStarted = AtomicBoolean(false)

    @Volatile
    var notifier: ResultNotifier? = null

    internal val workerDependencies = CommandWorkerDependencies(
        commandRepository = commandRepository,
        conversationRepository = conversationRepository,
        settingsRepository = settingsRepository,
        hermesClient = hermesClient,
        dispatcher = this,
        clock = clock,
    )

    fun enqueue(commandId: Long) {
        enqueueSubmission(commandId, replace = false)
    }

    suspend fun requestStop(commandId: Long): StopDisposition {
        val disposition = commandRepository.requestStop(commandId)
        when (disposition) {
            StopDisposition.STOP_REQUESTED -> enqueueStop(commandId)
            StopDisposition.CANCELLED_UNSENT -> commandRepository.get(commandId)?.let { command ->
                enqueueFollowers(command.targetProfileId, command.conversationGeneration)
            }
            StopDisposition.WAITING_FOR_ACCEPTANCE,
            StopDisposition.RUN_ID_UNAVAILABLE,
            StopDisposition.ALREADY_TERMINAL,
            StopDisposition.NOT_ACCEPTED,
            -> Unit
        }
        return disposition
    }

    suspend fun testConnection(): ConnectionTestResult {
        val settings = settingsRepository.current()
        return try {
            val capabilities = hermesClient.getCapabilities(settings.profileId)
            val after = settingsRepository.current()
            if (settings.profileId != after.profileId) {
                ConnectionTestResult.Failed(
                    HermesErrorCategory.PROFILE_CHANGED,
                    "Connection settings changed during the capability check",
                )
            } else if (!capabilities.supportsRequiredV1Api) {
                settingsRepository.recordCapabilities(after.profileId, capabilities, clock())
                ConnectionTestResult.Failed(
                    HermesErrorCategory.UNSUPPORTED_API,
                    "Hermes does not support the required v1 asynchronous run API",
                )
            } else {
                settingsRepository.recordCapabilities(after.profileId, capabilities, clock())
                ConnectionTestResult.Succeeded(capabilities)
            }
        } catch (error: HermesApiException) {
            ConnectionTestResult.Failed(error.category, error.message)
        } catch (error: CancellationException) {
            throw error
        } catch (_: Exception) {
            ConnectionTestResult.Failed(
                HermesErrorCategory.INTERNAL,
                "Connection test could not be completed",
            )
        }
    }

    suspend fun refresh(commandId: Long) {
        val settings = settingsRepository.current()
        val command = commandRepository.resumePaused(commandId, settings.profileId) ?: return
        if (command.targetProfileId != settings.profileId) return
        if (command.runId != null) enqueueMonitor(commandId, replace = true)
        if (command.state == CommandState.QUEUED) enqueueSubmission(commandId, replace = false)
    }

    suspend fun reconcileHistory(commandId: Long): Int {
        val command = commandRepository.get(commandId) ?: return 0
        val settings = settingsRepository.current()
        if (command.targetProfileId != settings.profileId) return 0
        val sessionId = command.sessionId ?: return 0
        val page = hermesClient.getHistory(sessionId, expectedProfileId = command.targetProfileId)
        val after = settingsRepository.current()
        if (after.profileId != settings.profileId) return 0
        if (page.sessionId != sessionId) {
            commandRepository.rotateSession(settings.profileId, command.conversationGeneration, sessionId, page.sessionId)
            commandRepository.mutate(commandId) { it.copy(currentSessionId = page.sessionId) }
        }
        return conversationRepository.storeHistory(
            settings.profileId,
            command.conversationGeneration,
            page,
        )
    }

    suspend fun recoverOnce() {
        if (!recoveryStarted.compareAndSet(false, true)) return
        try {
            recoverPending()
        } catch (error: Exception) {
            recoveryStarted.set(false)
            throw error
        }
    }

    private suspend fun recoverPending() {
        commandRepository.pauseForProfileChange(settingsRepository.current().profileId)
        val settings = settingsRepository.current()
        commandRepository.enforceRetentionAll(
            DEFAULT_RETENTION_MILLIS,
            clock(),
        )
        val recoverable = commandRepository.commandsByProfileAndStates(
            settings.profileId,
            setOf(
                CommandState.QUEUED,
                CommandState.SUBMITTING,
                CommandState.ACCEPTED,
                CommandState.RUNNING,
                CommandState.NEEDS_APPROVAL,
                CommandState.STOPPING,
            ),
        )
        recoverable.forEach { command ->
            when (command.state) {
                CommandState.QUEUED -> enqueueSubmission(command.id, replace = false)
                CommandState.SUBMITTING -> {
                    if (command.firstSubmissionAttemptAt == null) {
                        enqueueSubmission(command.id, replace = false)
                    } else {
                        when (settings.capabilities?.retryDecision(command.firstSubmissionAttemptAt, clock())) {
                            RetryDecision.SAFE -> enqueueSubmission(command.id, replace = false)
                            RetryDecision.DEADLINE_PASSED,
                            RetryDecision.UNSUPPORTED,
                            null,
                            -> commandRepository.markOutcomeUnknown(command.id)
                        }
                    }
                }
                CommandState.STOPPING -> {
                    if (command.runId == null) {
                        commandRepository.markOutcomeUnknown(
                            command.id,
                            HermesErrorCategory.INTERNAL,
                            "Accepted run ID is unavailable; review before retrying",
                        )
                    } else {
                        if (command.stopRequested) enqueueStop(command.id)
                        enqueueMonitor(command.id)
                    }
                }
                CommandState.ACCEPTED,
                CommandState.RUNNING,
                CommandState.NEEDS_APPROVAL,
                -> {
                    if (command.runId == null) {
                        commandRepository.markOutcomeUnknown(
                            command.id,
                            HermesErrorCategory.INTERNAL,
                            "Accepted run ID is unavailable; review before retrying",
                        )
                    } else {
                        enqueueMonitor(command.id)
                    }
                }
                else -> Unit
            }
        }
    }

    fun setForegroundActive(active: Boolean) {
        foregroundActive.set(active)
    }

    internal fun isForegroundActive(): Boolean = foregroundActive.get()

    internal fun enqueueAccepted(commandId: Long) {
        enqueueMonitor(commandId)
    }

    internal fun enqueueMonitorNow(commandId: Long) {
        enqueueMonitor(commandId)
    }

    internal suspend fun enqueueFollowers(profileId: String, conversationGeneration: Long) {
        val candidate = commandRepository.nextQueuedCandidate(
            profileId,
            conversationGeneration,
            clock(),
        ) ?: return
        if (!commandRepository.hasEarlierBlockingCommand(
                candidate.id,
                candidate.targetProfileId,
                candidate.conversationGeneration,
            )
        ) {
            enqueueSubmission(candidate.id, replace = true)
        }
    }

    internal fun enqueueStopNow(commandId: Long) {
        enqueueStop(commandId)
    }

    private fun enqueueSubmission(commandId: Long, replace: Boolean) {
        val request = OneTimeWorkRequestBuilder<CommandSubmissionWorker>()
            .setInputData(workDataOf(CommandSubmissionWorker.COMMAND_ID to commandId))
            .setConstraints(networkConstraints())
            .setBackoffCriteria(BackoffPolicy.EXPONENTIAL, 30, TimeUnit.SECONDS)
            .addTag(SUBMISSION_TAG)
            .addTag(commandTag(commandId))
            .build()
        workManager.enqueueUniqueWork(
            submissionWorkName(commandId),
            if (replace) ExistingWorkPolicy.REPLACE else ExistingWorkPolicy.KEEP,
            request,
        )
    }

    private fun enqueueMonitor(commandId: Long, replace: Boolean = false) {
        val request = OneTimeWorkRequestBuilder<RunMonitorWorker>()
            .setInputData(
                workDataOf(
                    RunMonitorWorker.COMMAND_ID to commandId,
                    RunMonitorWorker.FORCE_REFRESH to replace,
                ),
            )
            .setConstraints(networkConstraints())
            .setBackoffCriteria(BackoffPolicy.EXPONENTIAL, 30, TimeUnit.SECONDS)
            .addTag(MONITOR_TAG)
            .addTag(commandTag(commandId))
            .build()
        workManager.enqueueUniqueWork(
            monitorWorkName(commandId),
            if (replace) ExistingWorkPolicy.REPLACE else ExistingWorkPolicy.APPEND_OR_REPLACE,
            request,
        )
    }

    private fun enqueueStop(commandId: Long) {
        val request = OneTimeWorkRequestBuilder<RunStopWorker>()
            .setInputData(workDataOf(RunStopWorker.COMMAND_ID to commandId))
            .setConstraints(networkConstraints())
            .setBackoffCriteria(BackoffPolicy.EXPONENTIAL, 30, TimeUnit.SECONDS)
            .addTag(STOP_TAG)
            .addTag(commandTag(commandId))
            .build()
        workManager.enqueueUniqueWork(
            stopWorkName(commandId),
            ExistingWorkPolicy.KEEP,
            request,
        )
    }

    internal fun monitorDelayMillis(state: CommandState, foreground: Boolean): Long = when (state) {
        CommandState.NEEDS_APPROVAL -> APPROVAL_POLL_INTERVAL_MILLIS
        else -> if (foreground) ACTIVE_POLL_INTERVAL_MILLIS else BACKGROUND_POLL_INTERVAL_MILLIS
    }

    private fun networkConstraints() = Constraints.Builder()
        .setRequiredNetworkType(NetworkType.CONNECTED)
        .build()

    companion object {
        const val ACTIVE_POLL_INTERVAL_MILLIS = 3_000L
        const val BACKGROUND_POLL_INTERVAL_MILLIS = 30_000L
        const val APPROVAL_POLL_INTERVAL_MILLIS = 5L * 60L * 1000L
        const val MAX_WORK_DELAY_MILLIS = 15L * 60L * 1000L
        private const val SUBMISSION_TAG = "hermes-command-submission"
        private const val MONITOR_TAG = "hermes-run-monitor"
        private const val STOP_TAG = "hermes-run-stop"

        internal fun submissionWorkName(commandId: Long) = "hermes-command-$commandId"
        internal fun monitorWorkName(commandId: Long) = "hermes-monitor-$commandId"
        internal fun stopWorkName(commandId: Long) = "hermes-stop-$commandId"
        internal fun commandTag(commandId: Long) = "hermes-item-$commandId"
    }
}

interface ResultNotifier {
    suspend fun notifyCompleted(commandId: Long, captureId: Long?)
}

interface CommandBridge {
    suspend fun onCommandChanged(commandId: Long, state: CommandState)
}

sealed interface ConnectionTestResult {
    data class Succeeded(val capabilities: HermesCapabilities) : ConnectionTestResult
    data class Failed(val category: HermesErrorCategory, val message: String) : ConnectionTestResult
}

internal data class CommandWorkerDependencies(
    val commandRepository: CommandRepository,
    val conversationRepository: ConversationRepository,
    val settingsRepository: SettingsRepository,
    val hermesClient: HermesClient,
    val dispatcher: CommandDispatcher,
    val clock: () -> Long,
)

internal fun workerDependencies(context: android.content.Context): CommandWorkerDependencies {
    val application = context.applicationContext as HermesPt2Application
    return application.requireContainer().dispatcher.workerDependencies
}
