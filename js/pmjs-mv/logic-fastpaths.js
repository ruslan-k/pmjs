'use strict';

(function() {
  function env(name) {
    try {
      return NativeHost.runtime.env(name);
    } catch (_) {
      return undefined;
    }
  }

  var logicEpoch = 0;
  var logicEpochInstalled = false;
  function installLogicEpoch() {
    if (logicEpochInstalled) return true;
    if (typeof SceneManager === 'undefined' || !SceneManager ||
        typeof SceneManager.updateScene !== 'function') return false;
    var originalUpdateScene = SceneManager.updateScene;
    function updateSceneWithPmjsEpoch() {
      logicEpoch++;
      return originalUpdateScene.apply(this, arguments);
    }
    updateSceneWithPmjsEpoch._pmjsLogicEpoch = true;
    updateSceneWithPmjsEpoch._pmjsOriginal = originalUpdateScene;
    SceneManager.updateScene = updateSceneWithPmjsEpoch;
    logicEpochInstalled = true;
    return true;
  }

  function installRefreshCoalescing() {
    var raw = env('PMJS_REFRESH_COALESCE_TICKS');
    if (raw === undefined || raw === null || raw === '') return false;
    var ticks = Number(raw);
    if (!Number.isSafeInteger(ticks) || ticks < 1 || ticks > 16) {
      console.warn('[pmjs] ignoring invalid PMJS_REFRESH_COALESCE_TICKS=' + raw +
        ' (expected integer 1..16)');
      return false;
    }
    if (ticks === 1 || typeof Game_Map === 'undefined' ||
        !Game_Map.prototype ||
        typeof Game_Map.prototype.refreshIfNeeded !== 'function') return false;
    var original = Game_Map.prototype.refreshIfNeeded;
    if (original._pmjsRefreshCoalesced) return true;
    function refreshIfNeededCoalesced() {
      if (!this._needsRefresh) {
        this.__pmjsRefreshPendingTicks = 0;
        return original.apply(this, arguments);
      }
      var pending = (this.__pmjsRefreshPendingTicks || 0) + 1;
      if (pending < ticks) {
        this.__pmjsRefreshPendingTicks = pending;
        return;
      }
      this.__pmjsRefreshPendingTicks = 0;
      return original.apply(this, arguments);
    }
    refreshIfNeededCoalesced._pmjsRefreshCoalesced = true;
    refreshIfNeededCoalesced._pmjsOriginal = original;
    Game_Map.prototype.refreshIfNeeded = refreshIfNeededCoalesced;
    console.log('[pmjs] map refresh coalescing enabled ticks=' + ticks);
    return true;
  }

  function installMapEventsCache() {
    if (env('PMJS_CACHE_MAP_EVENTS') !== '1') return false;
    if (typeof Game_Map === 'undefined' || !Game_Map.prototype ||
        typeof Game_Map.prototype.events !== 'function' ||
        typeof SceneManager === 'undefined' || !SceneManager ||
        typeof SceneManager.updateScene !== 'function') return false;
    if (Game_Map.prototype.events._pmjsEventsCached) return true;

    if (!installLogicEpoch()) return false;

    var originalEvents = Game_Map.prototype.events;
    function cachedEvents() {
      var source = this._events;
      var length = source ? source.length : 0;
      if (this.__pmjsEventsCacheEpoch === logicEpoch &&
          this.__pmjsEventsCacheSource === source &&
          this.__pmjsEventsCacheLength === length &&
          this.__pmjsEventsCacheValue) {
        return this.__pmjsEventsCacheValue;
      }
      var result = originalEvents.apply(this, arguments);
      this.__pmjsEventsCacheEpoch = logicEpoch;
      this.__pmjsEventsCacheSource = source;
      this.__pmjsEventsCacheLength = length;
      this.__pmjsEventsCacheValue = result;
      return result;
    }
    cachedEvents._pmjsEventsCached = true;
    cachedEvents._pmjsOriginal = originalEvents;
    Game_Map.prototype.events = cachedEvents;

    function invalidate(method) {
      if (typeof Game_Map.prototype[method] !== 'function') return;
      var guest = Game_Map.prototype[method];
      if (guest._pmjsEventsCacheInvalidator) return;
      function wrapped() {
        this.__pmjsEventsCacheValue = null;
        this.__pmjsEventsCacheEpoch = -1;
        return guest.apply(this, arguments);
      }
      wrapped._pmjsEventsCacheInvalidator = true;
      wrapped._pmjsOriginal = guest;
      Game_Map.prototype[method] = wrapped;
    }
    invalidate('setupEvents');
    invalidate('eraseEvent');

    console.log('[pmjs] Game_Map.events per-update cache enabled');
    return true;
  }

  function installGameValueCache() {
    if (env('PMJS_CACHE_GAME_VALUES') !== '1') return false;
    if (!installLogicEpoch()) return false;

    function install(ctorName) {
      var ctor = globalThis[ctorName];
      if (!ctor || !ctor.prototype ||
          typeof ctor.prototype.value !== 'function' ||
          typeof ctor.prototype.setValue !== 'function') return false;
      var originalValue = ctor.prototype.value;
      var originalSetValue = ctor.prototype.setValue;
      if (originalValue._pmjsTickValueCache) return true;

      function cachedValue(id) {
        var key = Number(id) | 0;
        if (key <= 0) return originalValue.apply(this, arguments);
        if (this.__pmjsValueCacheEpoch !== logicEpoch) {
          this.__pmjsValueCacheEpoch = logicEpoch;
          if (this.__pmjsValueCacheSeen) this.__pmjsValueCacheSeen.length = 0;
        }
        var seen = this.__pmjsValueCacheSeen ||
          (this.__pmjsValueCacheSeen = []);
        var values = this.__pmjsValueCacheValues ||
          (this.__pmjsValueCacheValues = []);
        if (seen[key] === logicEpoch) return values[key];
        var result = originalValue.apply(this, arguments);
        values[key] = result;
        seen[key] = logicEpoch;
        return result;
      }
      cachedValue._pmjsTickValueCache = true;
      cachedValue._pmjsOriginal = originalValue;

      function setValueAndInvalidate(id) {
        var result = originalSetValue.apply(this, arguments);
        var key = Number(id) | 0;
        if (key > 0 && this.__pmjsValueCacheSeen) {
          this.__pmjsValueCacheSeen[key] = -1;
        }
        return result;
      }
      setValueAndInvalidate._pmjsTickValueCacheInvalidator = true;
      setValueAndInvalidate._pmjsOriginal = originalSetValue;

      ctor.prototype.value = cachedValue;
      ctor.prototype.setValue = setValueAndInvalidate;
      return true;
    }

    var switches = install('Game_Switches');
    var variables = install('Game_Variables');
    if (switches || variables) {
      console.log('[pmjs] per-tick game value cache enabled switches=' +
        switches + ' variables=' + variables);
      return true;
    }
    return false;
  }

  function installRequestImagesCache() {
    if (env('PMJS_CACHE_REQUEST_IMAGES') !== '1') return false;
    if (typeof Game_Interpreter === 'undefined' || !Game_Interpreter ||
        typeof Game_Interpreter.requestImages !== 'function' ||
        typeof WeakMap !== 'function') return false;
    var rawMs = Number(env('PMJS_REQUEST_IMAGES_CACHE_MS') || 5000);
    var ttlMs = Number.isFinite(rawMs) && rawMs >= 100 && rawMs <= 60000
      ? rawMs : 5000;
    var original = Game_Interpreter.requestImages;
    if (original._pmjsRequestImagesCached) return true;
    var lastScan = new WeakMap();
    function cachedRequestImages(list, commonList) {
      if (!list || (typeof list !== 'object' && typeof list !== 'function')) {
        return original.apply(this, arguments);
      }
      var now = performance.now();
      var previous = lastScan.get(list);
      if (previous !== undefined && now - previous < ttlMs) return;
      lastScan.set(list, now);
      return original.apply(this, arguments);
    }
    cachedRequestImages._pmjsRequestImagesCached = true;
    cachedRequestImages._pmjsOriginal = original;
    Game_Interpreter.requestImages = cachedRequestImages;
    console.log('[pmjs] Game_Interpreter.requestImages cache enabled ttl_ms=' +
      ttlMs);
    return true;
  }

  function installLogicFastPaths() {
    installRefreshCoalescing();
    installMapEventsCache();
    installGameValueCache();
    installRequestImagesCache();
  }

  if (typeof PMJS !== 'undefined' && PMJS.phases &&
      typeof PMJS.phases.on === 'function') {
    PMJS.phases.on('afterGuestPlugins', 'pmjs.mv.logic-fastpaths',
      installLogicFastPaths);
  }

  globalThis.pmjsMvInstallLogicFastPaths = installLogicFastPaths;
})();
