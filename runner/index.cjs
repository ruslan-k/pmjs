'use strict';

const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { performance } = require('node:perf_hooks');
const { createStorage } = require('./storage.cjs');

function resolveDefaults(input) {
  let title = input.title;
  let width = input.width;
  let height = input.height;

  if (input.config) {
    const configPath = path.resolve(input.config);
    if (!fs.existsSync(configPath)) {
      throw new Error(`config file not found: ${configPath}`);
    }
    let cfg;
    const configText = fs.readFileSync(configPath, 'utf8');
    if (configPath.endsWith('.json')) {
      try {
        cfg = JSON.parse(configText);
      } catch (err) {
        throw new Error(`invalid JSON in config file ${configPath}: ${err.message}`);
      }
    } else {
      const sandbox = { globalThis: {} };
      sandbox.window = sandbox.globalThis;
      try {
        vm.runInNewContext(configText, sandbox);
      } catch (err) {
        throw new Error(`error evaluating config file ${configPath}: ${err.message}`);
      }
      cfg = sandbox.globalThis.PMJS_GAME_CONFIG;
    }
    if (cfg) {
      if (title === undefined && cfg.title) title = cfg.title;
      if (width === undefined && cfg.display && cfg.display.width) width = Number(cfg.display.width);
      if (height === undefined && cfg.display && cfg.display.height) height = Number(cfg.display.height);
      assertDisableOptimizationsShape(cfg.disableOptimizations, configPath);
    }
  }

  if (input.gameRoot) {
    const rootPath = path.resolve(input.gameRoot);
    if (title === undefined) {
      const systemPath = path.join(rootPath, 'data', 'System.json');
      if (fs.existsSync(systemPath)) {
        try {
          const sys = JSON.parse(fs.readFileSync(systemPath, 'utf8'));
          if (sys && typeof sys.gameTitle === 'string' && sys.gameTitle.trim()) {
            title = sys.gameTitle.trim();
          }
        } catch (_) {}
      }
    }
    const pkgPath = path.join(rootPath, 'package.json');
    if (fs.existsSync(pkgPath)) {
      try {
        const pkg = JSON.parse(fs.readFileSync(pkgPath, 'utf8'));
        if (title === undefined && pkg.window && pkg.window.title) title = pkg.window.title;
        if (title === undefined && pkg.name) title = pkg.name;
        if (width === undefined && pkg.window && pkg.window.width) width = Number(pkg.window.width);
        if (height === undefined && pkg.window && pkg.window.height) height = Number(pkg.window.height);
      } catch (_) {}
    }
  }

  if (width === undefined) width = 816;
  if (height === undefined) height = 624;
  if (title === undefined) title = 'PMJS';

  return { ...input, width, height, title };
}

function assertDisableOptimizationsShape(value, configPath) {
  if (value === undefined) return;
  const ok = Array.isArray(value) && value.every(id => typeof id === 'string' && id) &&
    new Set(value).size === value.length;
  if (!ok) {
    throw new Error(`disableOptimizations in ${configPath} must be an array of unique nonempty strings`);
  }
}

const PMJS_MV_LOGIC_HZ = 60;
const PMJS_SUPPORTED_RENDER_HZ = [30, 60, 120];

function parseTimingConfig(env) {
  const source = env || {};
  if (source.PMJS_LOGIC_HZ !== undefined && source.PMJS_LOGIC_HZ !== '' &&
      Number(source.PMJS_LOGIC_HZ) !== PMJS_MV_LOGIC_HZ) {
    throw new Error(
      `PMJS_LOGIC_HZ must be ${PMJS_MV_LOGIC_HZ} ` +
      `(MV simulation is fixed at the authored rate): ${source.PMJS_LOGIC_HZ}`);
  }
  const rawCatchup = source.PMJS_CATCHUP_MODE;
  let catchupMode = 'burst';
  if (rawCatchup !== undefined && rawCatchup !== '') {
    if (rawCatchup !== 'smooth' && rawCatchup !== 'burst') {
      throw new Error(`PMJS_CATCHUP_MODE must be 'smooth' or 'burst': ${rawCatchup}`);
    }
    catchupMode = rawCatchup;
  }
  const uncappedFlag = source.PMJS_UNCAPPED === '1';
  const raw = source.PMJS_RENDER_HZ;
  if (raw === undefined || raw === '') {
    if (uncappedFlag) return { logicHz: PMJS_MV_LOGIC_HZ, renderHz: 0, uncapped: true, renderPeriod: Infinity, catchupMode };
    return { logicHz: PMJS_MV_LOGIC_HZ, renderHz: 60, uncapped: false,
      renderPeriod: 1000 / 60, catchupMode };
  }
  const renderHz = Number(raw);
  if (renderHz === 0) {
    return { logicHz: PMJS_MV_LOGIC_HZ, renderHz: 0, uncapped: true,
      renderPeriod: Infinity, catchupMode };
  }
  if (!PMJS_SUPPORTED_RENDER_HZ.includes(renderHz)) {
    throw new Error(
      `PMJS_RENDER_HZ must be one of 0 (uncapped), ` +
      `${PMJS_SUPPORTED_RENDER_HZ.join(', ')}: ${raw}`);
  }
  if (renderHz === 0 || uncappedFlag) {
    return { logicHz: PMJS_MV_LOGIC_HZ, renderHz: 0, uncapped: true,
      renderPeriod: Infinity, catchupMode };
  }
  return { logicHz: PMJS_MV_LOGIC_HZ, renderHz, uncapped: false,
    renderPeriod: 1000 / renderHz, catchupMode };
}

