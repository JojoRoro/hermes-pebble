package dev.hermespebble.companion.data.local

import android.content.Context
import androidx.room.Database
import androidx.room.Room
import androidx.room.RoomDatabase
import androidx.room.TypeConverters

@Database(
    entities = [
        CommandEntity::class,
        CaptureReceiptEntity::class,
        ConversationSessionEntity::class,
        ConversationMessageEntity::class,
    ],
    version = 1,
    exportSchema = true,
)
@TypeConverters(HermesDatabaseConverters::class)
abstract class HermesDatabase : RoomDatabase() {
    abstract fun commandDao(): CommandDao
    abstract fun captureReceiptDao(): CaptureReceiptDao
    abstract fun conversationSessionDao(): ConversationSessionDao
    abstract fun conversationMessageDao(): ConversationMessageDao

    companion object {
        fun create(context: Context): HermesDatabase = Room.databaseBuilder(
            context.applicationContext,
            HermesDatabase::class.java,
            "hermes-pebble.db",
        ).build()
    }
}
