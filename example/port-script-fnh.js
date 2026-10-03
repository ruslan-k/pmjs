// [fnh port] Runtime tuning loaded via PMJS_PORT_SCRIPT (no game files touched).
//
// The port script only selects policy. Shared runtime adapters own plugin
// recognition, safety checks, and implementation details.

(function () {
  var pmjs = globalThis.PMJS;
  var tddp = pmjs && pmjs.plugins && pmjs.plugins.tddpFluidTimestep;
  if (tddp && typeof tddp.configure === 'function') {
    // Device profiling on F&H shows ~9-15 ms per updateScene tick on the
    // target A53-class CPU. A cap of 5 preserves near-60 Hz authored logic
    // without allowing one presentation frame to accumulate 70-120+ ms of
    // catch-up work as an 8-step cap can.
    tddp.configure({ maxCatchup: 5, dropExcess: true });
    console.log('[fnh] bounded TDDP catch-up configured');
  } else {
    console.warn('[fnh] PMJS TDDP adapter unavailable; catch-up left unchanged');
  }
})();

(function () {
  var pmjs = globalThis.PMJS;
  var terrax = pmjs && pmjs.plugins && pmjs.plugins.terraxLighting;
  if (terrax && typeof terrax.configureMaskScale === 'function') {
    terrax.configureMaskScale(0.25);
    console.log('[fnh] Terrax quarter-resolution mask configured');
  } else {
    console.warn('[fnh] PMJS Terrax lighting capability unavailable; ' +
      'quarter-resolution mask skipped');
  }
})();
