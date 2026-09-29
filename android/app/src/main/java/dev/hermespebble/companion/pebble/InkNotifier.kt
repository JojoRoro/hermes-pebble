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
import dev.hermespebble.companion.data.local.HermesDatabase
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

class InkNotifier(private val context: Context, private val database: HermesDatabase) {
    private val mutex = Mutex()
    suspend fun notifySaved() = mutex.withLock {
        if (Build.VERSION.SDK_INT >= 33 && ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return@withLock
        val manager = context.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(NotificationChannel("handwritten-notes", "Handwritten notes", NotificationManager.IMPORTANCE_DEFAULT))
        for (note in database.inkNoteDao().unnotified()) {
            val intent = Intent(context, MainActivity::class.java)
                .setAction("dev.hermespebble.companion.OPEN_INK.${note.id}")
                .putExtra(MainActivity.EXTRA_INK_ID, note.id)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
            val pending = PendingIntent.getActivity(context, note.id.toInt(), intent, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
            val notification = NotificationCompat.Builder(context, "handwritten-notes")
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle("Handwritten note synced")
                .setContentText("Tap to view your handwriting")
                .setContentIntent(pending).setAutoCancel(true).setOnlyAlertOnce(true)
                .setVisibility(NotificationCompat.VISIBILITY_PRIVATE).build()
            try {
                NotificationManagerCompat.from(context).notify("ink", note.id.toInt(), notification)
                database.inkNoteDao().markNotified(note.id)
            } catch (_: SecurityException) { return@withLock }
        }
    }
}
