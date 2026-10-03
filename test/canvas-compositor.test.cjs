'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');

const source = fs.readFileSync(
  path.resolve(__dirname, '../js/pmjs-web/canvas.js'), 'utf8');

function loadCanvas() {
  const context = {
    console,
    NativeHost: {
      runtime: { env() { return ''; } },
      canvas: {}
    }
  };
  context.globalThis = context;
  vm.createContext(context);
  vm.runInContext(source, context, { filename: 'canvas.js' });
  return context;
}

function clamp(value) {
  return Math.max(0, Math.min(255, Math.round(value)));
}

function referenceComposite(pixel, red, green, blue, sourceAlpha, operation) {
  const destinationAlpha = pixel[3] / 255;
  let outputAlpha;
  let output = [0, 0, 0];
  operation = operation || 'source-over';
  const sourceColors = [red, green, blue];

  if (operation === 'copy') {
    outputAlpha = sourceAlpha;
    output = sourceColors;
  } else if (operation === 'destination-in') {
    outputAlpha = destinationAlpha * sourceAlpha;
    output = [pixel[0], pixel[1], pixel[2]];
  } else if (operation === 'source-atop') {
    outputAlpha = destinationAlpha;
    for (let channel = 0; channel < 3; channel++) {
      output[channel] = destinationAlpha <= 0 ? 0 :
        sourceColors[channel] * sourceAlpha +
        pixel[channel] * (1 - sourceAlpha);
    }
  } else {
    outputAlpha = operation === 'lighter'
      ? Math.min(1, sourceAlpha + destinationAlpha)
      : sourceAlpha + destinationAlpha * (1 - sourceAlpha);
    for (let channel = 0; channel < 3; channel++) {
      const source = sourceColors[channel];
      const destination = pixel[channel];
      let blended = source;
      if (operation === 'difference') blended = Math.abs(destination - source);
      else if (operation === 'saturation') {
        blended = pixel[0] * 0.299 + pixel[1] * 0.587 + pixel[2] * 0.114;
      }
      const premultiplied = operation === 'lighter'
        ? source * sourceAlpha + destination * destinationAlpha
        : blended * sourceAlpha * destinationAlpha +
          source * sourceAlpha * (1 - destinationAlpha) +
          destination * destinationAlpha * (1 - sourceAlpha);
      output[channel] = outputAlpha <= 0 ? 0 : premultiplied / outputAlpha;
    }
  }

  return [
    clamp(output[0]),
    clamp(output[1]),
    clamp(output[2]),
    clamp(outputAlpha * 255)
  ];
}

test('optimized pixel compositor matches the reference implementation', () => {
  const context = loadCanvas();
  assert.equal(typeof context.compositeCanvasPixel, 'function');

  const operations = [
    'source-over', 'copy', 'destination-in', 'source-atop',
    'lighter', 'difference', 'saturation'
  ];

  // Deterministic LCG so failures are reproducible in CI.
  let state = 0x51f15e5d;
  function random() {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 0x100000000;
  }

  for (const operation of operations) {
    for (let index = 0; index < 20000; index++) {
      const pixel = [
        Math.floor(random() * 256),
        Math.floor(random() * 256),
        Math.floor(random() * 256),
        Math.floor(random() * 256)
      ];
      const red = Math.floor(random() * 256);
      const green = Math.floor(random() * 256);
      const blue = Math.floor(random() * 256);
      const alpha = random();
      const expected = referenceComposite(
        pixel.slice(), red, green, blue, alpha, operation);
      const actual = Uint8Array.from(pixel);
      context.compositeCanvasPixel(
        actual, 0, red, green, blue, alpha, operation);
      assert.deepEqual(Array.from(actual), expected,
        operation + ' sample ' + index);
    }
  }
});
