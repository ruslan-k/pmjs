'use strict';

const path = require('node:path');
const { performance } = require('node:perf_hooks');

const addon = path.resolve(process.argv[2]);
const assets = path.resolve(process.argv[3]);
const iterations = Number(process.argv[4] || 10000);
const native = require(addon);

native.initialize({
  gameRoot: assets,
  assetRoot: '',
  width: 128,
  height: 128,
  windowTitle: 'pmjs steady-state perf'
});

for (let i = 0; i < 500; i++) {
  native.pollEvents();
  if (native.runtime.windowStateBits) native.runtime.windowStateBits();
  native.input.snapshot();
  native.beginFrame();
  native.renderFrame();
}
if (global.gc) global.gc();

const rssBefore = process.memoryUsage().rss;
const cpuBefore = process.cpuUsage();
const started = performance.now();
for (let i = 0; i < iterations; i++) {
  if (!native.pollEvents()) throw new Error('unexpected quit');
  if (native.runtime.windowStateBits) native.runtime.windowStateBits();
  native.input.snapshot();
  native.beginFrame();
  native.renderFrame();
}
const elapsedMs = performance.now() - started;
const cpu = process.cpuUsage(cpuBefore);
if (global.gc) global.gc();
const rssAfter = process.memoryUsage().rss;
const cpuMs = (cpu.user + cpu.system) / 1000;

console.log(JSON.stringify({
  benchmark: 'native-steady-state-frame',
  iterations,
  wall_us_per_frame: elapsedMs * 1000 / iterations,
  cpu_us_per_frame: cpuMs * 1000 / iterations,
  rss_before_mb: rssBefore / (1024 * 1024),
  rss_after_mb: rssAfter / (1024 * 1024),
  rss_drift_mb: (rssAfter - rssBefore) / (1024 * 1024)
}));