function resolveSwapDefault(env, timing) {
  const source = env || {};
  // The runner already owns frame pacing for capped modes. Leaving SDL/EGL
  // VSync enabled adds a second independent limiter: a small deadline miss can
  // then block until the following vblank and turn a minor overrun into a full
  // refresh-period hitch. Default to non-blocking swap for both paced and
  // uncapped rendering; PMJS_SWAP_INTERVAL=1 remains an explicit user override.
  if (source.PMJS_SWAP_INTERVAL === undefined ||
      source.PMJS_SWAP_INTERVAL === '') {
    return '0';
  }
  return null;
}

function advanceDeadline(deadline, now, period) {
  let next = deadline + period;
  if (next < now - period) {
    next += Math.floor((now - next) / period) * period;
  }
  return next;
}

function validate(input) {
  const options = resolveDefaults(input);
  const requiredPaths = ['addon', 'gameRoot', 'bootstrap', 'saveRoot'];
  for (const name of requiredPaths) {
    if (typeof options[name] !== 'string' || !options[name]) {
      throw new Error(`${name} is required`);
    }
  }
  for (const name of ['width', 'height']) {
    if (!Number.isInteger(options[name]) || options[name] < 1 || options[name] > 16384) {
      throw new Error(`${name} must be an integer between 1 and 16384`);
    }
  }
  const configuredWarmBytes = options.imageWarmCacheBytes ??
    process.env.PMJS_IMAGE_WARM_CACHE_BYTES;
  const imageWarmCacheBytes = configuredWarmBytes === undefined ? undefined :
    Number(configuredWarmBytes);
  if (imageWarmCacheBytes !== undefined &&
      (!Number.isSafeInteger(imageWarmCacheBytes) || imageWarmCacheBytes < 0)) {
    throw new Error('imageWarmCacheBytes must be a non-negative safe integer');
  }
  return {
    ...options,
    addon: path.resolve(options.addon),
    gameRoot: path.resolve(options.gameRoot),
    bootstrap: path.resolve(options.bootstrap),
    saveRoot: path.resolve(options.saveRoot),
    assetRoot: options.assetRoot ? path.resolve(options.assetRoot) : '',
    ...(imageWarmCacheBytes === undefined ? {} : { imageWarmCacheBytes }),
    title: options.title,
  };
}

