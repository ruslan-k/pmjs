'use strict';

const fs = require('node:fs');
const vm = require('node:vm');
const { performance: hostPerformance } = require('node:perf_hooks');

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

function loadScheduler(filename) {
  let clock = 0;
  const context = {
    PMJS: {},
    console: { error() {}, log() {}, warn() {} },
    performance: { now: () => clock },
  };
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(filename, 'utf8'), context, { filename });
  return {
    context,
    setClock(value) { clock = value; }
  };
}

function verify(filename) {
  const runtime = loadScheduler(filename);
  const c = runtime.context;
  const events = [];
  c.setTimeout(() => events.push('timeout'), 10);
  const interval = c.setInterval(() => events.push('interval'), 5);
  const cancelled = c.requestAnimationFrame(() => events.push('bad-raf'));
  c.cancelAnimationFrame(cancelled);
  c.requestAnimationFrame(() => events.push('raf'));
  runtime.setClock(5);
  c.pmjsDrainScheduler(5);
  runtime.setClock(10);
  c.pmjsDrainScheduler(10);
  c.clearInterval(interval);
  if (events.join(',') !== 'interval,raf,timeout,interval') {
    throw new Error('scheduler semantics mismatch: ' + events.join(','));
  }
}

function run(filename, iterations) {
  verify(filename);
  const samples = [];
  for (let sample = 0; sample < 7; sample++) {
    const runtime = loadScheduler(filename);
    const c = runtime.context;
    // Keep a non-empty timer map with nothing due. This models the common
    // gameplay case where delayed work exists but most rendered frames have
    // no timer to execute.
    c.setTimeout(() => {}, 60 * 60 * 1000);
    for (let i = 0; i < 20000; i++) c.pmjsDrainScheduler(0);
    if (global.gc) global.gc();
    const started = hostPerformance.now();
    for (let i = 0; i < iterations; i++) c.pmjsDrainScheduler(0);
    samples.push((hostPerformance.now() - started) * 1e6 / iterations);
  }
  return {
    benchmark: 'scheduler-idle',
    iterations,
    median_ns_per_frame: median(samples),
    samples_ns_per_frame: samples
  };
}

const filename = process.argv[2];
const iterations = Number(process.argv[3] || 500000);
if (!filename) throw new Error('usage: node --expose-gc scheduler-bench.cjs <scheduler.js> [iterations]');
console.log(JSON.stringify(run(filename, iterations)));
