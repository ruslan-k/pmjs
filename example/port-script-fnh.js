// [fnh port] Runtime tuning loaded via PMJS_PORT_SCRIPT (no game files touched).
//
// 1) Bounded logic catch-up: the stock TDDP_FluidTimestep drains the frame
//    accumulator without a cap, so on the slowest maps the engine processes
//    10+ logic ticks per frame (game logic stays real-time, but frames stretch
//    unboundedly). This replaces SceneManager.updateMain with the same loop
//    capped at FNH_CFG.timestepMaxCatchup ticks per frame; the excess backlog
//    is dropped (mod the accumulator). At >=8 fps nothing changes (the cap is
//    not reached and logic stays real-time); below that frames get shorter
//    and game speed degrades proportionally instead of frames stretching.
// 2) TerraxLighting lightmask at quarter resolution through the runtime's
//    generic Terrax capability. The runtime owns bitmap replacement, primitive
//    recorder setup, and mask-sprite scaling so port scripts do not race the
//    shared Terrax adapter or retain a stale full-resolution recorder/canvas.

window.FNH_CFG = window.FNH_CFG || {};
if (window.FNH_CFG.timestepMaxCatchup === undefined) {
  window.FNH_CFG.timestepMaxCatchup = 8;
}

(function () {
  if (typeof SceneManager === 'undefined' || !SceneManager.updateMain) return;
  var maxCatchup = window.FNH_CFG.timestepMaxCatchup;
  SceneManager.updateMain = function () {
    var newTime = this.getTimeInMs();
    var frameTime = (newTime - this._currentTime) / 1000;
    if (frameTime > 0.25) frameTime = 0.25;
    this._currentTime = newTime;
    this._accumulator += frameTime;
    var catchup = 0;
    while (this._accumulator >= this._dt && catchup < maxCatchup) {
      this.updateInputData();
      this.changeScene();
      this.updateScene();
      this._accumulator -= this._dt;
      this._t += this._dt;
      catchup++;
    }
    if (this._accumulator >= this._dt) {
      this._accumulator = this._accumulator % this._dt;
    }
    this.renderScene();
    this.requestUpdate();
  };
  console.log('[fnh] bounded timestep catch-up active (max ' + maxCatchup +
    ' ticks/frame)');
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