async function run(input, hooks = {}) {
  const options = validate(input);
  const hostProcess = process;

  const timing = parseTimingConfig(hostProcess.env);
  const swapDefault = resolveSwapDefault(hostProcess.env, timing);
  if (swapDefault !== null) {
    hostProcess.env.PMJS_SWAP_INTERVAL = swapDefault;
    console.log('[pmjs] runner pacing: defaulting PMJS_SWAP_INTERVAL=0 (was unset)');
  }
  const native = options.native || require(options.addon);
  native.initialize({ gameRoot: options.gameRoot, assetRoot: options.assetRoot,
    width: options.width, height: options.height, windowTitle: options.title,
    ...(options.imageWarmCacheBytes === undefined ? {} :
      { imageWarmCacheBytes: options.imageWarmCacheBytes }) });
  native.storage = createStorage(options.saveRoot);
  native.runtime.now = () => performance.now();
  native.runtime.platform = () => ({ platform: process.platform, arch: process.arch });
  native.runtime.loadScript = relative => {
    const source = native.fs.readText(relative);
    if (source === null) throw new Error(`cannot load script: ${relative}`);
    return vm.runInThisContext(source, { filename: path.join(options.gameRoot, relative) });
  };
  const hostSetTimeout = globalThis.setTimeout.bind(globalThis);
  const hostClearTimeout = globalThis.clearTimeout.bind(globalThis);
  const hostSetImmediate = typeof globalThis.setImmediate === 'function'
    ? globalThis.setImmediate.bind(globalThis) : null;
  const physicalDisplay = (native.runtime && typeof native.runtime.displaySize === 'function')
    ? native.runtime.displaySize()
    : {
        width: Number(process.env.PMJS_SCREEN_WIDTH || 640),
        height: Number(process.env.PMJS_SCREEN_HEIGHT || 480)
      };
  globalThis.NativeHost = { runtime: native.runtime, render: native.render,
    scene: native.scene, images: native.images, assets: native.assets, fs: native.fs,
    storage: native.storage, input: native.input, canvas: native.canvas,
    media: native.media, dialog: native.dialog };
  globalThis.__pmjsBuiltinRequire = require;
  globalThis.__pmjsNativeRuntime = true;
  globalThis.__pmjsTimingConfig = {
    renderHz: timing.renderHz,
    catchupMode: timing.catchupMode
  };
  globalThis.__pmjsGameInfo = {
    title: options.title,
    width: options.width,
    height: options.height,
    displayWidth: physicalDisplay.width,
    displayHeight: physicalDisplay.height
  };
  try {
    vm.runInThisContext(fs.readFileSync(options.bootstrap, 'utf8'), {
      filename: options.bootstrap, displayErrors: true,
    });
    if (typeof globalThis.__pmjsTick !== 'function' ||
        typeof globalThis.__pmjsRender !== 'function') {
      throw new Error('bootstrap did not install __pmjsTick and __pmjsRender');
    }
    if (hooks.afterBootstrap !== undefined) {
      if (typeof hooks.afterBootstrap !== 'function') throw new Error('afterBootstrap must be a function');
      await hooks.afterBootstrap({ native, options });
    }
  } catch (error) {
    try { native.runtime.quit(); } catch (_) {}
    throw error;
  }

  const period = timing.renderPeriod;
  let deadline = timing.uncapped ? 0 : native.runtime.monotonicNow() + period;
  const memoryTelemetryEnabled = hostProcess.env.PMJS_MEMORY_TELEMETRY === '1';
  const memoryTelemetryDetailEnabled =
    hostProcess.env.PMJS_MEMORY_TELEMETRY_DETAIL === '1';
  const configuredTelemetryMs = Number(
    hostProcess.env.PMJS_MEMORY_TELEMETRY_MS || 5000);
  const memoryTelemetryMs = Number.isFinite(configuredTelemetryMs) &&
      configuredTelemetryMs >= 1000 ? configuredTelemetryMs : 5000;
  let nextMemoryTelemetry = 0;
  let lastTelemetryFrames = null;
  let lastTelemetryFramesAt = 0;
  let lastTelemetryLogic = null;
  let lastTelemetryPlayer = null;
  let lastTelemetryOverload = null;
  const configuredTimingSampleEvery = Number(
    hostProcess.env.PMJS_TELEMETRY_TIMING_SAMPLE_EVERY || 4);
  const telemetryTimingSampleEvery = Number.isSafeInteger(configuredTimingSampleEvery) &&
      configuredTimingSampleEvery >= 1 && configuredTimingSampleEvery <= 120
    ? configuredTimingSampleEvery : 4;
  let telemetryTimingFrame = 0;
  let telemetryPhaseSamples = 0;
  let telemetryPhaseBeginMs = 0;
  let telemetryPhaseTickMs = 0;
  let telemetryPhaseRenderMs = 0;
  let telemetryPhaseNativeMs = 0;
  let telemetryPhaseAfterMs = 0;
  let telemetryPhaseSwapMs = 0;
  let telemetryPhaseMaxTotalMs = 0;
  let telemetryPhaseMaxTickMs = 0;
  let telemetryPhaseMaxNativeMs = 0;
  let telemetryPhaseMaxSwapMs = 0;
  let lastTelemetryRendererStats = null;
  let sceneWrapsInstalled = false;
  let sceneTimingSampleActive = false;
  const logicProfileEnabled = hostProcess.env.PMJS_LOGIC_PROFILE === '1';
  const configuredLogicProfileEvery = Number(
    hostProcess.env.PMJS_LOGIC_PROFILE_EVERY || 8);
  const logicProfileEvery = Number.isSafeInteger(configuredLogicProfileEvery) &&
      configuredLogicProfileEvery >= 1 && configuredLogicProfileEvery <= 120
    ? configuredLogicProfileEvery : 8;
  let logicProfileFrame = 0;
  let logicProfileSampleActive = false;
  let logicProfileInstalled = false;
  const logicProfileStats = Object.create(null);
  const transitionProfileEnabled =
    hostProcess.env.PMJS_TRANSITION_PROFILE === '1';
  const configuredTransitionThreshold = Number(
    hostProcess.env.PMJS_TRANSITION_PROFILE_MS || 80);
  const transitionProfileThresholdMs =
    Number.isFinite(configuredTransitionThreshold) &&
    configuredTransitionThreshold >= 20 && configuredTransitionThreshold <= 5000
      ? configuredTransitionThreshold : 80;
  let transitionProfileInstalled = false;
  let transitionFrameStats = Object.create(null);
  let sceneUpdateMs = 0;
  let sceneUpdateCalls = 0;
  let sceneRenderMs = 0;
  let sceneRenderCalls = 0;
  function logicProfileRecord(name, elapsed) {
    let item = logicProfileStats[name];
    if (!item) item = logicProfileStats[name] = { calls: 0, totalMs: 0, maxMs: 0 };
    item.calls++;
    item.totalMs += elapsed;
    if (elapsed > item.maxMs) item.maxMs = elapsed;
  }
  function installLogicProfileWrap(target, method, name) {
    if (!target || typeof target[method] !== 'function') return false;
    const original = target[method];
    if (original.__pmjsLogicProfileWrapped) return true;
    function wrappedLogicProfileMethod() {
      if (!logicProfileSampleActive) return original.apply(this, arguments);
      const started = performance.now();
      try {
        return original.apply(this, arguments);
      } finally {
        logicProfileRecord(name, performance.now() - started);
      }
    }
    wrappedLogicProfileMethod.__pmjsLogicProfileWrapped = true;
    wrappedLogicProfileMethod.__pmjsLogicProfileOriginal = original;
    target[method] = wrappedLogicProfileMethod;
    return true;
  }
  function installLogicProfileWraps() {
    if (!logicProfileEnabled || logicProfileInstalled) return logicProfileInstalled;
    const specs = [
      ['Game_Map', 'update', 'Game_Map.update'],
      ['Game_Map', 'refreshIfNeeded', 'Game_Map.refreshIfNeeded'],
      ['Game_Map', 'updateInterpreter', 'Game_Map.updateInterpreter'],
      ['Game_Map', 'updateScroll', 'Game_Map.updateScroll'],
      ['Game_Map', 'updateEvents', 'Game_Map.updateEvents'],
      ['Game_Map', 'updateVehicles', 'Game_Map.updateVehicles'],
      ['Game_Map', 'updateParallax', 'Game_Map.updateParallax'],
      ['Game_Event', 'update', 'Game_Event.update'],
      ['Game_CommonEvent', 'update', 'Game_CommonEvent.update'],
      ['Game_Interpreter', 'update', 'Game_Interpreter.update'],
      ['Game_Player', 'update', 'Game_Player.update'],
      ['Game_Followers', 'update', 'Game_Followers.update'],
      ['Scene_Map', 'update', 'Scene_Map.update'],
      ['Scene_Map', 'updateMain', 'Scene_Map.updateMain'],
      ['Scene_Map', 'updateMainMultiply', 'Scene_Map.updateMainMultiply'],
      ['Spriteset_Map', 'update', 'Spriteset_Map.update'],
      ['Spriteset_Base', 'update', 'Spriteset_Base.update'],
      ['Tilemap', 'update', 'Tilemap.update'],
      ['Weather', 'update', 'Weather.update'],
      ['WindowLayer', 'update', 'WindowLayer.update'],
      ['Lightmask', '_updateMask', 'Lightmask._updateMask']
    ];
    let installed = 0;
    for (const spec of specs) {
      const ctor = globalThis[spec[0]];
      if (ctor && ctor.prototype &&
          installLogicProfileWrap(ctor.prototype, spec[1], spec[2])) {
        installed++;
      }
    }
    if (globalThis.SceneManager) {
      const managerSpecs = [
        ['updateInputData', 'SceneManager.updateInputData'],
        ['changeScene', 'SceneManager.changeScene'],
        ['updateScene', 'SceneManager.updateScene'],
        ['renderScene', 'SceneManager.renderScene']
      ];
      for (const spec of managerSpecs) {
        if (installLogicProfileWrap(globalThis.SceneManager, spec[0], spec[1])) {
          installed++;
        }
      }
    }
    logicProfileInstalled = installed > 0;
    if (logicProfileInstalled) {
      console.log('[pmjs] logic profiler enabled methods=' + installed +
        ' sample_every=' + logicProfileEvery);
    }
    return logicProfileInstalled;
  }
  function logicProfileSnapshot() {
    const rows = Object.keys(logicProfileStats).map(name => {
      const value = logicProfileStats[name];
      return {
        name,
        calls: value.calls,
        totalMs: Math.round(value.totalMs * 100) / 100,
        avgMs: value.calls > 0
          ? Math.round(value.totalMs / value.calls * 1000) / 1000 : 0,
        maxMs: Math.round(value.maxMs * 1000) / 1000
      };
    }).sort((left, right) => right.totalMs - left.totalMs);
    for (const name of Object.keys(logicProfileStats)) delete logicProfileStats[name];
    return rows;
  }

  function transitionRecord(name, elapsed) {
    let item = transitionFrameStats[name];
    if (!item) item = transitionFrameStats[name] = { calls: 0, totalMs: 0, maxMs: 0 };
    item.calls++;
    item.totalMs += elapsed;
    if (elapsed > item.maxMs) item.maxMs = elapsed;
  }
  function installTransitionWrap(target, method, name) {
    if (!target || typeof target[method] !== 'function') return false;
    const original = target[method];
    if (original.__pmjsTransitionWrapped) return true;
    function wrappedTransitionMethod() {
      const started = performance.now();
      try {
        return original.apply(this, arguments);
      } finally {
        transitionRecord(name, performance.now() - started);
      }
    }
    wrappedTransitionMethod.__pmjsTransitionWrapped = true;
    wrappedTransitionMethod.__pmjsOriginal = original;
    target[method] = wrappedTransitionMethod;
    return true;
  }
  function installTransitionProfileWraps() {
    if (!transitionProfileEnabled || transitionProfileInstalled) {
      return transitionProfileInstalled;
    }
    const specs = [
      ['Scene_Map', 'create', 'Scene_Map.create'],
      ['Scene_Map', 'onMapLoaded', 'Scene_Map.onMapLoaded'],
      ['Scene_Map', 'createDisplayObjects', 'Scene_Map.createDisplayObjects'],
      ['Scene_Map', 'createSpriteset', 'Scene_Map.createSpriteset'],
      ['Scene_Map', 'start', 'Scene_Map.start'],
      ['Spriteset_Map', 'initialize', 'Spriteset_Map.initialize'],
      ['Spriteset_Map', 'createLowerLayer', 'Spriteset_Map.createLowerLayer'],
      ['Spriteset_Map', 'createTilemap', 'Spriteset_Map.createTilemap'],
      ['Spriteset_Map', 'createCharacters', 'Spriteset_Map.createCharacters'],
      ['Tilemap', '_paintAllTiles', 'Tilemap._paintAllTiles'],
      ['Tilemap', 'refresh', 'Tilemap.refresh'],
      ['Bitmap', '_requestImage', 'Bitmap._requestImage'],
      ['Bitmap', '_onLoad', 'Bitmap._onLoad'],
      ['ImageCache', '_truncateCache', 'ImageCache._truncateCache']
    ];
    let installed = 0;
    for (const spec of specs) {
      const ctor = globalThis[spec[0]];
      if (ctor && ctor.prototype &&
          installTransitionWrap(ctor.prototype, spec[1], spec[2])) installed++;
    }
    if (globalThis.DataManager) {
      for (const spec of [
        ['loadMapData', 'DataManager.loadMapData'],
        ['onLoad', 'DataManager.onLoad'],
        ['extractMetadata', 'DataManager.extractMetadata']
      ]) {
        if (installTransitionWrap(globalThis.DataManager, spec[0], spec[1])) installed++;
      }
    }
    transitionProfileInstalled = installed > 0;
    if (transitionProfileInstalled) {
      console.log('[pmjs] transition profiler enabled methods=' + installed +
        ' threshold_ms=' + transitionProfileThresholdMs);
    }
    return transitionProfileInstalled;
  }
  function transitionProfileFlush(frameMs) {
    if (!transitionProfileEnabled || !transitionProfileInstalled) {
      transitionFrameStats = Object.create(null);
      return;
    }
    if (frameMs >= transitionProfileThresholdMs) {
      const rows = Object.keys(transitionFrameStats).map(name => {
        const item = transitionFrameStats[name];
        return {
          name,
          calls: item.calls,
          totalMs: Math.round(item.totalMs * 100) / 100,
          maxMs: Math.round(item.maxMs * 100) / 100
        };
      }).sort((a, b) => b.totalMs - a.totalMs);
      const sceneManager = globalThis.SceneManager;
      const sceneName = sceneManager && sceneManager._scene &&
        sceneManager._scene.constructor
        ? sceneManager._scene.constructor.name : null;
      console.log('[pmjs-transition] ' + JSON.stringify({
        frameMs: Math.round(frameMs * 100) / 100,
        scene: sceneName,
        methods: rows.slice(0, 24)
      }));
    }
    transitionFrameStats = Object.create(null);
  }

  function installSceneTimingWraps() {
    const sceneManager = globalThis.SceneManager;
    if (!sceneManager || typeof sceneManager.updateScene !== 'function' ||
        typeof sceneManager.renderScene !== 'function') return false;
    if (sceneManager.__pmjsTimingWrapped === true) return true;
    const originalUpdateScene = sceneManager.updateScene;
    const originalRenderScene = sceneManager.renderScene;
    sceneManager.updateScene = function () {
      if (!sceneTimingSampleActive) {
        return originalUpdateScene.apply(this, arguments);
      }
      const started = performance.now();
      const result = originalUpdateScene.apply(this, arguments);
      sceneUpdateMs += performance.now() - started;
      sceneUpdateCalls++;
      return result;
    };
    sceneManager.renderScene = function () {
      if (!sceneTimingSampleActive) {
        return originalRenderScene.apply(this, arguments);
      }
      const started = performance.now();
      const result = originalRenderScene.apply(this, arguments);
      sceneRenderMs += performance.now() - started;
      sceneRenderCalls++;
      return result;
    };
    sceneManager.__pmjsTimingWrapped = true;
    return true;
  }
  if (hostProcess.env.PMJS_PROFILE_ON_SIGNAL === '1' &&
      typeof hostProcess.on === 'function') {
    const profileSeconds = Number(hostProcess.env.PMJS_PROFILE_SECONDS || 20);
    const profileDir = hostProcess.env.PMJS_PROFILE_DIR || '/tmp';
    let profiling = false;
    hostProcess.on('SIGUSR2', () => {
      if (profiling) return;
      let inspector;
      try { inspector = require('inspector'); } catch (_) { return; }
      let session;
      try {
        session = new inspector.Session();
        session.connect();
        session.post('Profiler.enable');
        session.post('Profiler.start');
      } catch (error) {
        console.warn('[pmjs-profile] start failed: ' + error);
        try { if (session) session.disconnect(); } catch (_) {}
        return;
      }
      profiling = true;
      console.log('[pmjs-profile] capture started (' + profileSeconds + 's)');
      setTimeout(() => {
        session.post('Profiler.stop', (error, result) => {
          try {
            if (error || !result || !result.profile) {
              console.warn('[pmjs-profile] stop failed: ' + error);
            } else {
              const target = profileDir + '/pmjs-' + Date.now() + '.cpuprofile';
              require('fs').writeFileSync(target, JSON.stringify(result.profile));
              console.log('[pmjs-profile] wrote ' + target);
            }
          } catch (writeError) {
            console.warn('[pmjs-profile] write failed: ' + writeError);
          } finally {
            try { session.disconnect(); } catch (_) {}
            profiling = false;
          }
        });
      }, profileSeconds * 1000);
    });
  }
  function reportMemoryTelemetry(now) {
    if (!memoryTelemetryEnabled || now < nextMemoryTelemetry) return;
    nextMemoryTelemetry = now + memoryTelemetryMs;
    try {
      const usage = hostProcess.memoryUsage();
      const stats = typeof native.render.stats === 'function'
        ? native.render.stats() : null;
      let fps = null;
      if (stats && typeof stats.frames === 'number') {
        if (lastTelemetryFrames !== null && now > lastTelemetryFramesAt) {
          fps = (stats.frames - lastTelemetryFrames) /
            ((now - lastTelemetryFramesAt) / 1000);
        }
        lastTelemetryFrames = stats.frames;
        lastTelemetryFramesAt = now;
      }
      let logicRatio = null;
      let logicDebtMs = null;
      const sceneManager = globalThis.SceneManager;
      if (sceneManager && typeof sceneManager._t === 'number') {
        if (lastTelemetryLogic !== null && now > lastTelemetryLogic.at) {
          logicRatio = (sceneManager._t - lastTelemetryLogic.t) /
            ((now - lastTelemetryLogic.at) / 1000);
        }
        lastTelemetryLogic = { t: sceneManager._t, at: now };
        if (typeof sceneManager._accumulator === 'number') {
          logicDebtMs = sceneManager._accumulator * 1000;
        }
      }
      let playerTilesPerSec = null;
      let playerPxPerSec = null;
      let playerMoving = null;
      let playerSpeed = null;
      const player = globalThis.$gamePlayer;
      if (player && typeof player._realX === 'number' &&
          typeof player._realY === 'number') {
        if (lastTelemetryPlayer !== null && now > lastTelemetryPlayer.at) {
          const seconds = (now - lastTelemetryPlayer.at) / 1000;
          const dx = Math.abs(player._realX - lastTelemetryPlayer.x);
          const dy = Math.abs(player._realY - lastTelemetryPlayer.y);
          const tileWidth = globalThis.$gameMap &&
              typeof globalThis.$gameMap.tileWidth === 'function'
            ? globalThis.$gameMap.tileWidth() : 48;
          playerTilesPerSec = Math.round(((dx + dy) / seconds) * 1000) / 1000;
          playerPxPerSec = Math.round(((dx + dy) * tileWidth / seconds) * 10) / 10;
        }
        lastTelemetryPlayer = { x: player._realX, y: player._realY, at: now };
        playerMoving = typeof player.isMoving === 'function'
          ? player.isMoving() : null;
        playerSpeed = typeof player._moveSpeed === 'number'
          ? player._moveSpeed : null;
      }
      const snapshot = {
        uptimeMs: Math.round(now),
        fps: fps === null ? null : Math.round(fps * 10) / 10,
        logicRatio: logicRatio === null ? null
          : Math.round(logicRatio * 1000) / 1000,
        logicDebtMs: logicDebtMs === null ? null
          : Math.round(logicDebtMs * 10) / 10,
        playerTilesPerSec: playerTilesPerSec,
        playerPxPerSec: playerPxPerSec,
        playerMoving: playerMoving,
        playerSpeed: playerSpeed,
        renderScale: typeof globalThis.__pmjsRenderScale === 'number'
          ? globalThis.__pmjsRenderScale : 1,
        overload: (function() {
          var totalDiscontinuities = Number(
            globalThis.__pmjsOverloadDiscontinuities || 0);
          var totalDroppedMs = Number(globalThis.__pmjsTddpDroppedMs || 0);
          var totalDeferredMs = Number(globalThis.__pmjsTddpDeferredMs || 0);
          var previous = lastTelemetryOverload;
          lastTelemetryOverload = {
            discontinuities: totalDiscontinuities,
            droppedMs: totalDroppedMs,
            deferredMs: totalDeferredMs
          };
          return {
            discontinuities: totalDiscontinuities,
            tddpDroppedMs: Math.round(totalDroppedMs * 100) / 100,
            tddpDeferredMs: Math.round(totalDeferredMs * 100) / 100,
            delta: previous ? {
              discontinuities: totalDiscontinuities - previous.discontinuities,
              tddpDroppedMs: Math.round(
                (totalDroppedMs - previous.droppedMs) * 100) / 100,
              tddpDeferredMs: Math.round(
                (totalDeferredMs - previous.deferredMs) * 100) / 100
            } : null
          };
        })(),
        process: {
          rss: usage.rss,
          heapTotal: usage.heapTotal,
          heapUsed: usage.heapUsed,
          external: usage.external,
          arrayBuffers: usage.arrayBuffers
        },
        frameTimingMs: telemetryPhaseSamples > 0 ? {
          samples: telemetryPhaseSamples,
          sampleEvery: telemetryTimingSampleEvery,
          begin: Math.round(telemetryPhaseBeginMs / telemetryPhaseSamples * 100) / 100,
          tick: Math.round(telemetryPhaseTickMs / telemetryPhaseSamples * 100) / 100,
          render: Math.round(telemetryPhaseRenderMs / telemetryPhaseSamples * 100) / 100,
          native: Math.round(telemetryPhaseNativeMs / telemetryPhaseSamples * 100) / 100,
          afterNative: Math.round(telemetryPhaseAfterMs / telemetryPhaseSamples * 100) / 100,
          swap: Math.round(telemetryPhaseSwapMs / telemetryPhaseSamples * 100) / 100,
          total: Math.round((telemetryPhaseBeginMs + telemetryPhaseTickMs +
            telemetryPhaseRenderMs + telemetryPhaseNativeMs +
            telemetryPhaseAfterMs + telemetryPhaseSwapMs) /
            telemetryPhaseSamples * 100) / 100,
          maxTotal: Math.round(telemetryPhaseMaxTotalMs * 100) / 100,
          maxTick: Math.round(telemetryPhaseMaxTickMs * 100) / 100,
          maxNative: Math.round(telemetryPhaseMaxNativeMs * 100) / 100,
          maxSwap: Math.round(telemetryPhaseMaxSwapMs * 100) / 100
        } : null,
        rendererStats: stats
      };
      if (stats && lastTelemetryRendererStats &&
          typeof stats.frames === 'number' &&
          stats.frames > lastTelemetryRendererStats.frames) {
        const frameDelta = stats.frames - lastTelemetryRendererStats.frames;
        snapshot.rendererPerFrame = {
          commands: Math.round((stats.commands - lastTelemetryRendererStats.commands) /
            frameDelta * 100) / 100,
          drawCalls: Math.round((stats.drawCalls - lastTelemetryRendererStats.drawCalls) /
            frameDelta * 100) / 100,
          bufferUploads: Math.round((stats.bufferUploads -
            lastTelemetryRendererStats.bufferUploads) / frameDelta * 100) / 100,
          filterDrawCalls: Math.round((stats.filterDrawCalls -
            lastTelemetryRendererStats.filterDrawCalls) / frameDelta * 100) / 100,
          baseSpriteDrawCalls: Math.round((stats.baseSpriteDrawCalls -
            lastTelemetryRendererStats.baseSpriteDrawCalls) / frameDelta * 100) / 100,
          effectSpriteDrawCalls: Math.round((stats.effectSpriteDrawCalls -
            lastTelemetryRendererStats.effectSpriteDrawCalls) / frameDelta * 100) / 100,
          tileDrawCalls: Math.round((stats.tileDrawCalls -
            lastTelemetryRendererStats.tileDrawCalls) / frameDelta * 100) / 100
        };
      }
      if (stats) lastTelemetryRendererStats = stats;
      if (logicProfileEnabled && logicProfileInstalled) {
        snapshot.logicProfile = {
          sampleEvery: logicProfileEvery,
          methods: logicProfileSnapshot()
        };
      }
      if (sceneWrapsInstalled && telemetryPhaseSamples > 0) {
        snapshot.sceneTimingMs = {
          frames: telemetryPhaseSamples,
          updateSceneCalls: sceneUpdateCalls,
          updateSceneCallsPerFrame:
            Math.round(sceneUpdateCalls / telemetryPhaseSamples * 100) / 100,
          updateSceneMsPerFrame:
            Math.round(sceneUpdateMs / telemetryPhaseSamples * 100) / 100,
          updateSceneMsPerCall: sceneUpdateCalls > 0
            ? Math.round(sceneUpdateMs / sceneUpdateCalls * 100) / 100 : null,
          renderSceneCalls: sceneRenderCalls,
          renderSceneMsPerFrame:
            Math.round(sceneRenderMs / telemetryPhaseSamples * 100) / 100,
          renderSceneMsPerCall: sceneRenderCalls > 0
            ? Math.round(sceneRenderMs / sceneRenderCalls * 100) / 100 : null
        };
      }
      sceneUpdateMs = 0;
      sceneUpdateCalls = 0;
      sceneRenderMs = 0;
      sceneRenderCalls = 0;
      telemetryPhaseSamples = 0;
      telemetryPhaseBeginMs = 0;
      telemetryPhaseTickMs = 0;
      telemetryPhaseRenderMs = 0;
      telemetryPhaseNativeMs = 0;
      telemetryPhaseAfterMs = 0;
      telemetryPhaseSwapMs = 0;
      telemetryPhaseMaxTotalMs = 0;
      telemetryPhaseMaxTickMs = 0;
      telemetryPhaseMaxNativeMs = 0;
      telemetryPhaseMaxSwapMs = 0;
      if (memoryTelemetryDetailEnabled) {
        snapshot.renderer = typeof native.render.memory === 'function'
          ? native.render.memory() : null;
        snapshot.images = native.images &&
            typeof native.images.memory === 'function'
          ? native.images.memory(5) : null;
        snapshot.canvases = native.canvas &&
            typeof native.canvas.memory === 'function'
          ? native.canvas.memory() : null;
      };
      console.log('[pmjs-memory] ' + JSON.stringify(snapshot));
    } catch (error) {
      console.warn('[pmjs-memory] telemetry failed: ' +
        (error && error.stack || error));
    }
  }
  console.log(`[pmjs] timing logic_hz=${timing.logicHz} ` +
    (timing.uncapped ? 'render_hz=uncapped' : `render_hz=${timing.renderHz}`));
  console.log(`[pmjs] ready size=${options.width}x${options.height}`);
  return new Promise((resolve, reject) => {
    function schedule() {
      if (timing.uncapped) { hostSetImmediate(tick); return; }
      const delay = Math.max(0, deadline - native.runtime.monotonicNow());
      if (delay < 1 && hostSetImmediate) hostSetImmediate(tick);
      else hostSetTimeout(tick, delay);
    }
    function tick() {
      try {
        if (!native.pollEvents()) { resolve(); return; }
        if (memoryTelemetryEnabled && !sceneWrapsInstalled) {
          sceneWrapsInstalled = installSceneTimingWraps();
        }
        if (logicProfileEnabled && !logicProfileInstalled) {
          installLogicProfileWraps();
        }
        if (transitionProfileEnabled && !transitionProfileInstalled) {
          installTransitionProfileWraps();
        }
        if (typeof globalThis.__pmjsUpdateWindowStateBits === 'function' &&
            typeof native.runtime.windowStateBits === 'function') {
          globalThis.__pmjsUpdateWindowStateBits(native.runtime.windowStateBits());
        } else if (typeof globalThis.__pmjsUpdateWindowState === 'function') {
          globalThis.__pmjsUpdateWindowState(native.runtime.windowState());
        }
        if (typeof globalThis.__pmjsReceiveInput === 'function' &&
            typeof native.input.snapshot === 'function') {
          globalThis.__pmjsReceiveInput(native.input.snapshot());
        }
        const now = performance.now();
        const transitionFrameStart = transitionProfileEnabled ? performance.now() : 0;
        const sampleFrameTiming = memoryTelemetryEnabled &&
          (++telemetryTimingFrame % telemetryTimingSampleEvery === 0);
        sceneTimingSampleActive = sampleFrameTiming;
        logicProfileSampleActive = logicProfileEnabled && logicProfileInstalled &&
          (++logicProfileFrame % logicProfileEvery === 0);
        let phaseStart = sampleFrameTiming ? performance.now() : 0;
        native.beginFrame();
        let phaseBeginEnd = sampleFrameTiming ? performance.now() : 0;
        globalThis.__pmjsTick(now);
        let phaseTickEnd = sampleFrameTiming ? performance.now() : 0;
        globalThis.__pmjsRender(now);
        let phaseRenderEnd = sampleFrameTiming ? performance.now() : 0;
        native.renderFrame();
        let phaseNativeEnd = sampleFrameTiming ? performance.now() : 0;
        if (typeof globalThis.__pmjsAfterNativeRender === 'function') {
          globalThis.__pmjsAfterNativeRender();
        }
        let phaseAfterEnd = sampleFrameTiming ? performance.now() : 0;
        native.swapFrame();
        if (sampleFrameTiming) {
          const phaseSwapEnd = performance.now();
          const beginMs = phaseBeginEnd - phaseStart;
          const tickMs = phaseTickEnd - phaseBeginEnd;
          const renderMs = phaseRenderEnd - phaseTickEnd;
          const nativeMs = phaseNativeEnd - phaseRenderEnd;
          const afterMs = phaseAfterEnd - phaseNativeEnd;
          const swapMs = phaseSwapEnd - phaseAfterEnd;
          const totalMs = phaseSwapEnd - phaseStart;
          telemetryPhaseSamples++;
          telemetryPhaseBeginMs += beginMs;
          telemetryPhaseTickMs += tickMs;
          telemetryPhaseRenderMs += renderMs;
          telemetryPhaseNativeMs += nativeMs;
          telemetryPhaseAfterMs += afterMs;
          telemetryPhaseSwapMs += swapMs;
          telemetryPhaseMaxTotalMs = Math.max(telemetryPhaseMaxTotalMs, totalMs);
          telemetryPhaseMaxTickMs = Math.max(telemetryPhaseMaxTickMs, tickMs);
          telemetryPhaseMaxNativeMs = Math.max(telemetryPhaseMaxNativeMs, nativeMs);
          telemetryPhaseMaxSwapMs = Math.max(telemetryPhaseMaxSwapMs, swapMs);
        }
        if (transitionProfileEnabled) {
          transitionProfileFlush(performance.now() - transitionFrameStart);
        }
        if (memoryTelemetryEnabled && now >= nextMemoryTelemetry) {
          reportMemoryTelemetry(now);
        }
        if (!timing.uncapped) {
          const monotonicNow = native.runtime.monotonicNow();
          deadline = advanceDeadline(deadline, monotonicNow, period);
        }
        schedule();
      } catch (error) {
        try { native.runtime.quit(); } catch (_) {}
        reject(error);
      }
    }
    schedule();
  });
}

module.exports = { run, validate, parseTimingConfig, resolveSwapDefault, advanceDeadline,
  PMJS_MV_LOGIC_HZ, PMJS_SUPPORTED_RENDER_HZ };
