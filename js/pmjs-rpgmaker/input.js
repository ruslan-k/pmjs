'use strict';

(function() {
  function installInputBridge() {
    if (typeof Input === 'undefined' || typeof Input.update !== 'function') return false;
    if (Input.update._pmjsNativeBridge) return true;
    var originalUpdate = Input.update;
    var previousKeys = null;
    var previousButtons = null;

    function copyMapper(mapper) {
      var copy = {};
      for (var code in mapper) copy[code] = mapper[code];
      return copy;
    }

    function actionHeld(input, action, state) {
      var keys = state.keysDown || [];
      var index;
      for (index = 0; index < keys.length; index++) {
        if (input.keyMapper[keys[index]] === action) return true;
      }
      keys = state.keysPressed || [];
      for (index = 0; index < keys.length; index++) {
        if (input.keyMapper[keys[index]] === action) return true;
      }
      var gamepadStates = input._gamepadStates || [];
      for (var pad = 0; pad < gamepadStates.length; pad++) {
        var buttons = gamepadStates[pad];
        if (!buttons) continue;
        for (var button = 0; button < buttons.length; button++) {
          if (buttons[button] && input.gamepadMapper[button] === action) return true;
        }
      }
      return false;
    }

    // Mapper objects are normally unchanged for the lifetime of a game.
    // Mutate the cached copies in place and allocate the "affected" set only
    // when a plugin actually changes a mapping. The old implementation copied
    // both mapper objects plus union objects on every 60 Hz Input.update().
    function reconcileMapper(input, mapper, previous, state) {
      var affected = null;
      var code;
      for (code in mapper) {
        var nextAction = mapper[code];
        var oldAction = previous[code];
        if (oldAction === nextAction) continue;
        if (!affected) affected = Object.create(null);
        if (oldAction) affected[oldAction] = true;
        if (nextAction) affected[nextAction] = true;
        previous[code] = nextAction;
      }
      for (code in previous) {
        if (Object.prototype.hasOwnProperty.call(mapper, code)) continue;
        var removedAction = previous[code];
        if (!affected) affected = Object.create(null);
        if (removedAction) affected[removedAction] = true;
        delete previous[code];
      }
      if (!affected) return false;
      for (var action in affected) {
        input._currentState[action] = actionHeld(input, action, state);
      }
      return true;
    }

    function nativeInputUpdate() {
      var state = globalThis.__pmjsInputSnapshot;
      if (state && this.keyMapper && this.gamepadMapper) {
        if (previousKeys) {
          reconcileMapper(this, this.keyMapper, previousKeys, state);
        } else {
          previousKeys = copyMapper(this.keyMapper);
        }
        if (previousButtons) {
          if (reconcileMapper(this, this.gamepadMapper, previousButtons, state)) {
            this._gamepadStates = [];
          }
        } else {
          previousButtons = copyMapper(this.gamepadMapper);
        }
      }
      try { return originalUpdate.apply(this, arguments); }
      finally {
        if (typeof globalThis.__pmjsFinishInputStep === 'function') {
          globalThis.__pmjsFinishInputStep();
        }
        if (NativeHost.input.consumePressed) NativeHost.input.consumePressed();
      }
    }
    nativeInputUpdate._pmjsNativeBridge = true;
    Input.update = nativeInputUpdate;
    Input._pmjsNativeBridgeInstalled = true;
    return true;
  }

  PMJS.phases.on('afterGuestPlugins', 'pmjs-rpgmaker.input', function() {
    if (!installInputBridge()) throw new Error('RPG Maker Input did not initialize');
  });
})();
