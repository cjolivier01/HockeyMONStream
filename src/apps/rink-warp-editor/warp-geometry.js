/* Shared still-image warp geometry. No playback or calibration mutations. */
(function (root) {
  "use strict";
  const EPS = 1e-4;
  const clamp = (x, a, b) => Math.max(a, Math.min(b, x));
  function finite(x) {
    return typeof x === "number" && Number.isFinite(x);
  }
  function search(a, x) {
    let lo = 0,
      hi = a.length - 1;
    while (lo + 1 < hi) {
      const m = (lo + hi) >> 1;
      if (a[m] <= x) lo = m;
      else hi = m;
    }
    return lo;
  }
  function interp(xs, ys, x) {
    const i = search(xs, x),
      t = clamp((x - xs[i]) / (xs[i + 1] - xs[i]), 0, 1);
    return ys[i] * (1 - t) + ys[i + 1] * t;
  }
  class Curve {
    constructor(x, y) {
      this.x = x;
      this.y = y;
      this.d = Array(y.length).fill(0);
      const h = x.slice(1).map((v, i) => v - x[i]);
      const s = y.slice(1).map((v, i) => (v - y[i]) / h[i]);
      if (x.length === 2) {
        this.d.fill(s[0]);
        return;
      }
      for (let i = 1; i < x.length - 1; i++) {
        if (s[i - 1] * s[i] > 0) {
          const w1 = 2 * h[i] + h[i - 1],
            w2 = h[i] + 2 * h[i - 1];
          this.d[i] = (w1 + w2) / (w1 / s[i - 1] + w2 / s[i]);
        }
      }
      for (const [i, j, k] of [
        [0, 0, 1],
        [y.length - 1, h.length - 1, h.length - 2],
      ]) {
        let d = ((2 * h[j] + h[k]) * s[j] - h[j] * s[k]) / (h[j] + h[k]);
        if (d * s[j] <= 0) d = 0;
        else if (s[j] * s[k] <= 0 && Math.abs(d) > 3 * Math.abs(s[j]))
          d = 3 * s[j];
        this.d[i] = d;
      }
    }
    at(x) {
      const i = search(this.x, x),
        h = this.x[i + 1] - this.x[i];
      const t = (x - this.x[i]) / h,
        t2 = t * t,
        t3 = t2 * t;
      return (
        (2 * t3 - 3 * t2 + 1) * this.y[i] +
        (t3 - 2 * t2 + t) * h * this.d[i] +
        (-2 * t3 + 3 * t2) * this.y[i + 1] +
        (t3 - t2) * h * this.d[i + 1]
      );
    }
  }
  function interval(z, x0, x1, y0, y1, d0, d1) {
    const slope = (y1 - y0) / (x1 - x0),
      t = clamp((z - x0) / (x1 - x0), 0, 1),
      p = t * (1 - t);
    return (
      y0 +
      ((y1 - y0) * (slope * t * t + d0 * p)) /
        (slope + (d0 + d1 - 2 * slope) * p)
    );
  }
  function validate(s) {
    const keys = [
      "schema",
      "source_size",
      "guides",
      "target_rows",
      "horizontal_crop",
    ];
    if (s?.schema === 3) keys.push("strength", "aspect_balance");
    if (
      !s ||
      ![2, 3].includes(s.schema) ||
      Object.keys(s).sort().join() !== keys.sort().join()
    )
      throw Error("Use a schema 2 or 3 warp guide document.");
    if (
      !Array.isArray(s.source_size) ||
      s.source_size.length !== 2 ||
      s.source_size.some((v) => !Number.isInteger(v) || v < 2 || v >= 32767)
    )
      throw Error("Image dimensions must be between 2 and 32766 pixels.");
    if (
      !Array.isArray(s.guides) ||
      s.guides.length < 2 ||
      s.guides.length > 1024
    )
      throw Error("Use 2 to 1024 guide points.");
    s.guides.forEach((r, i) => {
      if (
        !Array.isArray(r) ||
        r.length !== 3 ||
        !r.every(finite) ||
        r[0] < 0 ||
        r[0] > 1 ||
        r[1] < EPS ||
        r[1] > r[2] - EPS ||
        r[2] > 1 - EPS ||
        (i && r[0] - s.guides[i - 1][0] < EPS)
      )
        throw Error(
          "Guide points must stay ordered and the upper guide must stay above the lower guide.",
        );
    });
    if (s.guides[0][0] !== 0 || s.guides.at(-1)[0] !== 1)
      throw Error("Guides must reach both image edges.");
    for (const name of ["target_rows", "horizontal_crop"])
      if (
        !Array.isArray(s[name]) ||
        s[name].length !== 2 ||
        !s[name].every(finite)
      )
        throw Error("Invalid target or crop coordinates.");
    const [t, b] = s.target_rows,
      [l, r] = s.horizontal_crop;
    if (t < EPS || t > b - EPS || b > 1 - EPS)
      throw Error("Target rows must remain ordered inside the image.");
    if (s.schema === 2 && s.guides.some((p) => p[2] > b))
      throw Error(
        "Schema 2 requires the lower target below every lower guide.",
      );
    if (l < 0 || l > r - EPS || r > 1)
      throw Error("Crop edges must stay ordered inside the image.");
    if (
      s.schema === 3 &&
      [s.strength, s.aspect_balance].some((v) => !finite(v) || v < 0 || v > 1)
    )
      throw Error("Strength and proportion balance must be between 0 and 1.");
  }
  class Warp {
    constructor(spec) {
      validate(spec);
      this.spec = structuredClone(spec);
      this.strength = spec.schema === 2 ? 1 : spec.strength;
      this.balance = spec.schema === 2 ? 1 : spec.aspect_balance;
      const x = spec.guides.map((r) => r[0]),
        lower = spec.guides.map((r) => r[2]);
      this.lower = new Curve(x, lower);
      this.ratio = new Curve(
        x,
        spec.guides.map((r) => r[1] / r[2]),
      );
      const [left, right] = spec.horizontal_crop,
        n = spec.source_size[0];
      this.x = new Float64Array(n);
      this.u = new Float64Array(n);
      let lastScale = 0;
      for (let i = 0; i < n; i++) {
        const sx = left + ((right - left) * i) / (n - 1);
        this.x[i] = sx;
        const [up, low, top, bottom] = this.column(sx);
        const scale = 1 + this.balance * ((bottom - top) / (low - up) - 1);
        if (i)
          this.u[i] =
            this.u[i - 1] + ((lastScale + scale) * (sx - this.x[i - 1])) / 2;
        lastScale = scale;
      }
      const total = this.u[n - 1];
      // Match the offline renderer's ties-to-even rounding, including schema 2.
      const span = total * (n - 1),
        base = Math.floor(span);
      const rounded =
        span - base === 0.5 ? base + (base % 2) : Math.round(span);
      this.size = [1 + rounded, spec.source_size[1]];
      if (this.size[0] < 2 || this.size[0] >= 32767)
        throw Error(
          "Output would be too wide. Reduce strength or proportion balance, or widen the guide spacing.",
        );
      for (let i = 0; i < n; i++) this.u[i] /= total;
    }
    column(x) {
      const low = this.lower.at(x),
        up = low * this.ratio.at(x),
        a = this.strength;
      return [
        up,
        low,
        up * (1 - a) + this.spec.target_rows[0] * a,
        low * (1 - a) + this.spec.target_rows[1] * a,
      ];
    }
    inverseX(x) {
      return interp(this.u, this.x, x);
    }
    forwardX(x) {
      const [left, right] = this.spec.horizontal_crop;
      if (x < left) return (x - left) / (right - left);
      if (x > right) return 1 + (x - right) / (right - left);
      return interp(this.x, this.u, x);
    }
    inverseY(x, y) {
      const [up, low, top, bottom] = this.column(x);
      const first = up / top,
        middle = (low - up) / (bottom - top),
        shared = (2 * first * middle) / (first + middle);
      const end = Math.min(1, low + (1 - bottom) * middle);
      if (y < top) return interval(y, 0, top, 0, up, first, shared);
      if (y < bottom) return interval(y, top, bottom, up, low, shared, middle);
      return interval(y, bottom, 1, low, end, middle, middle);
    }
    forwardY(x, y) {
      if (y < 0 || y > this.inverseY(x, 1)) return y < 0 ? -1 : 2;
      let lo = 0,
        hi = 1;
      for (let i = 0; i < 36; i++) {
        const m = (lo + hi) / 2;
        if (this.inverseY(x, m) < y) lo = m;
        else hi = m;
      }
      return (lo + hi) / 2;
    }
    lut(n) {
      const data = new Float32Array(n * 4);
      for (let i = 0; i < n; i++) {
        const x = this.inverseX(i / (n - 1));
        const [up, low, top] = this.column(x);
        data.set([x, up, low, top], i * 4);
      }
      return data;
    }
  }
  function median(values) {
    const a = [...values].sort((a, b) => a - b);
    return a[Math.floor(a.length / 2)];
  }
  function solve(a, b) {
    const n = b.length,
      rows = a.map((r, i) => [...r, b[i]]);
    for (let k = 0; k < n; k++) {
      let p = k;
      for (let i = k + 1; i < n; i++)
        if (Math.abs(rows[i][k]) > Math.abs(rows[p][k])) p = i;
      [rows[p], rows[k]] = [rows[k], rows[p]];
      if (Math.abs(rows[k][k]) < 1e-12)
        throw Error(
          "Not enough well-spaced ice boundary samples to fit a curve. Draw the guide manually.",
        );
      const divisor = rows[k][k];
      for (let j = k; j <= n; j++) rows[k][j] /= divisor;
      for (let i = 0; i < n; i++)
        if (i !== k) {
          const f = rows[i][k];
          for (let j = k; j <= n; j++) rows[i][j] -= f * rows[k][j];
        }
    }
    return rows.map((r) => r[n]);
  }
  // Broad polynomial fit with robust reweighting; obstacles do not become knots.
  function fitBoundary(points, degree, symmetric) {
    if (points.length < 12)
      throw Error(
        "The mask has too few usable ice columns. Draw a lower guide instead.",
      );
    const powers = Array.from({ length: degree + 1 }, (_, i) => i).filter(
      (i) => !symmetric || i % 2 === 0,
    );
    const basis = points.map(([x]) => powers.map((p) => (2 * x - 1) ** p));
    let weights = points.map(() => 1),
      coeff;
    for (let iteration = 0; iteration < 8; iteration++) {
      const a = powers.map(() => powers.map(() => 0)),
        b = powers.map(() => 0);
      basis.forEach((row, k) =>
        row.forEach((v, i) => {
          b[i] += weights[k] * v * points[k][1];
          row.forEach((w, j) => {
            a[i][j] += weights[k] * v * w;
          });
        }),
      );
      coeff = solve(a, b);
      const residuals = basis.map(
        (row, k) => points[k][1] - row.reduce((s, v, i) => s + v * coeff[i], 0),
      );
      const center = median(residuals),
        scale = Math.max(
          1e-4,
          1.4826 * median(residuals.map((v) => Math.abs(v - center))),
        );
      weights = residuals.map((r) =>
        Math.max(
          0.02,
          Math.abs(r - center) < 4.685 * scale
            ? (1 - ((r - center) / (4.685 * scale)) ** 2) ** 2
            : 0.02,
        ),
      );
    }
    return (x) =>
      clamp(
        powers.reduce((s, p, i) => s + coeff[i] * (2 * x - 1) ** p, 0),
        EPS * 3,
        1 - EPS,
      );
  }
  root.RinkWarpGeometry = {
    Warp,
    Curve,
    interval,
    fitBoundary,
    validate,
    clamp,
    EPS,
  };
  if (typeof module !== "undefined") module.exports = root.RinkWarpGeometry;
})(globalThis);
