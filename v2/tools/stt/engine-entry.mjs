// The on-phone speech engine.
//
// Built by build.mjs into one classic script. The PebbleKit JS side keeps that script as a string
// and evals it the first time speech is needed, so the SDK's bundler never has to parse it and an
// app that never listens never pays for it.
//
// The host owns every byte that crosses the network: it downloads the runtime and the model files,
// checks their hashes, keeps them in IndexedDB and hands them over here. This file never fetches.

import { env, pipeline } from '@huggingface/transformers';

let recognizer = null;
let loadedKey = null;

function notFound() {
  return new Response('', { status: 404, statusText: 'Not Found' });
}

/**
 * @param {object} options
 * @param {ArrayBuffer} options.wasmBinary The ONNX Runtime WebAssembly binary.
 * @param {string} options.repo Hugging Face repo id, used only to recognise file requests.
 * @param {string} options.dtype Weight format, e.g. "q8".
 * @param {(path: string) => Promise<ArrayBuffer|null>} options.readFile Reads a model file the
 *     host stored, by its path in the repo.
 */
async function load(options) {
  const key = options.repo + '@' + options.dtype;
  if (recognizer && loadedKey === key) return;
  if (recognizer) await unload();

  env.allowLocalModels = false;
  env.allowRemoteModels = true;
  env.useBrowserCache = false;
  env.useFSCache = false;
  env.useCustomCache = true;
  // Anything the library asks for that the host did not store does not exist.
  env.fetch = async () => notFound();
  env.customCache = {
    async match(request) {
      const url = typeof request === 'string' ? request : request.url;
      const marker = '/' + options.repo + '/resolve/';
      const at = url.indexOf(marker);
      if (at < 0) return undefined;
      // .../resolve/<revision>/<path>
      const rest = url.slice(at + marker.length);
      const path = rest.slice(rest.indexOf('/') + 1);
      const bytes = await options.readFile(decodeURIComponent(path));
      if (!bytes) return undefined;
      return new Response(bytes, { status: 200, headers: { 'content-length': String(bytes.byteLength) } });
    },
    async put() {},
  };

  // The library would otherwise fetch its preferred runtime build from a CDN. This bundle carries
  // the loader for the plain WebAssembly build, and the host supplies the matching binary.
  env.useWasmCache = false;
  const wasm = env.backends.onnx.wasm;
  wasm.wasmPaths = undefined;
  wasm.numThreads = 1;   // no workers and no SharedArrayBuffer where this runs
  wasm.proxy = false;
  wasm.wasmBinary = options.wasmBinary;

  recognizer = await pipeline('automatic-speech-recognition', options.repo, {
    dtype: options.dtype,
    device: 'wasm',
  });
  loadedKey = key;
}

/**
 * @param {Float32Array} samples 16 kHz mono, -1..1.
 * @param {object} [options]
 * @param {string} [options.language] Language name or code for multilingual models.
 * @returns {Promise<string>}
 */
async function transcribe(samples, options) {
  if (!recognizer) throw new Error('speech engine is not loaded');
  const settings = {};
  if (options && options.multilingual) {
    settings.task = 'transcribe';
    if (options.language) settings.language = options.language;
  }
  if (options && options.maxNewTokens) settings.max_new_tokens = options.maxNewTokens;
  const result = await recognizer(samples, settings);
  const text = Array.isArray(result) ? result.map((r) => r.text).join(' ') : result.text;
  return String(text || '').replace(/\s+/g, ' ').trim();
}

async function unload() {
  const old = recognizer;
  recognizer = null;
  loadedKey = null;
  if (old && typeof old.dispose === 'function') {
    try { await old.dispose(); } catch (e) { /* already gone */ }
  }
}

globalThis.HermesSTT = {
  load,
  transcribe,
  unload,
  loaded: () => loadedKey,
};
