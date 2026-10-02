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

function measure(filename, iterations, prepare, step) {
  const samples = [];
  for (let sample = 0; sample < 7; sample++) {
    const runtime = loadScheduler(filename);
    const c = runtime.context;
    const state = prepare(runtime, c);
    for (let i = 0; i < Math.min(20000, iterations); i++) {
      step(runtime, c, state, i);
    }
    if (global.gc) global.gc();
    const started = hostPerformance.now();
    for (let i = 0; i < iterations; i++) step(runtime, c, state, i);
    samples.push((hostPerformance.now() - started) * 1e6 / iterations);
  }
  return { median: median(samples), samples };
}

function run(filename, iterations) {
  verify(filename);
  const idle = measure(filename, iterations,
    (_runtime, c) => {
      c.setTimeout(() => {}, 60 * 60 * 1000);
      return null;
    },
    (_runtime, c) => c.pmjsDrainScheduler(0));

  const activeIterations = Math.min(iterations, 150000);
  const raf = measure(filename, activeIterations,
    () => ({ noop() {} }),
    (_runtime, c, state, i) => {
      c.requestAnimationFrame(state.noop);
      c.pmjsDrainScheduler(i);
    });

  const timer = measure(filename, activeIterations,
    (_runtime, c) => {
      const state = { noop() {}, clock: 0 };
      c.setInterval(state.noop, 16);
      return state;
    },
    (runtime, c, state) => {
      state.clock += 16;
      runtime.setClock(state.clock);
      c.pmjsDrainScheduler(state.clock);
    });

  return {
    benchmark: 'scheduler',
    iterations,
    median_ns_per_frame: idle.median,
    samples_ns_per_frame: idle.samples,
    raf_iterations: activeIterations,
    raf_median_ns_per_frame: raf.median,
    raf_samples_ns_per_frame: raf.samples,
    timer_iterations: activeIterations,
    timer_median_ns_per_tick: timer.median,
    timer_samples_ns_per_tick: timer.samples
  };
}

const filename = process.argv[2];
const iterations = Number(process.argv[3] || 500000);
if (!filename) throw new Error('usage: node --expose-gc scheduler-bench.cjs <scheduler.js> [iterations]');
console.log(JSON.stringify(run(filename, iterations)));
