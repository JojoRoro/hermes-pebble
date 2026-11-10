// Builds the on-phone speech engine and the list of files it needs at runtime.
//
//   node build.mjs            write ../../src/pkjs/stt_engine.json
//   node build.mjs --catalog  also refresh ../../src/pkjs/stt_catalog.json from Hugging Face
//
// stt_engine.json holds the engine as a string: { "code": "..." }. The SDK bundles PebbleKit JS
// with webpack 1, whose parser stops at modern syntax, but it never looks inside a string.

import { build } from 'esbuild';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const outDir = join(here, '../../src/pkjs');
const ortDir = join(here, 'node_modules/onnxruntime-web');
const WASM_NAME = 'ort-wasm-simd-threaded.wasm';

const result = await build({
  entryPoints: [join(here, 'engine-entry.mjs')],
  bundle: true,
  format: 'iife',
  platform: 'browser',
  target: 'es2020',
  minify: true,
  write: false,
  legalComments: 'none',
  // The wasm-only runtime with its loader inlined: nothing to import at runtime, and the binary
  // is handed over as bytes.
  alias: {
    'onnxruntime-web/webgpu': join(ortDir, 'dist/ort.wasm.bundle.min.mjs'),
    'onnxruntime-web': join(ortDir, 'dist/ort.wasm.bundle.min.mjs'),
  },
  define: {
    // A classic script has no import.meta. The value only has to parse as a URL.
    'import.meta.url': '"https://hermes-stt.invalid/engine.js"',
  },
  logLevel: 'warning',
});

const code = result.outputFiles[0].text;
const ortVersion = JSON.parse(readFileSync(join(ortDir, 'package.json'), 'utf8')).version;
const wasm = readFileSync(join(ortDir, 'dist', WASM_NAME));

mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, 'stt_engine.json'), JSON.stringify({
  code,
  runtime: {
    url: `https://cdn.jsdelivr.net/npm/onnxruntime-web@${ortVersion}/dist/${WASM_NAME}`,
    size: wasm.length,
    sha256: createHash('sha256').update(wasm).digest('hex'),
  },
}));
console.log(`engine: ${(code.length / 1024).toFixed(0)} KB, runtime ${ortVersion}: ${(wasm.length / 1048576).toFixed(1)} MB`);

if (process.argv.includes('--catalog')) {
  const { refreshCatalog } = await import('./catalog.mjs');
  await refreshCatalog(join(outDir, 'stt_catalog.json'));
}
