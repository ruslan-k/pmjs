'use strict';

(function() {
  var nextId = 1;

  var pendingTasks = [];
  var pendingTaskHead = 0;
  function drainTasks() {
    var deadline = performance.now() + 4;
    var count = 0;
    while (pendingTaskHead < pendingTasks.length && count < 64 &&
        performance.now() < deadline) {
      pendingTasks[pendingTaskHead++]();
      count++;
    }
    // Avoid repeated Array.shift() reindexing while bounding retained callbacks.
    if (pendingTaskHead >= pendingTasks.length) {
      pendingTasks.length = 0;
      pendingTaskHead = 0;
    } else if (pendingTaskHead > 64) {
      pendingTasks.splice(0, pendingTaskHead);
      pendingTaskHead = 0;
    }
  }

  PMJS.tasks = {
    enqueue: function(callback) { pendingTasks.push(callback); },
    drain: drainTasks
  };

  var rafQueue = [];
  var rafDrainQueue = [];
  var cancelledRafs = new Set();
  var timers = new Map();
  var dueTimers = [];
  var nextTimerDeadline = Infinity;

  var schedulerNow = 0;
  var draining = false;

  function clockNow() {
    return draining ? schedulerNow : performance.now();
  }

  function reportAsyncError(error) {
    console.error('[pmjs] async error:', error);
  }

  function requestAnimationFrameCompat(callback) {
    if (typeof callback !== 'function') {
      throw new TypeError('requestAnimationFrame callback must be a function');
    }
    var id = nextId++;
    rafQueue.push({
      id: id,
      callback: callback,
      cancelled: false
    });
    return id;
  }

  function cancelAnimationFrameCompat(id) {
    cancelledRafs.add(id);
    for (var i = 0; i < rafQueue.length; i++) {
      if (rafQueue[i].id === id) {
        rafQueue[i].cancelled = true;
        break;
      }
    }
  }

  function setTimeoutCompat(callback, delay) {
    var id = nextId++;
    var args = null;
    if (arguments.length > 2) {
      args = [];
      for (var i = 2; i < arguments.length; i++) args.push(arguments[i]);
    }
    var deadline = clockNow() + Math.max(0, Number(delay) || 0);
    timers.set(id, {
      id: id,
      callback: callback,
      args: args,
      deadline: deadline,
      interval: 0
    });
    if (deadline < nextTimerDeadline) nextTimerDeadline = deadline;

    return id;
  }

  function clearTimeoutCompat(id) {
    timers.delete(id);
  }

  function setIntervalCompat(callback, delay) {
    var interval = Math.max(1, Number(delay) || 0);
    var id = nextId++;
    var args = null;
    if (arguments.length > 2) {
      args = [];
      for (var i = 2; i < arguments.length; i++) args.push(arguments[i]);
    }
    var deadline = clockNow() + interval;
    timers.set(id, {
      id: id,
      callback: callback,
      args: args,
      deadline: deadline,
      interval: interval
    });
    if (deadline < nextTimerDeadline) nextTimerDeadline = deadline;

    return id;
  }

  function clearIntervalCompat(id) {
    timers.delete(id);
  }

  function drainTimers(now) {
    // Most frames have no timer due. Avoid a Map walk, temporary array and sort
    // on that common path.
    if (now < nextTimerDeadline) return;

    dueTimers.length = 0;
    nextTimerDeadline = Infinity;
    timers.forEach(function(timer) {
      if (timer.deadline <= now) {
        dueTimers.push(timer);
      } else if (timer.deadline < nextTimerDeadline) {
        nextTimerDeadline = timer.deadline;
      }
    });

    dueTimers.sort(function(a, b) {
      return a.deadline - b.deadline || a.id - b.id;
    });

    for (var i = 0; i < dueTimers.length; i++) {
      var timer = dueTimers[i];

      if (!timers.has(timer.id)) continue;

      if (timer.interval > 0) {
        do {
          timer.deadline += timer.interval;
        } while (timer.deadline <= now);
        if (timer.deadline < nextTimerDeadline) nextTimerDeadline = timer.deadline;
      } else {
        timers.delete(timer.id);
      }

      try {
        if (typeof timer.callback === 'function') {
          timer.callback.apply(globalThis, timer.args);
        } else if (typeof timer.callback === 'string') {
          (0, eval)(timer.callback);
        }
      } catch (error) {
        reportAsyncError(error);
      }
    }
    dueTimers.length = 0;
  }

  function drainAnimationFrames(now) {
    // Zero-RAF frames dominate menus and many event-heavy RPG scenes. Avoid
    // even swapping the retained queues on that path.
    if (rafQueue.length === 0) {
      if (cancelledRafs.size) cancelledRafs.clear();
      return;
    }
    // Double-buffer the RAF queues instead of allocating a new array every
    // rendered frame.
    var callbacks = rafQueue;
    rafQueue = rafDrainQueue;
    rafDrainQueue = callbacks;
    rafQueue.length = 0;

    for (var i = 0; i < callbacks.length; i++) {
      var entry = callbacks[i];
      if (!entry.cancelled && !cancelledRafs.has(entry.id)) {
        try {
          entry.callback(now);
        } catch (error) {
          reportAsyncError(error);
        }
      }
      cancelledRafs.delete(entry.id);
    }
    callbacks.length = 0;
    cancelledRafs.clear();
  }

  function pmjsDrainScheduler(now) {
    if (typeof now !== 'number') now = performance.now();
    // Nothing can observe schedulerNow/draining when neither a timer nor RAF
    // callback can run.
    if (now < nextTimerDeadline && rafQueue.length === 0 &&
        cancelledRafs.size === 0) return;
    schedulerNow = now;
    draining = true;
    try {
      drainTimers(now);
      drainAnimationFrames(now);
    } finally {
      draining = false;
    }
  }

  globalThis.requestAnimationFrame = requestAnimationFrameCompat;
  globalThis.cancelAnimationFrame = cancelAnimationFrameCompat;

  globalThis.setTimeout = setTimeoutCompat;
  globalThis.clearTimeout = clearTimeoutCompat;
  globalThis.setInterval = setIntervalCompat;
  globalThis.clearInterval = clearIntervalCompat;

  globalThis.pmjsDrainScheduler = pmjsDrainScheduler;

  if (typeof window !== 'undefined') {
    window.requestAnimationFrame = requestAnimationFrameCompat;
    window.cancelAnimationFrame = cancelAnimationFrameCompat;
    window.setTimeout = setTimeoutCompat;
    window.clearTimeout = clearTimeoutCompat;
    window.setInterval = setIntervalCompat;
    window.clearInterval = clearIntervalCompat;
  }
})();
