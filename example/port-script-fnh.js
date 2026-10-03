// [fnh port] Runtime tuning loaded via PMJS_PORT_SCRIPT (no game files touched).
//
// 1) Catch-up config channel for TDDP_FluidTimestep (the plugin reads it per
//    frame; a no-op with the stock plugin, kept so a plugin that honours it
//    still picks it up).
// 2) TerraxLighting lightmask at quarter resolution: the stock plugin draws its
//    mask into an 866x630 bitmap on every rebuild - the dominant map cost on
//    TSPS (~20-30 ms per rebuild plus a 2.18 MB texture upload, rebuilt about
//    every 2nd tick while moving). This wraps the plugin's own prototype
//    methods at runtime (no game file edits): the mask bitmap is created at
//    1/4 size with a matching ctx.scale(1/4), so the stock drawing code lands
//    correctly at quarter resolution, and the display sprites are scaled 4x to
//    stretch it back to full screen. Upload drops to ~137 KB and drawn pixels
//    drop 16x; the shim transforms gradient centers/radii at creation time, so
//    the native gradient path stays correct under the scaled context.
window.FNH_CFG = window.FNH_CFG || {};
if (window.FNH_CFG.timestepMaxCatchup === undefined) {
  window.FNH_CFG.timestepMaxCatchup = 5;
}

(function () {
  var RES = 4;
  function makeQuarterRes(lightmask) {
    var full = lightmask._maskBitmap;
    if (!full || full.__fnhQuarterRes) return;
    var small = new Bitmap(Math.ceil(full.width / RES),
      Math.ceil(full.height / RES));
    small.__fnhQuarterRes = true;
    small._context.scale(1 / RES, 1 / RES);
    lightmask._maskBitmap = small;
  }
  function patchLightmaskPrototype(proto) {
    if (!proto || proto.__fnhQuarterResPatched) return;
    proto.__fnhQuarterResPatched = true;
    var originalCreateBitmap = proto._createBitmap;
    proto._createBitmap = function () {
      originalCreateBitmap.call(this);
      makeQuarterRes(this);
    };
    var originalAddSprite = proto._addSprite;
    proto._addSprite = function (x, y, selectedBitmap) {
      originalAddSprite.apply(this, arguments);
      if (selectedBitmap === this._maskBitmap) {
        var sprite = this._sprites[this._sprites.length - 1];
        if (sprite) {
          sprite.scale.x = RES;
          sprite.scale.y = RES;
        }
      }
    };
    console.log('[fnh] lightmask quarter-res wrapper active');
  }
  if (typeof Spriteset_Map !== 'undefined' && Spriteset_Map.prototype &&
      typeof Spriteset_Map.prototype.createLightmask === 'function') {
    var originalCreateLightmask = Spriteset_Map.prototype.createLightmask;
    Spriteset_Map.prototype.createLightmask = function () {
      var result = originalCreateLightmask.apply(this, arguments);
      var lightmask = this._lightmask;
      if (lightmask) {
        patchLightmaskPrototype(Object.getPrototypeOf(lightmask));
        makeQuarterRes(lightmask);
      }
      return result;
    };
  } else {
    console.warn('[fnh] Spriteset_Map.createLightmask missing; ' +
      'lightmask patch skipped');
  }
})();
