package dev.hermespebble.companion.data.local

import androidx.room.withTransaction
import dev.hermespebble.companion.network.HermesErrorCategory
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest
import java.util.UUID
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.channels.BufferOverflow

class CommandRepository internal constructor(
    private val database: HermesDatabase,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    private val commandDao = database.commandDao()
    private val receiptDao = database.captureReceiptDao()
    private val sessionDao = database.conversationSessionDao()

    val changes: kotlinx.coroutines.flow.MutableSharedFlow<CommandChange> = kotlinx.coroutines.flow.MutableSharedFlow(
        extraBufferCapacity = 64,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
    )

    fun observeRecent(profileId: String, limit: Int = DEFAULT_HISTORY_LIMIT): Flow<List<CommandItem>> =
        commandDao.observeRecent(profileId, limit.coerceIn(1, MAX_HISTORY_LIMIT)).map { commands ->
            commands.map(::mapCommand)
        }

    fun observeAllRecent(limit: Int = DEFAULT_HISTORY_LIMIT): Flow<List<CommandItem>> =
        commandDao.observeAllRecent(limit.coerceIn(1, MAX_HISTORY_LIMIT)).map { it.map(::mapCommand) }

    suspend fun resumePaused(commandId: Long, profileId: String): CommandItem? {
        val updated = mutate(commandId) { command ->
            if (command.targetProfileId != profileId || command.state != CommandState.PAUSED) command
            else if (command.runId != null) command.copy(state = CommandState.ACCEPTED, nextAttemptAt = null)
            else if (command.firstSubmissionAttemptAt == null) command.copy(state = CommandState.QUEUED, nextAttemptAt = null)
            else command
        }
        return mapCommand(updated)
    }

    fun observe(commandId: Long): Flow<CommandItem?> = commandDao.observe(commandId).map { command ->
        command?.let(::mapCommand)
    }

    suspend fun get(commandId: Long): CommandItem? = commandDao.get(commandId)?.let(::mapCommand)

    suspend fun getByCapture(watchIdentifier: String, captureId: Long): CommandItem? =
        commandDao.getByCapture(watchIdentifier, captureId)?.let(::mapCommand)

    suspend fun acceptWatchCommand(
        watchIdentifier: String,
        captureId: Long,
        input: String,
        kind: CommandKind,
        targetProfileId: String,
        conversationGeneration: Long,
    ): DurableCommandReceipt {
        require(kind == CommandKind.WATCH_REQUEST || kind == CommandKind.WATCH_NOTE)
        validateWatchIdentifier(watchIdentifier)
        validateCaptureId(captureId)
        validateInput(input, maxBytes = WATCH_INPUT_MAX_BYTES)
        require(targetProfileId.isNotEmpty())
        require(conversationGeneration >= 0)
        val inputDigest = digest(input.toByteArray(Charsets.UTF_8))
        val receipt = database.withTransaction {
            receiptDao.get(watchIdentifier, captureId)?.let { existing ->
                if (existing.inputDigest.isEmpty()) throw CommandValidationException("This capture was durably discarded")
                if (existing.itemKind != kind || existing.inputDigest != inputDigest) {
                    throw DuplicateCaptureConflictException(watchIdentifier, captureId)
                }
                val state = commandDao.get(existing.commandId)?.state ?: existing.state
                DurableCommandReceipt(
                    commandId = existing.commandId,
                    captureId = existing.captureId,
                    kind = existing.itemKind,
                    state = state,
                    duplicate = true,
                )
            } ?: run {
                val now = clock()
                val state = if (kind == CommandKind.WATCH_NOTE) CommandState.COMPLETED else CommandState.QUEUED
                val commandId = commandDao.insert(
                    CommandEntity(
                        kind = kind,
                        watchIdentifier = watchIdentifier,
                        captureId = captureId,
                        targetProfileId = targetProfileId,
                        conversationGeneration = conversationGeneration,
                        input = input,
                        state = state,
                        idempotencyKey = UUID.randomUUID().toString(),
                        createdAt = now,
                        updatedAt = now,
                        completedAt = if (state.terminal) now else null,
                    ),
                )
                receiptDao.insert(
                    CaptureReceiptEntity(
                        watchIdentifier = watchIdentifier,
                        captureId = captureId,
                        commandId = commandId,
                        itemKind = kind,
                        state = state,
                        inputDigest = inputDigest,
                        durableAt = now,
                        updatedAt = now,
                    ),
                )
                DurableCommandReceipt(
                    commandId = commandId,
                    captureId = captureId,
                    kind = kind,
                    state = state,
                    duplicate = false,
                )
            }
        }
        if (!receipt.duplicate) publish(receipt.commandId, receipt.state)
        return receipt
    }

    suspend fun discardWatchCapture(watchIdentifier: String, captureId: Long, profileId: String, generation: Long): Boolean {
        validateWatchIdentifier(watchIdentifier)
        validateCaptureId(captureId)
        val cancelled = database.withTransaction {
            val current = commandDao.getByCapture(watchIdentifier, captureId)
            if (current != null) {
                if (current.state.terminal) return@withTransaction current
                if (current.state != CommandState.QUEUED || current.firstSubmissionAttemptAt != null) return@withTransaction null
                current.copy(state = CommandState.CANCELLED, stopRequested = true,
                    completedAt = clock(), updatedAt = clock()).also {
                    commandDao.update(it)
                    updateReceipt(it)
                }
            } else {
                // Tombstone prevents a delayed/retried submission from executing after discard ACK.
                val now = clock()
                val command = CommandEntity(kind = CommandKind.WATCH_REQUEST,
                    watchIdentifier = watchIdentifier, captureId = captureId,
                    targetProfileId = profileId, conversationGeneration = generation, input = "",
                    visible = false, state = CommandState.CANCELLED, idempotencyKey = UUID.randomUUID().toString(),
                    createdAt = now, updatedAt = now, completedAt = now)
                val id = commandDao.insert(command)
                receiptDao.insert(CaptureReceiptEntity(watchIdentifier = watchIdentifier, captureId = captureId,
                    commandId = id, itemKind = command.kind, state = command.state, inputDigest = "",
                    durableAt = now, updatedAt = now))
                command.copy(id = id)
            }
        } ?: return false
        publish(cancelled.id, cancelled.state)
        return true
    }

    suspend fun acceptPhoneRequest(
        input: String,
        targetProfileId: String,
        conversationGeneration: Long,
    ): DurableCommandReceipt {
        validateInput(input, maxBytes = PHONE_INPUT_MAX_BYTES)
        require(targetProfileId.isNotEmpty())
        require(conversationGeneration >= 0)
        val now = clock()
        val commandId = database.withTransaction {
            val inserted = commandDao.insert(
                CommandEntity(
                    kind = CommandKind.PHONE_REQUEST,
                    watchIdentifier = null,
                    captureId = null,
                    targetProfileId = targetProfileId,
                    conversationGeneration = conversationGeneration,
                    input = input,
                    state = CommandState.QUEUED,
                    idempotencyKey = UUID.randomUUID().toString(),
                    createdAt = now,
                    updatedAt = now,
                ),
            )
            inserted
        }
        publish(commandId, CommandState.QUEUED)
        return DurableCommandReceipt(
            commandId = commandId,
            captureId = null,
            kind = CommandKind.PHONE_REQUEST,
            state = CommandState.QUEUED,
            duplicate = false,
        )
    }

    suspend fun getOrCreateSession(
        profileId: String,
        conversationGeneration: Long,
    ): ConversationSessionEntity = database.withTransaction {
        sessionDao.get(profileId, conversationGeneration) ?: run {
            val now = clock()
            val session = ConversationSessionEntity(
                profileId = profileId,
                conversationGeneration = conversationGeneration,
                sessionId = "pt2_${UUID.randomUUID()}",
                serverConfirmed = false,
                createdAt = now,
                updatedAt = now,
            )
            sessionDao.insert(session)
            session
        }
    }

    suspend fun confirmSession(
        profileId: String,
        conversationGeneration: Long,
        sessionId: String,
    ): ConversationSessionEntity? = database.withTransaction {
        val current = sessionDao.get(profileId, conversationGeneration) ?: return@withTransaction null
        if (current.sessionId != sessionId) return@withTransaction null
        val confirmed = current.copy(serverConfirmed = true, updatedAt = clock())
        sessionDao.update(confirmed)
        confirmed
    }

    suspend fun rotateSession(
        profileId: String,
        conversationGeneration: Long,
        expectedSessionId: String,
        sessionId: String,
    ): Boolean = database.withTransaction {
        sessionDao.updateSessionId(
            profileId = profileId,
            conversationGeneration = conversationGeneration,
            expectedSessionId = expectedSessionId,
            sessionId = sessionId,
            serverConfirmed = true,
            updatedAt = clock(),
        ) == 1
    }

    internal suspend fun acquireSubmission(
        commandId: Long,
        profileId: String,
        owner: String,
        nowMillis: Long,
        leaseMillis: Long,
    ): CommandEntity? = database.withTransaction {
        val command = commandDao.get(commandId) ?: return@withTransaction null
        if (
            command.targetProfileId != profileId ||
            command.state !in setOf(CommandState.QUEUED, CommandState.SUBMITTING) ||
            (command.leaseOwner != null && command.leaseOwner != owner && (command.leaseUntil ?: 0) > nowMillis)
        ) {
            return@withTransaction null
        }
        val blocking = commandDao.earlierBlockingCommand(
            profileId,
            command.conversationGeneration,
            command.id,
        )
        if (blocking != null) return@withTransaction null
        val updated = command.copy(
            state = CommandState.SUBMITTING,
            leaseOwner = owner,
            leaseUntil = nowMillis + leaseMillis,
            updatedAt = nowMillis,
        )
        commandDao.update(updated)
        updateReceipt(updated)
        updated
    }

    internal suspend fun ensureFrozenPayload(
        commandId: Long,
        owner: String,
        sessionId: String,
        payloadJson: String,
    ): CommandEntity = mutate(commandId) { command ->
        if (command.leaseOwner != owner) throw ConcurrentCommandMutationException()
        if (command.frozenPayloadJson == null) {
            command.copy(
                submittedSessionId = sessionId,
                currentSessionId = sessionId,
                frozenPayloadJson = payloadJson,
                updatedAt = clock(),
            )
        } else if (command.frozenPayloadJson != payloadJson) {
            throw FrozenPayloadConflictException()
        } else {
            command
        }
    }

    internal suspend fun markSubmissionAttempt(
        commandId: Long,
        owner: String,
        nowMillis: Long,
    ): CommandEntity = mutate(commandId) { command ->
        if (command.leaseOwner != owner) throw ConcurrentCommandMutationException()
        if (command.stopRequested && command.firstSubmissionAttemptAt == null) {
            command.copy(state = CommandState.CANCELLED, leaseOwner = null, leaseUntil = null,
                completedAt = nowMillis, updatedAt = nowMillis)
        } else command.copy(
            state = CommandState.SUBMITTING,
            firstSubmissionAttemptAt = command.firstSubmissionAttemptAt ?: nowMillis,
            lastSubmissionAttemptAt = nowMillis,
            nextAttemptAt = null,
            submissionAttemptCount = command.submissionAttemptCount + 1,
            updatedAt = nowMillis,
        )
    }

    internal suspend fun markAccepted(
        commandId: Long,
        runId: String,
        serverState: String,
        replayed: Boolean,
    ): CommandEntity = mutate(commandId) { command ->
        val acceptedState = if (command.stopRequested) CommandState.STOPPING else {
            if (serverState == "running") CommandState.RUNNING else CommandState.ACCEPTED
        }
        if (command.state == CommandState.PAUSED) {
            command.copy(
                serverState = serverState,
                serverStatusRaw = serverState,
                runId = runId,
                replayed = replayed,
                errorCategory = HermesErrorCategory.PROFILE_CHANGED,
                errorCode = "accepted_previous_profile",
                errorMessage = "Accepted under the previous connection profile; reconciliation required",
                requiresReconciliation = true,
                leaseOwner = null,
                leaseUntil = null,
                nextAttemptAt = null,
                updatedAt = clock(),
            )
        } else {
            command.copy(
                state = acceptedState,
                serverState = serverState,
                serverStatusRaw = serverState,
                runId = runId,
                replayed = replayed,
                errorCategory = null,
                errorCode = null,
                errorMessage = null,
                retryable = false,
                requiresReconciliation = false,
                leaseOwner = null,
                leaseUntil = null,
                nextAttemptAt = null,
                updatedAt = clock(),
                completedAt = null,
            )
        }
    }

    internal suspend fun applyRun(
        commandId: Long,
        run: dev.hermespebble.companion.network.HermesRun,
        nextPollAt: Long? = null,
    ): CommandEntity = mutate(commandId) { command ->
        if (
            command.state.terminal ||
            command.state == CommandState.PAUSED ||
            command.state == CommandState.OUTCOME_UNKNOWN
        ) {
            command
        } else {
            val state = when (run.status) {
                dev.hermespebble.companion.network.RunServerStatus.QUEUED,
                dev.hermespebble.companion.network.RunServerStatus.STARTED,
                -> CommandState.ACCEPTED
                dev.hermespebble.companion.network.RunServerStatus.RUNNING -> CommandState.RUNNING
                dev.hermespebble.companion.network.RunServerStatus.WAITING_FOR_APPROVAL -> CommandState.NEEDS_APPROVAL
                dev.hermespebble.companion.network.RunServerStatus.STOPPING -> CommandState.STOPPING
                dev.hermespebble.companion.network.RunServerStatus.COMPLETED -> CommandState.COMPLETED
                dev.hermespebble.companion.network.RunServerStatus.FAILED -> CommandState.FAILED
                dev.hermespebble.companion.network.RunServerStatus.CANCELLED -> CommandState.CANCELLED
                dev.hermespebble.companion.network.RunServerStatus.INTERRUPTED -> CommandState.INTERRUPTED
                dev.hermespebble.companion.network.RunServerStatus.UNKNOWN -> CommandState.OUTCOME_UNKNOWN
            }
            val now = clock()
            command.copy(
                state = state,
                serverState = run.status.name.lowercase(),
                serverStatusRaw = run.rawStatus.take(MAX_SERVER_STATUS_CHARS),
                currentSessionId = run.sessionId ?: command.currentSessionId,
                output = if (state == CommandState.COMPLETED) run.output else command.output,
                errorCategory = when (state) {
                    CommandState.FAILED -> HermesErrorCategory.SERVER
                    CommandState.OUTCOME_UNKNOWN -> HermesErrorCategory.SUBMISSION_OUTCOME_UNKNOWN
                    else -> null
                },
                errorCode = when (state) {
                    CommandState.FAILED -> "run_failed"
                    CommandState.OUTCOME_UNKNOWN -> "unknown_status"
                    else -> null
                },
                errorMessage = when (state) {
                    CommandState.FAILED -> sanitize(run.error ?: "Hermes run failed")
                    CommandState.OUTCOME_UNKNOWN -> "Outcome unknown — review before retrying"
                    else -> null
                },
                httpStatus = null,
                retryable = false,
                requiresReconciliation = state == CommandState.OUTCOME_UNKNOWN,
                lastPolledAt = now,
                monitorFailureCount = 0,
                leaseOwner = null,
                leaseUntil = null,
                nextAttemptAt = nextPollAt,
                updatedAt = now,
                completedAt = if (state.terminal || state == CommandState.OUTCOME_UNKNOWN) now else null,
            )
        }
    }

    internal suspend fun recordMonitorFailure(
        commandId: Long,
        category: HermesErrorCategory,
        message: String,
        httpStatus: Int?,
        retryable: Boolean,
        nextAttemptAt: Long?,
    ): CommandEntity = mutate(commandId) { command ->
        if (
            command.state.terminal ||
            command.state == CommandState.PAUSED ||
            command.state == CommandState.OUTCOME_UNKNOWN
        ) {
            command
        } else {
            command.copy(
                errorCategory = category,
                errorCode = category.name.lowercase(),
                errorMessage = sanitize(message),
                httpStatus = httpStatus,
                retryable = retryable,
                nextAttemptAt = nextAttemptAt,
                lastPolledAt = clock(),
                monitorFailureCount = command.monitorFailureCount + 1,
                updatedAt = clock(),
            )
        }
    }

    internal suspend fun markSubmissionRetry(
        commandId: Long,
        owner: String,
        category: HermesErrorCategory,
        message: String,
        httpStatus: Int?,
        nextAttemptAt: Long?,
    ): CommandEntity = mutate(commandId) { command ->
        if (
            command.state == CommandState.PAUSED ||
            command.state == CommandState.OUTCOME_UNKNOWN ||
            command.state.terminal
        ) {
            command
        } else {
            if (command.leaseOwner != owner) throw ConcurrentCommandMutationException()
            command.copy(
                state = CommandState.SUBMITTING,
                errorCategory = category,
                errorCode = category.name.lowercase(),
                errorMessage = sanitize(message),
                httpStatus = httpStatus,
                retryable = true,
                nextAttemptAt = nextAttemptAt,
                leaseOwner = null,
                leaseUntil = null,
                updatedAt = clock(),
            )
        }
    }

    internal suspend fun markFailed(
        commandId: Long,
        category: HermesErrorCategory,
        message: String,
        httpStatus: Int? = null,
    ): CommandEntity = mutate(commandId) { command ->
        if (
            command.state.terminal ||
            command.state == CommandState.PAUSED ||
            command.state == CommandState.OUTCOME_UNKNOWN
        ) {
            command
        } else {
            val now = clock()
            command.copy(
                state = CommandState.FAILED,
                errorCategory = category,
                errorCode = category.name.lowercase(),
                errorMessage = sanitize(message),
                httpStatus = httpStatus,
                retryable = false,
                requiresReconciliation = false,
                leaseOwner = null,
                leaseUntil = null,
                nextAttemptAt = null,
                updatedAt = now,
                completedAt = now,
            )
        }
    }

    internal suspend fun markOutcomeUnknown(
        commandId: Long,
        category: HermesErrorCategory = HermesErrorCategory.SUBMISSION_OUTCOME_UNKNOWN,
        message: String = "Outcome unknown — review before retrying",
        requiresReconciliation: Boolean = true,
    ): CommandEntity = mutate(commandId) { command ->
        if (command.state == CommandState.PAUSED || command.state.terminal) {
            command
        } else {
            val now = clock()
            command.copy(
                state = CommandState.OUTCOME_UNKNOWN,
                errorCategory = category,
                errorCode = category.name.lowercase(),
                errorMessage = sanitize(message),
                retryable = false,
                requiresReconciliation = requiresReconciliation,
                leaseOwner = null,
                leaseUntil = null,
                nextAttemptAt = null,
                updatedAt = now,
                completedAt = now,
            )
        }
    }

    internal suspend fun markPaused(
        commandId: Long,
        category: HermesErrorCategory,
        message: String,
        requiresReconciliation: Boolean,
    ): CommandEntity = mutate(commandId) { command ->
        if (command.state.terminal || command.state == CommandState.OUTCOME_UNKNOWN) {
            command
        } else {
            val now = clock()
            val nextState = if (command.firstSubmissionAttemptAt == null && command.stopRequested) {
                CommandState.CANCELLED
            } else {
                CommandState.PAUSED
            }
            command.copy(
                state = nextState,
                errorCategory = category,
                errorCode = category.name.lowercase(),
                errorMessage = sanitize(message),
                retryable = false,
                requiresReconciliation = requiresReconciliation,
                leaseOwner = null,
                leaseUntil = null,
                nextAttemptAt = null,
                updatedAt = now,
                completedAt = if (nextState == CommandState.CANCELLED) now else null,
            )
        }
    }

    internal suspend fun releaseSubmission(
        commandId: Long,
        owner: String,
        nextAttemptAt: Long?,
        category: HermesErrorCategory?,
        message: String?,
    ): CommandEntity = mutate(commandId) { command ->
        if (
            command.state == CommandState.PAUSED ||
            command.state == CommandState.OUTCOME_UNKNOWN ||
            command.state.terminal
        ) {
            command
        } else {
            if (command.leaseOwner != owner) throw ConcurrentCommandMutationException()
            val releasedState = when {
                command.firstSubmissionAttemptAt != null -> CommandState.SUBMITTING
                command.stopRequested -> CommandState.CANCELLED
                else -> CommandState.QUEUED
            }
            val now = clock()
            command.copy(
                state = releasedState,
                errorCategory = category,
                errorCode = category?.name?.lowercase(),
                errorMessage = message?.let(::sanitize),
                retryable = releasedState == CommandState.SUBMITTING,
                nextAttemptAt = if (releasedState == CommandState.SUBMITTING) nextAttemptAt else null,
                leaseOwner = null,
                leaseUntil = null,
                updatedAt = now,
                completedAt = if (releasedState == CommandState.CANCELLED) now else command.completedAt,
            )
        }
    }

    suspend fun pauseForProfileChange(newProfileId: String) {
        val changed = database.withTransaction {
            val commands = commandDao.byStates(unresolvedStateNames)
                .filter {
                    it.targetProfileId != newProfileId &&
                        it.state != CommandState.OUTCOME_UNKNOWN
                }
            commands.map { command ->
                val requiresReconciliation = command.runId != null ||
                    command.submissionAttemptCount > 0 ||
                    command.firstSubmissionAttemptAt != null
                val updated = command.copy(
                    state = CommandState.PAUSED,
                    errorCategory = HermesErrorCategory.PROFILE_CHANGED,
                    errorCode = "profile_changed",
                    errorMessage = "Waiting for the previous connection profile",
                    retryable = false,
                    requiresReconciliation = requiresReconciliation,
                    leaseOwner = null,
                    leaseUntil = null,
                    nextAttemptAt = null,
                    updatedAt = clock(),
                )
                commandDao.update(updated)
                updateReceipt(updated)
                updated
            }
        }
        changed.forEach { publish(it.id, it.state) }
    }

    suspend fun requestStop(commandId: Long): StopDisposition {
        val mutation = database.withTransaction {
            val command = commandDao.get(commandId)
                ?: return@withTransaction StopMutation(StopDisposition.NOT_ACCEPTED)
            if (command.kind == CommandKind.WATCH_NOTE || command.state.terminal) {
                return@withTransaction StopMutation(StopDisposition.ALREADY_TERMINAL)
            }
            if (command.state == CommandState.OUTCOME_UNKNOWN || command.state == CommandState.PAUSED) {
                return@withTransaction StopMutation(StopDisposition.RUN_ID_UNAVAILABLE)
            }
            val now = clock()
            val updated = when {
                command.state == CommandState.QUEUED && command.runId == null -> command.copy(
                    state = CommandState.CANCELLED,
                    stopRequested = true,
                    stopRequestedAt = now,
                    updatedAt = now,
                    completedAt = now,
                )
                command.runId != null -> command.copy(
                    state = CommandState.STOPPING,
                    stopRequested = true,
                    stopRequestedAt = now,
                    updatedAt = now,
                )
                command.state == CommandState.SUBMITTING -> command.copy(
                    stopRequested = true,
                    stopRequestedAt = now,
                    updatedAt = now,
                )
                else -> return@withTransaction StopMutation(StopDisposition.NOT_ACCEPTED)
            }
            commandDao.update(updated)
            updateReceipt(updated)
            val disposition = when {
                updated.state == CommandState.CANCELLED -> StopDisposition.CANCELLED_UNSENT
                updated.runId == null -> StopDisposition.WAITING_FOR_ACCEPTANCE
                else -> StopDisposition.STOP_REQUESTED
            }
            StopMutation(disposition, updated.id, updated.state)
        }
        if (mutation.changedState != null) {
            publish(requireNotNull(mutation.commandId), requireNotNull(mutation.changedState))
        }
        return mutation.disposition
    }

    suspend fun clearCurrentHistory(profileId: String): Int = database.withTransaction {
        val now = clock()
        val hasUnresolved = commandDao.byProfileAndStates(
            profileId,
            unresolvedStateNames,
        ).isNotEmpty()
        if (!hasUnresolved) {
            database.conversationMessageDao().deleteForProfile(profileId)
        }
        commandDao.byProfileAndStates(
            profileId,
            terminalStateNames,
        ).map { command ->
            val hidden = command.copy(
                visible = false,
                input = "",
                frozenPayloadJson = null,
                output = null,
                errorMessage = null,
                updatedAt = now,
            )
            commandDao.update(hidden)
            updateReceipt(hidden)
            hidden.id
        }.size
    }

    suspend fun enforceRetention(
        profileId: String,
        retentionMillis: Long,
        nowMillis: Long = clock(),
    ): Int = database.withTransaction {
        val cutoff = nowMillis - retentionMillis
        val removed = commandDao.terminalBefore(profileId, cutoff).map { command ->
            val hidden = command.copy(
                visible = false,
                input = "",
                frozenPayloadJson = null,
                output = null,
                errorMessage = null,
                updatedAt = nowMillis,
            )
            commandDao.update(hidden)
            hidden.id
        }
        val hasUnresolved = commandDao.byProfileAndStates(
            profileId,
            unresolvedStateNames,
        ).isNotEmpty()
        if (!hasUnresolved) {
            database.conversationMessageDao().deleteBefore(profileId, cutoff)
        }
        removed.size
    }

    suspend fun enforceRetentionAll(
        retentionMillis: Long,
        nowMillis: Long = clock(),
    ): Int {
        var removed = 0
        commandDao.profileIds().forEach { profileId ->
            removed += enforceRetention(profileId, retentionMillis, nowMillis)
        }
        return removed
    }

    suspend fun commandsByProfileAndStates(
        profileId: String,
        states: Set<CommandState>,
    ): List<CommandEntity> = commandDao.byProfileAndStates(profileId, states.map { it.name })

    suspend fun commandsWithRunAndStates(states: Set<CommandState>): List<CommandEntity> =
        commandDao.withRunByStates(states.map { it.name })

    internal suspend fun nextQueuedCandidate(
        profileId: String,
        conversationGeneration: Long,
        nowMillis: Long,
    ): CommandEntity? = commandDao.nextQueued(profileId, conversationGeneration, nowMillis)

    internal suspend fun hasEarlierBlockingCommand(
        commandId: Long,
        profileId: String,
        conversationGeneration: Long,
    ): Boolean = commandDao.earlierBlockingCommand(
        profileId,
        conversationGeneration,
        commandId,
    ) != null

    internal suspend fun mutate(
        commandId: Long,
        transform: (CommandEntity) -> CommandEntity,
    ): CommandEntity {
        val updated = database.withTransaction {
            val current = commandDao.get(commandId) ?: throw CommandNotFoundException(commandId)
            val changed = transform(current)
            if (
                changed.id != current.id ||
                changed.kind != current.kind ||
                changed.watchIdentifier != current.watchIdentifier ||
                changed.captureId != current.captureId ||
                changed.targetProfileId != current.targetProfileId ||
                changed.conversationGeneration != current.conversationGeneration ||
                changed.idempotencyKey != current.idempotencyKey ||
                changed.createdAt != current.createdAt
            ) {
                throw ImmutableCommandConflictException()
            }
            if (changed != current) {
                commandDao.update(changed)
                updateReceipt(changed)
            }
            changed
        }
        publish(commandId, updated.state)
        return updated
    }

    private suspend fun updateReceipt(command: CommandEntity) {
        val watchIdentifier = command.watchIdentifier ?: return
        val captureId = command.captureId ?: return
        val existing = receiptDao.get(watchIdentifier, captureId) ?: return
        if (existing.state != command.state || existing.updatedAt != command.updatedAt) {
            receiptDao.update(existing.copy(state = command.state, updatedAt = command.updatedAt))
        }
    }

    private fun publish(commandId: Long, state: CommandState) {
        changes.tryEmit(CommandChange(commandId, state, clock()))
    }

    private fun validateWatchIdentifier(value: String) {
        if (
            value.isEmpty() ||
            value.length > MAX_WATCH_IDENTIFIER_CHARS ||
            value.any { it.isWhitespace() || Character.isISOControl(it.code) || it.code in 0x7f..0x9f }
        ) {
            require(false) { "Invalid watch identifier" }
        }
    }

    private fun validateCaptureId(value: Long) {
        require(value > 0 && value <= MAX_CAPTURE_ID)
    }

    private fun validateInput(value: String, maxBytes: Int) {
        if (value.isBlank() || '\u0000' in value || value.toByteArray(Charsets.UTF_8).size > maxBytes) {
            throw CommandValidationException("Command text is empty or too large")
        }
        val roundTrip = try {
            Charsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(value.toByteArray(Charsets.UTF_8)))
                .toString()
        } catch (_: Exception) {
            throw CommandValidationException("Command text is not valid UTF-8")
        }
        if (roundTrip != value) throw CommandValidationException("Command text is not valid UTF-8")
    }

    private fun sanitize(value: String): String = value
        .map { if (Character.isISOControl(it.code) || it.code in 0x7f..0x9f) ' ' else it }
        .joinToString("")
        .take(MAX_ERROR_MESSAGE_CHARS)

    private fun digest(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256")
        .digest(bytes)
        .joinToString("") { byte -> "%02x".format(byte) }

    private fun mapCommand(command: CommandEntity) = CommandItem(
        id = command.id,
        kind = command.kind,
        watchIdentifier = command.watchIdentifier,
        captureId = command.captureId,
        targetProfileId = command.targetProfileId,
        conversationGeneration = command.conversationGeneration,
        input = command.input,
        state = command.state,
        serverState = command.serverState,
        serverStatusRaw = command.serverStatusRaw,
        runId = command.runId,
        sessionId = command.currentSessionId ?: command.submittedSessionId,
        output = command.output,
        errorCategory = command.errorCategory,
        errorCode = command.errorCode,
        errorMessage = command.errorMessage,
        httpStatus = command.httpStatus,
        retryable = command.retryable,
        requiresReconciliation = command.requiresReconciliation,
        replayed = command.replayed,
        stopRequested = command.stopRequested,
        firstSubmissionAttemptAt = command.firstSubmissionAttemptAt,
        lastSubmissionAttemptAt = command.lastSubmissionAttemptAt,
        nextAttemptAt = command.nextAttemptAt,
        submissionAttemptCount = command.submissionAttemptCount,
        monitorFailureCount = command.monitorFailureCount,
        createdAt = command.createdAt,
        updatedAt = command.updatedAt,
        completedAt = command.completedAt,
    )

    companion object {
        const val DEFAULT_HISTORY_LIMIT = 100
        const val MAX_HISTORY_LIMIT = 500
        const val WATCH_INPUT_MAX_BYTES = 1024
        const val PHONE_INPUT_MAX_BYTES = 16 * 1024
        const val DEFAULT_RETENTION_MILLIS = 30L * 24L * 60L * 60L * 1000L
        val unresolvedStateNames = setOf(
            CommandState.QUEUED,
            CommandState.SUBMITTING,
            CommandState.ACCEPTED,
            CommandState.RUNNING,
            CommandState.NEEDS_APPROVAL,
            CommandState.STOPPING,
            CommandState.OUTCOME_UNKNOWN,
            CommandState.PAUSED,
        ).map { it.name }
        val terminalStateNames = setOf(
            CommandState.COMPLETED,
            CommandState.FAILED,
            CommandState.CANCELLED,
            CommandState.INTERRUPTED,
        ).map { it.name }
        private const val MAX_WATCH_IDENTIFIER_CHARS = 512
        private const val MAX_CAPTURE_ID = 0xffff_ffffL
        private const val MAX_ERROR_MESSAGE_CHARS = 1024
        private const val MAX_SERVER_STATUS_CHARS = 128
    }
}

