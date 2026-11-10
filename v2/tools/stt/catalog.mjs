// Pins each offered speech model to one Hugging Face revision, with the size and SHA-256 of every
// file the engine loads. The phone refuses a download that does not match.

import { writeFileSync } from 'node:fs';

const MODELS = [
  {
    id: 'moonshine-tiny',
    name: 'Moonshine Tiny',
    note: 'English. Fastest.',
    repo: 'onnx-community/moonshine-tiny-ONNX',
    multilingual: false,
  },
  {
    id: 'moonshine-base',
    name: 'Moonshine Base',
    note: 'English. More accurate, slower.',
    repo: 'onnx-community/moonshine-base-ONNX',
    multilingual: false,
  },
  {
    id: 'whisper-tiny',
    name: 'Whisper Tiny',
    note: '99 languages.',
    repo: 'onnx-community/whisper-tiny',
    multilingual: true,
  },
  {
    id: 'whisper-base',
    name: 'Whisper Base',
    note: '99 languages. More accurate, slowest.',
    repo: 'onnx-community/whisper-base',
    multilingual: true,
  },
];

// What an automatic-speech-recognition pipeline reads with dtype "q8".
const WANTED = [
  'config.json',
  'generation_config.json',
  'preprocessor_config.json',
  'tokenizer.json',
  'tokenizer_config.json',
  'onnx/encoder_model_quantized.onnx',
  'onnx/decoder_model_merged_quantized.onnx',
];

async function getJson(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
  return response.json();
}

async function sha256Of(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
  const bytes = new Uint8Array(await response.arrayBuffer());
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return { size: bytes.length, sha256: Buffer.from(digest).toString('hex') };
}

export async function refreshCatalog(outFile) {
  const models = [];
  for (const model of MODELS) {
    const info = await getJson(`https://huggingface.co/api/models/${model.repo}`);
    const revision = info.sha;
    const tree = await getJson(
      `https://huggingface.co/api/models/${model.repo}/tree/${revision}?recursive=true`);
    const files = [];
    for (const path of WANTED) {
      const entry = tree.find((f) => f.path === path);
      if (!entry) throw new Error(`${model.repo} has no ${path}`);
      if (entry.lfs && entry.lfs.oid) {
        files.push({ path, size: entry.lfs.size, sha256: entry.lfs.oid });
      } else {
        // Small files are not in LFS, so their hash has to be computed.
        const hashed = await sha256Of(
          `https://huggingface.co/${model.repo}/resolve/${revision}/${path}`);
        files.push({ path, size: hashed.size, sha256: hashed.sha256 });
      }
    }
    const size = files.reduce((sum, f) => sum + f.size, 0);
    models.push({ ...model, revision, dtype: 'q8', size, files });
    console.log(`${model.id}: ${(size / 1048576).toFixed(1)} MB at ${revision.slice(0, 8)}`);
  }
  writeFileSync(outFile, JSON.stringify({ models }, null, 1) + '\n');
}
