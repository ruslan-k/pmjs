'use strict';

// Shared Terrax Lighting native accelerator.
// Gates the GPU recorder plus mask-sprite pooling; disabled means stock
// Terrax Canvas _updateMask and light-sprite handling.
(function() {
  var configuredMaskScale = 1;

  function configureMaskScale(scale) {
    scale = Number(scale);
    if (!Number.isFinite(scale) || scale <= 0 || scale > 1) {
      throw new RangeError('Terrax mask scale must be > 0 and <= 1');
    }
    configuredMaskScale = scale;
    console.log('[pmjs] Terrax mask scale=' + scale);
    return scale;
  }

  function applyMaskScale(lightmask) {
    var scale = configuredMaskScale;
    if (!(scale < 1) || !lightmask || !lightmask._maskBitmap ||
        typeof Bitmap !== 'function') return;
    var full = lightmask._maskBitmap;
    if (full.__pmjsTerraxScaledMask) return;
    var width = Math.max(1, Math.ceil(full.width * scale));
    var height = Math.max(1, Math.ceil(full.height * scale));
    var scaled = new Bitmap(width, height);
    scaled.__pmjsTerraxScaledMask = true;
    scaled.__pmjsTerraxMaskScale = scale;
    if (scaled._context && typeof scaled._context.scale === 'function') {
      scaled._context.scale(scale, scale);
    }
    lightmask._maskBitmap = scaled;
    if (typeof full.destroy === 'function') {
      try { full.destroy(); } catch (_) {}
    }
  }

  if (typeof PMJS !== 'undefined' && PMJS.plugins &&
      typeof PMJS.plugins.registerOptimization === 'function') {
    PMJS.plugins.registerOptimization('Terrax_Lighting', {
      id: 'terrax.native-lighting',
      owner: 'plugins/terrax/lighting',
      fallback: 'ordinary Terrax Canvas _updateMask and stock light-sprite handling'
    });
  }

  function fnSource(fn) {
    return Function.prototype.toString.call(fn);
  }

  function hasTokens(fn, tokens) {
    if (typeof fn !== 'function') return false;
    var source = fnSource(fn);
    for (var index = 0; index < tokens.length; index++) {
      if (source.indexOf(tokens[index]) === -1) return false;
    }
    return true;
  }

  function looksLikeKnownCreateLightmask(fn) {
    return hasTokens(fn, ['_lightmask', 'new Lightmask', 'addChild']);
  }

  function looksLikeKnownAddSprite(fn) {
    return hasTokens(fn, ['new Sprite', '_sprites.push', 'addChild',
      'bitmap', 'blendMode']);
  }

  function looksLikeKnownRemoveSprite(fn) {
    return hasTokens(fn, ['_sprites.pop', 'removeChild']);
  }

  function looksLikeKnownUpdateMask(fn) {
    return hasTokens(fn, ['_maskBitmap', 'fillRect']);
  }

  function installTerraxLightingFastPaths() {
    var useNativeLighting = PMJS.optimizations.isEnabled('terrax.native-lighting');
    if (!useNativeLighting) return false;
    if (typeof Spriteset_Map === 'undefined' ||
        typeof Spriteset_Map.prototype.createLightmask !== 'function') {
      PMJS.optimizations.refuse('terrax.native-lighting',
        'Terrax createLightmask method unavailable');
      return false;
    }
    if (Spriteset_Map.prototype.createLightmask._pmjsTerraxGuard) return true;
    if (!looksLikeKnownCreateLightmask(
        Spriteset_Map.prototype.createLightmask)) {
      PMJS.optimizations.refuse('terrax.native-lighting',
        'unrecognized Terrax createLightmask method composition');
      return false;
    }
    function installGpuLightRecorder(lightmask) {
      if (!looksLikeKnownUpdateMask(lightmask._updateMask)) return;
      var bitmap = lightmask._maskBitmap;
      var recorder = PMJS.mv && PMJS.mv.bitmap &&
        PMJS.mv.bitmap.createPrimitiveRecorder(bitmap);
      if (!recorder) return;
      var originalUpdateMask = lightmask._updateMask;
      lightmask._updateMask = function() {
        var mask = this;
        var args = arguments;
        return recorder.record(function() { return originalUpdateMask.apply(mask, args); });
      };
    }

    var createLightmask = Spriteset_Map.prototype.createLightmask;
    Spriteset_Map.prototype.createLightmask = function() {
      var result = createLightmask.apply(this, arguments);
      var lightmask = this._lightmask;
      if (!lightmask || !lightmask._sprites) return result;
      applyMaskScale(lightmask);
      installGpuLightRecorder(lightmask);

      if (looksLikeKnownAddSprite(lightmask._addSprite) &&
          looksLikeKnownRemoveSprite(lightmask._removeSprite)) {
        lightmask._addSprite = function(x, y, bitmap) {
          var sprite = this._pmjsMaskSprite;
          if (!sprite) {
            sprite = this._pmjsMaskSprite = new Sprite(this.viewport);
            this.addChild(sprite);
          }
          sprite.bitmap = bitmap;
          sprite.opacity = 255;
          sprite.blendMode = 2;
          sprite.x = x;
          sprite.y = y;
          sprite.rotation = 0;
          var maskScale = bitmap && bitmap.__pmjsTerraxMaskScale || 1;
          sprite.scale.x = 1 / maskScale;
          sprite.scale.y = 1 / maskScale;
          sprite.ax = 0;
          sprite.ay = 0;
          sprite.visible = true;
          if (this._sprites.indexOf(sprite) < 0) this._sprites.push(sprite);
        };
        lightmask._removeSprite = function() {
          var sprite = this._sprites.pop();
          if (sprite) sprite.visible = false;
        };
      }
      return result;
    };
    Spriteset_Map.prototype.createLightmask._pmjsTerraxGuard = true;
    return true;
  }

  PMJS.phases.on('afterGuestPlugins', 'pmjs.adapter.terrax-lighting',
    installTerraxLightingFastPaths);

  globalThis.pmjsInstallTerraxLightingFastPaths = installTerraxLightingFastPaths;
  PMJS.plugins.terraxLighting = PMJS.plugins.terraxLighting || {};
  PMJS.plugins.terraxLighting.configureMaskScale = configureMaskScale;
})();
