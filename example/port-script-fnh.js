// [fnh port] Runtime tuning loaded via PMJS_PORT_SCRIPT (no game files touched).
//
// The port script only selects policy. Shared runtime adapters own plugin
// recognition, safety checks, and implementation details.

(function () {
  var tddp = window.PMJS && PMJS.plugins && PMJS.plugins.tddpFluidTimestep;
  if (tddp && typeof tddp.configure === 'function') {
    tddp.configure({ maxCatchup: 8, dropExcess: true });
    console.log('[fnh] bounded TDDP catch-up configured');
  } else {
    console.warn('[fnh] PMJS TDDP adapter unavailable; catch-up left unchanged');
  }
})();

(function () {
  var terrax = window.PMJS && PMJS.plugins && PMJS.plugins.terraxLighting;
  if (terrax && typeof terrax.configureMaskScale === 'function') {
    terrax.configureMaskScale(0.25);
    console.log('[fnh] Terrax quarter-resolution mask configured');
  } else {
    console.warn('[fnh] PMJS Terrax lighting capability unavailable; ' +
      'quarter-resolution mask skipped');
  }
})();
