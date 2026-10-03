'use strict';

// TDDP_FluidTimestep adapter.
//
// The plugin owns SceneManager.updateMain and drains _accumulator in a direct
// while loop. PMJS must not replace that method wholesale: another plugin may
// have added work around the loop. Instead, when a bounded policy is enabled,
// temporarily wrap updateScene while the original updateMain runs. On the Nth
// authored logic step, trim only whole-step debt from _accumulator. TDDP then
// performs its normal "_accumulator -= _dt" and exits naturally, retaining the
// fractional remainder.

(function() {
  var optimizationId = 'tddp.bounded-catchup';
  if (typeof PMJS !== 'undefined' && PMJS.plugins &&
      typeof PMJS.plugins.registerOptimization === 'function') {
    PMJS.plugins.registerOptimization('TDDP_FluidTimestep', {
      id: optimizationId,
      owner: 'plugins/tddp/fluid-timestep',
      fallback: 'leave TDDP_FluidTimestep updateMain and catch-up behavior unchanged'
    });
  }

  var policy = {
    maxCatchup: 0,
    dropExcess: true
  };

  function sourceOf(fn) {
    try { return Function.prototype.toString.call(fn); } catch (_) { return ''; }
  }

  function recognizesTddpUpdateMain(fn) {
    var source = sourceOf(fn);
    return source.indexOf('getTimeInMs') !== -1 &&
      source.indexOf('this._accumulator') !== -1 &&
      source.indexOf('this._dt') !== -1 &&
      source.indexOf('this._t') !== -1 &&
      /while\s*\(\s*this\._accumulator\s*>=\s*this\._dt\s*\)/.test(source) &&
      source.indexOf('this.updateInputData()') !== -1 &&
      source.indexOf('this.changeScene()') !== -1 &&
      source.indexOf('this.updateScene()') !== -1 &&
      source.indexOf('this._accumulator -= this._dt') !== -1 &&
      source.indexOf('this.renderScene()') !== -1 &&
      source.indexOf('this.requestUpdate()') !== -1;
  }

  function configure(options) {
    options = options || {};
    if (options.maxCatchup !== undefined) {
      var value = Number(options.maxCatchup);
      if (!Number.isSafeInteger(value) || value < 0 || value > 60) {
        throw new RangeError('TDDP maxCatchup must be an integer from 0 to 60');
      }
      policy.maxCatchup = value;
    }
    if (options.dropExcess !== undefined) {
      policy.dropExcess = !!options.dropExcess;
    }
    console.log('[pmjs] TDDP catch-up policy max=' + policy.maxCatchup +
      ' dropExcess=' + policy.dropExcess);
    return {
      maxCatchup: policy.maxCatchup,
      dropExcess: policy.dropExcess
    };
  }

  function install() {
    if (!PMJS.optimizations.isEnabled(optimizationId)) return false;
    if (typeof SceneManager === 'undefined' || !SceneManager ||
        typeof SceneManager.updateMain !== 'function') return false;
    if (SceneManager.updateMain._pmjsTddpBoundedCatchup) return true;
    if (!recognizesTddpUpdateMain(SceneManager.updateMain)) {
      PMJS.optimizations.refuse(optimizationId,
        'unrecognized TDDP_FluidTimestep updateMain composition');
      return false;
    }

    var originalUpdateMain = SceneManager.updateMain;
    function boundedTddpUpdateMain() {
      var maxCatchup = policy.maxCatchup;
      if (!(maxCatchup > 0) || !policy.dropExcess ||
          typeof this.updateScene !== 'function') {
        return originalUpdateMain.apply(this, arguments);
      }

      var manager = this;
      var originalUpdateScene = manager.updateScene;
      var steps = 0;
      var droppedSeconds = 0;
      manager.updateScene = function() {
        steps++;
        var result = originalUpdateScene.apply(this, arguments);
        if (steps >= maxCatchup &&
            Number.isFinite(manager._accumulator) &&
            Number.isFinite(manager._dt) && manager._dt > 0 &&
            manager._accumulator >= manager._dt) {
          var remainder = manager._accumulator % manager._dt;
          var retained = manager._dt + remainder;
          droppedSeconds += Math.max(0, manager._accumulator - retained);
          manager._accumulator = retained;
        }
        return result;
      };
      try {
        return originalUpdateMain.apply(manager, arguments);
      } finally {
        manager.updateScene = originalUpdateScene;
        if (droppedSeconds > 0) {
          globalThis.__pmjsOverloadDiscontinuities =
            (globalThis.__pmjsOverloadDiscontinuities || 0) + 1;
          globalThis.__pmjsTddpDroppedMs =
            (globalThis.__pmjsTddpDroppedMs || 0) + droppedSeconds * 1000;
        }
      }
    }
    boundedTddpUpdateMain._pmjsTddpBoundedCatchup = true;
    boundedTddpUpdateMain._pmjsOriginal = originalUpdateMain;
    SceneManager.updateMain = boundedTddpUpdateMain;
    return true;
  }

  PMJS.phases.on('afterGuestPlugins', 'pmjs.adapter.tddp-fluid-timestep',
    install);

  PMJS.plugins.tddpFluidTimestep = PMJS.plugins.tddpFluidTimestep || {};
  PMJS.plugins.tddpFluidTimestep.configure = configure;
  PMJS.plugins.tddpFluidTimestep.recognizesUpdateMain = recognizesTddpUpdateMain;
  globalThis.pmjsInstallTddpFluidTimestepAdapter = install;
})();
