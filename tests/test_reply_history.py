"""Execute the actual DAO query to check conversation/profile isolation."""
import json
from pathlib import Path
import re
import sqlite3
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReplyHistoryTests(unittest.TestCase):
    def test_only_prior_completed_turns_from_this_conversation(self):
        schema = json.loads((ROOT / 'android/app/schemas/dev.hermespebble.companion.data.local.HermesDatabase/2.json').read_text())['database']
        entity = next(e for e in schema['entities'] if e['tableName'] == 'commands')
        db = sqlite3.connect(':memory:')
        db.execute(entity['createSql'].replace('${TABLE_NAME}', 'commands'))
        columns = db.execute('PRAGMA table_info(commands)').fetchall()
        defaults = {name: (0 if 'INT' in kind else '') for _, name, kind, required, default, _ in columns if required and default is None}
        def insert(id, **changes):
            row = dict(defaults, id=id, targetProfileId='profile', conversationGeneration=3,
                       state='COMPLETED', kind='WATCH_REQUEST', input=f'user {id}', output=f'answer {id}')
            row.update(changes)
            db.execute(f"INSERT INTO commands ({','.join(row)}) VALUES ({','.join('?' for _ in row)})", tuple(row.values()))
        insert(1)
        insert(2, targetProfileId='other')
        insert(3, conversationGeneration=4)
        insert(4, kind='WATCH_NOTE')
        insert(5, state='RUNNING')
        insert(6, state='FAILED')
        insert(7, input='', output=None)
        insert(8, kind='PHONE_REQUEST')
        insert(9)
        source = (ROOT / 'android/app/src/main/java/dev/hermespebble/companion/data/local/Daos.kt').read_text()
        query = re.search(r'@Query\("""(.*?)"""\)\s*suspend fun replyHistory', source, re.S).group(1)
        rows = db.execute(query, dict(profileId='profile', generation=3, beforeId=9)).fetchall()
        self.assertEqual(rows, [('user 8', 'answer 8'), ('user 1', 'answer 1')])
        self.assertEqual(db.execute(query, dict(profileId='profile', generation=99, beforeId=9)).fetchall(), [])
        for id in range(10, 45): insert(id)
        rows = db.execute(query, dict(profileId='profile', generation=3, beforeId=45)).fetchall()
        self.assertEqual(len(rows), 20)
        self.assertEqual(rows[0], ('user 44', 'answer 44'))
        self.assertEqual(rows[-1], ('user 25', 'answer 25'))
