'use strict';

(function() {
  function pmjsMvInstallPluginManagerHooks() {
    if (typeof PluginManager === 'undefined') return false;
    if (PluginManager._pmjsLifecycleInstalled) return true;

    PluginManager.loadScript = function(name) {
      NativeHost.runtime.loadScript(this._path + name);
    };

    PluginManager.setup = function(plugins) {
      var pending = [];
      var isAlreadyLoaded = function(scripts, name) {
        if (typeof scripts.contains === 'function') return scripts.contains(name);
        return scripts.indexOf(name) !== -1;
      };
      plugins.forEach(function(plugin) {
        if (plugin.status &&
            !isAlreadyLoaded(this._scripts, plugin.name) &&
            !pending.some(function(p) { return p.name === plugin.name; })) {
          pending.push(plugin);
        }
      }, this);

      pending.forEach(function(plugin) {
        this.setParameters(plugin.name, plugin.parameters);
      }, this);

      pending.forEach(function(plugin) {
        this._scripts.push(plugin.name);
        var loadedName = String(plugin.name).replace(/\.js$/i, '');
        var self = this;
        var file = plugin.name + '.js';
        PMJS.plugins.execute(loadedName, function() {
          self.loadScript(file);
        });
      }, this);
    };
    PluginManager._pmjsLifecycleInstalled = true;
    return true;
  }

  function pmjsMvLoadPluginManifest() {
    globalThis.pmjsLoadRpgMakerPluginManifest();
  }

  function afterPlugins() {
    if (globalThis.pmjsPixiRenderPreflight) globalThis.pmjsPixiRenderPreflight.scan();

    if (typeof installNativeStorageManager === 'function') {
      installNativeStorageManager();
    }

    if (typeof pmjsMvInstallTimingContract === 'function') {
      pmjsMvInstallTimingContract();
    }
  }

  function pmjsMvLoadPortScript() {
    var script = '';
    try {
      script = NativeHost.runtime.env('PMJS_PORT_SCRIPT') || '';
    } catch (_) {}
    script = String(script).trim();
    if (!script) return false;
    if (script.indexOf('..') !== -1 || script.charAt(0) === '/' ||
        script.charAt(0) === '\\') {
      throw new Error('PMJS_PORT_SCRIPT must be a game-root relative path');
    }
    console.log('[pmjs] loading port script: ' + script);
    NativeHost.runtime.loadScript(script);
    return true;
  }

  function pmjsMvInitializePlugins() {
    globalThis.pmjsInitializeRpgMakerPlugins(
      pmjsMvInstallPluginManagerHooks, afterPlugins);
    // Port-specific compatibility/performance patches must run after guest
    // plugins and after PMJS-owned method wrappers are installed, but before
    // js/main.js starts the game. HTML injection is intentionally not part of
    // the native runtime and therefore cannot be relied on here.
    pmjsMvLoadPortScript();
  }

  globalThis.pmjsMvInstallPluginManagerHooks = pmjsMvInstallPluginManagerHooks;
  globalThis.pmjsMvLoadPluginManifest = pmjsMvLoadPluginManifest;
  globalThis.pmjsMvInitializePlugins = pmjsMvInitializePlugins;
  globalThis.pmjsMvLoadPortScript = pmjsMvLoadPortScript;
})();
