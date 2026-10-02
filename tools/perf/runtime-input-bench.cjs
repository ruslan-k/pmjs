'use strict';

const fs = require('node:fs');
const vm = require('node:vm');
const { performance } = require('node:perf_hooks');

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

function loadRuntime(filename) {
  const context = {
    console: { log() {}, warn() {}, error() {} },
    NativeHost: {
      runtime: {
        env() { return ''; },
        displaySize() { return { width: 640, height: 480 }; },
        windowSize() { return { width: 640, height: 480 }; },
        platform() { return { platform: 'linux', arch: 'arm64' }; },
        now() { return performance.now(); },
        setWindowTitle() {}
      }
    },
    __pmjsGameInfo: {
      title: 'perf', width: 640, height: 480,
      displayWidth: 640, displayHeight: 480
    }
  };
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(filename, 'utf8'), context, { filename });
  return context;
}

function measure(filename, iterations, edgeMode) {
  const samples = [];
  const heapSamples = [];
  for (let run = 0; run < 7; run++) {
    const c = loadRuntime(filename);
    const pad = {
      id: 'perf pad',
      index: 0,
      instance: 1,
      connected: true,
      buttonsDown: [0, 4, 12],
      buttonsPressed: edgeMode ? [1, 5, 9] : [],
      axes: [0.2, -0.3, 0, 0]
    };
    const state = {
      keysDown: [], keysPressed: [], keyEvents: [], gamepads: [pad]
    };
    for (let i = 0; i < 20000; i++) {
      c.__pmjsReceiveInput(state);
      c.__pmjsFinishInputStep();
    }
    if (global.gc) global.gc();
    const heapBefore = process.memoryUsage().heapUsed;
    const started = performance.now();
    for (let i = 0; i < iterations; i++) {
      c.__pmjsReceiveInput(state);
      c.__pmjsFinishInputStep();
    }
    const elapsed = performance.now() - started;
    const heapAfter = process.memoryUsage().heapUsed;
    samples.push(elapsed * 1e6 / iterations);
    heapSamples.push((heapAfter - heapBefore) / 1024);
  }
  return {
    median_ns_per_update: median(samples),
    samples_ns_per_update: samples,
    median_heap_delta_kb: median(heapSamples),
    samples_heap_delta_kb: heapSamples
  };
}

const filename = process.argv[2];
const iterations = Number(process.argv[3] || 200000);
if (!filename) {
  throw new Error('usage: node --expose-gc runtime-input-bench.cjs <runtime.js> [iterations]');
}
console.log(JSON.stringify({
  benchmark: 'web-runtime-gamepad',
  iterations,
  steady: measure(filename, iterations, false),
  edge: measure(filename, iterations, true)
}));
