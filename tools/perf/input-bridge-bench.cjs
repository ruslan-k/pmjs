'use strict';

const fs = require('node:fs');
const vm = require('node:vm');
const { performance } = require('node:perf_hooks');

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

function loadBridge(filename) {
  let install = null;
  globalThis.PMJS = {
    phases: {
      on(_phase, _owner, callback) { install = callback; }
    }
  };
  globalThis.NativeHost = { input: { consumePressed() {} } };
  globalThis.__pmjsFinishInputStep = function() {};
  globalThis.__pmjsInputSnapshot = {
    keysDown: [],
    keysPressed: [],
    keyEvents: [],
    gamepads: []
  };
  globalThis.Input = {
    keyMapper: {
      13: 'ok', 27: 'escape', 32: 'ok', 37: 'left', 38: 'up',
      39: 'right', 40: 'down', 65: 'pageup', 83: 'pagedown',
      88: 'escape', 90: 'ok'
    },
    gamepadMapper: {
      0: 'ok', 1: 'cancel', 2: 'shift', 3: 'menu',
      4: 'pageup', 5: 'pagedown', 12: 'up', 13: 'down',
      14: 'left', 15: 'right'
    },
    _currentState: Object.create(null),
    _gamepadStates: [],
    update() {}
  };
  vm.runInThisContext(fs.readFileSync(filename, 'utf8'), { filename });
  if (typeof install !== 'function') throw new Error('input bridge did not register');
  install();
}

function run(filename, iterations) {
  loadBridge(filename);
  for (let i = 0; i < 20000; i++) Input.update();

  const timeSamples = [];
  const heapSamples = [];
  for (let sample = 0; sample < 7; sample++) {
    if (global.gc) global.gc();
    const heapBefore = process.memoryUsage().heapUsed;
    const started = performance.now();
    for (let i = 0; i < iterations; i++) Input.update();
    const elapsed = performance.now() - started;
    const heapAfter = process.memoryUsage().heapUsed;
    timeSamples.push(elapsed * 1e6 / iterations);
    heapSamples.push((heapAfter - heapBefore) / 1024);
  }
  return {
    benchmark: 'rpgmaker-input-steady-state',
    iterations,
    median_ns_per_update: median(timeSamples),
    samples_ns_per_update: timeSamples,
    median_heap_delta_kb: median(heapSamples),
    samples_heap_delta_kb: heapSamples
  };
}

const filename = process.argv[2];
const iterations = Number(process.argv[3] || 300000);
if (!filename) {
  throw new Error('usage: node --expose-gc input-bridge-bench.cjs <input.js> [iterations]');
}
console.log(JSON.stringify(run(filename, iterations)));
