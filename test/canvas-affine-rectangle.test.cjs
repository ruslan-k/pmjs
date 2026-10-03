'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');

const source = fs.readFileSync(
  path.resolve(__dirname, '../js/pmjs-web/canvas.js'), 'utf8');

function oldCoverage(width, height, transform, x, y, rectWidth, rectHeight) {
  const t = transform;
  const x0 = t[0] * x + t[2] * y + t[4];
  const y0 = t[1] * x + t[3] * y + t[5];
  const x1 = t[0] * (x + rectWidth) + t[2] * y + t[4];
  const y1 = t[1] * (x + rectWidth) + t[3] * y + t[5];
  const x2 = t[0] * (x + rectWidth) + t[2] * (y + rectHeight) + t[4];
  const y2 = t[1] * (x + rectWidth) + t[3] * (y + rectHeight) + t[5];
  const x3 = t[0] * x + t[2] * (y + rectHeight) + t[4];
  const y3 = t[1] * x + t[3] * (y + rectHeight) + t[5];
  const points = [x0, y0, x1, y1, x2, y2, x3, y3];
  const left = Math.max(0, Math.floor(Math.min(x0, x1, x2, x3)));
  const top = Math.max(0, Math.floor(Math.min(y0, y1, y2, y3)));
  const right = Math.min(width, Math.ceil(Math.max(x0, x1, x2, x3)));
  const bottom = Math.min(height, Math.ceil(Math.max(y0, y1, y2, y3)));
  const covered = new Set();
  if (right <= left || bottom <= top) return covered;

  for (let targetY = top; targetY < bottom; targetY++) {
    for (let targetX = left; targetX < right; targetX++) {
      let inside = true;
      let sign = 0;
      for (let edge = 0; edge < 4; edge++) {
        const firstOffset = edge * 2;
        const secondOffset = ((edge + 1) & 3) * 2;
        const firstX = points[firstOffset];
        const firstY = points[firstOffset + 1];
        const secondX = points[secondOffset];
        const secondY = points[secondOffset + 1];
        const cross = (secondX - firstX) * (targetY + 0.5 - firstY) -
          (secondY - firstY) * (targetX + 0.5 - firstX);
        if (Math.abs(cross) < 0.000001) continue;
        const currentSign = cross < 0 ? -1 : 1;
        if (sign && sign !== currentSign) {
          inside = false;
          break;
        }
        sign = currentSign;
      }
      if (inside) covered.add(targetY * width + targetX);
    }
  }
  return covered;
}

test('inverse-mapped affine rectangle matches legacy edge coverage', () => {
  let write = null;
  const context = {
    console,
    NativeHost: {
      runtime: { env() { return ''; } },
      canvas: {
        readPixels(_handle, _left, _top, width, height) {
          return new Uint8Array(width * height * 4).fill(255);
        },
        writePixels(_handle, left, top, width, height, pixels) {
          write = { left, top, width, height, pixels: Uint8Array.from(pixels) };
        }
      }
    }
  };
  context.globalThis = context;
  vm.createContext(context);
  vm.runInContext(source, context, { filename: 'canvas.js' });

  let state = 0x15a4e35d;
  function random() {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 0x100000000;
  }

  const canvasWidth = 48;
  const canvasHeight = 40;
  for (let sample = 0; sample < 500; sample++) {
    const angle = (random() - 0.5) * Math.PI * 1.8;
    const scaleX = (0.4 + random() * 1.6) * (random() < 0.2 ? -1 : 1);
    const scaleY = 0.4 + random() * 1.6;
    const shear = (random() - 0.5) * 0.5;
    const cos = Math.cos(angle);
    const sin = Math.sin(angle);
    const transform = [
      cos * scaleX,
      sin * scaleX,
      -sin * scaleY + shear,
      cos * scaleY,
      8 + random() * 30,
      6 + random() * 26
    ];
    const determinant = transform[0] * transform[3] -
      transform[1] * transform[2];
    if (Math.abs(determinant) < 0.05) continue;

    const x = (random() - 0.5) * 8;
    const y = (random() - 0.5) * 8;
    const rectWidth = (3 + random() * 14) * (random() < 0.2 ? -1 : 1);
    const rectHeight = (3 + random() * 12) * (random() < 0.2 ? -1 : 1);
    const expected = oldCoverage(
      canvasWidth, canvasHeight, transform, x, y, rectWidth, rectHeight);

    write = null;
    const drawing = {
      canvas: {
        width: canvasWidth,
        height: canvasHeight,
        _ensureNativeCanvas() { return { handle: 7 }; }
      },
      _transform: transform.slice(),
      _clipPaths: [],
      globalCompositeOperation: 'source-over',
      globalAlpha: 1
    };
    context.paintAffineRectangle(
      drawing, x, y, rectWidth, rectHeight, 0xffffffff, true);

    const actual = new Set();
    if (write) {
      for (let py = 0; py < write.height; py++) {
        for (let px = 0; px < write.width; px++) {
          const offset = (py * write.width + px) * 4;
          if (write.pixels[offset + 3] === 0) {
            actual.add((write.top + py) * canvasWidth + write.left + px);
          }
        }
      }
    }
    assert.deepEqual(
      Array.from(actual).sort((a, b) => a - b),
      Array.from(expected).sort((a, b) => a - b),
      'sample ' + sample);
  }
});
