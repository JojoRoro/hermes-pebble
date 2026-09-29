"""Exercise the handwritten-note migration against Room's generated schema."""
import json
from pathlib import Path
import re
import sqlite3
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / 'android/app/schemas/dev.hermespebble.companion.data.local.HermesDatabase/2.json'


class InkMigrationTests(unittest.TestCase):
    def test_migration_preserves_existing_tables_and_matches_room(self):
        schema = json.loads(SCHEMA.read_text())['database']
        connection = sqlite3.connect(':memory:')
        # v1 entity definitions are unchanged; create all pre-existing tables.
        original = [e for e in schema['entities'] if e['tableName'] != 'ink_notes']
        for entity in original:
            connection.execute(entity['createSql'].replace('${TABLE_NAME}', entity['tableName']))
        old_sql = connection.execute("SELECT name, sql FROM sqlite_master WHERE type='table'").fetchall()
        connection.execute("INSERT INTO conversation_sessions (profileId, conversationGeneration, sessionId, serverConfirmed, createdAt, updatedAt) VALUES ('existing', 4, 'session-kept', 1, 100, 200)")
        source = (ROOT / 'android/app/src/main/java/dev/hermespebble/companion/data/local/HermesDatabase.kt').read_text()
        for sql in re.findall(r'db\.execSQL\("([^"\n]+)"\)', source):
            connection.execute(sql)
        self.assertEqual(connection.execute('SELECT sessionId FROM conversation_sessions').fetchone(), ('session-kept',))
        for name, sql in old_sql:
            self.assertEqual(connection.execute('SELECT sql FROM sqlite_master WHERE name=?', (name,)).fetchone(), (sql,))
        expected = sqlite3.connect(':memory:')
        ink = next(e for e in schema['entities'] if e['tableName'] == 'ink_notes')
        expected.execute(ink['createSql'].replace('${TABLE_NAME}', 'ink_notes'))
        self.assertEqual(connection.execute('PRAGMA table_info(ink_notes)').fetchall(), expected.execute('PRAGMA table_info(ink_notes)').fetchall())
        # Partial chunks and completed bytes can be durably stored; capture identity is unique.
        row = ('watch', 15, 20, 42, b'ink', 1, 100, None, 0)
        connection.execute('INSERT INTO ink_notes (watchIdentifier,captureId,total,checksum,bytes,receivedMask,receivedAt,completedAt,notified) VALUES (?,?,?,?,?,?,?,?,?)', row)
        with self.assertRaises(sqlite3.IntegrityError):
            connection.execute('INSERT INTO ink_notes (watchIdentifier,captureId,total,checksum,bytes,receivedMask,receivedAt,completedAt,notified) VALUES (?,?,?,?,?,?,?,?,?)', row)


if __name__ == '__main__':
    unittest.main()
