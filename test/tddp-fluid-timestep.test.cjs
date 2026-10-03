'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const adapterSource = fs.readFileSync(
  path.join(root, 'js/pmjs-plugins/tddp/fluid-timestep.js'), 'utf8');

function load(extra) {
  const context = {
    console,
    globalThis: null,
    performance,
    PMJS: {
      plugins: {
        registerOptimization() {}
      },
      optimizations: {
        isEnabled() { return true; },
        refuse() {}
      },
      phases: {
        hooks: {},
        on(_phase, id, fn) { this.hooks[id] = fn; }
      }
    }
  };
  context.globalThis = context;
  if (extra) extra(context);
  vm.createContext(context);
  vm.runInContext(adapterSource, context);
  return context;
}

function tddpScene() {
  return {
    _currentTime: 0,
    _accumulator: 0,
    _dt: 1 / 60,
    _t: 0,
    _now: 0,
    steps: 0,
    renders: 0,
    getTimeInMs() { return this._now; },
    updateInputData() {},
    changeScene() {},
    updateScene() { this.steps++; },
    renderScene() { this.renders++; },
    requestUpdate() {},
    updateMain() {
      var newTime = this.getTimeInMs();
      var frameTime = (newTime - this._currentTime) / 1000;
      if (frameTime > 0.25) frameTime = 0.25;
      this._currentTime = newTime;
      this._accumulator += frameTime;
      while (this._accumulator >= this._dt) {
        this.updateInputData();
        this.changeScene();
        this.updateScene();
        this._accumulator -= this._dt;
        this._t += this._dt;
      }
      this.renderScene();
      this.requestUpdate();
    }
  };
}

test('recognizes TDDP-shaped direct-step updateMain', () => {
  const ctx = load();
  const scene = tddpScene();
  assert.equal(
    ctx.PMJS.plugins.tddpFluidTimestep.recognizesUpdateMain(scene.updateMain),
    true);
  assert.equal(
    ctx.PMJS.plugins.tddpFluidTimestep.recognizesUpdateMain(function() {
      this.updateScene();
    }),
    false);
});

test('bounded policy caps direct-step drain and retains fractional remainder', () => {
  const ctx = load(context => {
    context.SceneManager = tddpScene();
  });
  assert.equal(ctx.pmjsInstallTddpFluidTimestepAdapter(), true);
  ctx.PMJS.plugins.tddpFluidTimestep.configure({
    maxCatchup: 3,
    dropExcess: true
  });

  ctx.SceneManager._now = 100;
  ctx.SceneManager.updateMain();

  assert.equal(ctx.SceneManager.steps, 3);
  assert.equal(ctx.SceneManager.renders, 1);
  assert.ok(ctx.SceneManager._accumulator >= 0);
  assert.ok(ctx.SceneManager._accumulator < ctx.SceneManager._dt);
  assert.ok(ctx.__pmjsTddpDroppedMs > 40);
  assert.equal(ctx.__pmjsOverloadDiscontinuities, 1);
});

test('disabled bounded policy delegates to original TDDP behavior', () => {
  const ctx = load(context => {
    context.SceneManager = tddpScene();
  });
  const original = ctx.SceneManager.updateMain;
  assert.equal(ctx.pmjsInstallTddpFluidTimestepAdapter(), true);
  ctx.PMJS.plugins.tddpFluidTimestep.configure({ maxCatchup: 0 });

  ctx.SceneManager._now = 100;
  ctx.SceneManager.updateMain();

  assert.equal(ctx.SceneManager.steps, 6);
  assert.equal(ctx.SceneManager.renders, 1);
  assert.equal(ctx.SceneManager.updateMain._pmjsTddpBoundedCatchup, true);
  assert.equal(ctx.SceneManager.updateMain._pmjsOriginal, original);
  assert.equal(ctx.__pmjsTddpDroppedMs, undefined);
});

test('unknown updateMain composition is refused and left untouched', () => {
  let refused = '';
  const ctx = load(context => {
    context.SceneManager = tddpScene();
    context.SceneManager.updateMain = function() {
      this.updateScene();
      this.renderScene();
    };
    context.PMJS.optimizations.refuse = function(_id, reason) {
      refused = reason;
    };
  });
  const original = ctx.SceneManager.updateMain;

  assert.equal(ctx.pmjsInstallTddpFluidTimestepAdapter(), false);
  assert.equal(ctx.SceneManager.updateMain, original);
  assert.match(refused, /unrecognized/);
});
