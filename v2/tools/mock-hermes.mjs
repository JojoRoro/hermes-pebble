// A stand-in for the Hermes Agent API server, for the emulator and the tests.
//
//   node tools/mock-hermes.mjs [port] [api-key]
//
// Runs move queued -> running -> completed on a clock. Words in the question steer the outcome:
// "approval" waits for approval, "fail" fails, "long" answers at length, "slow" takes a minute.

import { createServer } from 'node:http';
import { pathToFileURL } from 'node:url';

const LONG = [
  '## Plan for the week',
  '',
  'Here is **what I found** in your calendar and inbox:',
  '',
  '- Monday: dentist at 09:30, then the *quarterly review* at 14:00.',
  '- Tuesday: nothing fixed. A good day for the report.',
  '- Wednesday: flight to Zürich at 07:15 — leave by 05:30.',
  '',
  'The report is the item most at risk. It needs about four hours and the only free block that long is Tuesday morning. I would protect it now.',
  '',
  '| Day | Free hours |',
  '| --- | --- |',
  '| Mon | 2 |',
  '| Tue | 7 |',
  '',
  'Three things I can do next, if you want:',
  '',
  '1. Block Tuesday 08:00 to 12:00 for the report.',
  '2. Book a taxi for Wednesday at 05:15.',
  '3. Draft the agenda for the quarterly review.',
  '',
  'Say which, or say `all three`. '.repeat(1),
  '',
  'One more note: the review moved rooms last time without notice, so I will check the room on Monday morning and tell you if it changes. '.repeat(6),
].join('\n');

export function createMock(apiKey = 'test-key', clock = () => Date.now()) {
  const runs = new Map();
  const byKey = new Map();
  const log = [];
  let counter = 0;

  function view(run) {
    const age = clock() - run.createdAt;
    const pace = run.slow ? 60000 : 4000;
    let status = run.status;
    if (!status) {
      if (age < pace / 4) status = 'queued';
      else if (run.kind === 'approval') status = 'waiting_for_approval';
      else if (age < pace) status = 'running';
      else status = run.kind === 'fail' ? 'failed' : 'completed';
    }
    const body = { object: 'hermes.run', run_id: run.id, status, session_id: run.sessionId, model: 'hermes-agent' };
    if (status === 'completed') {
      body.output = run.kind === 'long' ? LONG
        : `You said: “${run.input}”. This is turn ${run.turn} of the conversation — all good.`;
    }
    if (status === 'failed') body.error = { message: 'The tool crashed while reading your calendar' };
    return body;
  }

  function handle(method, path, headers, body) {
    log.push({ method, path, headers, body });
    if (method === 'GET' && (path === '/health' || path === '/v1/health')) return [200, { status: 'ok' }];
    if (headers.authorization !== 'Bearer ' + apiKey) return [401, { error: { message: 'Invalid API key' } }];
    if (method === 'GET' && path === '/v1/models') return [200, { object: 'list', data: [{ id: 'hermes-agent' }] }];
    if (method === 'POST' && path === '/v1/runs') {
      const key = headers['idempotency-key'];
      if (key && byKey.has(key)) return [202, { run_id: byKey.get(key), status: 'started' }, { 'Idempotency-Replayed': 'true' }];
      if (body.session_id && ![...runs.values()].some((r) => r.sessionId === body.session_id)) {
        return [404, { error: { message: 'Unknown session' } }];
      }
      const input = String(body.input || '');
      const sessionId = body.session_id || 'sess_' + (++counter);
      const turn = [...runs.values()].filter((r) => r.sessionId === sessionId).length + 1;
      const kind = /approval/i.test(input) ? 'approval' : /\bfail/i.test(input) ? 'fail' : /\blong\b/i.test(input) ? 'long' : 'ok';
      const run = {
        id: 'run_' + (++counter), input, sessionId, turn, kind, slow: /\bslow\b/i.test(input),
        createdAt: clock(), status: '', instructions: body.instructions || '',
      };
      runs.set(run.id, run);
      if (key) byKey.set(key, run.id);
      return [202, { run_id: run.id, status: 'started' }];
    }
    const match = /^\/v1\/runs\/([^/]+)(\/stop)?$/.exec(path);
    if (match) {
      const run = runs.get(match[1]);
      if (!run) return [404, { error: { message: 'Unknown run' } }];
      if (match[2] && method === 'POST') {
        if (view(run).status !== 'completed') run.status = 'cancelled';
        return [200, { status: 'stopping' }];
      }
      if (method === 'GET') return [200, view(run)];
    }
    return [404, { error: { message: 'Not found' } }];
  }

  return { handle, runs, log };
}

export function serve(port, apiKey) {
  const mock = createMock(apiKey);
  const server = createServer((request, response) => {
    let raw = '';
    request.on('data', (chunk) => { raw += chunk; });
    request.on('end', () => {
      let body = {};
      try { body = raw ? JSON.parse(raw) : {}; } catch (e) { /* not JSON */ }
      const [status, payload, extra] = mock.handle(request.method, new URL(request.url, 'http://x').pathname, request.headers, body);
      response.writeHead(status, { 'content-type': 'application/json', ...(extra || {}) });
      response.end(JSON.stringify(payload));
      console.log(request.method, request.url, status);
    });
  });
  server.listen(port, '127.0.0.1', () => console.log(`mock Hermes on http://127.0.0.1:${port} (key ${apiKey})`));
  return server;
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  serve(Number(process.argv[2]) || 8642, process.argv[3] || 'test-key');
}
