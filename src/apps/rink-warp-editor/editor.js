(function () {
  "use strict";
  const G = RinkWarpGeometry,
    $ = (s) => document.querySelector(s),
    $$ = (s) => [...document.querySelectorAll(s)];
  const boot = JSON.parse($("#warp-bootstrap").textContent),
    defaults = boot.defaults;
  const status = (message, error = false) => {
    $("#warp-status").textContent = message;
    $("#warp-status").classList.toggle("warp-error", error);
  };
  if (boot.notice) {
    $("#warp-notice").textContent = boot.notice;
    $("#warp-notice").hidden = false;
  }
  const input = (name) => $(`[name="${name}"]`),
    clone = (obj) => structuredClone(obj);
  const loadImage = (url) =>
    new Promise((resolve, reject) => {
      const im = new Image();
      im.onload = () => resolve(im);
      im.onerror = () =>
        reject(
          Error("The image could not be decoded. Use a PNG or JPEG still."),
        );
      im.src = url;
    });
  const dataURL = (blob) =>
    new Promise((resolve, reject) => {
      const r = new FileReader();
      r.onload = () => resolve(r.result);
      r.onerror = () => reject(Error("Could not read the selected file."));
      r.readAsDataURL(blob);
    });
  function download(blob, name) {
    const a = document.createElement("a"),
      url = URL.createObjectURL(blob);
    a.href = url;
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
  }
  let image = null,
    spec = null,
    warp = null,
    initial = null,
    mask = null,
    boundary = [],
    maskExtents = [];
  let undo = [],
    redo = [],
    selected = null,
    drawing = false,
    stroke = [],
    dragging = null,
    scheduled = false,
    imageTicket = 0,
    fileRequest = 0,
    exporting = false;
  const panes = [];
  try {
    if (
      !defaults ||
      !Array.isArray(defaults.target_rows) ||
      !Array.isArray(defaults.horizontal_crop) ||
      !Number.isInteger(defaults.guide_count) ||
      defaults.guide_count < 3 ||
      defaults.guide_count > 129 ||
      !["upper", "lower"].includes(defaults.active_guide) ||
      ![2, 4, 6].includes(defaults.fit_degree) ||
      !Number.isInteger(defaults.preview_max_dimension) ||
      defaults.preview_max_dimension < 256 ||
      defaults.preview_max_dimension > 16384 ||
      !["symmetric", "show_mask", "show_guides"].every(
        (k) => typeof defaults[k] === "boolean",
      )
    )
      throw Error(
        "Rink warp defaults are missing or invalid. Rebuild against the current baseline.",
      );
    for (const name of ["source", "result"]) {
      const element = $(`[data-pane="${name}"]`),
        canvas = element.querySelector(".warp-image"),
        overlay = element.querySelector(".warp-overlay");
      const renderer = new RinkWarpRenderer(canvas);
      const pane = {
        name,
        element,
        canvas,
        overlay,
        renderer,
        zoom: 1,
        center: [0.5, 0.5],
        rect: [0, 0, 1, 1],
      };
      panes.push(pane);
      canvas.addEventListener("webglcontextlost", (event) => {
        event.preventDefault();
        status(
          "The GPU context was lost. Save your guides, then reopen this editor.",
          true,
        );
      });
      canvas.addEventListener("webglcontextrestored", () =>
        status(
          "Reload this editor to restore the GPU preview. Saved guides can be reloaded.",
          true,
        ),
      );
    }
    $("#warp-device").textContent = panes[1].renderer.device;
    $("#warp-device").title = panes[1].renderer.device;
    input("symmetric").checked = defaults.symmetric;
    input("show_guides").checked = defaults.show_guides;
    input("show_mask").checked = defaults.show_mask;
    input("degree").value = String(defaults.fit_degree);
    input("guide").value = defaults.active_guide === "upper" ? "1" : "2";
  } catch (error) {
    status(error.message, true);
    $$("button").forEach((b) => (b.disabled = true));
    return;
  }

  function controls() {
    if (!spec) return;
    for (const key of ["strength", "aspect_balance"]) {
      input(key).value = spec[key];
      $(`[data-value="${key}"]`).textContent =
        `${Math.round(spec[key] * 100)}%`;
    }
    for (const [name, value] of [
      ["top", spec.target_rows[0]],
      ["bottom", spec.target_rows[1]],
      ["left", spec.horizontal_crop[0]],
      ["right", spec.horizontal_crop[1]],
    ])
      input(name).value = +(value * 100).toFixed(3);
    $('[data-action="undo"]').disabled = !undo.length;
    $('[data-action="redo"]').disabled = !redo.length;
    $('[data-action="fit-mask"]').disabled = !boundary.length;
    $('[data-action="delete-point"]').disabled =
      selected === null ||
      selected === 0 ||
      selected === spec.guides.length - 1;
    $("#source-size").textContent = `${spec.source_size.join(" × ")}`;
  }
  function remember() {
    if (spec) {
      undo.push(clone(spec));
      if (undo.length > 100) undo.shift();
      redo = [];
    }
  }
  function apply(candidate, history = true) {
    try {
      const next = new G.Warp(candidate);
      if (history && JSON.stringify(candidate) !== JSON.stringify(spec))
        remember();
      spec = clone(candidate);
      warp = next;
      for (const pane of panes) pane.renderer.setWarp(warp);
      controls();
      schedule();
      return true;
    } catch (error) {
      status(error.message, true);
      controls();
      return false;
    }
  }
  function newSpec(size) {
    return {
      schema: 3,
      source_size: size,
      guides: Array.from({ length: defaults.guide_count }, (_, i) => [
        i / (defaults.guide_count - 1),
        ...defaults.target_rows,
      ]),
      target_rows: [...defaults.target_rows],
      horizontal_crop: [...defaults.horizontal_crop],
      strength: defaults.strength,
      aspect_balance: defaults.aspect_balance,
    };
  }
  function normalizeImport(document, size = spec.source_size) {
    G.validate(document);
    if (document.source_size.some((v, i) => v !== size[i]))
      throw Error(
        "These guides belong to a different image size. Open their source image first.",
      );
    return document.schema === 2
      ? { ...document, schema: 3, strength: 1, aspect_balance: 1 }
      : document;
  }
  async function setImage(
    url,
    title,
    guides = null,
    maskURL = null,
    request = fileRequest,
  ) {
    const ticket = ++imageTicket;
    status("Loading still image…");
    const loaded = await loadImage(url);
    if (ticket !== imageTicket || request !== fileRequest) return;
    const size = [loaded.naturalWidth, loaded.naturalHeight],
      candidate = guides ? normalizeImport(guides, size) : newSpec(size);
    new G.Warp(candidate);
    if (size[0] * size[1] > 96 * 1024 * 1024)
      throw Error("Choose a still below 96 megapixels.");
    for (const pane of panes)
      pane.renderer.setImage(loaded, defaults.preview_max_dimension);
    image = loaded;
    mask = null;
    boundary = [];
    maskExtents = [];
    selected = null;
    undo = [];
    redo = [];
    dragging = null;
    drawing = false;
    stroke = [];
    $("[data-action=draw]").setAttribute("aria-pressed", "false");
    if (!apply(candidate, false)) return;
    initial = clone(spec);
    $("#warp-title").textContent = title || "Panorama";
    if (maskURL) {
      try {
        await setMask(maskURL, ticket, !guides, request);
      } catch (error) {
        status(error.message, true);
      }
    }
    if (ticket !== imageTicket || request !== fileRequest) return;
    initial = clone(spec);
    fitViews();
    controls();
  }
  async function setMask(
    url,
    ticket = imageTicket,
    auto = false,
    request = fileRequest,
  ) {
    if (!spec) throw Error("Open a panorama before its mask.");
    const loaded = await loadImage(url);
    if (ticket !== imageTicket || request !== fileRequest) return;
    if (
      loaded.naturalWidth !== spec.source_size[0] ||
      loaded.naturalHeight !== spec.source_size[1]
    )
      throw Error(
        "The ice mask must have exactly the same dimensions as the source image.",
      );
    const scale = Math.min(
        1,
        2048 / loaded.naturalWidth,
        2048 / loaded.naturalHeight,
      ),
      c = document.createElement("canvas");
    c.width = Math.max(2, Math.round(loaded.naturalWidth * scale));
    c.height = Math.max(2, Math.round(loaded.naturalHeight * scale));
    const ctx = c.getContext("2d", { willReadFrequently: true });
    ctx.drawImage(loaded, 0, 0, c.width, c.height);
    const pixels = ctx.getImageData(0, 0, c.width, c.height).data,
      edge = [],
      extents = [];
    for (let x = 0; x < c.width; x++) {
      let top = -1,
        bottom = -1,
        count = 0;
      for (let y = 0; y < c.height; y++) {
        const i = (y * c.width + x) * 4;
        if (
          pixels[i + 3] > 127 &&
          Math.max(pixels[i], pixels[i + 1], pixels[i + 2]) > 127
        ) {
          if (top < 0) top = y;
          bottom = y;
          count++;
        }
      }
      if (top >= 0) {
        const sx = x / (c.width - 1);
        extents.push([sx, top / (c.height - 1), bottom / (c.height - 1)]);
        if (count >= Math.max(3, c.height * 0.02) && bottom < c.height - 1)
          edge.push([sx, bottom / (c.height - 1)]);
      }
    }
    mask = loaded;
    boundary = edge;
    maskExtents = extents;
    if (auto && edge.length >= 12) fitMask(false, true);
    else {
      controls();
      schedule();
    }
  }
  function fitMask(history = true, initialize = false) {
    try {
      const fit = G.fitBoundary(
          boundary,
          +input("degree").value,
          input("symmetric").checked,
        ),
        next = clone(spec);
      const ratio = defaults.target_rows[0] / defaults.target_rows[1];
      next.guides = Array.from({ length: defaults.guide_count }, (_, i) => {
        const x = i / (defaults.guide_count - 1),
          low = fit(x);
        const up = initialize
          ? low * ratio
          : Math.min(warp.column(x)[0], low - G.EPS * 2);
        return [x, Math.max(G.EPS, up), low];
      });
      if (initialize) {
        // Pick the lowest common target that retains the sampled mask ends.
        // Below the lower guide, source y is low+(1-bottom)*middle.
        // Solving that inequality avoids a jagged envelope or repeated remaps.
        const candidate = new G.Warp(next),
          top = next.target_rows[0];
        let bottom = next.target_rows[1];
        for (const [x, , edge] of maskExtents) {
          const [up, low] = candidate.column(x),
            needed = Math.min(1, edge + 2 / (next.source_size[1] - 1));
          if (needed > low)
            bottom = Math.min(
              bottom,
              (low - up + (needed - low) * top) / (needed - up),
            );
        }
        next.target_rows[1] = Math.max(top + G.EPS * 2, bottom - G.EPS);
      }
      selected = null;
      apply(next, history);
    } catch (error) {
      status(error.message, true);
    }
  }
  function smooth(points = null) {
    try {
      const rail = +input("guide").value,
        data = points || spec.guides.map((p) => [p[0], p[rail]]);
      // Densify hand-edited knots for the same broad robust fit.
      const curve = points
        ? null
        : new G.Curve(
            data.map((p) => p[0]),
            data.map((p) => p[1]),
          );
      const samples =
        points ||
        Array.from({ length: 129 }, (_, i) => [i / 128, curve.at(i / 128)]);
      const fit = G.fitBoundary(
          samples,
          +input("degree").value,
          input("symmetric").checked,
        ),
        next = clone(spec);
      next.guides = Array.from({ length: defaults.guide_count }, (_, i) => {
        const x = i / (defaults.guide_count - 1),
          [up, low] = warp.column(x);
        return [x, up, low];
      });
      for (const row of next.guides)
        row[rail] = G.clamp(
          fit(row[0]),
          rail === 1 ? G.EPS : row[1] + G.EPS * 2,
          rail === 1 ? row[2] - G.EPS * 2 : 1 - G.EPS,
        );
      selected = null;
      apply(next);
    } catch (error) {
      status(error.message, true);
    }
  }
  function fitViews() {
    for (const pane of panes) {
      pane.zoom = 1;
      pane.center = [0.5, 0.5];
    }
    schedule();
  }
  function schedule() {
    if (!scheduled) {
      scheduled = true;
      requestAnimationFrame(render);
    }
  }
  function layout(pane) {
    const bounds = pane.overlay.getBoundingClientRect(),
      dpr = Math.min(2, window.devicePixelRatio || 1);
    const w = Math.max(1, Math.round(bounds.width * dpr)),
      h = Math.max(1, Math.round(bounds.height * dpr));
    if (pane.canvas.width !== w || pane.canvas.height !== h) {
      pane.canvas.width = w;
      pane.canvas.height = h;
      pane.overlay.width = w;
      pane.overlay.height = h;
    }
    const size = pane.name === "source" ? spec.source_size : warp.size,
      scale = Math.min(w / size[0], h / size[1]) * 0.96 * pane.zoom;
    pane.rect = [
      w / 2 - pane.center[0] * size[0] * scale,
      h / 2 - pane.center[1] * size[1] * scale,
      size[0] * scale,
      size[1] * scale,
    ];
  }
  function xy(pane, x, y) {
    return [pane.rect[0] + x * pane.rect[2], pane.rect[1] + y * pane.rect[3]];
  }
  function overlay(pane) {
    const ctx = pane.overlay.getContext("2d");
    ctx.clearRect(0, 0, pane.overlay.width, pane.overlay.height);
    ctx.lineWidth = 1.5;
    const source = pane.name === "source";
    const line = (points, color, dash = []) => {
      ctx.strokeStyle = color;
      ctx.setLineDash(dash);
      ctx.beginPath();
      points.forEach(([x, y], i) => {
        const p = xy(pane, x, y);
        if (i) ctx.lineTo(...p);
        else ctx.moveTo(...p);
      });
      ctx.stroke();
      ctx.setLineDash([]);
    };
    if (input("show_mask").checked && maskExtents.length) {
      for (const [x, top, bottom] of maskExtents) {
        const sx = source ? x : warp.forwardX(x),
          ty = source ? top : warp.forwardY(x, top),
          by = source ? bottom : warp.forwardY(x, bottom);
        const outside =
          warp.forwardX(x) < 0 ||
          warp.forwardX(x) > 1 ||
          bottom > warp.inverseY(x, 1);
        ctx.fillStyle = outside ? "#ff675f" : "#5be9b0";
        for (const y of [ty, by]) {
          const p = xy(pane, sx, y);
          ctx.fillRect(p[0] - 1, p[1] - 1, 2, 2);
        }
      }
    }
    if (input("show_guides").checked) {
      if (source) {
        for (const [rail, color] of [
          [1, "#65b8ff"],
          [2, "#ffc168"],
        ]) {
          line(
            Array.from({ length: 513 }, (_, i) => {
              const x = i / 512;
              return [x, warp.column(x)[rail - 1]];
            }),
            color,
          );
          spec.guides.forEach((row, i) => {
            const p = xy(pane, row[0], row[rail]);
            ctx.beginPath();
            ctx.arc(
              ...p,
              selected === i && +input("guide").value === rail ? 6 : 4,
              0,
              Math.PI * 2,
            );
            ctx.fillStyle =
              selected === i && +input("guide").value === rail ? "#fff" : color;
            ctx.fill();
            ctx.strokeStyle = "#101722";
            ctx.stroke();
          });
        }
        for (const x of spec.horizontal_crop)
          line(
            [
              [x, 0],
              [x, 1],
            ],
            "#dd94ff",
            [5, 5],
          );
      } else {
        for (const [index, color] of [
          [0, "#65b8ff"],
          [1, "#ffc168"],
        ])
          line(
            [
              [0, spec.target_rows[index]],
              [1, spec.target_rows[index]],
            ],
            color,
            [8, 6],
          );
      }
    }
    if (source && stroke.length) line(stroke, "#fff");
  }
  function render() {
    scheduled = false;
    if (!warp || !image) return;
    const start = performance.now();
    for (const pane of panes) {
      layout(pane);
      pane.renderer.draw(pane.rect, pane.name === "result");
      overlay(pane);
    }
    let clipped = 0;
    for (const [x, top, bottom] of maskExtents)
      if (
        warp.forwardX(x) < 0 ||
        warp.forwardX(x) > 1 ||
        bottom > warp.inverseY(x, 1)
      )
        clipped++;
    const texture = panes[0].renderer.imageSize;
    const note = clipped
      ? `${clipped} sampled mask columns extend outside the output. Adjust targets or crop.`
      : mask
        ? "Sampled mask boundary is inside the output."
        : "No ice mask: inspect the rink ends before exporting.";
    status(note, clipped > 0);
    $("#result-size").textContent = warp.size.join(" × ");
    $("#warp-timing").textContent =
      `${(performance.now() - start).toFixed(1)} ms update · texture ${texture.join(" × ")}`;
    document.documentElement.dataset.warpReady = "true";
  }
  function pointer(pane, event) {
    const b = pane.overlay.getBoundingClientRect();
    return [
      ((event.clientX - b.left) * pane.overlay.width) / b.width,
      ((event.clientY - b.top) * pane.overlay.height) / b.height,
    ];
  }
  function normalized(pane, p) {
    return [
      (p[0] - pane.rect[0]) / pane.rect[2],
      (p[1] - pane.rect[1]) / pane.rect[3],
    ];
  }
  function movePoint(x, y, history = true) {
    const next = clone(spec),
      rail = +input("guide").value,
      i = selected,
      row = next.guides[i];
    const oldX = row[0];
    row[0] =
      i === 0
        ? 0
        : i === next.guides.length - 1
          ? 1
          : G.clamp(
              x,
              next.guides[i - 1][0] + G.EPS * 2,
              next.guides[i + 1][0] - G.EPS * 2,
            );
    row[rail] = G.clamp(
      y,
      rail === 1 ? G.EPS : row[1] + G.EPS * 2,
      rail === 1 ? row[2] - G.EPS * 2 : 1 - G.EPS,
    );
    if (input("symmetric").checked) {
      const peer = next.guides.findIndex(
        (r, j) => j !== i && Math.abs(r[0] - (1 - oldX)) < G.EPS,
      );
      if (Math.abs(oldX - 0.5) < G.EPS) row[0] = 0.5;
      if (peer >= 0) {
        next.guides[peer][0] = 1 - row[0];
        next.guides[peer][rail] = row[rail];
      }
      next.guides.sort((a, b) => a[0] - b[0]);
    }
    if (JSON.stringify(next) === JSON.stringify(spec)) return false;
    return apply(next, history);
  }
  function addPoint(x, y) {
    if (
      x <= G.EPS ||
      x >= 1 - G.EPS ||
      spec.guides.some((r) => Math.abs(r[0] - x) < G.EPS * 2)
    )
      return;
    const next = clone(spec),
      rail = +input("guide").value,
      [up, low] = warp.column(x),
      row = [x, up, low];
    row[rail] = G.clamp(
      y,
      rail === 1 ? G.EPS : up + G.EPS * 2,
      rail === 1 ? low - G.EPS * 2 : 1 - G.EPS,
    );
    next.guides.push(row);
    if (input("symmetric").checked && Math.abs(x - 0.5) > G.EPS) {
      const peer = next.guides.find(
        (r) => Math.abs(r[0] - (1 - x)) < G.EPS * 2,
      );
      if (peer) {
        peer[1] = row[1];
        peer[2] = row[2];
      } else next.guides.push([1 - x, row[1], row[2]]);
    }
    next.guides.sort((a, b) => a[0] - b[0]);
    selected = next.guides.findIndex((r) => r === row);
    apply(next);
  }
  function deletePoint() {
    if (
      selected === null ||
      selected === 0 ||
      selected === spec.guides.length - 1
    )
      return;
    const next = clone(spec),
      x = next.guides[selected][0];
    next.guides = next.guides.filter(
      (r, i) =>
        i !== selected &&
        (!input("symmetric").checked || Math.abs(r[0] - (1 - x)) >= G.EPS),
    );
    selected = null;
    apply(next);
  }
  for (const pane of panes) {
    new ResizeObserver(schedule).observe(pane.element);
    pane.overlay.addEventListener("contextmenu", (e) => e.preventDefault());
    pane.overlay.addEventListener(
      "wheel",
      (e) => {
        e.preventDefault();
        if (!spec) return;
        const before = normalized(pane, pointer(pane, e));
        pane.zoom = G.clamp(pane.zoom * Math.exp(-e.deltaY * 0.001), 1, 20);
        layout(pane);
        const after = normalized(pane, pointer(pane, e));
        pane.center[0] += before[0] - after[0];
        pane.center[1] += before[1] - after[1];
        schedule();
      },
      { passive: false },
    );
    pane.overlay.addEventListener("pointerdown", (e) => {
      if (!spec) return;
      pane.overlay.focus();
      const p = pointer(pane, e),
        point = normalized(pane, p);
      if (e.button === 2 || e.button === 1) {
        dragging = { pane, pan: p, center: [...pane.center] };
      } else if (pane.name === "source" && e.button === 0) {
        if (drawing) {
          stroke = [point];
          dragging = { pane, draw: true };
        } else {
          let best = 14 * Math.min(2, window.devicePixelRatio || 1),
            hit = null;
          for (let i = 0; i < spec.guides.length; i++)
            for (const rail of [1, 2]) {
              const q = xy(pane, spec.guides[i][0], spec.guides[i][rail]),
                distance = Math.hypot(p[0] - q[0], p[1] - q[1]);
              if (distance < best) {
                best = distance;
                hit = [i, rail];
              }
            }
          if (hit) {
            selected = hit[0];
            input("guide").value = hit[1];
            dragging = { pane, point: true, changed: false };
            controls();
            schedule();
          } else {
            selected = null;
            controls();
            schedule();
          }
        }
      }
      if (dragging) pane.overlay.setPointerCapture(e.pointerId);
    });
    pane.overlay.addEventListener("pointermove", (e) => {
      if (!dragging || dragging.pane !== pane) return;
      const p = pointer(pane, e),
        [x, y] = normalized(pane, p);
      if (dragging.pan) {
        pane.center = [
          dragging.center[0] - (p[0] - dragging.pan[0]) / pane.rect[2],
          dragging.center[1] - (p[1] - dragging.pan[1]) / pane.rect[3],
        ];
        schedule();
      } else if (dragging.draw) {
        stroke.push([G.clamp(x, 0, 1), G.clamp(y, G.EPS, 1 - G.EPS)]);
        schedule();
      } else if (movePoint(x, y, !dragging.changed)) dragging.changed = true;
    });
    const finish = () => {
      if (dragging?.pane !== pane) return;
      if (dragging.draw) {
        const points = stroke;
        stroke = [];
        drawing = false;
        $('[data-action="draw"]').setAttribute("aria-pressed", "false");
        if (
          points.length >= 12 &&
          Math.max(...points.map((p) => p[0])) -
            Math.min(...points.map((p) => p[0])) >
            0.25
        )
          smooth(points);
        else
          status(
            "Draw across at least a quarter of the source image; the broad curve will span its width.",
            true,
          );
      }
      dragging = null;
    };
    pane.overlay.addEventListener("pointerup", finish);
    pane.overlay.addEventListener("pointercancel", finish);
    pane.overlay.addEventListener("dblclick", (e) => {
      if (pane.name === "source" && spec && !drawing)
        addPoint(...normalized(pane, pointer(pane, e)));
    });
    pane.overlay.addEventListener("keydown", (e) => {
      if (pane.name !== "source" || selected === null || !spec) return;
      if (e.key === "Delete" || e.key === "Backspace") {
        e.preventDefault();
        deletePoint();
        return;
      }
      const directions = {
        ArrowUp: [0, -1],
        ArrowDown: [0, 1],
        ArrowLeft: [-1, 0],
        ArrowRight: [1, 0],
      };
      if (!directions[e.key]) return;
      e.preventDefault();
      const d = directions[e.key],
        r = spec.guides[selected],
        step = e.shiftKey ? 10 : 1;
      movePoint(
        r[0] + (d[0] * step) / spec.source_size[0],
        r[+input("guide").value] + (d[1] * step) / spec.source_size[1],
      );
    });
  }
  // Value controls never change on a wheel gesture; scroll their nearest pane.
  document.addEventListener(
    "wheel",
    (e) => {
      if (!e.target.closest("input,select,button,textarea")) return;
      e.preventDefault();
      e.stopImmediatePropagation();
      for (let node = e.target.parentElement; node; node = node.parentElement) {
        const style = getComputedStyle(node),
          y = node.scrollTop,
          x = node.scrollLeft;
        if (/auto|scroll/.test(style.overflowY))
          node.scrollTop +=
            e.deltaY *
            (e.deltaMode === 1
              ? 16
              : e.deltaMode === 2
                ? node.clientHeight
                : 1);
        if (/auto|scroll/.test(style.overflowX))
          node.scrollLeft += e.deltaX * (e.deltaMode === 1 ? 16 : 1);
        if (node.scrollTop !== y || node.scrollLeft !== x) break;
      }
    },
    { capture: true, passive: false },
  );
  $$("input[type=range]").forEach((el) => {
    let gesture = null;
    el.addEventListener("pointerdown", () => {
      gesture = { changed: false };
    });
    for (const event of ["pointerup", "pointercancel", "blur"])
      el.addEventListener(event, () => {
        gesture = null;
      });
    el.addEventListener("input", () => {
      if (spec) {
        const next = clone(spec);
        next[el.name] = +el.value;
        if (
          next[el.name] !== spec[el.name] &&
          apply(next, !gesture?.changed) &&
          gesture
        )
          gesture.changed = true;
      }
    });
  });
  $$("input[type=number]").forEach((el) =>
    el.addEventListener("change", () => {
      if (!spec) return;
      const next = clone(spec),
        value = Number(el.value) / 100;
      const map = {
          top: ["target_rows", 0],
          bottom: ["target_rows", 1],
          left: ["horizontal_crop", 0],
          right: ["horizontal_crop", 1],
        },
        [key, i] = map[el.name];
      next[key][i] = value;
      apply(next);
    }),
  );
  input("symmetric").addEventListener("change", () => {
    if (!spec || !input("symmetric").checked) return;
    const next = clone(spec);
    next.guides = Array.from({ length: defaults.guide_count }, (_, i) => {
      const x = i / (defaults.guide_count - 1),
        a = warp.column(x),
        b = warp.column(1 - x);
      return [x, (a[0] + b[0]) / 2, (a[1] + b[1]) / 2];
    });
    selected = null;
    apply(next);
  });
  for (const name of ["show_guides", "show_mask", "guide"])
    input(name).addEventListener("change", schedule);
  let fileAction = "";
  async function exportPNG() {
    if (!spec || exporting) return;
    exporting = true;
    const button = $('[data-action="export"]');
    button.disabled = true;
    status("Rendering full-resolution PNG…");
    let renderer;
    try {
      const savedWarp = warp,
        savedImage = image,
        size = savedWarp.size,
        c = document.createElement("canvas");
      renderer = new RinkWarpRenderer(c);
      if (
        savedImage.naturalWidth > renderer.maxTexture ||
        savedImage.naturalHeight > renderer.maxTexture
      )
        throw Error(
          `Full-resolution export exceeds this GPU's ${renderer.maxTexture}px texture limit. Save guides and use the offline renderer.`,
        );
      if (size[0] * size[1] > 96 * 1024 * 1024)
        throw Error(
          "This PNG exceeds the 96-megapixel browser export limit. Save guides and use the offline renderer.",
        );
      renderer.setImage(savedImage, renderer.maxTexture);
      renderer.setWarp(savedWarp);
      const out = document.createElement("canvas");
      out.width = size[0];
      out.height = size[1];
      const ctx = out.getContext("2d");
      const tile = Math.min(2048, renderer.maxRender);
      for (let y = 0; y < size[1]; y += tile)
        for (let x = 0; x < size[0]; x += tile) {
          c.width = Math.min(tile, size[0] - x);
          c.height = Math.min(tile, size[1] - y);
          renderer.draw([-x, -y, ...size], true);
          ctx.drawImage(c, x, y);
          await new Promise((resolve) => setTimeout(resolve, 0));
        }
      const blob = await new Promise((resolve) =>
        out.toBlob(resolve, "image/png"),
      );
      if (!blob)
        throw Error(
          "PNG encoding failed. Save guides and use the offline renderer.",
        );
      download(blob, "rink-warp.png");
      status(`Exported ${size.join(" × ")} PNG. Calibration is unchanged.`);
    } catch (error) {
      status(error.message, true);
    } finally {
      renderer?.dispose();
      exporting = false;
      button.disabled = false;
    }
  }
  $$("[data-action]").forEach((button) =>
    button.addEventListener("click", async () => {
      const action = button.dataset.action;
      try {
        if (action.startsWith("open-")) {
          fileAction = action;
          $("#warp-file").accept =
            action === "open-guides"
              ? ".json"
              : "image/png,image/jpeg,image/webp";
          $("#warp-file").value = "";
          $("#warp-file").click();
          return;
        }
        if (!spec) throw Error("Open a panorama first.");
        if (action === "save-guides")
          download(
            new Blob([JSON.stringify(spec, null, 2) + "\n"], {
              type: "application/json",
            }),
            "rink-warp-guides.json",
          );
        else if (action === "export") await exportPNG();
        else if (action === "undo" && undo.length) {
          redo.push(clone(spec));
          selected = null;
          apply(undo.pop(), false);
        } else if (action === "redo" && redo.length) {
          undo.push(clone(spec));
          selected = null;
          apply(redo.pop(), false);
        } else if (action === "reset") {
          selected = null;
          apply(clone(initial));
          fitViews();
        } else if (action === "fit-mask") fitMask();
        else if (action === "smooth") smooth();
        else if (action === "delete-point") deletePoint();
        else if (action === "draw") {
          drawing = !drawing;
          button.setAttribute("aria-pressed", String(drawing));
          status(
            drawing
              ? "Draw the selected guide across the source image."
              : "Drag guide points to adjust the warp.",
          );
        } else if (action === "fit-view") fitViews();
        else if (action.startsWith("maximize-")) {
          const pane = panes.find((p) => p.name === action.slice(9)),
            on = !pane.element.classList.contains("warp-maximized");
          panes.forEach((p) => p.element.classList.remove("warp-maximized"));
          pane.element.classList.toggle("warp-maximized", on);
          $$("[data-action^=maximize-]").forEach((b) =>
            b.setAttribute("aria-pressed", "false"),
          );
          button.setAttribute("aria-pressed", String(on));
          schedule();
        }
      } catch (error) {
        status(error.message, true);
      }
    }),
  );
  $("#warp-file").addEventListener("change", async (e) => {
    const file = e.target.files[0];
    if (!file) return;
    const request = ++fileRequest,
      action = fileAction,
      ticket = imageTicket;
    try {
      if (file.size > 256 * 1024 * 1024)
        throw Error("Choose an image or guide file smaller than 256 MiB.");
      if (action === "open-guides") {
        if (!spec) throw Error("Open the source image first.");
        if (file.size > 1024 * 1024)
          throw Error("Guide files must be smaller than 1 MiB.");
        const text = await file.text();
        if (request !== fileRequest || ticket !== imageTicket) return;
        selected = null;
        apply(normalizeImport(JSON.parse(text)));
      } else {
        const url = await dataURL(file);
        if (request !== fileRequest) return;
        if (action === "open-image")
          await setImage(url, file.name, null, null, request);
        else await setMask(url, ticket, false, request);
      }
    } catch (error) {
      status(error.message, true);
    }
  });
  window.addEventListener("keydown", (e) => {
    if (
      (e.ctrlKey || e.metaKey) &&
      e.key.toLowerCase() === "z" &&
      !e.target.closest("input,textarea,select")
    ) {
      e.preventDefault();
      $(`[data-action="${e.shiftKey ? "redo" : "undo"}"]`).click();
    }
  });
  if (boot.image)
    setImage(
      boot.image,
      boot.title,
      boot.guides || null,
      boot.mask || null,
    ).catch((error) => status(error.message, true));
  // Read-only diagnostics for rendered interaction tests and performance evidence.
  window.rinkWarpEditor = {
    snapshot: () => spec && clone(spec),
    outputSize: () => warp && [...warp.size],
    imageSize: () => image && [image.naturalWidth, image.naturalHeight],
  };
})();
