'use strict';

const path = require('node:path');
const { performance } = require('node:perf_hooks');

const addon = path.resolve(process.argv[2]);
const assets = path.resolve(process.argv[3]);
const native = require(addon);
native.initialize({
  gameRoot: assets,
  assetRoot: '',
  width: 128,
  height: 128,
  windowTitle: 'pmjs perf'
});
const startupRendererStats = native.render.stats();
const startupRenderTargetBytes = Number.isFinite(startupRendererStats.renderTargetBytes)
  ? startupRendererStats.renderTargetBytes
  : 128 * 128 * 36;

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

function sample(name, iterations, fn) {
  for (let i = 0; i < Math.min(iterations, 20); i++) fn(i);
  const values = [];
  for (let run = 0; run < 5; run++) {
    if (global.gc) global.gc();
    const started = performance.now();
    for (let i = 0; i < iterations; i++) fn(i);
    values.push((performance.now() - started) * 1000 / iterations);
  }
  return {
    benchmark: name,
    iterations,
    median_us_per_op: median(values),
    samples_us_per_op: values
  };
}

const results = [];

results.push(sample('napi-four-args', 250000, () => {
  native.render.setClearColor(0.1, 0.2, 0.3, 1);
}));

results.push(sample('input-snapshot-idle', 250000, () => {
  native.input.snapshot();
}));

const canvas = native.canvas.create(256, 256);
// Force deferred canvas realization so these measurements cover the CPU pixel
// hot paths rather than command queuing.
native.canvas.readPixels(canvas.handle, 0, 0, 1, 1);

results.push(sample('canvas-fill-clear-256', 80, () => {
  native.canvas.fillRect(canvas.handle, 0, 0, 256, 256, 0x345678ff);
  native.canvas.clearRect(canvas.handle, 0, 0, 256, 256);
}));

native.canvas.fillRect(canvas.handle, 0, 0, 256, 256, 0x89abcde0);
results.push(sample('canvas-readback-256', 120, () => {
  const pixels = native.canvas.readPixels(canvas.handle, 0, 0, 256, 256);
  if (pixels.length !== 256 * 256 * 4) throw new Error('bad readback');
}));

results.push(sample('canvas-blur-256', 12, () => {
  native.canvas.blur(canvas.handle);
}));

function slotRound() {
  const handles = [];
  for (let i = 0; i < 4096; i++) handles.push(native.canvas.create(1, 1).handle);
  for (let i = 0; i < handles.length; i += 2) {
    native.canvas.release(handles[i]);
    handles[i] = 0;
  }
  const started = performance.now();
  const replacements = [];
  for (let i = 0; i < 2048; i++) replacements.push(native.canvas.create(1, 1).handle);
  const elapsedUs = (performance.now() - started) * 1000 / replacements.length;
  for (const handle of handles) if (handle) native.canvas.release(handle);
  for (const handle of replacements) native.canvas.release(handle);
  return elapsedUs;
}
const slotSamples = [];
for (let i = 0; i < 5; i++) slotSamples.push(slotRound());
results.push({
  benchmark: 'canvas-slot-reuse',
  iterations: 2048,
  median_us_per_op: median(slotSamples),
  samples_us_per_op: slotSamples
});

const image = native.images.load('fixture.png');
const count = 512;
const stride = native.scene.schema.valueStride;
const metadata = new Uint32Array(count * native.scene.schema.metadataStride);
const values = new Float32Array(count * stride);
for (let i = 0; i < count; i++) {
  const mo = i * 7;
  metadata[mo] = 1;
  metadata[mo + 1] = 0xffffffff;
  metadata[mo + 2] = image.handle;
  metadata[mo + 3] = 0xffffff;
  const vo = i * stride;
  values[vo] = 1;
  values[vo + 3] = 1;
  values[vo + 6] = 1;
  values[vo + 9] = 0;
  values[vo + 10] = 0;
  values[vo + 11] = 2;
  values[vo + 12] = 2;
  values[vo + 13] = 2;
  values[vo + 14] = 2;
}
results.push(sample('scene-submit-512', 300, () => {
  native.beginFrame();
  native.scene.submit(native.scene.packetVersion, metadata, values, count);
}));
results.push(sample('scene-render-512', 120, () => {
  native.beginFrame();
  native.scene.submit(native.scene.packetVersion, metadata, values, count);
  native.renderFrame();
}));

// Stress bounded-filter bookkeeping with many disjoint sprites. This models
// RPG Maker maps/lighting groups where a filter encloses hundreds of display
// objects and catches accidental O(n^2) region compaction.
const filteredSprites = 512;
const filteredCount = filteredSprites + 2;
const metadataStride = native.scene.schema.metadataStride;
const filterMetadata = new Uint32Array(filteredCount * metadataStride);
const filterValues = new Float32Array(filteredCount * stride);
function initNode(index, kind, resource, blend) {
  const mo = index * metadataStride;
  filterMetadata[mo] = kind;
  filterMetadata[mo + 1] = 0xffffffff;
  filterMetadata[mo + 2] = resource;
  filterMetadata[mo + 3] = 0xffffff;
  filterMetadata[mo + 4] = blend || 0;
  const vo = index * stride;
  filterValues[vo] = 1;
  filterValues[vo + 3] = 1;
  filterValues[vo + 6] = 1;
  return vo;
}
// filterBegin + alpha filter (kind 20) has zero padding, enabling region bounds.
let vo = initNode(0, 6, 0, 20);
filterValues[vo + 7] = 1;
filterValues[vo + 33] = 1;
for (let i = 0; i < filteredSprites; i++) {
  vo = initNode(i + 1, 1, image.handle, 0);
  filterValues[vo + 4] = (i % 32) * 4;
  filterValues[vo + 5] = Math.floor(i / 32) * 4;
  filterValues[vo + 9] = 0;
  filterValues[vo + 10] = 0;
  filterValues[vo + 11] = 2;
  filterValues[vo + 12] = 2;
  filterValues[vo + 13] = 2;
  filterValues[vo + 14] = 2;
}
initNode(filteredCount - 1, 7, 0, 0);
results.push(sample('scene-filter-bounds-512', 80, () => {
  native.beginFrame();
  native.scene.submit(native.scene.packetVersion,
    filterMetadata, filterValues, filteredCount);
  native.renderFrame();
}));
native.beginFrame();

native.images.release(image.handle);
native.canvas.release(canvas.handle);

console.log(JSON.stringify({
  results,
  startup_render_target_bytes: startupRenderTargetBytes,
  final_render_target_bytes: Number(native.render.stats().renderTargetBytes || startupRenderTargetBytes),
  rss_mb: process.memoryUsage().rss / (1024 * 1024)
}));
