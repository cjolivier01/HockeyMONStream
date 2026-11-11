"use strict";
const assert = require("node:assert/strict");
const { Warp, fitBoundary, validate } = require("./warp-geometry.js");
const spec = {
  schema: 3,
  source_size: [401, 201],
  guides: [
    [0, 0.3, 0.5],
    [0.5, 0.45, 0.8],
    [1, 0.3, 0.5],
  ],
  target_rows: [0.55, 0.9],
  horizontal_crop: [0, 1],
  strength: 1,
  aspect_balance: 1,
};
for (const strength of [0, 0.05, 0.5, 1])
  for (const aspect_balance of [0, 0.4, 1]) {
    const w = new Warp({ ...spec, strength, aspect_balance });
    for (let i = 0; i <= 60; i++) {
      const x = i / 60;
      assert.ok(Math.abs(w.forwardX(w.inverseX(x)) - x) < 1e-10);
      let previous = -1;
      for (let j = 0; j <= 70; j++) {
        const y = j / 70,
          v = w.inverseY(x, y);
        assert.ok(v > previous);
        previous = v;
        assert.ok(Math.abs(w.forwardY(x, v) - y) < 1e-9);
        if (strength === 0) assert.ok(Math.abs(v - y) < 1e-12);
      }
    }
    if (strength === 0) assert.deepEqual(w.size, spec.source_size);
  }
const points = Array.from({ length: 250 }, (_, i) => {
  const x = i / 249;
  return [x, 0.85 - 0.35 * (2 * x - 1) ** 2 + (i % 17 === 0 ? -0.3 : 0)];
});
const fitted = fitBoundary(points, 4, true);
for (let i = 0; i <= 30; i++) {
  const x = i / 30;
  assert.ok(Math.abs(fitted(x) - (0.85 - 0.35 * (2 * x - 1) ** 2)) < 0.015);
  assert.ok(Math.abs(fitted(x) - fitted(1 - x)) < 1e-12);
}
for (const invalid of [
  { ...spec, strength: NaN },
  { ...spec, aspect_balance: 2 },
  { ...spec, target_rows: [0.95, 0.5] },
  {
    ...spec,
    guides: [
      [0, 0.4, 0.3],
      [1, 0.2, 0.5],
    ],
  },
])
  assert.throws(() => validate(invalid));
console.log(
  "Geometry: positive maps, identity, inverse consistency, robust fitting and validation passed.",
);
