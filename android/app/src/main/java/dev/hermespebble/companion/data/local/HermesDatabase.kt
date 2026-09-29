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
        InkNoteEntity::class,
    ],
    version = 2,
    exportSchema = true,
)
@TypeConverters(HermesDatabaseConverters::class)
abstract class HermesDatabase : RoomDatabase() {
    abstract fun inkNoteDao(): InkNoteDao
    abstract fun commandDao(): CommandDao
    abstract fun captureReceiptDao(): CaptureReceiptDao
    abstract fun conversationSessionDao(): ConversationSessionDao
    abstract fun conversationMessageDao(): ConversationMessageDao

    companion object {
        fun create(context: Context): HermesDatabase = Room.databaseBuilder(
            context.applicationContext,
            HermesDatabase::class.java,
            "hermes-pebble.db",
        ).addMigrations(object : androidx.room.migration.Migration(1, 2) {
            override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                db.execSQL("CREATE TABLE IF NOT EXISTS ink_notes (id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, watchIdentifier TEXT NOT NULL, captureId INTEGER NOT NULL, total INTEGER NOT NULL, checksum INTEGER NOT NULL, bytes BLOB NOT NULL, receivedMask INTEGER NOT NULL, receivedAt INTEGER NOT NULL, completedAt INTEGER, notified INTEGER NOT NULL)")
                db.execSQL("CREATE UNIQUE INDEX IF NOT EXISTS index_ink_notes_watchIdentifier_captureId ON ink_notes (watchIdentifier, captureId)")
            }
        }).build()
    }
}
