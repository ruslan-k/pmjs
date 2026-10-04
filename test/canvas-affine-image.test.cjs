'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');

const source = fs.readFileSync(
  path.resolve(__dirname, '../js/pmjs-web/canvas.js'), 'utf8');

function legacySample(width, height, sourceWidth, sourceHeight, sourcePixels,
    transform, sx, sy, sw, sh, dx, dy, dw, dh) {
  const t = transform;
  const determinant = t[0] * t[3] - t[1] * t[2];
  const x0 = t[0] * dx + t[2] * dy + t[4];
  const y0 = t[1] * dx + t[3] * dy + t[5];
  const x1 = t[0] * (dx + dw) + t[2] * dy + t[4];
  const y1 = t[1] * (dx + dw) + t[3] * dy + t[5];
  const x2 = t[0] * (dx + dw) + t[2] * (dy + dh) + t[4];
  const y2 = t[1] * (dx + dw) + t[3] * (dy + dh) + t[5];
  const x3 = t[0] * dx + t[2] * (dy + dh) + t[4];
  const y3 = t[1] * dx + t[3] * (dy + dh) + t[5];
  const left = Math.max(0, Math.floor(Math.min(x0, x1, x2, x3)));
  const top = Math.max(0, Math.floor(Math.min(y0, y1, y2, y3)));
  const right = Math.min(width, Math.ceil(Math.max(x0, x1, x2, x3)));
  const bottom = Math.min(height, Math.ceil(Math.max(y0, y1, y2, y3)));
  const output = new Uint8Array(Math.max(0, right - left) *
    Math.max(0, bottom - top) * 4);
  if (right <= left || bottom <= top) return { left, top, right, bottom, output };

  const inverseA = t[3] / determinant;
  const inverseB = -t[1] / determinant;
  const inverseC = -t[2] / determinant;
  const inverseD = t[0] / determinant;
  for (let y = top; y < bottom; y++) {
    for (let x = left; x < right; x++) {
      const shiftedX = x + 0.5 - t[4];
      const shiftedY = y + 0.5 - t[5];
      const localX = inverseA * shiftedX + inverseC * shiftedY;
      const localY = inverseB * shiftedX + inverseD * shiftedY;
      const u = (localX - dx) / dw;
      const v = (localY - dy) / dh;
      if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
      const sampleX = Math.max(0, Math.min(sourceWidth - 1,
        Math.floor(sx + u * sw)));
      const sampleY = Math.max(0, Math.min(sourceHeight - 1,
        Math.floor(sy + v * sh)));
      const so = (sampleY * sourceWidth + sampleX) * 4;
      const oo = ((y - top) * (right - left) + x - left) * 4;
      output[oo] = sourcePixels[so];
      output[oo + 1] = sourcePixels[so + 1];
      output[oo + 2] = sourcePixels[so + 2];
      output[oo + 3] = sourcePixels[so + 3];
    }
  }
  return { left, top, right, bottom, output };
}

test('incremental affine image sampling matches legacy per-pixel inverse mapping', () => {
  let written = null;
  const sourceWidth = 17;
  const sourceHeight = 13;
  const sourcePixels = new Uint8Array(sourceWidth * sourceHeight * 4);
  for (let y = 0; y < sourceHeight; y++) {
    for (let x = 0; x < sourceWidth; x++) {
      const o = (y * sourceWidth + x) * 4;
      sourcePixels[o] = x * 11;
      sourcePixels[o + 1] = y * 17;
      sourcePixels[o + 2] = (x * 7 + y * 13) & 255;
      sourcePixels[o + 3] = 255;
    }
  }

  const context = {
    console,
    NativeHost: {
      runtime: { env() { return ''; } },
      canvas: {
        readPixels(handle, x, y, width, height) {
          if (handle === 2) {
            const pixels = new Uint8Array(width * height * 4);
            for (let row = 0; row < height; row++) {
              const sourceOffset = ((y + row) * sourceWidth + x) * 4;
              pixels.set(sourcePixels.subarray(
                sourceOffset, sourceOffset + width * 4), row * width * 4);
            }
            return pixels;
          }
          return new Uint8Array(width * height * 4);
        },
        writePixels(_handle, left, top, width, height, pixels) {
          written = { left, top, width, height, pixels: Uint8Array.from(pixels) };
        }
      }
    }
  };
  context.globalThis = context;
  vm.createContext(context);
  vm.runInContext(source, context, { filename: 'canvas.js' });

  let state = 0xa11f1e42;
  function random() {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 0x100000000;
  }

  const destWidth = 64;
  const destHeight = 56;
  const sourceObject = {
    width: sourceWidth,
    height: sourceHeight,
    _nativeCanvas: { handle: 2 }
  };
  for (let sample = 0; sample < 400; sample++) {
    const angle = (random() - 0.5) * Math.PI * 1.8;
    const scaleX = (0.5 + random() * 1.8) * (random() < 0.3 ? -1 : 1);
    const scaleY = (0.5 + random() * 1.8) * (random() < 0.2 ? -1 : 1);
    const shear = (random() - 0.5) * 0.35;
    const cos = Math.cos(angle);
    const sin = Math.sin(angle);
    const transform = [
      cos * scaleX,
      sin * scaleX,
      -sin * scaleY + shear,
      cos * scaleY,
      12 + random() * 36,
      10 + random() * 30
    ];
    const determinant = transform[0] * transform[3] -
      transform[1] * transform[2];
    if (Math.abs(determinant) < 0.05) continue;

    const sx = Math.floor(random() * 4);
    const sy = Math.floor(random() * 3);
    const sw = 6 + Math.floor(random() * (sourceWidth - sx - 5));
    const sh = 5 + Math.floor(random() * (sourceHeight - sy - 4));
    const dx = -3 + random() * 6;
    const dy = -3 + random() * 6;
    const dw = (8 + random() * 20) * (random() < 0.2 ? -1 : 1);
    const dh = (8 + random() * 18) * (random() < 0.2 ? -1 : 1);

    const expected = legacySample(destWidth, destHeight,
      sourceWidth, sourceHeight, sourcePixels, transform,
      sx, sy, sw, sh, dx, dy, dw, dh);

    written = null;
    const drawing = {
      canvas: {
        width: destWidth,
        height: destHeight,
        _ensureNativeCanvas() { return { handle: 1 }; }
      },
      _transform: transform.slice(),
      _clipPaths: [],
      globalAlpha: 1,
      globalCompositeOperation: 'copy'
    };
    context.drawAffineImage(drawing, sourceObject, sourceObject._nativeCanvas,
      sx, sy, sw, sh, dx, dy, dw, dh, 0);

    if (expected.right <= expected.left || expected.bottom <= expected.top) {
      assert.equal(written, null);
      continue;
    }
    assert.ok(written, 'sample ' + sample + ' did not write');
    assert.equal(written.left, expected.left);
    assert.equal(written.top, expected.top);
    assert.equal(written.width, expected.right - expected.left);
    assert.equal(written.height, expected.bottom - expected.top);
    assert.deepEqual(Array.from(written.pixels), Array.from(expected.output),
      'sample ' + sample);
  }
});
