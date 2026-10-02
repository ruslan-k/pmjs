'use strict';

(function() {
  function env(name) {
    try {
      return NativeHost.runtime.env(name);
    } catch (_) {
      return undefined;
    }
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

    var epoch = 0;
    var originalUpdateScene = SceneManager.updateScene;
    SceneManager.updateScene = function() {
      epoch++;
      return originalUpdateScene.apply(this, arguments);
    };
    SceneManager.updateScene._pmjsEventsCacheEpoch = true;

    var originalEvents = Game_Map.prototype.events;
    function cachedEvents() {
      var source = this._events;
      var length = source ? source.length : 0;
      if (this.__pmjsEventsCacheEpoch === epoch &&
          this.__pmjsEventsCacheSource === source &&
          this.__pmjsEventsCacheLength === length &&
          this.__pmjsEventsCacheValue) {
        return this.__pmjsEventsCacheValue;
      }
      var result = originalEvents.apply(this, arguments);
      this.__pmjsEventsCacheEpoch = epoch;
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

  function installLogicFastPaths() {
    installRefreshCoalescing();
    installMapEventsCache();
  }

  if (typeof PMJS !== 'undefined' && PMJS.phases &&
      typeof PMJS.phases.on === 'function') {
    PMJS.phases.on('afterGuestPlugins', 'pmjs.mv.logic-fastpaths',
      installLogicFastPaths);
  }

  globalThis.pmjsMvInstallLogicFastPaths = installLogicFastPaths;
})();
