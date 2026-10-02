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
  var timerHeap = [];
  var deferredTimers = [];
  var drainingTimers = false;
  var nextTimerDeadline = Infinity;

  var schedulerNow = 0;
  var draining = false;

  function clockNow() {
    return draining ? schedulerNow : performance.now();
  }

  function reportAsyncError(error) {
    console.error('[pmjs] async error:', error);
  }

  function timerLess(a, b) {
    return a.deadline < b.deadline ||
      (a.deadline === b.deadline && a.id < b.id);
  }

  function heapPush(timer) {
    var index = timerHeap.length;
    timerHeap.push(timer);
    while (index > 0) {
      var parent = (index - 1) >> 1;
      var parentTimer = timerHeap[parent];
      if (!timerLess(timer, parentTimer)) break;
      timerHeap[index] = parentTimer;
      index = parent;
    }
    timerHeap[index] = timer;
  }

  function heapPop() {
    var first = timerHeap[0];
    var last = timerHeap.pop();
    if (timerHeap.length !== 0) {
      var index = 0;
      while (true) {
        var left = index * 2 + 1;
        if (left >= timerHeap.length) break;
        var right = left + 1;
        var child = left;
        if (right < timerHeap.length &&
            timerLess(timerHeap[right], timerHeap[left])) {
          child = right;
        }
        if (!timerLess(timerHeap[child], last)) break;
        timerHeap[index] = timerHeap[child];
        index = child;
      }
      timerHeap[index] = last;
    }
    return first;
  }

  function pruneTimerHeap() {
    while (timerHeap.length) {
      var timer = timerHeap[0];
      if (timers.get(timer.id) === timer) break;
      heapPop();
    }
  }

  function refreshTimerDeadline() {
    pruneTimerHeap();
    nextTimerDeadline = timerHeap.length ? timerHeap[0].deadline : Infinity;
  }

  function queueTimer(timer) {
    if (drainingTimers) {
      deferredTimers.push(timer);
    } else {
      heapPush(timer);
      if (timer.deadline < nextTimerDeadline) {
        nextTimerDeadline = timer.deadline;
      }
    }
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

  function timerArgs(start, argsLike) {
    if (argsLike.length <= start) return null;
    var args = new Array(argsLike.length - start);
    for (var i = start; i < argsLike.length; i++) args[i - start] = argsLike[i];
    return args;
  }

  function setTimeoutCompat(callback, delay) {
    var id = nextId++;
    var timer = {
      id: id,
      callback: callback,
      args: timerArgs(2, arguments),
      deadline: clockNow() + Math.max(0, Number(delay) || 0),
      interval: 0
    };
    timers.set(id, timer);
    queueTimer(timer);
    return id;
  }

  function clearTimeoutCompat(id) {
    if (timers.delete(id) && timerHeap.length && timerHeap[0].id === id) {
      refreshTimerDeadline();
    }
  }

  function setIntervalCompat(callback, delay) {
    var interval = Math.max(1, Number(delay) || 0);
    var id = nextId++;
    var timer = {
      id: id,
      callback: callback,
      args: timerArgs(2, arguments),
      deadline: clockNow() + interval,
      interval: interval
    };
    timers.set(id, timer);
    queueTimer(timer);
    return id;
  }

  function clearIntervalCompat(id) {
    clearTimeoutCompat(id);
  }

  function invokeTimer(timer) {
    if (typeof timer.callback === 'function') {
      if (timer.args === null) timer.callback.call(globalThis);
      else timer.callback.apply(globalThis, timer.args);
    } else if (typeof timer.callback === 'string') {
      (0, eval)(timer.callback);
    }
  }

  function drainTimers(now) {
    if (now < nextTimerDeadline) return;

    drainingTimers = true;
    try {
      while (true) {
        pruneTimerHeap();
        if (timerHeap.length === 0 || timerHeap[0].deadline > now) break;

        var timer = heapPop();
        if (timers.get(timer.id) !== timer) continue;

        if (timer.interval > 0) {
          do {
            timer.deadline += timer.interval;
          } while (timer.deadline <= now);
          heapPush(timer);
        } else {
          timers.delete(timer.id);
        }

        try {
          invokeTimer(timer);
        } catch (error) {
          reportAsyncError(error);
        }
      }
    } finally {
      drainingTimers = false;
      for (var i = 0; i < deferredTimers.length; i++) {
        var timer = deferredTimers[i];
        if (timers.get(timer.id) === timer) heapPush(timer);
      }
      deferredTimers.length = 0;
      refreshTimerDeadline();
    }
  }

  function drainAnimationFrames(now) {
    if (rafQueue.length === 0) {
      if (cancelledRafs.size) cancelledRafs.clear();
      return;
    }

    var callbacks = rafQueue;
    rafQueue = rafDrainQueue;
    rafDrainQueue = callbacks;
    rafQueue.length = 0;

    for (var i = 0; i < callbacks.length; i++) {
      var entry = callbacks[i];
      var cancelled = entry.cancelled ||
        (cancelledRafs.size !== 0 && cancelledRafs.has(entry.id));
      if (!cancelled) {
        try {
          entry.callback(now);
        } catch (error) {
          reportAsyncError(error);
        }
      }
      if (cancelledRafs.size !== 0) cancelledRafs.delete(entry.id);
    }
    callbacks.length = 0;
    if (cancelledRafs.size) cancelledRafs.clear();
  }

  function pmjsDrainScheduler(now) {
    if (typeof now !== 'number') now = performance.now();
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
