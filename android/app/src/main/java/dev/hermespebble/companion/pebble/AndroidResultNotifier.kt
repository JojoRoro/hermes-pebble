package dev.hermespebble.companion.pebble

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import dev.hermespebble.companion.MainActivity
import dev.hermespebble.companion.data.local.CommandRepository
import dev.hermespebble.companion.data.preferences.SettingsRepository
import dev.hermespebble.companion.dispatch.ResultNotifier

class AndroidResultNotifier(
    context: Context,
    private val settingsRepository: SettingsRepository,
    private val commandRepository: CommandRepository,
) : ResultNotifier {
    private val applicationContext = context.applicationContext

    init {
        val manager = applicationContext.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(
                CHANNEL_ID,
                applicationContext.getString(dev.hermespebble.companion.R.string.notification_channel_name),
                NotificationManager.IMPORTANCE_DEFAULT,
            ).apply {
                description = "Hermes request results"
                setShowBadge(true)
            },
        )
    }

    override suspend fun notifyCompleted(commandId: Long, captureId: Long?) {
        if (!settingsRepository.current().resultNotificationsEnabled) return
        if (
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(applicationContext, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        val command = commandRepository.get(commandId) ?: return
        val intent = Intent(applicationContext, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP
            putExtra(MainActivity.EXTRA_COMMAND_ID, commandId)
        }
        val pendingIntent = PendingIntent.getActivity(
            applicationContext,
            commandId.coerceIn(1, Int.MAX_VALUE.toLong()).toInt(),
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val publicNotification = NotificationCompat.Builder(applicationContext, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_notify_sync)
            .setContentTitle("Hermes request finished")
            .setContentText("Open Hermes Pebble for the result")
            .setAutoCancel(true)
            .build()
        val notification = NotificationCompat.Builder(applicationContext, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_notify_sync)
            .setContentTitle("Hermes: ${command.state.name}")
            .setContentText(command.output?.take(160) ?: "Open the app for details")
            .setStyle(NotificationCompat.BigTextStyle().bigText(command.output ?: "Open the app for details"))
            .setContentIntent(pendingIntent)
            .setAutoCancel(true)
            .setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
            .setPublicVersion(publicNotification)
            .setOnlyAlertOnce(true)
            .build()
        try {
            NotificationManagerCompat.from(applicationContext).notify(commandId.toInt(), notification)
        } catch (_: SecurityException) {
        }
    }

    private companion object {
        const val CHANNEL_ID = "hermes-results"
    }
}
