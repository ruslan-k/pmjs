'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');

const source = fs.readFileSync(path.resolve(__dirname, '../js/pmjs-web/canvas.js'), 'utf8');

test('concentric radial gradient fill uses the native canvas fast path', () => {
  const calls = [];
  const context = {
    console,
    NativeHost: {
      runtime: { env: function() { return ''; } },
      canvas: {
        fillRadialGradient: function() { calls.push(Array.from(arguments)); }
      }
    }
  };
  vm.createContext(context);
  vm.runInContext(source, context);
  const canvas = {
    width: 100,
    height: 80,
    _ensureNativeCanvas: function() { return { handle: 7 }; }
  };
  const drawing = new context.CanvasContext2D(canvas);
  const gradient = drawing.createRadialGradient(50, 40, 0, 50, 40, 30);
  gradient.addColorStop(0, '#ffffff');
  gradient.addColorStop(1, 'rgba(0,0,0,0)');
  drawing.fillStyle = gradient;
  drawing.fillRect(20, 10, 60, 60);

  assert.equal(calls.length, 1);
  assert.deepEqual(calls[0].slice(0, 9), [7, 20, 10, 60, 60, 50, 40, 0, 30]);
  assert.deepEqual(Array.from(calls[0][9]), [0, 1]);
  assert.equal(calls[0][11], false);

  drawing.globalCompositeOperation = 'lighter';
  drawing.fillRect(20, 10, 60, 60);
  assert.equal(calls.length, 2);
  assert.equal(calls[1][11], true);
});

test('radial gradients reject negative radii and invalid stops', () => {
  const context = { console, NativeHost: { runtime: { env: function() { return ''; } } } };
  vm.createContext(context);
  vm.runInContext(source, context);
  const drawing = new context.CanvasContext2D({});
  assert.throws(
    () => drawing.createRadialGradient(0, 0, -1, 0, 0, 1),
    /negative radial gradient radius/,
  );
  const gradient = drawing.createRadialGradient(0, 0, 0, 0, 0, 1);
  assert.throws(() => gradient.addColorStop(2, '#fff'), /invalid color stop/);
});


test('axis-aligned lighter fill uses the native additive rectangle path', () => {
  const additive = [];
  const fallbackReads = [];
  const context = {
    console,
    NativeHost: {
      runtime: { env: function() { return ''; } },
      canvas: {
        fillRectAdditive: function() { additive.push(Array.from(arguments)); },
        readPixels: function() { fallbackReads.push(Array.from(arguments)); return new Uint8Array(16); },
        writePixels: function() {}
      }
    }
  };
  vm.createContext(context);
  vm.runInContext(source, context);
  const canvas = {
    width: 32,
    height: 32,
    _ensureNativeCanvas: function() { return { handle: 11 }; }
  };
  const drawing = new context.CanvasContext2D(canvas);
  drawing.globalCompositeOperation = 'lighter';
  drawing.globalAlpha = 0.5;
  drawing.fillStyle = '#804020';
  drawing.fillRect(2, 3, 8, 9);

  assert.equal(additive.length, 1);
  assert.deepEqual(additive[0].slice(0, 5), [11, 2, 3, 8, 9]);
  assert.equal(additive[0][5] & 0xff, 128);
  assert.equal(fallbackReads.length, 0);
});
