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

  function pmjsMvLoadConfiguredPortScript(envName, label) {
    var script = '';
    try {
      script = NativeHost.runtime.env(envName) || '';
    } catch (_) {}
    script = String(script).trim();
    if (!script) return false;
    if (script.indexOf('..') !== -1 || script.charAt(0) === '/' ||
        script.charAt(0) === '\\') {
      throw new Error(envName + ' must be a game-root relative path');
    }
    console.log('[pmjs] loading ' + label + ' port script: ' + script);
    NativeHost.runtime.loadScript(script);
    return true;
  }

  function pmjsMvLoadPortScript() {
    return pmjsMvLoadConfiguredPortScript('PMJS_PORT_SCRIPT', 'post-plugin');
  }

  function pmjsMvLoadPrePluginPortScript() {
    return pmjsMvLoadConfiguredPortScript('PMJS_PORT_PRE_SCRIPT', 'pre-plugin');
  }

  function pmjsMvInitializePlugins() {
    // Pre-plugin scripts are for configuration globals or compatibility shims
    // that guest plugins read during their own evaluation.
    pmjsMvLoadPrePluginPortScript();
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
  globalThis.pmjsMvLoadPrePluginPortScript = pmjsMvLoadPrePluginPortScript;
})();