data class CommandItem(
    val id: Long,
    val kind: CommandKind,
    val watchIdentifier: String?,
    val captureId: Long?,
    val targetProfileId: String,
    val conversationGeneration: Long,
    val input: String,
    val state: CommandState,
    val serverState: String?,
    val serverStatusRaw: String?,
    val runId: String?,
    val sessionId: String?,
    val output: String?,
    val errorCategory: HermesErrorCategory?,
    val errorCode: String?,
    val errorMessage: String?,
    val httpStatus: Int?,
    val retryable: Boolean,
    val requiresReconciliation: Boolean,
    val replayed: Boolean,
    val stopRequested: Boolean,
    val firstSubmissionAttemptAt: Long?,
    val lastSubmissionAttemptAt: Long?,
    val nextAttemptAt: Long?,
    val submissionAttemptCount: Int,
    val monitorFailureCount: Int,
    val createdAt: Long,
    val updatedAt: Long,
    val completedAt: Long?,
)

data class DurableCommandReceipt(
    val commandId: Long,
    val captureId: Long?,
    val kind: CommandKind,
    val state: CommandState,
    val duplicate: Boolean,
)

data class CommandChange(
    val commandId: Long,
    val state: CommandState,
    val changedAt: Long,
)

private data class StopMutation(
    val disposition: StopDisposition,
    val commandId: Long? = null,
    val changedState: CommandState? = null,
)

enum class StopDisposition {
    CANCELLED_UNSENT,
    STOP_REQUESTED,
    WAITING_FOR_ACCEPTANCE,
    RUN_ID_UNAVAILABLE,
    ALREADY_TERMINAL,
    NOT_ACCEPTED,
}

class CommandValidationException(message: String) : Exception(message)
class DuplicateCaptureConflictException(watchIdentifier: String, captureId: Long) :
    Exception("A different command already exists for watch capture $captureId")
class CommandNotFoundException(commandId: Long) : Exception("Command $commandId was not found")
class ConcurrentCommandMutationException : Exception("Command ownership changed")
class FrozenPayloadConflictException : Exception("Frozen submission payload conflict")
class ImmutableCommandConflictException : Exception("Immutable command state conflict")
