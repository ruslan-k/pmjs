// [fnh port] Runtime tuning loaded via PMJS_PORT_SCRIPT (no game files touched).
//
// 1) Catch-up config channel for TDDP_FluidTimestep.
// 2) TerraxLighting lightmask at quarter resolution through the runtime's
//    generic Terrax capability. The runtime owns bitmap replacement, primitive
//    recorder setup, and mask-sprite scaling so port scripts do not race the
//    shared Terrax adapter or retain a stale full-resolution recorder/canvas.

window.FNH_CFG = window.FNH_CFG || {};
if (window.FNH_CFG.timestepMaxCatchup === undefined) {
  window.FNH_CFG.timestepMaxCatchup = 5;
}

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
