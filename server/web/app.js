(() => {
  'use strict';

  const state = {
    catalog: { list: [], byId: new Map() },
    widgets: { list: [], byType: new Map() },
    profiles: {},
    themes: { list: [], byId: new Map() },
    themeDefault: 'neon-cyber',
    themeAliases: {},
    profile: '320x480',
    baseProfile: '320x480',
    layout: null,
    selectedId: null,
    values: {},
    history: new Map(),
    ws: null,
    wsTimer: null,
    dirty: false,
    interacting: false,
    paletteDragging: false,
    devices: [],
    deviceId: null,
    serverUrl: '',
    forward: { enabled: false, base_url: '', provider: 'auto' },
    lastRuntimeKey: '',
    modalMode: null,
    selectOpen: false,
    inspectorPending: false,
    lastInspectorRefresh: 0,
  };

  const $ = (id) => document.getElementById(id);
  const canvas = $('canvas');
  const canvasWrap = $('canvas-wrap');
  const canvasScroll = document.querySelector('.canvas-scroll');
  const paletteEl = $('palette');
  const inspectorEl = $('inspector');
  const profileSelect = $('profile-select');
  const themeSelect = $('theme-select');
  const frameworkSelect = $('framework-select');
  const toastEl = $('toast');
  const deviceSelect = $('device-select');
  const deviceModal = $('device-modal');

  const escHtml = (value) =>
    String(value).replace(/[&<>"']/g, (ch) => ({
      '&': '&amp;',
      '<': '&lt;',
      '>': '&gt;',
      '"': '&quot;',
      "'": '&#39;',
    }[ch]));

  const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
  const esc = (value) =>
    window.CSS && CSS.escape
      ? CSS.escape(value)
      : String(value).replace(/[^a-zA-Z0-9_-]/g, '\\$&');

  let toastTimer = null;
  function toast(message) {
    toastEl.textContent = message;
    toastEl.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => toastEl.classList.remove('show'), 2200);
  }

  async function api(path, options = {}) {
    const response = await fetch(path, {
      ...options,
      headers: { 'Content-Type': 'application/json', ...(options.headers || {}) },
    });
    const text = await response.text();
    let data = null;
    if (text) {
      try {
        data = JSON.parse(text);
      } catch {
        data = text;
      }
    }
    if (!response.ok) {
      const detail =
        data && typeof data === 'object' && data.detail
          ? data.detail
          : `HTTP ${response.status}`;
      throw new Error(typeof detail === 'string' ? detail : JSON.stringify(detail));
    }
    return data;
  }

  function metricDef(id) {
    return id ? state.catalog.byId.get(id) || null : null;
  }
  function widgetDef(type) {
    return state.widgets.byType.get(type) || null;
  }
  function activeProfile() {
    return state.profiles[state.profile] || null;
  }
  /* 单页模型：代理仍保留 pages[0] 以兼容协议，编辑器不再有多页概念。 */
  function currentWidgets() {
    const pages = state.layout && state.layout.pages;
    return Array.isArray(pages) && pages[0] && Array.isArray(pages[0].widgets)
      ? pages[0].widgets
      : [];
  }
  /* 界面框架：widgets=小组件网格（默认），classic=固定式传统主题 */
  function isClassic() {
    return !!(state.layout && state.layout.framework === 'classic');
  }
  const THEME_VARS = {
    bg: '--bg',
    surface: '--surface',
    surface_2: '--surface-2',
    surface_grad_from: '--surface-grad-from',
    surface_grad_to: '--surface-grad-to',
    border: '--border',
    text: '--text',
    text_dim: '--dim',
    text_mut: '--muted',
    accent: '--accent',
    accent_2: '--accent-2',
    cyan: '--cyan',
    ok: '--ok',
    warn: '--warn',
    err: '--err',
    pink: '--pink',
    glow: '--glow',
  };
  function applyTheme() {
    if (!state.layout) return;
    if (isClassic()) {
      const ct = state.layout.classic_theme || 'instrument';
      canvas.dataset.theme = `classic-${ct}`;
      canvas.dataset.skin = `classic-${ct}`;
      if (themeSelect.value !== ct) themeSelect.value = ct;
      return;
    }
    const theme =
      state.themes.byId.get(state.layout.theme) ||
      state.themes.byId.get(state.themeDefault) ||
      state.themes.byId.get('neon-cyber');
    if (theme && state.layout.theme !== theme.id) state.layout.theme = theme.id;
    const tokens = (theme && theme.tokens) || {};
    for (const [token, cssVar] of Object.entries(THEME_VARS)) {
      if (tokens[token]) canvas.style.setProperty(cssVar, tokens[token]);
    }
    const radius = Number(tokens.radius);
    canvas.style.setProperty('--radius', `${Number.isFinite(radius) ? radius : 12}px`);
    const borderWidth = Number(tokens.border_width);
    canvas.style.setProperty('--bw', `${Number.isFinite(borderWidth) ? borderWidth : 1}px`);
    const cornerLen = Number(tokens.corner_len);
    canvas.style.setProperty('--corner-len', `${Number.isFinite(cornerLen) ? cornerLen : 0}px`);
    const glowOpa = Number(tokens.glow_opa);
    canvas.style.setProperty(
      '--glow-opa',
      `${Number.isFinite(glowOpa) ? Math.max(0, Math.min(100, glowOpa)) : 0}%`
    );
    const tracking = Number(tokens.tracking);
    canvas.style.setProperty(
      '--tracking',
      `${Number.isFinite(tracking) ? tracking : 0}px`
    );
    const skin = tokens.skin || (theme && theme.id) || 'neon-cyber';
    canvas.dataset.theme = (theme && theme.id) || 'neon-cyber';
    canvas.dataset.skin = skin;
    if (themeSelect.value !== state.layout.theme) {
      themeSelect.value = state.layout.theme;
    }
  }
  function totalWidgets() {
    return currentWidgets().length;
  }
  function normalizeLayout(layout) {
    if (layout.v === 2 && Array.isArray(layout.pages)) {
      layout.pages.forEach((page, index) => {
        if (!page || typeof page !== 'object') {
          layout.pages[index] = { name: `第 ${index + 1} 页`, widgets: [] };
          return;
        }
        if (!Array.isArray(page.widgets)) page.widgets = [];
        if (typeof page.name !== 'string' || !page.name.trim()) {
          page.name = `第 ${index + 1} 页`;
        }
      });
      if (!layout.pages.length) {
        layout.pages = [{ name: '主页', widgets: [] }];
      }
      if (layout.pages.length > 1) {
        const merged = layout.pages.flatMap((page) =>
          Array.isArray(page.widgets) ? page.widgets : []
        );
        layout.pages = [{ name: '主页', widgets: merged }];
      }
      delete layout.page_animation;
      if (typeof layout.theme !== 'string' || !layout.theme) {
        layout.theme = state.themeDefault || 'neon-cyber';
      }
      layout.theme = state.themeAliases[layout.theme] || layout.theme;
      if (layout.framework !== 'classic') layout.framework = 'widgets';
      if (layout.framework === 'classic' && !layout.classic_theme) {
        layout.classic_theme = 'instrument';
      }
      return layout;
    }
    const rebuilt = {
      v: 2,
      profile: layout.profile,
      theme: state.themeAliases[layout.theme] || layout.theme || state.themeDefault,
      pages: [
        {
          name: '主页',
          widgets: Array.isArray(layout.widgets) ? layout.widgets : [],
        },
      ],
    };
    if (layout.framework === 'classic') {
      rebuilt.framework = 'classic';
      rebuilt.classic_theme = layout.classic_theme || 'instrument';
    }
    return rebuilt;
  }
  function cellSize(profile) {
    const cw =
      (profile.width - 2 * profile.padding - (profile.cols - 1) * profile.gap) /
      profile.cols;
    const ch =
      (profile.height - 2 * profile.padding - (profile.rows - 1) * profile.gap) /
      profile.rows;
    return { cw, ch };
  }
  function rectPx(widget, profile) {
    const { cw, ch } = cellSize(profile);
    return {
      left: profile.padding + widget.x * (cw + profile.gap),
      top: profile.padding + widget.y * (ch + profile.gap),
      width: widget.w * cw + (widget.w - 1) * profile.gap,
      height: widget.h * ch + (widget.h - 1) * profile.gap,
    };
  }
  function intersects(a, b) {
    return (
      a.x < b.x + b.w &&
      b.x < a.x + a.w &&
      a.y < b.y + b.h &&
      b.y < a.y + a.h
    );
  }
  function sizeOptions(def) {
    const list = (def && def.size_options) || [];
    if (list.length) return list;
    const size = (def && def.default_size) || [1, 1];
    return [size];
  }
  function widgetSize(def, widget) {
    const options = sizeOptions(def);
    if (widget && Number.isInteger(widget.w) && Number.isInteger(widget.h)) {
      const hit = options.find(([w, h]) => w === widget.w && h === widget.h);
      if (hit) return [hit[0], hit[1]];
    }
    const fallback = (def && def.default_size) || options[0] || [1, 1];
    return [Number(fallback[0]) || 1, Number(fallback[1]) || 1];
  }
  function nearestSize(def, w, h) {
    const options = sizeOptions(def);
    let best = null;
    let bestScore = Infinity;
    for (const option of options) {
      const score = Math.abs(option[0] - w) + Math.abs(option[1] - h);
      if (score < bestScore) {
        best = option;
        bestScore = score;
      }
    }
    return best || [w, h];
  }

  function isValidPlacement(candidate, ignoreId = null) {
    const profile = activeProfile();
    if (!profile) return false;
    if (
      candidate.x < 0 ||
      candidate.y < 0 ||
      candidate.x + candidate.w > profile.cols ||
      candidate.y + candidate.h > profile.rows
    ) {
      return false;
    }
    return !currentWidgets().some(
      (widget) => widget.id !== ignoreId && intersects(candidate, widget)
    );
  }
  function findFreeSpot(def, profile) {
    const [width, height] = widgetSize(def);
    for (let y = 0; y <= profile.rows - height; y += 1) {
      for (let x = 0; x <= profile.cols - width; x += 1) {
        if (isValidPlacement({ x, y, w: width, h: height })) {
          return { x, y };
        }
      }
    }
    return null;
  }

  // 一份组件数据 + 另一方向的位置覆盖：布局只在主方向存一份组件，
  // 切换方向时用 alt 覆盖或按比例自动映射，不复制组件定义。
  function baseRectOf(widget) {
    return widget._base || { x: widget.x, y: widget.y, w: widget.w, h: widget.h };
  }

  function captureOrientation() {
    if (!state.layout) return;
    if (state.profile !== state.baseProfile) return;
    for (const widget of currentWidgets()) {
      widget._base = { x: widget.x, y: widget.y, w: widget.w, h: widget.h };
    }
  }

  function mapPositionToProfile(rect, base, target, w, h) {
    const b = cellSize(base);
    const t = cellSize(target);
    const left = base.padding + rect.x * (b.cw + base.gap);
    const top = base.padding + rect.y * (b.ch + base.gap);
    return {
      x: clamp(
        Math.round(((left / base.width) * target.width - target.padding) / (t.cw + target.gap)),
        0,
        target.cols - w
      ),
      y: clamp(
        Math.round(((top / base.height) * target.height - target.padding) / (t.ch + target.gap)),
        0,
        target.rows - h
      ),
      w,
      h,
    };
  }

  function placeFreeRect(rect, target, occupied) {
    if (!occupied.some((other) => intersects(rect, other))) return rect;
    let best = null;
    let bestScore = 0;
    for (let y = 0; y <= target.rows - rect.h; y += 1) {
      for (let x = 0; x <= target.cols - rect.w; x += 1) {
        const candidate = { x, y, w: rect.w, h: rect.h };
        if (occupied.some((other) => intersects(candidate, other))) continue;
        const score = Math.abs(x - rect.x) + Math.abs(y - rect.y);
        if (!best || score < bestScore) {
          best = candidate;
          bestScore = score;
        }
      }
    }
    return best || rect;
  }

  function applyOrientationView() {
    if (!state.layout) return;
    const base = state.profiles[state.baseProfile];
    const target = state.profiles[state.profile];
    if (!base || !target) return;
    const widgets = currentWidgets();
    for (const widget of widgets) {
      if (!widget._base) {
        widget._base = { x: widget.x, y: widget.y, w: widget.w, h: widget.h };
      }
    }
    if (state.profile === state.baseProfile) {
      for (const widget of widgets) {
        const [w, h] = widgetSize(widgetDef(widget.type), widget);
        widget.w = w;
        widget.h = h;
        widget.x = clamp(widget._base.x, 0, base.cols - w);
        widget.y = clamp(widget._base.y, 0, base.rows - h);
        widget._base.w = w;
        widget._base.h = h;
      }
      return;
    }
    const explicit = new Set();
    const occupied = [];
    const pending = [];
    for (const widget of widgets) {
      const alt = widget.alt;
      if (!alt || !Number.isInteger(alt.x) || !Number.isInteger(alt.y)) continue;
      const [w, h] = widgetSize(widgetDef(widget.type), widget);
      const rect = {
        x: clamp(alt.x, 0, target.cols - w),
        y: clamp(alt.y, 0, target.rows - h),
        w,
        h,
      };
      widget._altRect = rect;
      explicit.add(widget);
      occupied.push(rect);
    }
    for (const widget of widgets) {
      if (explicit.has(widget)) continue;
      const [w, h] = widgetSize(widgetDef(widget.type), widget);
      pending.push({
        widget,
        rect: mapPositionToProfile(baseRectOf(widget), base, target, w, h),
      });
    }
    pending.sort((a, b) => b.rect.w * b.rect.h - a.rect.w * a.rect.h);
    for (const item of pending) {
      const rect = placeFreeRect(item.rect, target, occupied);
      occupied.push(rect);
      item.widget._altRect = rect;
    }
    for (const widget of widgets) {
      const rect = widget._altRect;
      delete widget._altRect;
      if (!rect) continue;
      widget.x = rect.x;
      widget.y = rect.y;
      widget.w = rect.w;
      widget.h = rect.h;
    }
  }

  function findFreeBaseSpot(def) {
    const base = state.profiles[state.baseProfile];
    if (!base) return null;
    const [width, height] = widgetSize(def);
    const occupied = currentWidgets().map((widget) => baseRectOf(widget));
    for (let y = 0; y <= base.rows - height; y += 1) {
      for (let x = 0; x <= base.cols - width; x += 1) {
        const rect = { x, y, w: width, h: height };
        if (!occupied.some((other) => intersects(rect, other))) return rect;
      }
    }
    return null;
  }
  function pointToCell(clientX, clientY) {
    const profile = activeProfile();
    const rect = canvas.getBoundingClientRect();
    const scale = rect.width / profile.width;
    const x = (clientX - rect.left) / scale;
    const y = (clientY - rect.top) / scale;
    const { cw, ch } = cellSize(profile);
    return {
      x: clamp(
        Math.floor((x - profile.padding + profile.gap / 2) / (cw + profile.gap)),
        0,
        profile.cols - 1
      ),
      y: clamp(
        Math.floor((y - profile.padding + profile.gap / 2) / (ch + profile.gap)),
        0,
        profile.rows - 1
      ),
    };
  }

  function metricValue(id) {
    if (!id) return null;
    const item = state.values[id];
    if (!item || !item.a) return null;
    return item.v;
  }
  function numberValue(id) {
    const value = metricValue(id);
    return typeof value === 'number' && Number.isFinite(value) ? value : null;
  }
  function textValue(id) {
    const value = metricValue(id);
    return typeof value === 'string' ? value : null;
  }
  function boolValue(id) {
    const value = metricValue(id);
    return typeof value === 'boolean' ? value : null;
  }
  function formatNumber(value, metric) {
    if (value === null || value === undefined || !Number.isFinite(value)) {
      return '—';
    }
    const precision = metric ? Number(metric.precision || 0) : 0;
    return value.toLocaleString('zh-CN', {
      minimumFractionDigits: precision,
      maximumFractionDigits: precision,
    });
  }
  function valueWithUnit(value, metric) {
    const number = formatNumber(value, metric);
    if (number === '—') return '—';
    return metric && metric.unit ? `${number} ${metric.unit}` : number;
  }
  function widgetTitle(widget) {
    if (
      widget.options &&
      typeof widget.options.title === 'string' &&
      widget.options.title.trim()
    ) {
      return widget.options.title;
    }
    const def = widgetDef(widget.type);
    return def ? def.label : widget.type;
  }

  function markDirty() {
    state.dirty = true;
    $('dirty').classList.remove('hidden');
  }
  function updateDirty() {
    $('dirty').classList.toggle('hidden', !state.dirty);
  }
  function setConn(text, cls) {
    const el = $('conn');
    el.textContent = text;
    el.classList.remove('online', 'offline');
    if (cls) el.classList.add(cls);
  }

  function fitCanvas() {
    if (!state.layout) return;
    if (isClassic()) {
      fitClassicCanvas();
      return;
    }
    const profile = activeProfile();
    if (!profile) return;
    const mobile = window.matchMedia('(max-width: 860px)').matches;
    const availW = Math.max(220, canvasScroll.clientWidth - 16);
    const availH = Math.max(
      280,
      (canvasScroll.clientHeight || window.innerHeight * 0.6) - 16
    );
    const widthScale = availW / profile.width;
    const heightScale = availH / profile.height;
    const rawScale = mobile ? widthScale : Math.min(widthScale, heightScale);
    const scale = Math.max(0.45, Math.min(mobile ? 1.4 : 1.8, rawScale));
    canvas.style.width = `${profile.width}px`;
    canvas.style.height = `${profile.height}px`;
    canvas.style.transform = `scale(${scale})`;
    canvasWrap.style.width = `${profile.width * scale}px`;
    canvasWrap.style.height = `${profile.height * scale}px`;
    const { cw, ch } = cellSize(profile);
    canvas.style.setProperty('--grid-x', `${cw + profile.gap}px`);
    canvas.style.setProperty('--grid-y', `${ch + profile.gap}px`);
    canvas.style.setProperty('--grid-offset-x', `${profile.padding}px`);
    canvas.style.setProperty('--grid-offset-y', `${profile.padding}px`);
    $('canvas-title').textContent =
      `${profile.label} · ${profile.cols}×${profile.rows} 网格`;
  }

  function renderPalette() {
    if (isClassic()) {
      paletteEl.innerHTML =
        '<p class="muted" style="padding:12px;line-height:1.8">' +
        '传统框架为固定式界面，不使用组件库。<br>' +
        '可在上方「配色」切换传统主题：<br>磷光示波器 / 战术 HUD。</p>';
      return;
    }
    paletteEl.innerHTML = '';
    const groups = new Map();
    for (const def of state.widgets.list) {
      const key = def.category || '其他';
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push(def);
    }
    for (const [category, defs] of groups) {
      const head = document.createElement('div');
      head.className = 'palette-group';
      head.textContent = category;
      paletteEl.appendChild(head);
      for (const def of defs) {
        const item = document.createElement('div');
        item.className = 'palette-item';
        item.draggable = true;
        item.dataset.type = def.type;

        const title = document.createElement('strong');
        title.textContent = def.label;
        const desc = document.createElement('span');
        desc.textContent = def.description || def.category || '';
        item.append(title, desc);

        item.addEventListener('dragstart', (event) => {
          state.paletteDragging = true;
          event.dataTransfer.setData('text/plain', def.type);
          event.dataTransfer.effectAllowed = 'copy';
        });
        item.addEventListener('dragend', () => {
          setTimeout(() => {
            state.paletteDragging = false;
          }, 0);
        });
        item.addEventListener('click', () => {
          if (!state.paletteDragging) addWidget(def.type);
        });
        paletteEl.appendChild(item);
      }
    }
  }

  function renderStatusPill(widget, body) {
    const online = boolValue(widget.bindings.online);
    const model = textValue(widget.bindings.model);
    const phase = textValue(widget.bindings.phase);

    const row = document.createElement('div');
    row.className = 'status-row';
    const dot = document.createElement('span');
    dot.className =
      'status-dot' + (online === true ? ' on' : online === false ? ' off' : '');
    const name = document.createElement('span');
    name.className = 'status-model';
    name.textContent = model || (online === false ? '离线' : '—');
    const pill = document.createElement('span');
    pill.className = 'phase-pill';
    pill.textContent = phase || (online === null ? '未知' : 'idle');
    row.append(dot, name, pill);
    body.appendChild(row);
  }

  function renderKpi(widget, body) {
    const metric = metricDef(widget.bindings.value);
    const value = numberValue(widget.bindings.value);
    const main = document.createElement('div');
    main.className = 'kpi-value';
    const number = document.createElement('span');
    number.textContent = formatNumber(value, metric);
    main.appendChild(number);
    if (value !== null && metric && metric.unit) {
      const unit = document.createElement('span');
      unit.className = 'kpi-unit';
      unit.textContent = metric.unit;
      main.appendChild(unit);
    }
    const label = document.createElement('div');
    label.className = 'kpi-metric';
    label.textContent = metric ? metric.label : '未绑定指标';
    const gauge = document.createElement('div');
    gauge.className = 'kpi-gauge';
    const ratio =
      typeof value === 'number' ? Math.max(0, Math.min(1, value / 100)) : 0.62;
    gauge.style.setProperty('--ratio', String(ratio));
    body.append(gauge, main, label);
  }

  function renderSparkline(widget, body) {
    const metric = metricDef(widget.bindings.value);
    const value = numberValue(widget.bindings.value);

    const top = document.createElement('div');
    top.className = 'spark-top';
    const valueEl = document.createElement('span');
    valueEl.className = 'spark-value';
    valueEl.textContent = valueWithUnit(value, metric);
    top.appendChild(valueEl);
    body.appendChild(top);

    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', '0 0 100 30');
    svg.setAttribute('preserveAspectRatio', 'none');
    svg.classList.add('spark-svg');

    const history = (state.history.get(widget.bindings.value) || []).slice(-60);
    if (history.length >= 2) {
      const max = Math.max(...history, 1);
      const points = history.map((sample, index) => {
        const x = (index / (history.length - 1)) * 100;
        const y = 28 - (Math.max(0, sample) / max) * 24;
        return `${x.toFixed(2)},${y.toFixed(2)}`;
      });
      const area = document.createElementNS('http://www.w3.org/2000/svg', 'polygon');
      area.setAttribute('points', `0,30 ${points.join(' ')} 100,30`);
      area.setAttribute('class', 'spark-fill');
      const line = document.createElementNS(
        'http://www.w3.org/2000/svg',
        'polyline'
      );
      line.setAttribute('points', points.join(' '));
      line.setAttribute('class', 'spark-line');
      svg.append(area, line);
    } else {
      const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
      line.setAttribute('x1', '0');
      line.setAttribute('y1', '29');
      line.setAttribute('x2', '100');
      line.setAttribute('y2', '29');
      line.setAttribute('class', 'spark-line');
      svg.appendChild(line);
    }
    body.appendChild(svg);
  }

  function renderServerCard(widget, body) {
    const grid = document.createElement('div');
    grid.className = 'server-grid';

    const customLabels =
      widget.options && widget.options.labels ? widget.options.labels : {};
    const labelFor = (key, fallback) => {
      const custom = customLabels[key];
      if (typeof custom === 'string' && custom.trim()) return custom.trim();
      const metric = metricDef(widget.bindings[key]);
      return metric ? metric.label : fallback;
    };
    const valueFor = (key) => {
      const metric = metricDef(widget.bindings[key]);
      const value = metricValue(widget.bindings[key]);
      if (value === null || value === undefined) return '—';
      if (typeof value === 'boolean') return value ? '是' : '否';
      if (typeof value === 'string') return value;
      if (typeof value === 'number') return valueWithUnit(value, metric);
      return String(value);
    };
    const ramValue = () => {
      const used = metricValue(widget.bindings.ram_used);
      const total = metricValue(widget.bindings.ram_total);
      if (typeof used !== 'number' || typeof total !== 'number') {
        return valueFor('ram_used');
      }
      const usedMetric = metricDef(widget.bindings.ram_used);
      const totalMetric = metricDef(widget.bindings.ram_total);
      if (
        usedMetric &&
        totalMetric &&
        usedMetric.unit === 'MB' &&
        totalMetric.unit === 'MB'
      ) {
        return `${(used / 1024).toFixed(1)} / ${(total / 1024).toFixed(0)} GB`;
      }
      return `${valueWithUnit(used, usedMetric)} / ${valueWithUnit(total, totalMetric)}`;
    };

    const addRow = (label, value) => {
      const labelEl = document.createElement('div');
      labelEl.className = 'label';
      labelEl.textContent = label;
      const valueEl = document.createElement('div');
      valueEl.className = 'value';
      valueEl.textContent = value;
      grid.append(labelEl, valueEl);
    };

    addRow(labelFor('cpu', 'CPU'), valueFor('cpu'));
    addRow(labelFor('ram_used', '内存'), ramValue());
    addRow(labelFor('gpu_temp', 'GPU'), valueFor('gpu_temp'));
    addRow(labelFor('text', '版本'), valueFor('text'));
    body.appendChild(grid);
  }

  function formatListValue(value, metric, kind, format) {
    if (value === null || value === undefined) return '—';
    let text;
    if (typeof value === 'boolean') {
      text = kind === 'status' ? (value ? '在线' : '离线') : (value ? '是' : '否');
    } else if (typeof value === 'number') {
      text = valueWithUnit(value, metric);
    } else {
      text = String(value);
    }
    if (typeof format === 'string' && format.trim()) {
      const suffix = format.trim();
      text = suffix.includes('{v}')
        ? suffix.split('{v}').join(text)
        : `${text} ${suffix}`;
    }
    return text;
  }

  function renderMetricList(widget, body) {
    const options = widget.options || {};
    const rows = Array.isArray(options.rows) ? options.rows : [];
    const showLabels = options.show_labels !== false;
    const font = options.font_size || 'M';
    const rowHeight = Number(options.row_height) || 22;
    const labelWidth = Number(options.label_width) || 60;

    body.classList.add('metric-list-body');
    body.style.setProperty(
      '--list-font',
      font === 'S' ? '11px' : font === 'L' ? '15px' : '13px'
    );
    body.style.setProperty('--row-h', `${rowHeight}px`);
    body.style.setProperty('--label-w', `${labelWidth}px`);

    const list = document.createElement('div');
    list.className = 'metric-list';
    if (!rows.length) {
      const empty = document.createElement('div');
      empty.className = 'list-empty';
      empty.textContent = '在属性面板添加行';
      list.appendChild(empty);
    }

    for (const row of rows) {
      const metric = metricDef(row.binding);
      const value = metricValue(row.binding);
      const kind = row.kind || 'auto';
      const rowEl = document.createElement('div');
      rowEl.className = 'metric-row';

      const showDot =
        kind === 'status' || (kind === 'auto' && metric && metric.type === 'bool');
      if (showDot) {
        const dot = document.createElement('span');
        dot.className = 'metric-dot';
        if (typeof value === 'boolean') dot.classList.add(value ? 'on' : 'off');
        rowEl.appendChild(dot);
      }
      if (showLabels) {
        const label = document.createElement('span');
        label.className = 'metric-label';
        label.textContent = row.label || (metric ? metric.label : '—');
        rowEl.appendChild(label);
      }
      const valueEl = document.createElement('span');
      valueEl.className = 'metric-value';
      valueEl.textContent = formatListValue(value, metric, kind, row.format);
      rowEl.appendChild(valueEl);
      list.appendChild(rowEl);
    }

    body.appendChild(list);
  }

  const PET_LABELS = {
    sleep: '睡眠',
    idle: '待机',
    happy: '开心',
    busy: '忙碌',
    alert: '警戒',
    offline: '离线',
  };
  const PET_FX = {
    sleep: 'Zzz',
    idle: '',
    happy: '',
    busy: '...',
    alert: '!',
    offline: '?',
  };
  function petState(widget) {
    const metric = metricDef(widget.bindings.value);
    const value = metricValue(widget.bindings.value);
    if (!metric || value === null || value === undefined) return 'offline';
    if (widget.bindings.busy && boolValue(widget.bindings.busy) === true) {
      return 'busy';
    }
    if (typeof value === 'boolean') return value ? 'happy' : 'offline';
    if (typeof value !== 'number' || !Number.isFinite(value)) return 'idle';
    const options = widget.options || {};
    const sleepBelow = Number(options.sleep_below ?? 20);
    const idleBelow = Number(options.idle_below ?? 60);
    const happyBelow = Number(options.happy_below ?? 85);
    if (value < sleepBelow) return 'sleep';
    if (value < idleBelow) return 'idle';
    if (value < happyBelow) return 'happy';
    return 'alert';
  }
  function petValueText(widget) {
    const metric = metricDef(widget.bindings.value);
    const value = metricValue(widget.bindings.value);
    if (value === null || value === undefined) return '—';
    if (typeof value === 'boolean') return value ? '是' : '否';
    if (typeof value !== 'number') return String(value);
    return `${formatNumber(value, metric)}${metric && metric.unit ? ` ${metric.unit}` : ''}`;
  }
  function petSvg(widget, state) {
    const tokens =
      (state.layout && state.themes.byId.get(state.layout.theme)?.tokens) || {};
    const skin = tokens.skin || 'neon-cyber';
    const head =
      skin === 'neon-cyber'
        ? '<rect class="pet-head" x="21" y="21" width="58" height="48" rx="6"></rect>'
        : skin === 'vital-pulse'
          ? '<rect class="pet-head" x="22" y="23" width="56" height="44" rx="20"></rect>'
          : '<ellipse class="pet-head" cx="50" cy="45" rx="29" ry="24"></ellipse>';
    const torso =
      skin === 'minimal-air'
        ? '<circle class="pet-torso" cx="50" cy="73" r="25"></circle>'
        : skin === 'vital-pulse'
          ? '<rect class="pet-torso" x="21" y="56" width="58" height="34" rx="17"></rect>'
          : '<ellipse class="pet-torso" cx="50" cy="73" rx="30" ry="18"></ellipse>';
    const deco =
      skin === 'neon-cyber'
        ? '<line class="pet-antenna" x1="50" y1="21" x2="50" y2="12"></line><circle class="pet-antenna-dot" cx="50" cy="10" r="2.4"></circle><rect class="pet-visor" x="35" y="38" width="30" height="10" rx="3"></rect>'
        : skin === 'vintage-gauge'
          ? '<circle class="pet-gauge" cx="50" cy="45" r="26"></circle>'
          : skin === 'vital-pulse'
            ? '<circle class="pet-aura" cx="50" cy="52" r="38"></circle>'
            : '';
    return `<svg class="pet-svg pet-state-${state} pet-variant-${skin}" viewBox="0 0 100 100" preserveAspectRatio="xMidYMid meet" aria-hidden="true">
      <ellipse class="pet-shadow" cx="50" cy="91" rx="24" ry="5"></ellipse>
      ${deco}
      <g class="pet-actor">
        <circle class="pet-ear" cx="29" cy="18" r="9"></circle>
        <circle class="pet-ear" cx="71" cy="18" r="9"></circle>
        ${torso}
        ${head}
        <ellipse class="pet-cheek" cx="31" cy="55" rx="5" ry="3"></ellipse>
        <ellipse class="pet-cheek" cx="69" cy="55" rx="5" ry="3"></ellipse>
        <ellipse class="pet-eye" cx="40" cy="43" rx="3.6" ry="4.8"></ellipse>
        <ellipse class="pet-eye" cx="60" cy="43" rx="3.6" ry="4.8"></ellipse>
        <rect class="pet-mouth" x="44" y="53" width="12" height="3.2" rx="1.6"></rect>
        <circle class="pet-tail" cx="82" cy="69" r="7"></circle>
      </g>
      <text class="pet-fx" x="80" y="15">${PET_FX[state]}</text>
    </svg>
    <div class="pet-badge">${PET_LABELS[state]}</div>
    <div class="pet-value">${petValueText(widget)}</div>`;
  }
  function renderPet(widget, body) {
    const state = petState(widget);
    body.classList.add('pet-body');
    body.innerHTML = petSvg(widget, state);
  }
  function updatePetWidget(widget, body) {
    const svg = body.querySelector('.pet-svg');
    if (!svg) {
      renderPet(widget, body);
      return;
    }
    const tokens =
      (state.layout && state.themes.byId.get(state.layout.theme)?.tokens) || {};
    const skin = tokens.skin || 'neon-cyber';
    const stateName = petState(widget);
    const cls = `pet-svg pet-state-${stateName} pet-variant-${skin}`;
    if (svg.getAttribute('class') !== cls) svg.setAttribute('class', cls);
    const fx = svg.querySelector('.pet-fx');
    if (fx) fx.textContent = PET_FX[stateName];
    const badge = body.querySelector('.pet-badge');
    if (badge) badge.textContent = PET_LABELS[stateName];
    const valueEl = body.querySelector('.pet-value');
    if (valueEl) valueEl.textContent = petValueText(widget);
  }

  /* ================= generation 2 小组件（mini 系列） =================
   * 预览视觉与固件 ui/src/ui_mini.c 对齐：同一套数值缩放（mini_ratio）、
   * 阈值着色（mini_value_color）与布局分层（compact/stack/hero）。 */

  const SVG_NS = 'http://www.w3.org/2000/svg';
  function svgEl(name, attrs) {
    const el = document.createElementNS(SVG_NS, name);
    if (attrs) {
      for (const [key, value] of Object.entries(attrs)) {
        el.setAttribute(key, String(value));
      }
    }
    return el;
  }
  function miniHistory(id) {
    return (state.history.get(id) || []).slice(-60);
  }
  /** 单位：options.unit 覆盖优先，否则指标自身单位（对齐固件 spec.unit） */
  function miniUnit(options, metric) {
    const override =
      options && typeof options.unit === 'string' ? options.unit.trim() : '';
    return override || (metric && metric.unit) || '';
  }
  /** 量程：显式 max_value > %/°C 归 100 > 0（软缩放） */
  function miniMaxOf(options, unit) {
    const explicit = Number(options && options.max_value);
    if (Number.isFinite(explicit) && explicit > 0) return explicit;
    if (unit === '%' || (unit && unit.indexOf('°C') >= 0)) return 100;
    return 0;
  }
  /** 填充比例：对齐固件 mini_ratio（无显式量程时 v/(v+1) 软缩放） */
  function miniRatio(value, options, unit) {
    if (typeof value !== 'number' || !Number.isFinite(value)) return 0;
    const v = Math.max(0, value);
    const maxv = miniMaxOf(options, unit);
    if (maxv > 0) return Math.min(1, v / maxv);
    return Math.min(1, v / (v + 1));
  }
  /** 阈值着色：越危险线 err、越警告线 warn，否则 null（交给 CSS 主题主色） */
  function miniValueColor(value, options) {
    if (typeof value !== 'number' || !Number.isFinite(value)) return null;
    const danger = Number(options && options.danger_above);
    const warn = Number(options && options.warn_above);
    if (Number.isFinite(danger) && danger > 0 && value >= danger) return 'var(--err)';
    if (Number.isFinite(warn) && warn > 0 && value >= warn) return 'var(--warn)';
    return null;
  }
  /** 百分比字面量：两位小数，避免序列化出 15 位浮点尾巴 */
  function pctOf(ratio) {
    const v = Math.max(0, Math.min(1, Number(ratio) || 0));
    return `${(v * 100).toFixed(2)}%`;
  }
  /** 迷你数值文本：与固件一致的 k 缩写 */
  function miniCompactNumber(value, precision) {
    if (!Number.isFinite(value)) return '--';
    if (Math.abs(value) >= 1000) return `${(value / 1000).toFixed(1)}k`;
    const prec = Math.max(0, Math.min(2, Number.isFinite(precision) ? precision : 0));
    return value.toFixed(prec);
  }

  /* ---- gauge_ring：270° 圆弧 + 刻度 + 阈值标记 + 中心数值 ---- */
  function renderGaugeRing(widget, body) {
    const metric = metricDef(widget.bindings.value);
    const value = numberValue(widget.bindings.value);
    const options = widget.options || {};
    const unit = miniUnit(options, metric);
    const ratio = miniRatio(value, options, unit);
    const color = miniValueColor(value, options);
    const compact = widget.w <= 2 || widget.h <= 2;
    const showTicks = options.show_ticks !== false && !compact;

    const cx = 50;
    const cy = 50;
    const r = 33.5;
    const a0 = 135;
    const sweep = 270;
    const polar = (rad, deg) => {
      const a = (deg * Math.PI) / 180;
      return [cx + rad * Math.cos(a), cy + rad * Math.sin(a)];
    };
    const arcPath = (rad, from, to) => {
      const [x1, y1] = polar(rad, from);
      const [x2, y2] = polar(rad, to);
      const large = to - from > 180 ? 1 : 0;
      return `M ${x1.toFixed(2)} ${y1.toFixed(2)} A ${rad} ${rad} 0 ${large} 1 ${x2.toFixed(2)} ${y2.toFixed(2)}`;
    };

    const svg = svgEl('svg', {
      // 紧凑模式裁掉留白，让弧形填满小格子（对齐固件 compact 无刻度/无单位）
      viewBox: compact ? '10 10 80 70' : '0 0 100 104',
      preserveAspectRatio: 'xMidYMid meet',
      class: 'gauge-svg',
    });
    svg.appendChild(
      svgEl('path', { d: arcPath(r, a0, a0 + sweep), class: 'gauge-track' })
    );

    // 刻度环画在弧带外侧（与固件 gauge_tick_cb 的 r_out = 弧半径 + 3 一致）
    if (showTicks) {
      for (let i = 0; i <= 12; i += 1) {
        const ang = a0 + (sweep * i) / 12;
        const major = i % 3 === 0;
        const [x1, y1] = polar(38, ang);
        const [x2, y2] = polar(major ? 43.5 : 41, ang);
        svg.appendChild(
          svgEl('line', {
            x1: x1.toFixed(2),
            y1: y1.toFixed(2),
            x2: x2.toFixed(2),
            y2: y2.toFixed(2),
            class: `gauge-tick${major ? ' major' : ''}`,
          })
        );
      }
    }

    // 复古机械：46 格游丝刻度 + 内表圈（超集 DOM，默认由 CSS 隐藏）
    if (showTicks) {
      const fine = svgEl('g', { class: 'gauge-ticks-fine' });
      for (let i = 0; i <= 45; i += 1) {
        const ang = a0 + (sweep * i) / 45;
        const major = i % 5 === 0;
        const [x1, y1] = polar(major ? 37.5 : 40.5, ang);
        const [x2, y2] = polar(43.5, ang);
        fine.appendChild(
          svgEl('line', {
            x1: x1.toFixed(2),
            y1: y1.toFixed(2),
            x2: x2.toFixed(2),
            y2: y2.toFixed(2),
            class: `gauge-tick-fine${major ? ' major' : ''}`,
          })
        );
      }
      svg.appendChild(fine);
      svg.appendChild(
        svgEl('circle', { cx: cx, cy, r: 34.5, class: 'gauge-bezel' })
      );
    }

    const valueArc = svgEl('path', {
      d: arcPath(r, a0, a0 + sweep * Math.max(ratio, 0.02)),
      class: 'gauge-arc',
    });
    if (color) valueArc.style.stroke = color;
    svg.appendChild(valueArc);

    // 复古机械：黄铜指针（45% → 82% 半径）；紧凑/无刻度模式跳过，避免 viewBox 变化错位
    if (showTicks) {
      const pct = Math.max(0, Math.min(1, Number(ratio) || 0));
      const nx1 = cx + r * 0.45;
      const nx2 = cx + r * 0.82;
      const needle = svgEl('g', { class: 'gauge-needle' });
      needle.style.transform = `rotate(${(a0 + sweep * pct).toFixed(2)}deg)`;
      needle.appendChild(
        svgEl('line', {
          x1: nx1.toFixed(2),
          y1: 52,
          x2: nx2.toFixed(2),
          y2: 52,
          class: 'gauge-needle-line',
        })
      );
      needle.appendChild(
        svgEl('circle', {
          cx: nx2.toFixed(2),
          cy: 52,
          r: 2,
          class: 'gauge-needle-tip',
        })
      );
      // 中心轴帽：与固件一致，机械表的转轴
      needle.appendChild(
        svgEl('circle', { cx, cy: 52, r: 1.6, class: 'gauge-hub' })
      );
      svg.appendChild(needle);
    }

    const valueText = svgEl('text', {
      x: cx,
      y: compact ? cy + 4 : cy + 1,
      class: 'gauge-value',
    });
    valueText.textContent = value === null ? '--' : formatNumber(value, metric);
    if (color) valueText.style.fill = color;
    svg.appendChild(valueText);

    if (!compact && unit) {
      const unitText = svgEl('text', { x: cx, y: cy + 14, class: 'gauge-unit' });
      unitText.textContent = unit;
      svg.appendChild(unitText);
    }
    body.appendChild(svg);
  }

  /* ---- led_dot：呼吸灯，2x2 纯净 / 大方差带副文本 ---- */
  function renderLedDot(widget, body) {
    const on = boolValue(widget.bindings.value);
    const sub = textValue(widget.bindings.subtitle);
    const options = widget.options || {};
    body.classList.add('led-body');
    const tiny = widget.w <= 2 && widget.h <= 2;
    const hasSub = !tiny && widget.h >= 4 && sub !== null;

    const dot = document.createElement('span');
    dot.className =
      'led-dot' + (on === true ? ' on' : on === false ? ' off' : ' unknown');
    body.appendChild(dot);

    if (!tiny) {
      const text = document.createElement('span');
      text.className =
        'led-text' + (on === true ? ' on' : on === false ? ' off' : ' unknown');
      text.textContent =
        on === true
          ? options.on_text || '在线'
          : on === false
            ? options.off_text || '离线'
            : '—';
      body.appendChild(text);
    }
    if (hasSub) {
      const subEl = document.createElement('span');
      subEl.className = 'led-sub';
      subEl.textContent = sub || '—';
      body.appendChild(subEl);
    }
  }

  /* ---- led_matrix：点阵状态墙，按宽高比自动排布 ---- */
  function renderLedMatrix(widget, body) {
    const options = widget.options || {};
    const rows = Array.isArray(options.rows) ? options.rows : [];
    const showLabels = options.show_labels !== false;
    body.classList.add('matrix-body');
    const grid = document.createElement('div');
    grid.className = 'matrix-grid';

    const n = Math.max(1, rows.length);
    const aspect = widget.w / Math.max(1, widget.h);
    // 取最接近正方形的列数（与固件 lround 一致）：向上取整会让 4 个灯排成 3+1
    const cols = Math.max(1, Math.min(n, Math.round(Math.sqrt(n * aspect))));
    const gridRows = Math.ceil(n / cols);
    // 单元格偏高 → 点上文下；偏宽 → 点左文右（对齐固件 build_matrix）
    const cellAspect = widget.h / gridRows / (widget.w / cols);
    if (cellAspect >= 0.8) grid.classList.add('stack');
    grid.style.gridTemplateColumns = `repeat(${cols}, minmax(0, 1fr))`;
    grid.style.gridTemplateRows = `repeat(${gridRows}, minmax(0, 1fr))`;

    if (!rows.length) {
      const empty = document.createElement('div');
      empty.className = 'list-empty';
      empty.textContent = '在属性面板添加行';
      grid.appendChild(empty);
    }
    for (const row of rows) {
      const value = metricValue(row.binding);
      const cell = document.createElement('div');
      cell.className = 'matrix-cell';
      const dot = document.createElement('span');
      dot.className =
        'matrix-dot' +
        (typeof value === 'boolean' ? (value ? ' on' : ' off') : ' unknown');
      cell.appendChild(dot);
      if (showLabels) {
        const label = document.createElement('span');
        label.className = 'matrix-label';
        label.textContent = row.label || '—';
        cell.appendChild(label);
      }
      grid.appendChild(cell);
    }
    body.appendChild(grid);
  }

  /* ---- bar_strip：inline / 堆叠 / hero 三种高度分层 ---- */
  function renderBarStrip(widget, body) {
    const metric = metricDef(widget.bindings.value);
    const value = numberValue(widget.bindings.value);
    const options = widget.options || {};
    const unit = miniUnit(options, metric);
    const ratio = miniRatio(value, options, unit);
    const color = miniValueColor(value, options);
    const maxv = miniMaxOf(options, unit) || 100;
    const valueText =
      value === null
        ? '--'
        : `${formatNumber(value, metric)}${unit ? ` ${unit}` : ''}`;

    // 卡片高度 ≥ 60px 时标题栏在，内联标签改显示指标名，避免同一标题出现两次
    const inlineTitle =
      widget.h >= 3 && metric && metric.label
        ? metric.label
        : widgetTitle(widget);
    const attachMarks = (track, vertical) => {
      for (const [key, cls] of [
        ['warn_above', 'warn'],
        ['danger_above', 'danger'],
      ]) {
        const t = Number(options[key]);
        if (!Number.isFinite(t) || t <= 0) continue;
        const el = document.createElement('span');
        el.className = `bar-mark ${cls}`;
        const pct = pctOf(t / maxv);
        if (vertical) el.style.bottom = pct;
        else el.style.left = pct;
        track.appendChild(el);
      }
    };

    if (widget.h <= 2) {
      body.classList.add('bar-inline-body');
      const row = document.createElement('div');
      row.className = 'bar-inline';
      const label = document.createElement('span');
      label.className = 'bar-inline-label';
      label.textContent = widgetTitle(widget);
      const track = document.createElement('span');
      track.className = 'bar-track';
      const fill = document.createElement('span');
      fill.className = 'bar-fill';
      fill.style.width = pctOf(Math.max(ratio, 0.02));
      if (color) fill.style.background = color;
      track.appendChild(fill);
      attachMarks(track, false);
      const valueEl = document.createElement('span');
      valueEl.className = 'bar-value';
      valueEl.textContent = valueText;
      if (color) valueEl.style.color = color;
      row.append(label, track, valueEl);
      body.appendChild(row);
    } else if (widget.h === 3) {
      body.classList.add('bar-stack-body');
      const wrap = document.createElement('div');
      wrap.className = 'bar-stack';
      const top = document.createElement('div');
      top.className = 'bar-stack-top';
      const label = document.createElement('span');
      label.className = 'bar-stack-title';
      label.textContent = inlineTitle;
      const valueEl = document.createElement('span');
      valueEl.className = 'bar-stack-value';
      valueEl.textContent = valueText;
      if (color) valueEl.style.color = color;
      top.append(label, valueEl);
      const track = document.createElement('span');
      track.className = 'bar-track thick';
      const fill = document.createElement('span');
      fill.className = 'bar-fill';
      fill.style.width = pctOf(Math.max(ratio, 0.02));
      if (color) fill.style.background = color;
      track.appendChild(fill);
      attachMarks(track, false);
      wrap.append(top, track);
      body.appendChild(wrap);
    } else {
      body.classList.add('bar-hero-body');
      const wrap = document.createElement('div');
      wrap.className = 'bar-hero';
      const left = document.createElement('div');
      left.className = 'bar-hero-left';
      const label = document.createElement('div');
      label.className = 'bar-hero-title';
      label.textContent = inlineTitle;
      const valueEl = document.createElement('div');
      valueEl.className = 'bar-hero-value';
      valueEl.textContent = value === null ? '--' : formatNumber(value, metric);
      if (color) valueEl.style.color = color;
      left.append(label, valueEl);
      if (unit) {
        const unitEl = document.createElement('div');
        unitEl.className = 'bar-hero-unit';
        unitEl.textContent = unit;
        left.appendChild(unitEl);
      }
      const track = document.createElement('div');
      track.className = 'bar-track vertical';
      const fill = document.createElement('div');
      fill.className = 'bar-fill vertical';
      fill.style.height = pctOf(Math.max(ratio, 0.02));
      if (color) fill.style.background = color;
      track.appendChild(fill);
      attachMarks(track, true);
      wrap.append(left, track);
      body.appendChild(wrap);
    }
  }

  /* ---- vbar_gauge：温度计式垂直柱 ---- */
  function renderVbarGauge(widget, body) {
    const metric = metricDef(widget.bindings.value);
    const value = numberValue(widget.bindings.value);
    const options = widget.options || {};
    const unit = miniUnit(options, metric);
    const ratio = miniRatio(value, options, unit);
    const color = miniValueColor(value, options);
    const maxv = miniMaxOf(options, unit) || 100;

    body.classList.add('vbar-body');
    const track = document.createElement('div');
    track.className = 'vbar-track';
    const fill = document.createElement('div');
    fill.className = 'vbar-fill';
    fill.style.height = pctOf(Math.max(ratio, 0.02));
    if (color) fill.style.background = color;
    track.appendChild(fill);
    for (const [key, cls] of [
      ['warn_above', 'warn'],
      ['danger_above', 'danger'],
    ]) {
      const t = Number(options[key]);
      if (!Number.isFinite(t) || t <= 0) continue;
      const el = document.createElement('span');
      el.className = `vbar-mark ${cls}`;
      el.style.bottom = pctOf(t / maxv);
      track.appendChild(el);
    }
    body.appendChild(track);

    const valueEl = document.createElement('div');
    valueEl.className = 'vbar-value';
    valueEl.textContent = value === null ? '--' : formatNumber(value, metric);
    if (color) valueEl.style.color = color;
    body.appendChild(valueEl);
    if (unit) {
      const unitEl = document.createElement('div');
      unitEl.className = 'vbar-unit';
      unitEl.textContent = unit;
      body.appendChild(unitEl);
    }
  }

  /* ---- multi_bars：柱状图 + 网格 + 柱顶数值 + 最大值高亮 ---- */
  function renderMultiBars(widget, body) {
    const options = widget.options || {};
    const rows = Array.isArray(options.rows) ? options.rows : [];
    const showValues = options.show_values !== false && widget.w >= 3;
    const showLabels = options.show_labels !== false;
    body.classList.add('mbars-body');

    const values = rows.map((row) => {
      const v = numberValue(row.binding);
      return typeof v === 'number' && v > 0 ? v : 0;
    });
    let maxv = 1;
    let argmax = 0;
    values.forEach((v, i) => {
      if (v > maxv) maxv = v;
      if (v >= values[argmax]) argmax = i;
    });
    maxv *= 1.15;

    const wrap = document.createElement('div');
    wrap.className = 'mbars';
    if (!rows.length) {
      const empty = document.createElement('div');
      empty.className = 'list-empty';
      empty.textContent = '在属性面板添加行';
      wrap.appendChild(empty);
    }
    rows.forEach((row, i) => {
      const col = document.createElement('div');
      col.className =
        'mbar-col' + (i === argmax && rows.length > 1 ? ' max' : '');
      if (showValues) {
        const cap = document.createElement('div');
        cap.className = 'mbar-cap';
        cap.textContent = miniCompactNumber(values[i], 1);
        col.appendChild(cap);
      }
      const plot = document.createElement('div');
      plot.className = 'mbar-plot';
      const bar = document.createElement('div');
      bar.className = 'mbar-bar';
      bar.style.height = pctOf(values[i] / maxv);
      plot.appendChild(bar);
      col.appendChild(plot);
      if (showLabels) {
        const label = document.createElement('div');
        label.className = 'mbar-label';
        label.textContent = row.label || '—';
        col.appendChild(label);
      }
      wrap.appendChild(col);
    });
    body.appendChild(wrap);
  }

  /* ---- dual_line：双折线 + 主线面积填充 + 图例 ---- */
  function renderDualLine(widget, body) {
    const options = widget.options || {};
    const showArea = options.show_area !== false;
    body.classList.add('dual-body');

    const legend = document.createElement('div');
    legend.className = 'dual-legend';
    const series = [
      { key: 'value', cls: 's1', unitKey: 'unit' },
      { key: 'value2', cls: 's2', unitKey: 'unit2' },
    ];
    for (const s of series) {
      const metric = metricDef(widget.bindings[s.key]);
      const value = numberValue(widget.bindings[s.key]);
      const unit = miniUnit({ unit: options[s.unitKey] }, metric);
      const item = document.createElement('div');
      item.className = `dual-item ${s.cls}`;
      const dot = document.createElement('span');
      dot.className = `dual-dot ${s.cls}`;
      const name = document.createElement('span');
      name.className = 'dual-name';
      name.textContent = metric ? metric.label : '未绑定';
      const valueEl = document.createElement('b');
      valueEl.textContent =
        value === null
          ? '--'
          : `${formatNumber(value, metric)}${unit ? ` ${unit}` : ''}`;
      item.append(dot, name, valueEl);
      legend.appendChild(item);
    }
    body.appendChild(legend);

    const svg = svgEl('svg', {
      viewBox: '0 0 100 40',
      preserveAspectRatio: 'none',
      class: 'dual-chart',
    });
    const drawSeries = (id, cls) => {
      const history = miniHistory(id);
      if (history.length < 2) {
        svg.appendChild(
          svgEl('line', { x1: 0, y1: 39, x2: 100, y2: 39, class: `dual-line ${cls}` })
        );
        return;
      }
      let lo = Math.min(...history);
      let hi = Math.max(...history);
      if (hi - lo < 1) hi = lo + 1;
      const pts = history.map((sample, index) => {
        const x = (index / (history.length - 1)) * 100;
        const y = 38 - ((sample - lo) / (hi - lo)) * 36;
        return `${x.toFixed(2)},${y.toFixed(2)}`;
      });
      if (showArea && cls === 's1') {
        svg.appendChild(
          svgEl('polygon', {
            points: `0,40 ${pts.join(' ')} 100,40`,
            class: 'dual-fill',
          })
        );
      }
      svg.appendChild(
        svgEl('polyline', { points: pts.join(' '), class: `dual-line ${cls}` })
      );
    };
    drawSeries(widget.bindings.value, 's1');
    drawSeries(widget.bindings.value2, 's2');
    body.appendChild(svg);
  }

  /* ---- big_stat：hero 大数字 + 趋势 + 背景曲线 ---- */
  function renderBigStat(widget, body) {
    const metric = metricDef(widget.bindings.value);
    const value = numberValue(widget.bindings.value);
    const options = widget.options || {};
    const unit = miniUnit(options, metric);
    const color = miniValueColor(value, options);
    body.classList.add('big-body');

    const history = miniHistory(widget.bindings.value);
    const spark = svgEl('svg', {
      viewBox: '0 0 100 40',
      preserveAspectRatio: 'none',
      class: 'big-spark',
    });
    if (history.length >= 2) {
      const max = Math.max(...history, 1);
      const pts = history.map((sample, index) => {
        const x = (index / (history.length - 1)) * 100;
        const y = 38 - (Math.max(0, sample) / max) * 36;
        return `${x.toFixed(2)},${y.toFixed(2)}`;
      });
      spark.appendChild(
        svgEl('polygon', { points: `0,40 ${pts.join(' ')} 100,40`, class: 'big-spark-fill' })
      );
      spark.appendChild(
        svgEl('polyline', { points: pts.join(' '), class: 'big-spark-line' })
      );
    }
    body.appendChild(spark);

    const valueEl = document.createElement('div');
    valueEl.className = 'big-value';
    // 字号阶梯：对齐固件 build_big（大卡 48 / 中卡 28 / 小卡 20）
    const bodyW = widget.w * 26.7 - 26;
    const bodyH = widget.h * 26.7 - 34;
    const heroFont =
      bodyW >= 110 && bodyH >= 90 ? 48 : bodyW >= 80 ? 28 : 20;
    valueEl.style.fontSize = `${heroFont}px`;
    const number = document.createElement('span');
    number.textContent = value === null ? '--' : formatNumber(value, metric);
    if (color) number.style.color = color;
    valueEl.appendChild(number);
    if (unit) {
      const unitEl = document.createElement('span');
      unitEl.className = 'big-unit';
      unitEl.textContent = unit;
      valueEl.appendChild(unitEl);
    }
    body.appendChild(valueEl);
    if (options.show_trend !== false) {
      const trend = document.createElement('div');
      trend.className = 'big-trend';
      const oldest = history.length >= 2 ? history[0] : null;
      if (oldest !== null && value !== null && oldest > 0.05) {
        const pct = ((value - oldest) / oldest) * 100;
        const up = pct >= 0;
        trend.classList.add(up ? 'up' : 'down');
        trend.textContent = `${up ? '▲' : '▼'} ${pct >= 0 ? '+' : ''}${pct.toFixed(1)}% / 60s`;
      } else {
        trend.textContent = '◆ 采样中';
      }
      body.appendChild(trend);
    }
  }

  /* ---- spark_row：横排迷你曲线，每列独立纵轴 ---- */
  function renderSparkRow(widget, body) {
    const options = widget.options || {};
    const rows = Array.isArray(options.rows) ? options.rows : [];
    const showLabels = options.show_labels !== false;
    body.classList.add('srow-body');
    const wrap = document.createElement('div');
    wrap.className = 'srow';
    if (!rows.length) {
      const empty = document.createElement('div');
      empty.className = 'list-empty';
      empty.textContent = '在属性面板添加行';
      wrap.appendChild(empty);
    }
    for (const row of rows) {
      const metric = metricDef(row.binding);
      const value = numberValue(row.binding);
      const col = document.createElement('div');
      col.className = 'srow-col';

      const valueEl = document.createElement('div');
      valueEl.className = 'srow-value';
      const unit = metric && metric.unit;
      valueEl.textContent =
        value === null ? '--' : `${formatNumber(value, metric)}${unit ? ` ${unit}` : ''}`;
      col.appendChild(valueEl);

      const svg = svgEl('svg', {
        viewBox: '0 0 100 30',
        preserveAspectRatio: 'none',
        class: 'srow-svg',
      });
      const history = miniHistory(row.binding);
      if (history.length >= 2) {
        const max = Math.max(...history, 1);
        const pts = history.map((sample, index) => {
          const x = (index / (history.length - 1)) * 100;
          const y = 28 - (Math.max(0, sample) / max) * 26;
          return `${x.toFixed(2)},${y.toFixed(2)}`;
        });
        svg.appendChild(
          svgEl('polygon', { points: `0,30 ${pts.join(' ')} 100,30`, class: 'srow-fill' })
        );
        svg.appendChild(
          svgEl('polyline', { points: pts.join(' '), class: 'srow-line' })
        );
      } else {
        svg.appendChild(
          svgEl('line', { x1: 0, y1: 29, x2: 100, y2: 29, class: 'srow-line' })
        );
      }
      col.appendChild(svg);

      if (showLabels) {
        const label = document.createElement('div');
        label.className = 'srow-label';
        label.textContent = row.label || '—';
        col.appendChild(label);
      }
      wrap.appendChild(col);
    }
    body.appendChild(wrap);
  }

  /* ---- text_badge：左侧装饰条 + 自动降档字号 ---- */
  function renderTextBadge(widget, body) {
    const metric = metricDef(widget.bindings.value);
    const raw = metricValue(widget.bindings.value);
    const options = widget.options || {};
    body.classList.add('tbadge-body');

    let text = '--';
    let color = null;
    if (raw === null || raw === undefined) {
      text = '--';
    } else if (typeof raw === 'boolean') {
      text = raw ? '在线' : '离线';
      color = raw ? 'var(--ok)' : 'var(--err)';
    } else if (typeof raw === 'number') {
      const unit = miniUnit(options, metric);
      // 窄卡（2 格）只放数字：单位会把 4 位数字挤成两行，与固件一致
      const narrow = widget.w < 3;
      text = `${formatNumber(raw, metric)}${unit && !narrow ? ` ${unit}` : ''}`;
      color = miniValueColor(raw, options);
    } else {
      text = String(raw);
    }

    const wrap = document.createElement('div');
    wrap.className = 'tbadge';
    if (widget.h >= 2) {
      const bar = document.createElement('span');
      bar.className = 'tbadge-bar';
      wrap.appendChild(bar);
    }
    const valueEl = document.createElement('div');
    valueEl.className =
      'tbadge-text' + (options.align === 'center' ? ' center' : '');
    valueEl.textContent = text;
    if (color) valueEl.style.color = color;

    // 字号阶梯：对齐固件 badge_font，按文本长度与尺寸档位自动降档防溢出
    const tier = options.font_size || 'M';
    const start = tier === 'S' ? 2 : tier === 'L' ? 0 : 1;
    const cjk = /[^\x00-\x7F]/.test(text);
    const ladder = cjk ? [22, 17, 14, 12, 11] : [26, 20, 15, 13, 11];
    const len = Math.min(text.length, 24);
    const lines =
      widget.h >= 3
        ? Math.max(1, Math.floor((widget.h * 26.7 - 34) / 18))
        : 1;
    const widthPx = Math.max(40, widget.w * 26.7 - 30);
    let font = ladder[ladder.length - 1];
    for (let i = start; i < ladder.length; i += 1) {
      const cw = cjk ? ladder[i] : ladder[i] * 0.62;
      if (len * cw <= widthPx * lines) {
        font = ladder[i];
        break;
      }
    }
    valueEl.style.fontSize = `${font}px`;
    if (widget.h >= 3) valueEl.classList.add('wrap');
    wrap.appendChild(valueEl);
    body.appendChild(wrap);
  }

  function renderWidgetBody(widget, body) {
    body.innerHTML = '';
    body.classList.remove('pet-body');
    const def = widgetDef(widget.type);
    if (!def) {
      body.textContent = `未知组件 ${widget.type}`;
      return;
    }
    if (widget.type === 'status_pill') renderStatusPill(widget, body);
    else if (widget.type === 'kpi') renderKpi(widget, body);
    else if (widget.type === 'sparkline') renderSparkline(widget, body);
    else if (widget.type === 'server_card') renderServerCard(widget, body);
    else if (widget.type === 'metric_list') renderMetricList(widget, body);
    else if (widget.type === 'desktop_pet') renderPet(widget, body);
    else if (widget.type === 'gauge_ring') renderGaugeRing(widget, body);
    else if (widget.type === 'led_dot') renderLedDot(widget, body);
    else if (widget.type === 'led_matrix') renderLedMatrix(widget, body);
    else if (widget.type === 'bar_strip') renderBarStrip(widget, body);
    else if (widget.type === 'vbar_gauge') renderVbarGauge(widget, body);
    else if (widget.type === 'multi_bars') renderMultiBars(widget, body);
    else if (widget.type === 'dual_line') renderDualLine(widget, body);
    else if (widget.type === 'big_stat') renderBigStat(widget, body);
    else if (widget.type === 'spark_row') renderSparkRow(widget, body);
    else if (widget.type === 'text_badge') renderTextBadge(widget, body);
    else body.textContent = def.label;
  }

  function buildWidget(widget, profile) {
    const def = widgetDef(widget.type);
    const rect = rectPx(widget, profile);
    const el = document.createElement('div');
    const classes = ['widget'];
    if (widget.id === state.selectedId) classes.push('selected');
    if (widget.options && widget.options.show_title === false) {
      classes.push('title-hidden');
    }
    // 与固件一致：mini 卡片高度不足 60px 时不画标题栏，空间全留给图形
    if (def && def.generation === 2 && rect.height < 60) {
      classes.push('title-hidden');
    }
    el.className = classes.join(' ');
    el.dataset.id = widget.id;
    el.style.left = `${rect.left}px`;
    el.style.top = `${rect.top}px`;
    el.style.width = `${rect.width}px`;
    el.style.height = `${rect.height}px`;

    const header = document.createElement('div');
    header.className = 'widget-header';
    const title = document.createElement('div');
    title.className = 'widget-title';
    title.textContent = widgetTitle(widget);
    header.appendChild(title);

    const body = document.createElement('div');
    body.className = 'widget-body';
    renderWidgetBody(widget, body);

    const deco = document.createElement('div');
    deco.className = 'widget-deco';
    deco.innerHTML =
      '<i class="dc tl"></i><i class="dc tr"></i><i class="dc bl"></i><i class="dc br"></i>' +
      '<i class="dc ticks"></i><i class="dc pulse"></i>';

    el.append(header, body, deco);
    el.addEventListener('pointerdown', (event) => {
      selectWidget(widget.id);
      startMove(event, widget, el);
    });
    el.addEventListener('pointermove', (event) => {
      if (event.buttons) return;
      el.style.cursor = 'move';
    });
    el.addEventListener('pointerleave', () => {
      el.style.cursor = '';
    });
    header.addEventListener('pointerdown', (event) =>
      startMove(event, widget, el)
    );

    if (def && def.resizable) {
      const handle = document.createElement('div');
      handle.className = 'widget-resize';
      handle.title = '拖动调整尺寸';
      handle.addEventListener('pointerdown', (event) =>
        startResize(event, widget, el)
      );
      el.appendChild(handle);
    }

    if (!def) {
      body.textContent = `未知组件 ${widget.type}`;
    }
    return el;
  }

  /* ============================ 传统框架（classic）预览 ============================ */
  /* 与固件 ui/src/ui_classic.c 同布局同配色：480×320 固定界面，
   * 编辑器内用 DOM 复刻渲染并跟随实时数据刷新。 */

  const CLASSIC_THEME_META = {
    instrument: {
      label: '磷光示波器',
      pal: {
        bg: '#07110B', bg2: '#0A1911', surface: '#08150E', border: '#163626',
        text: '#D9F8E6', dim: '#4D8B6A', mut: '#2F5A44',
        acc: '#38F28A', acc2: '#A9FFCE', acc3: '#FFD166',
        warn: '#FFD166', err: '#FF5347', track: '#1C3A28',
      },
    },
    hud: {
      label: '战术 HUD',
      pal: {
        bg: '#0A0F0C', bg2: '#0D1512', surface: '#12201A', border: '#27402D',
        text: '#D8F5D0', dim: '#6D8673', mut: '#3E5740',
        acc: '#7CFF4F', acc2: '#FFB020', acc3: '#3BE0C0',
        warn: '#FFB020', err: '#FF5347', track: '#1B2F21',
      },
    },
    mecha: {
      label: '机甲反应堆',
      pal: {
        bg: '#0E1116', bg2: '#121A24', surface: '#10161D', border: '#232D3A',
        text: '#E8F0F7', dim: '#6B7A8C', mut: '#3D4754',
        acc: '#FF6B35', acc2: '#00D1FF', acc3: '#9DB4C8',
        warn: '#FFB020', err: '#FF5347', track: '#161D27',
      },
    },
    stealth: {
      label: '潜行终端',
      pal: {
        bg: '#14171A', bg2: '#111417', surface: '#121518', border: '#28322E',
        text: '#D9E5DE', dim: '#5F6B66', mut: '#3A423E',
        acc: '#4ADE80', acc2: '#9CA3AF', acc3: '#A7C4B5',
        warn: '#D9B44A', err: '#FF5347', track: '#1D2422',
      },
    },
    arcade: {
      label: '街机计分板',
      pal: {
        bg: '#120A1C', bg2: '#1B0F2A', surface: '#170D24', border: '#3D2660',
        text: '#FFF6D6', dim: '#8A7AA8', mut: '#57496F',
        acc: '#FFCF00', acc2: '#FF3EA5', acc3: '#7EF9FF',
        warn: '#FFCF00', err: '#FF5347', track: '#2C1B42',
      },
    },
    drone: {
      label: '无人机链路',
      pal: {
        bg: '#07110D', bg2: '#0A1A14', surface: '#0A1610', border: '#1A3A2C',
        text: '#D6FFF0', dim: '#4E7A68', mut: '#2F4F42',
        acc: '#00FF9D', acc2: '#FFB020', acc3: '#7DFFD4',
        warn: '#FFB020', err: '#FF5347', track: '#12261E',
      },
    },
    scope: {
      label: '狙击镜',
      pal: {
        bg: '#07110D', bg2: '#08170F', surface: '#0A170F', border: '#1B3E2C',
        text: '#D8FFF0', dim: '#3F6B57', mut: '#27493A',
        acc: '#00FF9D', acc2: '#9FFFE0', acc3: '#FFB020',
        warn: '#FFB020', err: '#FF5347', track: '#122A1F',
      },
    },
    heatsink: {
      label: '散热塔',
      pal: {
        bg: '#0E1116', bg2: '#131A22', surface: '#10161D', border: '#232D3A',
        text: '#E8F0F7', dim: '#6B7A8C', mut: '#3D4754',
        acc: '#FF6B35', acc2: '#00D1FF', acc3: '#FFD166',
        warn: '#FFB020', err: '#FF5347', track: '#1B232E',
      },
    },
    loadout: {
      label: '装备槽',
      pal: {
        bg: '#0A0F0C', bg2: '#0E1712', surface: '#0D1712', border: '#22381F',
        text: '#D8F5D0', dim: '#5D7A5A', mut: '#3A5240',
        acc: '#7CFF4F', acc2: '#FFB020', acc3: '#3BE0C0',
        warn: '#FFB020', err: '#FF5347', track: '#1A2A1C',
      },
    },
    armor: {
      label: '动力装甲',
      pal: {
        bg: '#0E1116', bg2: '#131B24', surface: '#10161D', border: '#232D3A',
        text: '#E8F0F7', dim: '#6B7A8C', mut: '#3D4754',
        acc: '#FF6B35', acc2: '#00D1FF', acc3: '#FFD166',
        warn: '#FFB020', err: '#FF5347', track: '#1B232E',
      },
    },
    sweep: {
      label: '战术雷达',
      pal: {
        bg: '#0A0F0C', bg2: '#0D1611', surface: '#0B130E', border: '#22381F',
        text: '#D8F5D0', dim: '#5D7A5A', mut: '#3A5240',
        acc: '#7CFF4F', acc2: '#FFB020', acc3: '#3BE0C0',
        warn: '#FFB020', err: '#FF5347', track: '#1A2A1C',
      },
    },
  };
  const classicThemeId = () => {
    if (!isClassic()) return 'instrument';
    const t = state.layout && state.layout.classic_theme;
    return CLASSIC_THEME_META[t] ? t : 'instrument';
  };
  const classicThemeLabel = (id) =>
    (CLASSIC_THEME_META[id] || CLASSIC_THEME_META.instrument).label;

  let classicRoot = null;
  let classicRefs = {};
  let classicThemeCur = 'instrument';

  const CL_ARC_C = 2 * Math.PI * 44;
  const CL_ARC_LEN = CL_ARC_C * 0.75; /* 270° 弧长，缺口朝下 */

  /* 六个占用率：0 GPU 1 CPU 2 KV 3 MEM 4 HIT 5 ACC（与固件 pct_value 一致） */
  function classicPct(idx) {
    switch (idx) {
      case 0: return numberValue('host.gpu_util_pct');
      case 1: return numberValue('host.cpu_pct');
      case 2: return numberValue('llm.kv_pct');
      case 3: {
        const used = numberValue('host.ram_used_mb');
        const total = numberValue('host.ram_total_mb');
        return total > 0 && used !== null ? (used / total) * 100 : null;
      }
      case 4: return numberValue('llm.cache_token_hit_rate');
      case 5: return numberValue('llm.draft_accept_pct');
      default: return null;
    }
  }
  function fmtTokens(v) {
    if (v >= 1e6) return `${(v / 1e6).toFixed(1)}M`;
    if (v >= 1e3) return `${(v / 1e3).toFixed(1)}K`;
    return String(Math.round(v));
  }
  function classicValues() {
    const online = boolValue('llm.online');
    const busy = boolValue('llm.busy') === true;
    const phase =
      textValue('llm.phase') ||
      (online === true ? (busy ? 'BUSY' : 'IDLE')
        : online === false ? 'OFFLINE' : '--');
    const used = numberValue('llm.kv_used_tokens');
    const pool = numberValue('llm.kv_pool_positions');
    const vu = numberValue('host.gpu_vram_used_mb');
    const vt = numberValue('host.gpu_vram_total_mb');
    const vramOk = vu !== null && vt !== null && vt > 0;
    const memUsed = numberValue('host.ram_used_mb');
    const memTotal = numberValue('host.ram_total_mb');
    const memGb =
      memUsed !== null && memTotal !== null && memTotal > 0
        ? memUsed / 1024 : null;
    const ctx = numberValue('llm.context_len');
    return {
      model: textValue('llm.model') || '--',
      phase,
      online: online === true,
      busy,
      svc: [
        online === true,
        boolValue('llm.cache_enabled') === true,
        boolValue('llm.drafter_loaded') === true,
      ],
      kvText:
        used !== null && pool !== null && pool > 0
          ? `KV ${fmtTokens(used)} / ${fmtTokens(pool)}`
          : 'KV --',
      conc: numberValue('llm.slots'),
      queue: numberValue('llm.requests_deferred'),
      temp: numberValue('host.gpu_temp_c'),
      power: numberValue('host.gpu_power_w'),
      decode: numberValue('llm.decode_tps'),
      prefill: numberValue('llm.prefill_tps'),
      davg: numberValue('llm.decode_avg'),
      dpeak: numberValue('llm.decode_peak'),
      vramOk,
      vramPct: vramOk ? (vu / vt) * 100 : null,
      vramText:
        vramOk ? `${(vu / 1024).toFixed(1)}/${(vt / 1024).toFixed(0)}G` : '--',
      memGb,
      memText:
        memUsed !== null && memTotal !== null
          ? `${(memUsed / 1024).toFixed(1)}/${(memTotal / 1024).toFixed(0)}G`
          : '--',
      ctxText: ctx !== null ? fmtTokens(ctx) : '--',
      cacheText: (() => {
        const cbytes = numberValue('llm.cache_bytes');
        const ccap = numberValue('llm.cache_cap_mb');
        if (cbytes !== null && ccap !== null && ccap > 0) {
          return `${Math.round(cbytes)}/${Math.round(ccap)}M`;
        }
        const cent = numberValue('llm.cache_entries');
        return cent !== null ? `${Math.round(cent)} ENT` : '--';
      })(),
      draftTokens: numberValue('llm.draft_tokens'),
    };
  }

  function ensureClassicStyle() {
    if (document.getElementById('classic-preview-style')) return;
    const style = document.createElement('style');
    style.id = 'classic-preview-style';
    style.textContent = `
      .classic-preview{position:absolute;inset:0;overflow:hidden;border-radius:inherit;
        font-family:'Montserrat','Segoe UI',system-ui,sans-serif;
        font-variant-numeric:tabular-nums;letter-spacing:.4px;user-select:none;}
      .classic-preview .cl-abs{position:absolute;line-height:1.15;white-space:nowrap;}
      .classic-preview .cl-card{position:absolute;border-radius:2px;}
      .classic-preview .cl-corners>i{position:absolute;width:9px;height:9px;border:0 solid;pointer-events:none;}
      .classic-preview .cl-led{position:absolute;border-radius:50%;}
      .classic-preview .cl-track{position:absolute;border-radius:99px;}
      .classic-preview .cl-fill{position:absolute;left:0;top:0;height:100%;border-radius:99px;}
      .classic-preview .cl-seg{position:absolute;display:flex;gap:2px;}
      .classic-preview .cl-seg>i{flex:none;border-radius:1px;}
      .classic-preview .cl-ellip{overflow:hidden;text-overflow:ellipsis;}
    `;
    document.head.appendChild(style);
  }

  const clLbl = (x, y, text, color, size, opt = {}) => {
    const style = [
      `left:${x}px`, `top:${y}px`, `color:${color}`, `font-size:${size}px`,
      opt.w ? `width:${opt.w}px` : '',
      opt.center ? 'text-align:center' : '',
      opt.right ? 'text-align:right' : '',
      opt.ellip ? 'overflow:hidden;text-overflow:ellipsis' : '',
      opt.bold ? 'font-weight:600' : '',
    ].filter(Boolean).join(';');
    const ref = opt.ref ? ` data-ref="${opt.ref}"` : '';
    return `<div class="cl-abs${opt.ellip ? ' cl-ellip' : ''}"${ref} style="${style}">${escHtml(text)}</div>`;
  };
  const clCorners = (color) => `
    <i style="left:3px;top:3px;border-left-width:1px;border-top-width:1px;border-color:${color}"></i>
    <i style="right:3px;top:3px;border-right-width:1px;border-top-width:1px;border-color:${color}"></i>
    <i style="left:3px;bottom:3px;border-left-width:1px;border-bottom-width:1px;border-color:${color}"></i>
    <i style="right:3px;bottom:3px;border-right-width:1px;border-bottom-width:1px;border-color:${color}"></i>`;
  const clCard = (x, y, w, h, pal, inner = '', corners = false) => `
    <div class="cl-card${corners ? ' cl-corners' : ''}"
      style="left:${x}px;top:${y}px;width:${w}px;height:${h}px;background:${pal.surface};border:1px solid ${pal.border}">
      ${corners ? clCorners(pal.acc) : ''}${inner}
    </div>`;
  const clLed = (x, y, size, color, ref) =>
    `<i class="cl-led"${ref ? ` data-ref="${ref}"` : ''} style="left:${x}px;top:${y}px;width:${size}px;height:${size}px;background:${color}"></i>`;
  const clBar = (x, y, w, h, pal, ref) => `
    <div class="cl-track" style="left:${x}px;top:${y}px;width:${w}px;height:${h}px;background:${pal.track}">
      <div class="cl-fill" data-ref="${ref}" style="width:0%;background:${pal.acc}"></div>
    </div>`;
  const clSeg = (x, y, w, h, pal, ref) => {
    const gap = 2;
    const sw = Math.floor((w - (10 - 1) * gap) / 10);
    let segs = '';
    for (let i = 0; i < 10; i += 1) {
      segs += `<i style="width:${sw}px;height:${h}px;background:${pal.track}"></i>`;
    }
    return `<div class="cl-seg" data-ref="${ref}" style="left:${x}px;top:${y}px;gap:${gap}px">${segs}</div>`;
  };
  const clArc = (x, y, size, pal, opt = {}) => {
    const ref = opt.ref || 'arcind';
    const sw = opt.sw || 8;
    const rot = opt.full ? '' : ' transform="rotate(135 50 50)"';
    const trackDash = opt.full ? `${CL_ARC_C} ${CL_ARC_C}` : `${CL_ARC_LEN} ${CL_ARC_C}`;
    return `
    <svg class="cl-abs" style="left:${x}px;top:${y}px" width="${size}" height="${size}" viewBox="0 0 100 100">
      <circle cx="50" cy="50" r="44" fill="none" stroke="${pal.track}" stroke-width="${sw}"
        stroke-linecap="round" stroke-dasharray="${trackDash}"${rot}></circle>
      <circle data-ref="${ref}" cx="50" cy="50" r="44" fill="none" stroke="${pal.acc}" stroke-width="${sw}"
        stroke-linecap="round" stroke-dasharray="0 ${CL_ARC_C}"${rot}></circle>
    </svg>`;
  };
  const clRect = (x, y, w, h, color, opa = 1) =>
    `<i class="cl-abs" style="left:${x}px;top:${y}px;width:${w}px;height:${h}px;background:${color};opacity:${opa}"></i>`;
  const clBox = (x, y, w, h, color) =>
    `<i class="cl-abs" style="left:${x}px;top:${y}px;width:${w}px;height:${h}px;border:1px solid ${color}"></i>`;
  const clCircle = (x, y, size, color) =>
    `<svg class="cl-abs" style="left:${x}px;top:${y}px" width="${size}" height="${size}">
      <circle cx="${size / 2}" cy="${size / 2}" r="${size / 2 - 1}" fill="none" stroke="${color}" stroke-width="1"></circle>
    </svg>`;
  /* 可变段数横排分段条 */
  const clSegN = (x, y, w, h, pal, ref, n) => {
    const gap = 2;
    const sw = Math.floor((w - (n - 1) * gap) / n);
    let segs = '';
    for (let i = 0; i < n; i += 1) {
      segs += `<i style="width:${sw}px;height:${h}px;background:${pal.track}"></i>`;
    }
    return `<div class="cl-seg" data-ref="${ref}" style="left:${x}px;top:${y}px;gap:${gap}px">${segs}</div>`;
  };
  /* 可变段数纵向能量柱（第 0 段在底部，与固件一致） */
  const clSegNV = (x, y, w, h, pal, ref, n) => {
    const gap = 2;
    const sh = Math.floor((h - (n - 1) * gap) / n);
    let segs = '';
    for (let i = 0; i < n; i += 1) {
      segs += `<i style="width:${w}px;height:${sh}px;background:${pal.track}"></i>`;
    }
    return `<div class="cl-seg" data-ref="${ref}" style="left:${x}px;top:${y}px;flex-direction:column-reverse;gap:${gap}px">${segs}</div>`;
  };

  function classicInstrumentHtml(p) {
    const labels = ['GPU', 'CPU', 'KV', 'MEM', 'HIT', 'ACC'];
    let rows = '';
    for (let i = 0; i < 6; i += 1) {
      const y = 28 + i * 23;
      rows += clLbl(12, y, labels[i], p.dim, 12);
      rows += clBar(56, y + 3, 170, 8, p, `bar${i}`);
      rows += clLbl(236, y, '--', p.text, 13, { w: 48, right: true, ref: `pctv${i}` });
    }
    let svc = '';
    for (let i = 0; i < 3; i += 1) {
      const y = 26 + i * 23;
      svc += clLed(14, y + 3, 7, p.mut, `svcled${i}`);
      svc += clLbl(28, y, ['LLM', 'PFX', 'DRF'][i], p.dim, 12);
      svc += clLbl(66, y, 'OFF', p.mut, 12, { ref: `svcst${i}` });
    }
    return (
      clCard(0, 0, 480, 28, p) +
      clLed(10, 10, 8, p.mut, 'led_main') +
      clLbl(24, 6, 'INFERENCE SCOPE', p.acc, 13, { bold: true }) +
      clLbl(192, 7, '--', p.acc3, 12, { w: 130, ellip: true, ref: 'model_top' }) +
      clLbl(330, 7, '--', p.dim, 12, { w: 96, right: true, ref: 'phase_top' }) +
      clLbl(440, 6, 'RUN', p.acc, 12) +
      clCard(10, 34, 296, 168, p,
        clLbl(12, 6, 'RESOURCE LOAD', p.dim, 12) + rows) +
      clCard(314, 34, 156, 168, p,
        clLbl(12, 6, 'GPU LOAD', p.dim, 12) +
        clArc(30, 22, 96, p) +
        clLbl(30, 54, '--', p.text, 22, { w: 96, center: true, ref: 'arcv' }) +
        clLbl(14, 124, 'TEMP', p.dim, 12) +
        clLbl(14, 140, '--', p.text, 17, { ref: 'temp' }) +
        clLbl(86, 124, 'PWR', p.dim, 12) +
        clLbl(86, 140, '--', p.text, 17, { ref: 'power' })) +
      clCard(10, 210, 460, 100, p,
        clLbl(12, 6, 'SERVICES', p.dim, 12) + svc +
        clLbl(150, 6, 'MODEL', p.dim, 12) +
        clLbl(150, 24, '--', p.acc2, 17, { w: 130, ellip: true, ref: 'model' }) +
        clLbl(300, 6, 'CONC', p.dim, 12) +
        clLbl(300, 24, '--', p.text, 22, { ref: 'conc' }) +
        clLbl(372, 6, 'QUEUE', p.dim, 12) +
        clLbl(372, 24, '--', p.text, 22, { ref: 'queue' }) +
        clLbl(150, 62, 'KV POOL', p.dim, 12) +
        clLbl(150, 80, 'KV --', p.text, 13, { ref: 'kvtext' }))
    );
  }

  function classicHudHtml(p) {
    const labels = ['CPU', 'MEM', 'HIT', 'ACC'];
    const bIdx = [1, 3, 4, 5];
    let loadRows = '';
    for (let i = 0; i < 4; i += 1) {
      const y = 26 + i * 26;
      const idx = bIdx[i];
      loadRows += clLbl(14, y, labels[i], p.dim, 12);
      loadRows += clSeg(50, y + 2, 70, 10, p, `seg${idx}`);
      loadRows += clLbl(124, y, '--', p.text, 12, { w: 36, right: true, ref: `pctv${idx}` });
    }
    let svc = '';
    for (let i = 0; i < 3; i += 1) {
      const y = 26 + i * 24;
      svc += clLed(12, y + 3, 7, p.mut, `svcled${i}`);
      svc += clLbl(26, y, ['LLM', 'PFX', 'DRF'][i], p.dim, 12);
      svc += clLbl(62, y, 'OFF', p.mut, 12, { ref: `svcst${i}` });
    }
    return (
      clCard(0, 0, 480, 26, p) +
      clLed(9, 9, 8, p.mut, 'led_main') +
      clLbl(22, 5, 'TACTICAL HUD', p.acc, 13, { bold: true }) +
      clLbl(152, 6, '// --', p.text, 12, { w: 140, ellip: true, ref: 'model_top' }) +
      clLbl(300, 6, '--', p.dim, 12, { w: 100, right: true, ref: 'phase_top' }) +
      clLbl(416, 5, 'ONLINE', p.acc, 12) +
      clCard(8, 30, 140, 134, p,
        clLbl(16, 6, 'GPU', p.dim, 12) +
        clArc(24, 24, 92, p) +
        clLbl(24, 56, '--', p.acc, 22, { w: 92, center: true, ref: 'arcv' }), true) +
      clCard(154, 30, 166, 134, p,
        clLbl(16, 6, 'LOAD', p.dim, 12) + loadRows, true) +
      clCard(326, 30, 146, 134, p,
        clLbl(16, 6, 'THERMAL', p.dim, 12) +
        clLbl(10, 24, '--', p.acc2, 25, { ref: 'temp' }) +
        clLbl(78, 38, 'C', p.dim, 12) +
        clLbl(10, 60, '--', p.acc3, 25, { ref: 'power' }) +
        clLbl(92, 74, 'W', p.dim, 12) +
        clLbl(10, 100, 'KV', p.dim, 12) +
        clSeg(36, 102, 70, 8, p, 'seg2') +
        clLbl(112, 100, '--', p.text, 12, { w: 28, right: true, ref: 'pctv2' }), true) +
      clCard(8, 170, 240, 142, p,
        clLbl(16, 6, 'SERVICES', p.dim, 12) + svc +
        clLbl(10, 104, 'MODEL', p.dim, 12) +
        clLbl(66, 104, '--', p.text, 13, { w: 164, ellip: true, ref: 'model' }), true) +
      clCard(254, 170, 218, 142, p,
        clLbl(16, 6, 'THROUGHPUT', p.dim, 12) +
        clLbl(10, 28, '--', p.acc, 31, { ref: 'conc' }) +
        clLbl(10, 72, 'CONC', p.dim, 12) +
        clLbl(118, 28, '--', p.acc2, 31, { ref: 'queue' }) +
        clLbl(118, 72, 'QUEUE', p.dim, 12) +
        clLbl(10, 108, 'KV --', p.dim, 12, { ref: 'kvtext' }), true)
    );
  }

  /* ==================== 主题预览生成器（H02–H10，镜像固件坐标） ==================== */

  /* 通用顶栏（左标题 + model + phase + 右缀） */
  function clTop(p, o) {
    return (
      clLed(o.led[0], o.led[1], 7, p.mut, 'led_main') +
      clLbl(o.title[0], o.title[1], o.title[2], o.titleColor, 13, { bold: true }) +
      (o.model
        ? clLbl(o.model[0], o.model[1], '// --', p.dim, 12,
          { w: o.model[2], ellip: true, ref: 'model_top' })
        : '') +
      clLbl(o.phase[0], o.phase[1], '--', p.dim, 12,
        { w: o.phase[2], right: true, ref: 'phase_top' }) +
      (o.extra || '')
    );
  }

  function classicMechaHtml(p) {
    let lrows = '';
    for (let i = 0; i < 4; i += 1) {
      const y = 64 + i * 50;
      lrows += clLbl(14, y + 1, ['PREFILL', 'SLOTS', 'VRAM', 'KV'][i], p.dim, 12);
      lrows += clLbl(40, y - 2, '--', p.text, 13, { w: 102, right: true, ref: `rowv${i}` });
      lrows += clBar(14, y + 22, 128, 4, p, `rowbar${i}`);
    }
    let rrows = '';
    for (let i = 0; i < 4; i += 1) {
      const y = 64 + i * 50;
      rrows += clLbl(338, y + 1, ['QUEUE', 'GPU', 'TEMP', 'PWR'][i], p.dim, 12);
      rrows += clLbl(364, y - 2, '--', p.text, 13, { w: 102, right: true, ref: `rowv${i + 4}` });
      rrows += clBar(338, y + 22, 128, 4, p, `rowbar${i + 4}`);
    }
    return (
      clTop(p, {
        led: [10, 9], title: [24, 5, 'MECHA CORE'], titleColor: p.acc,
        model: [120, 6, 150], phase: [264, 6, 72],
        extra: clLbl(386, 6, 'SYS', p.dim, 12) +
          clLed(354, 9, 8, p.mut, 'svcled1') + clLed(372, 9, 8, p.mut, 'svcled2') +
          clLbl(404, 6, 'UNIT-07', p.acc2, 12, { w: 72, right: true }),
      }) +
      clSegN(18, 44, 444, 4, p, 'seg0', 56) +
      clArc(145, 62, 190, p, { sw: 7 }) +
      clArc(174, 91, 132, p, { ref: 'arcind2', sw: 3 }) +
      clLbl(190, 124, 'DECODE', p.dim, 12, { w: 100, center: true }) +
      clLbl(190, 142, '--', p.text, 36, { w: 100, center: true, ref: 'big0' }) +
      clLbl(190, 186, 'TOK/S', p.acc, 12, { w: 100, center: true }) +
      lrows + rrows +
      clRect(18, 282, 444, 1, p.border) +
      clLbl(20, 290, '--', p.dim, 12, { ref: 'txt0' }) +
      clLed(396, 293, 7, p.mut, 'svcled0') +
      clLbl(410, 290, 'STABLE', p.acc, 12, { ref: 'svcst0' })
    );
  }

  function classicStealthHtml(p) {
    let rows = '';
    for (let i = 0; i < 4; i += 1) {
      const y = 64 + i * 30;
      rows += clLbl(262, y + 2, ['AVG', 'PREFILL', 'SLOTS', 'QUEUE'][i], p.dim, 12);
      rows += clLbl(330, y, '--', p.text, 13, { w: 118, right: true, ref: `rowv${i}` });
      rows += clRect(262, y + 24, 186, 1, p.border);
    }
    return (
      clCard(10, 10, 460, 300, p, '', false) +
      clLed(448, 16, 7, p.mut, 'led_main') +
      clLbl(24, 14, 'STEALTH OPS', p.acc2, 13, { bold: true }) +
      clLbl(160, 15, '--', p.dim, 12, { w: 150, ellip: true, ref: 'model_top' }) +
      clLbl(330, 15, '--', p.dim, 12, { w: 108, right: true, ref: 'phase_top' }) +
      clRect(24, 38, 432, 1, p.border) +
      clLbl(32, 58, 'DECODE', p.dim, 12) +
      clLbl(32, 78, '--', p.text, 43, { ref: 'big0' }) +
      clLbl(32, 134, 'TOK/S', p.dim, 12) +
      clLbl(32, 156, 'AVG --', p.acc, 12, { ref: 'txt0' }) +
      clLbl(32, 186, 'LOW OBSERVABILITY LINK', p.dim, 12) +
      rows +
      clRect(32, 204, 416, 1, p.border) +
      clLbl(32, 214, 'CPU --', p.text, 12, { ref: 'txt1' }) +
      clLbl(136, 214, 'GPU --', p.text, 12, { ref: 'txt2' }) +
      clLbl(240, 214, 'RAM --', p.text, 12, { ref: 'txt3' }) +
      clLbl(344, 214, 'KV --', p.text, 12, { ref: 'txt4' }) +
      clLbl(32, 244, 'THERMAL LOAD', p.dim, 12) +
      clLbl(356, 240, '--', p.text, 17, { ref: 'temp' }) +
      clSeg(32, 268, 416, 8, p, 'seg0') +
      clLbl(32, 288, 'SILENT MODE • NO UPLINK', p.dim, 12, { ref: 'txt5' }) +
      clLed(424, 292, 6, p.mut, 'svcled0') +
      clRect(438, 292, 6, 6, p.track) +
      clRect(452, 292, 6, 6, p.track) +
      clRect(456, 292, 6, 6, p.track)
    );
  }

  function classicArcadeHtml(p) {
    let mid = '';
    for (let i = 0; i < 8; i += 1) {
      mid += clRect(244, 56 + i * 16, 4, 12,
        i % 3 === 0 ? p.acc2 : p.track);
    }
    let bots = '';
    for (let i = 0; i < 3; i += 1) {
      const x = 18 + i * 152;
      bots += clLbl(x, 226, ['KV', 'MEM', 'HIT'][i], p.acc2, 12);
      bots += clLbl(x + 70, 224, '--', p.text, 13, { w: 66, right: true, ref: `rowv${i}` });
      bots += clSegN(x, 246, 136, 10, p, `seg${i + 2}`, 12);
    }
    return (
      clCard(0, 0, 480, 34, { surface: p.bg2, border: p.bg2, acc: p.acc }) +
      clLbl(18, 9, 'PLAYER 1', p.acc2, 12) +
      clLbl(150, 9, '--', p.acc, 12, { w: 180, center: true, ellip: true, ref: 'model_top' }) +
      clLbl(350, 9, '--', p.acc2, 12, { w: 112, right: true, ref: 'phase_top' }) +
      clRect(0, 34, 480, 2, p.acc) +
      clRect(18, 44, 7, 7, p.acc) +
      clRect(29, 44, 7, 7, p.acc2) +
      clRect(40, 44, 7, 7, p.track) +
      clLbl(52, 56, 'CPU', p.acc2, 12, { w: 168, center: true }) +
      clLbl(52, 72, '--', p.acc, 43, { w: 168, center: true, ref: 'big0' }) +
      clLbl(52, 128, 'PROCESSOR', p.dim, 12, { w: 168, center: true }) +
      clSegN(98, 150, 76, 12, p, 'seg0', 5) +
      mid +
      clLbl(260, 56, 'GPU', p.acc, 12, { w: 168, center: true }) +
      clLbl(260, 72, '--', p.acc2, 43, { w: 168, center: true, ref: 'big1' }) +
      clLbl(260, 128, 'GRAPHICS', p.dim, 12, { w: 168, center: true }) +
      clSegN(306, 150, 76, 12, p, 'seg1', 5) +
      clCard(18, 192, 444, 24, p,
        clLbl(8, 6, 'KV --', p.text, 12, { ref: 'txt0' }) +
        clLbl(118, 6, 'T --', p.text, 12, { ref: 'txt1' }) +
        clLbl(228, 6, 'P --', p.text, 12, { ref: 'txt2' }) +
        clLbl(338, 6, 'VRAM --', p.text, 12, { ref: 'txt3' })) +
      bots +
      clRect(18, 272, 444, 1, p.border) +
      clLbl(22, 282, 'KV --', p.dim, 12, { ref: 'kvtext' }) +
      clLbl(210, 282, '--', p.dim, 12, { ref: 'txt4' }) +
      clLbl(300, 282, 'PRESS START', p.acc2, 12, { w: 158, right: true, ref: 'press' })
    );
  }

  function classicDroneHtml(p) {
    let lrows = '';
    for (let i = 0; i < 3; i += 1) {
      const y = 108 + i * 46;
      lrows += clLbl(14, y + 2, ['GPU', 'TEMP', 'VRAM'][i], p.dim, 12);
      lrows += clLbl(20, y - 2, '--', p.text, 13, { w: 98, right: true, ref: `rowv${i}` });
      lrows += clBar(14, y + 22, 104, 4, p, `rowbar${i}`);
    }
    let rrows = '';
    for (let i = 0; i < 3; i += 1) {
      const y = 108 + i * 46;
      rrows += clLbl(362, y + 2, ['QUEUE', 'ACC', 'HIT'][i], p.dim, 12);
      rrows += clLbl(368, y - 2, '--', p.text, 13, { w: 98, right: true, ref: `rowv${i + 3}` });
      rrows += clBar(362, y + 22, 104, 4, p, `rowbar${i + 3}`);
    }
    return (
      clCard(0, 0, 480, 26, p) +
      clLed(18, 10, 7, p.mut, 'led_main') +
      clLbl(30, 5, 'DRONE LINK', p.acc, 13, { bold: true }) +
      clLbl(150, 6, '// --', p.dim, 12, { w: 130, ellip: true, ref: 'model_top' }) +
      clLbl(292, 6, 'LINK', p.dim, 12) +
      clLbl(322, 6, '--', p.dim, 12, { w: 72, right: true, ref: 'phase_top' }) +
      clLbl(396, 6, 'SAT 12', p.dim, 12) +
      clRect(18, 34, 444, 1, p.border) +
      clLbl(140, 40, 'DECODE', p.dim, 12, { w: 208, center: true }) +
      clLbl(140, 56, '--', p.text, 36, { w: 208, center: true, ref: 'big0' }) +
      clLbl(140, 100, 'TOK/S', p.dim, 12, { w: 208, center: true }) +
      clRect(209, 147, 70, 1, p.acc, 0.4) +
      clRect(209, 167, 70, 1, p.acc, 0.4) +
      clRect(144, 177, 200, 2, p.acc, 0.8) +
      clRect(209, 191, 70, 1, p.acc, 0.4) +
      clRect(209, 211, 70, 1, p.acc, 0.4) +
      clRect(214, 170, 60, 1, p.acc2) +
      clLbl(140, 240, 'HDG -- • PITCH --', p.dim, 12, { w: 208, center: true, ref: 'txt0' }) +
      clLbl(140, 258, 'WP 04 • 1.8KM', p.dim, 12, { w: 120, ref: 'txt1' }) +
      clLbl(250, 258, 'STANDBY', p.acc, 12, { w: 104, right: true, ref: 'svcst0' }) +
      clSeg(144, 276, 200, 5, p, 'seg0') +
      clLbl(14, 88, 'TELEMETRY / L', p.acc, 12) +
      lrows +
      clLbl(362, 88, 'TELEMETRY / R', p.acc, 12) +
      rrows +
      clRect(130, 92, 1, 162, p.border) +
      clRect(354, 92, 1, 162, p.border) +
      clRect(18, 290, 444, 1, p.border) +
      clLbl(20, 298, 'KV --', p.dim, 12, { ref: 'kvtext' }) +
      clLbl(200, 298, 'CTX --', p.dim, 12, { ref: 'txt2' }) +
      clLbl(330, 298, 'AUTO', p.acc, 12)
    );
  }

  function classicScopeHtml(p) {
    return (
      clLbl(20, 10, 'SNIPER SCOPE', p.acc, 13, { bold: true }) +
      clLed(446, 14, 7, p.mut, 'led_main') +
      clLbl(356, 11, '--', p.dim, 12, { w: 76, right: true, ref: 'phase_top' }) +
      clRect(20, 34, 440, 1, p.border) +
      clLbl(24, 44, 'GPU', p.dim, 12) +
      clLbl(24, 62, '--', p.text, 17, { ref: 'rowv0' }) +
      clLbl(366, 44, 'MEM', p.dim, 12, { w: 90, right: true }) +
      clLbl(366, 62, '--', p.text, 17, { w: 90, right: true, ref: 'rowv1' }) +
      clArc(145, 62, 190, p, { full: true, sw: 4 }) +
      clRect(150, 156, 64, 1, p.acc, 0.6) +
      clRect(266, 156, 64, 1, p.acc, 0.6) +
      clRect(239, 70, 1, 64, p.acc, 0.6) +
      clRect(239, 184, 1, 64, p.acc, 0.6) +
      clCircle(206, 123, 68, p.border) +
      clLbl(140, 136, '--', p.acc, 36, { w: 200, center: true, ref: 'big0' }) +
      clLbl(140, 182, 'CPU LOAD', p.dim, 12, { w: 200, center: true }) +
      clLbl(24, 268, 'THERMAL', p.dim, 12) +
      clLbl(100, 266, '--', p.text, 17, { ref: 'temp' }) +
      clSeg(24, 292, 180, 9, p, 'seg0') +
      clLbl(240, 268, 'PWR', p.dim, 12) +
      clLbl(240, 286, '--', p.text, 17, { ref: 'power' }) +
      clLbl(390, 268, 'KV', p.dim, 12, { w: 66, right: true }) +
      clLbl(390, 286, '--', p.text, 17, { w: 66, right: true, ref: 'rowv4' }) +
      clLbl(24, 306, 'KV --', p.dim, 12, { ref: 'kvtext' }) +
      clLbl(360, 306, 'STABLE', p.acc, 12, { w: 96, right: true, ref: 'svcst0' })
    );
  }

  function classicHeatsinkHtml(p) {
    let cols = '';
    for (let i = 0; i < 3; i += 1) {
      const x = 30 + i * 150;
      cols += clLbl(x, 132, ['GPU', 'VRAM', 'KV'][i], p.dim, 12);
      cols += clLbl(x, 150, '--', p.text, 17, { ref: `rowv${i}` });
      cols += clSegNV(x + 23, 178, 64, 112, p, `vseg${i}`, 10);
      cols += clLbl(x, 300, '--', p.dim, 12, { ref: `txt${i + 4}` });
    }
    return (
      clLbl(20, 6, 'HEAT SINK', p.acc, 13, { bold: true }) +
      clLed(404, 10, 7, p.mut, 'led_main') +
      clLbl(300, 7, '--', p.dim, 12, { w: 96, right: true, ref: 'phase_top' }) +
      clRect(20, 32, 440, 1, p.border) +
      clLbl(22, 40, 'DECODE', p.dim, 12) +
      clLbl(22, 58, '--', p.text, 43, { ref: 'big0' }) +
      clLbl(126, 84, 'TOK/S', p.dim, 12) +
      clLbl(22, 114, '--', p.acc2, 12, { ref: 'txt0' }) +
      clLbl(350, 42, 'AVG --', p.dim, 12, { w: 108, right: true, ref: 'txt1' }) +
      clLbl(350, 60, 'MAX --', p.dim, 12, { w: 108, right: true, ref: 'txt2' }) +
      clLbl(350, 78, 'GPU --', p.acc, 12, { w: 108, right: true, ref: 'txt3' }) +
      cols
    );
  }

  function classicLoadoutHtml(p) {
    const nums = ['01', '02', '03', '04'];
    const names = ['CPU', 'GPU', 'MEMORY', 'CACHE'];
    const descs = ['RYZEN 9 7950X', 'RTX 4070 TI', 'DDR5 6000 32G', 'TOKEN HIT RATE'];
    const cx = [14, 254, 14, 254];
    const cy = [40, 40, 172, 172];
    let cards = '';
    for (let i = 0; i < 4; i += 1) {
      const color = i % 2 === 0 ? p.acc : p.acc2;
      cards += clCard(cx[i], cy[i], 226, 126, p,
        clBox(12, 46, 36, 36, p.border) +
        clRect(24, 58, 12, 12, color, 0.7) +
        clLbl(58, 10, nums[i], p.dim, 12) +
        clLbl(78, 8, names[i], p.text, 13) +
        clLbl(58, 28, descs[i], p.dim, 12) +
        clLbl(58, 48, '--', p.dim, 12, { w: 104, ref: `txt${i}` }) +
        clLbl(148, 10, '--', color, 21, { w: 66, right: true, ref: `rowv${i}` }) +
        clSeg(58, 98, 150, 7, p, `seg${i}`), true);
    }
    return (
      clLbl(20, 6, 'LOADOUT', p.acc, 13, { bold: true }) +
      clLed(404, 10, 7, p.mut, 'led_main') +
      clLbl(300, 7, '--', p.dim, 12, { w: 96, right: true, ref: 'phase_top' }) +
      clRect(20, 32, 440, 1, p.border) +
      cards +
      clLbl(22, 306, '--', p.dim, 12, { ref: 'txt4' }) +
      clLbl(360, 306, 'ALL OK', p.acc, 12, { w: 96, right: true, ref: 'svcst0' })
    );
  }

  function classicArmorHtml(p) {
    return (
      clLbl(20, 6, 'POWER ARMOR', p.acc2, 13, { bold: true }) +
      clLed(404, 10, 7, p.mut, 'led_main') +
      clLbl(310, 7, '--', p.acc, 12, { w: 86, right: true, ref: 'phase_top' }) +
      clLbl(20, 28, 'FUSION CORE / MK-VII', p.dim, 12) +
      clLbl(24, 48, 'PREFILL', p.dim, 12) +
      clLbl(24, 66, '--', p.text, 17, { ref: 'rowv0' }) +
      clLbl(356, 48, 'VRAM', p.dim, 12, { w: 100, right: true }) +
      clLbl(356, 66, '--', p.text, 17, { w: 100, right: true, ref: 'rowv1' }) +
      clLbl(24, 248, 'TEMP', p.dim, 12) +
      clLbl(24, 266, '--', p.text, 17, { ref: 'temp' }) +
      clLbl(356, 248, 'QUEUE', p.dim, 12, { w: 100, right: true }) +
      clLbl(356, 266, '--', p.text, 17, { w: 100, right: true, ref: 'queue' }) +
      clArc(140, 56, 200, p, { sw: 6 }) +
      clArc(172, 88, 136, p, { ref: 'arcind2', sw: 3 }) +
      clLbl(190, 122, 'DECODE', p.dim, 12, { w: 100, center: true }) +
      clLbl(190, 140, '--', p.text, 36, { w: 100, center: true, ref: 'big0' }) +
      clLbl(190, 184, 'TOK/S', p.acc, 12, { w: 100, center: true }) +
      clBar(24, 296, 432, 4, p, 'rowbar0') +
      clLbl(24, 306, 'KV --', p.dim, 12, { ref: 'kvtext' }) +
      clLbl(356, 306, 'ACC --', p.acc2, 12, { w: 100, right: true, ref: 'svcst0' })
    );
  }

  function classicSweepHtml(p) {
    const cells = ['CPU', 'MEM', 'GPU', 'HIT'];
    let cards = '';
    for (let i = 0; i < 4; i += 1) {
      const color = i === 2 ? p.acc2 : p.acc;
      cards += clCard(14 + i * 118, 248, 106, 52, p,
        clLbl(6, 6, cells[i], p.dim, 12) +
        clLbl(40, 4, '--', color, 13, { w: 60, right: true, ref: `rowv${i}` }) +
        clSeg(6, 32, 94, 8, p, `seg${i}`));
    }
    return (
      clLbl(20, 6, 'SWEEP HUD', p.acc, 13, { bold: true }) +
      clLbl(150, 7, '// --', p.dim, 12, { w: 140, ellip: true, ref: 'model_top' }) +
      clLed(404, 10, 7, p.mut, 'led_main') +
      clLbl(310, 7, '--', p.dim, 12, { w: 86, right: true, ref: 'phase_top' }) +
      clLbl(22, 30, 'RADAR / PC-01', p.dim, 12) +
      clLbl(300, 30, 'GAIN 12 • R 5M', p.dim, 12, { w: 158, right: true }) +
      clCircle(190, 46, 200, p.border) +
      clCircle(224, 80, 132, p.track) +
      clCircle(257, 113, 66, p.track) +
      clRect(196, 145, 188, 1, p.track) +
      clRect(289, 52, 1, 188, p.track) +
      clRect(294, 144, 82, 3, p.acc, 0.6) +
      clLed(286, 142, 8, p.acc, 'radar_main') +
      clLed(250, 88, 6, p.acc) +
      clLed(330, 118, 6, p.acc2) +
      clLed(238, 188, 5, p.acc) +
      clLed(338, 198, 5, p.acc2) +
      clLed(268, 160, 5, p.acc) +
      cards +
      clLbl(22, 306, '--', p.dim, 12, { ref: 'txt0' }) +
      clLbl(300, 306, '--', p.acc, 12, { w: 158, right: true, ref: 'txt1' })
    );
  }

  const CLASSIC_HTML = {
    instrument: classicInstrumentHtml,
    hud: classicHudHtml,
    mecha: classicMechaHtml,
    stealth: classicStealthHtml,
    arcade: classicArcadeHtml,
    drone: classicDroneHtml,
    scope: classicScopeHtml,
    heatsink: classicHeatsinkHtml,
    loadout: classicLoadoutHtml,
    armor: classicArmorHtml,
    sweep: classicSweepHtml,
  };

  function renderClassicCanvas() {
    ensureClassicStyle();
    const id = classicThemeId();
    const meta = CLASSIC_THEME_META[id];
    canvas.innerHTML = '';
    canvas.style.width = '480px';
    canvas.style.height = '320px';
    applyTheme();
    const wrap = document.createElement('div');
    wrap.className = 'classic-preview';
    wrap.style.background =
      `linear-gradient(180deg, ${meta.pal.bg} 0%, ${meta.pal.bg2} 100%)`;
    wrap.style.color = meta.pal.text;
    wrap.innerHTML = CLASSIC_HTML[id](meta.pal);
    canvas.appendChild(wrap);
    classicRoot = wrap;
    classicThemeCur = id;
    classicRefs = {};
    wrap.querySelectorAll('[data-ref]').forEach((el) => {
      classicRefs[el.dataset.ref] = el;
    });
    classicApply();
    fitCanvas();
    requestAnimationFrame(fitCanvas);
  }

  function fitClassicCanvas() {
    const mobile = window.matchMedia('(max-width: 860px)').matches;
    const availW = Math.max(220, canvasScroll.clientWidth - 16);
    const availH = Math.max(
      280,
      (canvasScroll.clientHeight || window.innerHeight * 0.6) - 16
    );
    const rawScale = mobile ? availW / 480 : Math.min(availW / 480, availH / 320);
    const scale = Math.max(0.45, Math.min(mobile ? 1.4 : 1.8, rawScale));
    canvas.style.width = '480px';
    canvas.style.height = '320px';
    canvas.style.transform = `scale(${scale})`;
    canvasWrap.style.width = `${480 * scale}px`;
    canvasWrap.style.height = `${320 * scale}px`;
    $('canvas-title').textContent =
      `传统框架 · ${classicThemeLabel(classicThemeCur)} · 480 × 320 固定界面`;
  }

  /* 数值刷新：与固件 ui_classic_apply 同逻辑 */
  function classicApply() {
    if (!classicRoot || !isClassic()) return;
    const p = CLASSIC_THEME_META[classicThemeCur].pal;
    const id = classicThemeCur;
    const set = (ref, text, color) => {
      const el = classicRefs[ref];
      if (!el) return;
      if (text !== undefined) el.textContent = text;
      if (color) el.style.color = color;
    };
    const vals = classicValues();
    const pcts = [0, 1, 2, 3, 4, 5].map(classicPct);
    const colorOf = (v) =>
      v === null ? p.mut : v >= 90 ? p.err : v >= 75 ? p.warn : p.acc;
    /* HIT（缓存命中率）/ ACC（草稿接受率）越高越好，阈值方向相反 */
    const colorOfHi = (v) =>
      v === null ? p.mut : v < 50 ? p.err : v < 75 ? p.warn : p.acc;
    const clamp01 = (v) => Math.max(0, Math.min(100, v));

    /* 顶栏：主 LED / 模型 / 阶段（hud/mecha/drone/sweep 带 // 前缀） */
    const ledMain = classicRefs.led_main;
    if (ledMain) {
      ledMain.style.background = vals.online ? p.acc : p.mut;
      ledMain.style.opacity = vals.online ? '1' : '0.4';
    }
    const prefixed = ['hud', 'mecha', 'drone', 'sweep'].includes(id);
    set('model_top', prefixed ? `// ${vals.model}` : vals.model);
    set('phase_top', vals.phase,
      vals.busy ? p.acc2 : vals.online ? p.dim : p.err);

    if (id === 'instrument' || id === 'hud') {
      /* 占用率（连续条 instrument / 分段条 hud） */
      for (let i = 0; i < 6; i += 1) {
        const v = pcts[i];
        const c = i === 4 || i === 5 ? colorOfHi(v) : colorOf(v);
        set(`pctv${i}`, v === null ? '--' : `${Math.round(v)}%`, c);
        const fill = classicRefs[`bar${i}`];
        if (fill) {
          fill.style.width = `${v === null ? 0 : clamp01(v)}%`;
          fill.style.background = c;
        }
        const seg = classicRefs[`seg${i}`];
        if (seg) {
          const on = v === null ? 0 : Math.round((10 * clamp01(v)) / 100);
          Array.prototype.forEach.call(seg.children, (s, j) => {
            s.style.background = j < on ? c : p.track;
          });
        }
      }

      /* GPU 环 */
      const gpu = pcts[0];
      const ind = classicRefs.arcind;
      if (ind) {
        const ratio = gpu === null ? 0 : clamp01(gpu) / 100;
        ind.setAttribute('stroke-dasharray', `${ratio * CL_ARC_LEN} ${CL_ARC_C}`);
        ind.setAttribute('stroke', gpu === null ? p.mut : colorOf(gpu));
      }
      set('arcv', gpu === null ? '--' : `${Math.round(gpu)}%`,
        gpu === null ? p.mut : id === 'hud' ? colorOf(gpu) : p.text);

      /* 温度 / 功率 */
      set('temp', vals.temp === null ? '--' : String(Math.round(vals.temp)),
        vals.temp === null ? p.mut
          : vals.temp >= 85 ? p.err : vals.temp >= 75 ? p.warn : p.text);
      set('power', vals.power === null ? '--' : String(Math.round(vals.power)),
        vals.power === null ? p.mut : id === 'hud' ? p.acc3 : p.text);

      /* 服务状态 */
      for (let i = 0; i < 3; i += 1) {
        const led = classicRefs[`svcled${i}`];
        if (led) {
          led.style.background = vals.svc[i] ? p.acc : p.mut;
          led.style.opacity = vals.svc[i] ? '1' : '0.4';
        }
        set(`svcst${i}`, vals.svc[i] ? 'ON' : 'OFF', vals.svc[i] ? p.acc : p.mut);
      }

      /* 模型 / 并发 / 排队 / KV 池 */
      set('model', vals.model);
      set('conc', vals.conc === null ? '--' : String(Math.round(vals.conc)),
        id === 'hud' ? p.acc : undefined);
      set('queue', vals.queue === null ? '--' : String(Math.round(vals.queue)),
        vals.queue !== null && vals.queue > 0 ? (id === 'hud' ? p.acc2 : p.warn)
          : id === 'hud' ? p.acc : p.text);
      set('kvtext', vals.kvText, id === 'hud' ? p.dim : p.text);
      return;
    }

    applyClassicExtras(id, p, vals, pcts, set, colorOf, clamp01);
  }

  /* H02–H10 主题数值刷新（与固件各 apply_* 同逻辑） */
  function applyClassicExtras(id, p, vals, pcts, set, colorOf, clamp01) {
    const colorOfHi = (v) =>
      v === null ? p.mut : v < 50 ? p.err : v < 75 ? p.warn : p.acc;
    const colorOfAt = (i, v) => (i === 4 || i === 5 ? colorOfHi(v) : colorOf(v));
    const tempCol = (t) =>
      t === null ? p.mut : t >= 85 ? p.err : t >= 75 ? p.warn : p.text;
    const setNum = (ref, v, fmt, color) =>
      set(ref, v === null ? '--' : fmt(v), v === null ? p.mut : color);
    const segSet = (ref, v, color, n = 10) => {
      const seg = classicRefs[ref];
      if (!seg) return;
      const on = v === null ? 0 : Math.round((n * clamp01(v)) / 100);
      Array.prototype.forEach.call(seg.children, (s, j) => {
        s.style.background = j < on ? color : p.track;
      });
    };
    const barSet = (ref, v, color) => {
      const fill = classicRefs[ref];
      if (!fill) return;
      fill.style.width = `${v === null ? 0 : clamp01(v)}%`;
      fill.style.background = color;
    };
    const arcSet = (ref, v, color, full) => {
      const ind = classicRefs[ref];
      if (!ind) return;
      const ratio = v === null ? 0 : clamp01(v) / 100;
      const len = full ? CL_ARC_C : CL_ARC_LEN;
      ind.setAttribute('stroke-dasharray', `${ratio * len} ${CL_ARC_C}`);
      ind.setAttribute('stroke', v === null ? p.mut : color);
    };
    const svcLed = (i, on, color) => {
      const led = classicRefs[`svcled${i}`];
      if (led) {
        led.style.background = on ? color : p.mut;
        led.style.opacity = on ? '1' : '0.4';
      }
    };
    const round = (v) => String(Math.round(v));

    switch (id) {
      case 'mecha': {
        segSet('seg0', pcts[0], colorOf(pcts[0]), 56);
        arcSet('arcind', pcts[0], colorOf(pcts[0]));
        arcSet('arcind2', pcts[5], colorOfAt(5, pcts[5]));
        setNum('big0', vals.decode, (x) => x.toFixed(1), p.text);
        setNum('rowv0', vals.prefill, (x) => `${round(x)} t/s`, p.acc);
        barSet('rowbar0', vals.prefill === null ? null : vals.prefill / 24, p.acc);
        setNum('rowv1', vals.conc, round, p.acc);
        barSet('rowbar1', vals.conc === null ? null : vals.conc * 12.5, p.acc);
        setNum('rowv2', vals.vramOk ? vals.vramPct : null,
          (x) => vals.vramText, p.acc2);
        barSet('rowbar2', vals.vramPct, p.acc2);
        setNum('rowv3', pcts[5], (x) => `${Math.round(x)}%`, colorOfAt(5, pcts[5]));
        barSet('rowbar3', pcts[5], p.acc2);
        setNum('rowv4', vals.queue, round, p.acc2);
        barSet('rowbar4', vals.queue === null ? null : vals.queue * 10, p.acc2);
        setNum('rowv5', pcts[0], (x) => `${Math.round(x)}%`, colorOf(pcts[0]));
        barSet('rowbar5', pcts[0], colorOf(pcts[0]));
        setNum('rowv6', vals.temp, (x) => `${round(x)}°`, tempCol(vals.temp));
        barSet('rowbar6', vals.temp, tempCol(vals.temp));
        setNum('rowv7', vals.power, (x) => `${round(x)}W`, p.acc3);
        barSet('rowbar7', vals.power === null ? null : vals.power / 4, p.acc3);
        set('txt0',
          vals.temp !== null && vals.power !== null
            ? `CORE ${round(vals.temp)}C • PWR ${round(vals.power)}W • COOL ${round(100 - vals.temp)}%`
            : 'CORE -- • PWR --',
          p.dim);
        svcLed(0, vals.busy, p.acc);
        set('svcst0',
          vals.online ? (vals.busy ? 'STABLE' : 'IDLE') : 'OFFLINE',
          vals.online ? (vals.busy ? p.acc : p.dim) : p.err);
        svcLed(1, vals.svc[1], p.acc2);
        svcLed(2, vals.svc[2], p.acc2);
        break;
      }
      case 'stealth': {
        setNum('big0', vals.decode, (x) => x.toFixed(1), p.text);
        set('txt0', vals.davg === null ? 'AVG --' : `AVG ${vals.davg.toFixed(1)}`, p.acc);
        setNum('rowv0', vals.davg, (x) => x.toFixed(1), p.text);
        setNum('rowv1', vals.prefill, round, p.text);
        setNum('rowv2', vals.conc, round, p.text);
        setNum('rowv3', vals.queue, round, p.text);
        set('txt1', pcts[1] === null ? 'CPU --' : `CPU ${Math.round(pcts[1])}%`, p.text);
        set('txt2', pcts[0] === null ? 'GPU --' : `GPU ${Math.round(pcts[0])}%`, p.text);
        set('txt3', vals.memGb === null ? 'RAM --' : `RAM ${vals.memGb.toFixed(1)}G`, p.text);
        set('txt4', pcts[5] === null ? 'ACC --' : `ACC ${Math.round(pcts[5])}%`, p.text);
        set('temp', vals.temp === null ? '--' : round(vals.temp), tempCol(vals.temp));
        segSet('seg0', vals.temp, tempCol(vals.temp));
        svcLed(0, vals.online, p.acc);
        break;
      }
      case 'arcade': {
        setNum('big0', pcts[1], (x) => `${Math.round(x)}%`, colorOf(pcts[1]));
        segSet('seg0', pcts[1], colorOf(pcts[1]), 5);
        setNum('big1', pcts[0], (x) => `${Math.round(x)}%`, colorOf(pcts[0]));
        segSet('seg1', pcts[0], colorOf(pcts[0]), 5);
        set('txt0', pcts[5] === null ? 'ACC --' : `ACC ${Math.round(pcts[5])}%`,
          pcts[5] === null ? p.mut : colorOfHi(pcts[5]));
        set('txt1', vals.temp === null ? 'T --' : `T ${round(vals.temp)}C`,
          vals.temp === null ? p.mut : tempCol(vals.temp));
        set('txt2', vals.power === null ? 'P --' : `P ${round(vals.power)}W`,
          vals.power === null ? p.mut : p.text);
        set('txt3', vals.vramOk ? `VRAM ${Math.round(vals.vramPct)}%` : 'VRAM --',
          vals.vramOk ? p.text : p.mut);
        const idx = [2, 3, 4];
        for (let i = 0; i < 3; i += 1) {
          const v = pcts[idx[i]];
          setNum(`rowv${i}`, v, (x) => `${Math.round(x)}%`, colorOfAt(idx[i], v));
          segSet(`seg${i + 2}`, v, colorOfAt(idx[i], v), 12);
        }
        set('kvtext', vals.kvText, p.dim);
        set('txt4',
          vals.temp !== null && vals.power !== null
            ? `${round(vals.temp)}C • ${round(vals.power)}W` : '--', p.dim);
        break;
      }
      case 'drone': {
        setNum('big0', vals.decode, (x) => x.toFixed(1), p.text);
        setNum('rowv0', pcts[0], (x) => `${Math.round(x)}%`, colorOf(pcts[0]));
        barSet('rowbar0', pcts[0], colorOf(pcts[0]));
        setNum('rowv1', vals.temp, (x) => `${round(x)}°`, tempCol(vals.temp));
        barSet('rowbar1', vals.temp, tempCol(vals.temp));
        setNum('rowv2', vals.vramOk ? vals.vramPct : null,
          (x) => `${Math.round(x)}%`, vals.vramOk ? p.acc : p.mut);
        barSet('rowbar2', vals.vramPct, p.acc);
        setNum('rowv3', vals.queue, round, p.acc2);
        barSet('rowbar3', vals.queue === null ? null : vals.queue * 10, p.acc2);
        setNum('rowv4', pcts[5], (x) => `${Math.round(x)}%`, colorOfAt(5, pcts[5]));
        barSet('rowbar4', pcts[5], colorOfAt(5, pcts[5]));
        setNum('rowv5', pcts[4], (x) => `${Math.round(x)}%`, colorOfAt(4, pcts[4]));
        barSet('rowbar5', pcts[4], colorOfAt(4, pcts[4]));
        segSet('seg0', pcts[2], colorOf(pcts[2]));
        set('svcst0', vals.busy ? 'AUTO HOLD' : 'STANDBY', vals.busy ? p.acc : p.dim);
        set('kvtext', vals.kvText, p.dim);
        set('txt2', `CTX ${vals.ctxText}`, p.dim);
        break;
      }
      case 'scope': {
        arcSet('arcind', pcts[1], colorOf(pcts[1]), true);
        setNum('big0', pcts[1], (x) => `${Math.round(x)}%`, colorOf(pcts[1]));
        setNum('rowv0', pcts[0], (x) => `${Math.round(x)}%`, colorOf(pcts[0]));
        set('rowv1', vals.memText, vals.memGb !== null ? p.text : p.mut);
        set('temp', vals.temp === null ? '--' : round(vals.temp), tempCol(vals.temp));
        segSet('seg0', vals.temp, tempCol(vals.temp));
        set('power', vals.power === null ? '--' : round(vals.power),
          vals.power === null ? p.mut : p.text);
        setNum('rowv4', pcts[2], (x) => `${Math.round(x)}%`, colorOf(pcts[2]));
        set('kvtext', vals.kvText, p.dim);
        set('svcst0',
          vals.online ? (vals.busy ? 'STABLE' : 'IDLE') : 'OFFLINE',
          vals.online ? (vals.busy ? p.acc : p.dim) : p.err);
        break;
      }
      case 'heatsink': {
        setNum('big0', vals.decode, (x) => x.toFixed(1), p.text);
        set('txt0',
          vals.conc !== null ? `${vals.model} • SEQ ${round(vals.conc)}` : '--',
          p.acc2);
        set('txt1', vals.davg === null ? 'AVG --' : `AVG ${vals.davg.toFixed(1)}`, p.dim);
        set('txt2', vals.dpeak === null ? 'MAX --' : `MAX ${vals.dpeak.toFixed(1)}`, p.dim);
        set('txt3', pcts[0] === null ? 'GPU --' : `GPU ${Math.round(pcts[0])}%`,
          pcts[0] === null ? p.mut : p.acc);
        const hv = [pcts[0], vals.vramPct, pcts[5]];
        const hc = [p.acc, p.acc2, p.acc];
        for (let i = 0; i < 3; i += 1) {
          setNum(`rowv${i}`, hv[i], (x) => `${Math.round(x)}%`, hc[i]);
          segSet(`vseg${i}`, hv[i], hv[i] === null ? p.mut : hc[i]);
        }
        set('txt4',
          vals.temp !== null && vals.power !== null
            ? `P ${round(vals.power)}W • ${round(vals.temp)}C` : '--', p.dim);
        set('txt5', vals.memText, p.dim);
        set('txt6', vals.draftTokens === null
          ? 'DRAFT --' : `DRAFT ${fmtTokens(vals.draftTokens)}`, p.dim);
        break;
      }
      case 'loadout': {
        const idx = [1, 0, 3, 4];
        for (let i = 0; i < 4; i += 1) {
          const v = pcts[idx[i]];
          setNum(`rowv${i}`, v, (x) => `${Math.round(x)}%`, colorOfAt(idx[i], v));
          segSet(`seg${i}`, v, colorOfAt(idx[i], v));
        }
        set('txt0', vals.temp === null ? 'TEMP --' : `TEMP ${round(vals.temp)}C`, p.dim);
        set('txt1',
          vals.power !== null && vals.temp !== null
            ? `${round(vals.power)}W • ${round(vals.temp)}C` : '--', p.dim);
        set('txt2', vals.memText, p.dim);
        set('txt3', vals.cacheText, p.dim);
        set('txt4',
          vals.temp !== null && vals.power !== null
            ? `TEMP ${round(vals.temp)}C • PWR ${round(vals.power)}W`
            : 'TEMP -- • PWR --', p.dim);
        set('svcst0', vals.online ? 'ALL OK' : 'OFFLINE',
          vals.online ? p.acc : p.err);
        break;
      }
      case 'armor': {
        setNum('rowv0', vals.prefill, (x) => `${round(x)} t/s`, p.acc3);
        set('rowv1', vals.vramText, vals.vramOk ? p.text : p.mut);
        set('temp', vals.temp === null ? '--' : `${round(vals.temp)}C`,
          tempCol(vals.temp));
        set('queue', vals.queue === null ? '--' : round(vals.queue),
          vals.queue === null ? p.mut : p.text);
        arcSet('arcind', pcts[0], colorOf(pcts[0]));
        arcSet('arcind2', pcts[5], colorOfAt(5, pcts[5]));
        setNum('big0', vals.decode, (x) => x.toFixed(1), p.text);
        barSet('rowbar0', pcts[5], colorOfAt(5, pcts[5]));
        set('kvtext', vals.kvText, p.dim);
        set('svcst0', pcts[5] === null ? 'ACC --' : `ACC ${Math.round(pcts[5])}%`,
          pcts[5] === null ? p.mut : colorOfHi(pcts[5]));
        break;
      }
      case 'sweep': {
        const idx = [1, 3, 0, 4];
        for (let i = 0; i < 4; i += 1) {
          const v = pcts[idx[i]];
          setNum(`rowv${i}`, v, (x) => `${Math.round(x)}%`, colorOfAt(idx[i], v));
          segSet(`seg${i}`, v, colorOfAt(idx[i], v));
        }
        set('txt0',
          vals.temp !== null && vals.power !== null
            ? `T ${round(vals.temp)}C • P ${round(vals.power)}W • ACC ${pcts[5] === null ? '--' : `${Math.round(pcts[5])}%`}`
            : 'T -- • P --', p.dim);
        set('txt1',
          vals.conc !== null ? `SEQ ${round(vals.conc)} • Q ${round(vals.queue || 0)}` : 'SEQ --',
          p.acc);
        break;
      }
      default: break;
    }
  }

  /* 配色下拉：classic 模式下切换为传统框架主题，widgets 模式恢复主题列表 */
  function syncThemeOptions() {
    const classic = isClassic();
    const wanted = classic
      ? (state.layout && state.layout.classic_theme) || 'instrument'
      : (state.layout && state.layout.theme) || state.themeDefault;
    themeSelect.innerHTML = '';
    if (classic) {
      for (const [id, meta] of Object.entries(CLASSIC_THEME_META)) {
        const option = document.createElement('option');
        option.value = id;
        option.textContent = meta.label;
        themeSelect.appendChild(option);
      }
    } else {
      for (const theme of state.themes.list) {
        const option = document.createElement('option');
        option.value = theme.id;
        option.textContent = theme.label || theme.id;
        if (theme.description) option.title = theme.description;
        themeSelect.appendChild(option);
      }
    }
    themeSelect.value = wanted;
    if (themeSelect.value !== wanted) themeSelect.selectedIndex = 0;
  }

  function renderCanvas() {
    if (!state.layout) return;
    if (isClassic()) {
      renderClassicCanvas();
      return;
    }
    const profile = activeProfile();
    canvas.innerHTML = '';
    canvas.style.width = `${profile.width}px`;
    canvas.style.height = `${profile.height}px`;
    applyTheme();
    const widgets = currentWidgets();
    for (const widget of widgets) {
      canvas.appendChild(buildWidget(widget, profile));
    }
    if (widgets.length === 0) {
      const hint = document.createElement('div');
      hint.className = 'empty-hint';
      hint.textContent = '从左侧组件库点击或拖入组件';
      canvas.appendChild(hint);
    }
    fitCanvas();
    requestAnimationFrame(fitCanvas);
  }

  function refreshWidgetBodies() {
    if (!state.layout) return;
    for (const widget of currentWidgets()) {
      const body = canvas.querySelector(
        `[data-id="${esc(widget.id)}"] .widget-body`
      );
      if (!body) continue;
      if (widget.type === 'desktop_pet') updatePetWidget(widget, body);
      else renderWidgetBody(widget, body);
    }
  }

  function selectWidget(id) {
    if (state.selectedId === id) return;
    state.selectedId = id;
    document.querySelectorAll('.widget').forEach((el) => {
      el.classList.toggle('selected', el.dataset.id === id);
    });
    renderInspector();
  }

  function makeField(labelText) {
    const wrap = document.createElement('div');
    wrap.className = 'field';
    const label = document.createElement('label');
    label.textContent = labelText;
    wrap.appendChild(label);
    return wrap;
  }

  function metricOptionsForKind(kind) {
    if (kind === 'number') {
      return state.catalog.list.filter((metric) => metric.type === 'number');
    }
    return state.catalog.list;
  }

  const SERVER_CARD_BINDINGS = {
    cpu: ['host.cpu_pct', 'server.cpu_pct'],
    ram_used: ['host.ram_used_mb', 'server.ram_used_mb'],
    ram_total: ['host.ram_total_mb', 'server.ram_total_mb'],
    gpu_temp: ['host.gpu_temp_c', 'server.gpu_temp_c'],
    text: ['llm.version', 'llm.model', 'llm.drafter', 'llm.capability_probe'],
  };
  // 主机监控地址独立于 API 类型：配置后这些测点由设备轮询 host agent 得到
  const HOST_AGENT_METRICS = new Set([
    'host.gpu_available',
    'host.gpu_util_pct',
    'host.gpu_temp_c',
    'host.gpu_power_w',
    'host.gpu_vram_used_mb',
    'host.gpu_vram_total_mb',
    'host.ram_used_mb',
    'host.ram_total_mb',
  ]);
  function appendDisabledGroup(select, label, text) {
    const group = document.createElement('optgroup');
    group.label = label;
    group.disabled = true;
    const option = document.createElement('option');
    option.disabled = true;
    option.textContent = text;
    group.appendChild(option);
    select.appendChild(group);
  }

  function appendMetricGroup(select, metrics, prefix, liveIds) {
    const buckets = new Map();
    for (const metric of metrics) {
      const key = metric.group || '其他';
      if (!buckets.has(key)) buckets.set(key, []);
      buckets.get(key).push(metric);
    }
    for (const [groupName, items] of buckets) {
      const group = document.createElement('optgroup');
      group.label = `${prefix} · ${groupName}`;
      for (const metric of items) {
        const option = document.createElement('option');
        option.value = metric.id;
        // 来源当前取不到值的测点如实标注，避免选中后满屏 --
        const dead = liveIds && !liveIds.has(metric.id);
        option.textContent = `${metric.label} · ${metric.id}${dead ? '（暂无数据）' : ''}`;
        // 悬停显示该测点在引擎 API 里的原始遥测出处
        if (metric.api) option.title = metric.api;
        group.appendChild(option);
      }
      select.appendChild(group);
    }
  }

  /** 控件属性里的测点：只列当前屏幕真实可用的来源。
   *  专用服务端 = 服务端 snapshot 里已有数据的指标；
   *  设备直连 / 服务端转发 = 按屏幕上报的 provider 过滤；
   *  设备主机监控 = 该屏配置了主机监控地址时由设备轮询 host agent 得到的测点。 */
  function appendMetricOptions(select, metrics) {
    const available = new Set(
      Object.entries(state.values || {})
        .filter(([, value]) => value && value.a !== false)
        .map(([id]) => id)
    );
    const serverMetrics = metrics.filter((metric) => available.has(metric.id));
    if (serverMetrics.length) {
      appendMetricGroup(select, serverMetrics, '专用服务端');
    } else {
      appendDisabledGroup(
        select, '专用服务端 · 等待数据…', '服务端暂未上报可用测点'
      );
    }

    const device = currentDevice();
    const apiUrl = device && device.api ? device.api.url : '';
    const hostAgent = device && device.api ? device.api.host_agent : '';
    const hostMetrics = hostAgent
      ? metrics.filter(
          (metric) =>
            metric.source === 'api' && HOST_AGENT_METRICS.has(metric.id)
        )
      : [];
    const hostIds = new Set(hostMetrics.map((metric) => metric.id));

    if (apiUrl) {
      const usesForward = deviceUsesForward(device);
      const provider = deviceRuntimeProvider(device);
      if (usesForward && !state.forward.enabled) {
        appendDisabledGroup(
          select, '服务端转发 · 未启用', '请先在控制台「服务设置」启用转发'
        );
      } else if (!provider) {
        appendDisabledGroup(
          select, '设备 API · 等待识别',
          '屏幕上报 API 类型后自动显示对应测点'
        );
      } else {
        const source = usesForward
          ? `服务端转发 · ${providerLabel(provider)}`
          : `设备直连 · ${providerLabel(provider)}`;
        const apiMetrics = metrics.filter(
          (metric) =>
            metric.source === 'api' &&
            !hostIds.has(metric.id) &&
            Array.isArray(metric.providers) &&
            metric.providers.includes(provider)
        );
        if (apiMetrics.length) {
          const offline = device.runtime && device.runtime.api_ok === false;
          appendMetricGroup(
            select,
            apiMetrics,
            `${source}${offline ? '（API 未连接）' : ''}`,
            available
          );
        }
      }
    }

    if (hostMetrics.length) {
      appendMetricGroup(select, hostMetrics, '设备主机监控', available);
    }
  }

  /** 绑定值不在候选里时给出准确原因，别一律说“数据源暂不可用”。 */
  function bindingIssue(bindingId, widgetType) {
    if (widgetType === 'server_card') return '不适用于此字段';
    return metricDef(bindingId) ? '当前来源无此测点' : '测点已下线';
  }

  function appendPetEditor(widget) {
    const options = widget.options || (widget.options = {});
    const head = document.createElement('div');
    head.className = 'row-editor-head';
    const title = document.createElement('div');
    title.className = 'row-editor-title';
    title.textContent = '状态阈值';
    head.appendChild(title);
    inspectorEl.appendChild(head);

    const specs = [
      ['sleep_below', '睡眠阈值', 20],
      ['idle_below', '待机阈值', 60],
      ['happy_below', '开心阈值', 85],
    ];
    const grid = document.createElement('div');
    grid.className = 'pet-thresholds';
    for (const [key, labelText, fallback] of specs) {
      const field = document.createElement('label');
      field.className = 'pet-threshold';
      const span = document.createElement('span');
      span.textContent = labelText;
      const input = document.createElement('input');
      input.type = 'number';
      input.min = '0';
      input.step = '1';
      input.value = String(options[key] ?? fallback);
      input.addEventListener('input', () => {
        const value = Number(input.value);
        if (!Number.isFinite(value)) return;
        options[key] = value;
        markDirty();
        refreshWidgetBodies();
      });
      field.append(span, input);
      grid.appendChild(field);
    }
    inspectorEl.appendChild(grid);

    const note = document.createElement('p');
    note.className = 'panel-note';
    note.textContent =
      '数值：< 睡眠阈值 → 睡眠，< 待机阈值 → 待机，< 开心阈值 → 开心，否则警戒；忙碌状态为 true 时优先显示忙碌。布尔指标：true → 开心，false → 离线；指标不可用时显示离线。';
    inspectorEl.appendChild(note);
  }

  /** 行配置编辑器：metric_list / led_matrix / multi_bars / spark_row 共用。
   *  新行默认类型按组件语义给：led_matrix→status，图表类→number。 */
  function appendRowEditor(widget, def) {
    const options = widget.options || (widget.options = {});
    if (!Array.isArray(options.rows)) options.rows = [];
    const rows = options.rows;
    const spec = def.rows || {};
    const minRows = Number(spec.min) || 1;
    const maxRows = Number(spec.max) || 8;
    const newRowKind =
      widget.type === 'led_matrix'
        ? 'status'
        : widget.type === 'metric_list'
          ? 'auto'
          : 'number';

    const head = document.createElement('div');
    head.className = 'row-editor-head';
    const heading = document.createElement('div');
    heading.className = 'row-editor-title';
    heading.textContent = widget.type === 'led_matrix' ? '点阵配置' : '行配置';
    const addButton = document.createElement('button');
    addButton.type = 'button';
    addButton.className = 'btn small';
    addButton.textContent = '添加行';
    addButton.disabled = rows.length >= maxRows;
    addButton.addEventListener('click', () => {
      const pool =
        newRowKind === 'status'
          ? state.catalog.list.filter((metric) => metric.type === 'bool')
          : newRowKind === 'number'
            ? state.catalog.list.filter((metric) => metric.type === 'number')
            : state.catalog.list;
      const fallback = pool[0] || state.catalog.list[0];
      rows.push({
        label: newRowKind === 'status' ? '状态' : '指标',
        binding: fallback ? fallback.id : '',
        kind: newRowKind,
        format: '',
      });
      markDirty();
      renderCanvas();
      renderInspector();
    });
    head.append(heading, addButton);
    inspectorEl.appendChild(head);

    rows.forEach((row, index) => {
      const card = document.createElement('div');
      card.className = 'row-editor';

      const top = document.createElement('div');
      top.className = 'row-editor-top';
      const labelInput = document.createElement('input');
      labelInput.type = 'text';
      labelInput.placeholder = '行标签';
      labelInput.value = row.label || '';
      labelInput.addEventListener('input', () => {
        row.label = labelInput.value;
        markDirty();
        refreshWidgetBodies();
      });
      top.appendChild(labelInput);

      const up = document.createElement('button');
      up.type = 'button';
      up.className = 'icon-btn';
      up.textContent = '↑';
      up.disabled = index === 0;
      up.addEventListener('click', () => {
        if (index === 0) return;
        [rows[index - 1], rows[index]] = [rows[index], rows[index - 1]];
        markDirty();
        renderCanvas();
        renderInspector();
      });

      const down = document.createElement('button');
      down.type = 'button';
      down.className = 'icon-btn';
      down.textContent = '↓';
      down.disabled = index === rows.length - 1;
      down.addEventListener('click', () => {
        if (index === rows.length - 1) return;
        [rows[index + 1], rows[index]] = [rows[index], rows[index + 1]];
        markDirty();
        renderCanvas();
        renderInspector();
      });

      const remove = document.createElement('button');
      remove.type = 'button';
      remove.className = 'icon-btn danger-icon';
      remove.textContent = '×';
      remove.disabled = rows.length <= minRows;
      remove.addEventListener('click', () => {
        rows.splice(index, 1);
        markDirty();
        renderCanvas();
        renderInspector();
      });
      top.append(up, down, remove);

      const kindSelect = document.createElement('select');
      for (const [value, text] of [
        ['auto', '自动'],
        ['status', '状态'],
        ['text', '文本'],
        ['number', '数值'],
      ]) {
        const option = document.createElement('option');
        option.value = value;
        option.textContent = text;
        kindSelect.appendChild(option);
      }
      kindSelect.value = row.kind || 'auto';
      kindSelect.addEventListener('change', () => {
        row.kind = kindSelect.value;
        row.binding = '';
        markDirty();
        renderCanvas();
        renderInspector();
      });

      const metricSelect = document.createElement('select');
      const candidates = metricOptionsForKind(row.kind || 'auto');
      appendMetricOptions(metricSelect, candidates);
      if (
        row.binding &&
        !Array.from(metricSelect.options).some(
          (option) => option.value === row.binding
        )
      ) {
        const option = document.createElement('option');
        option.value = row.binding;
        option.textContent = `当前绑定 · ${row.binding}（${bindingIssue(
          row.binding,
          widget.type
        )}）`;
        metricSelect.appendChild(option);
      }
      metricSelect.value = row.binding || '';
      metricSelect.addEventListener('change', () => {
        row.binding = metricSelect.value;
        markDirty();
        refreshWidgetBodies();
      });

      const formatInput = document.createElement('input');
      formatInput.type = 'text';
      formatInput.placeholder = '格式后缀，例如 {v} °C';
      formatInput.value = row.format || '';
      formatInput.addEventListener('input', () => {
        row.format = formatInput.value;
        markDirty();
        refreshWidgetBodies();
      });

      card.append(top, kindSelect, metricSelect, formatInput);
      inspectorEl.appendChild(card);
    });
  }

  /** metric_list 专属行选项：显示标签 / 字号档位 / 行高 / 标签宽度 */
  function appendMetricListEditor(widget, def) {
    appendRowEditor(widget, def);
    const options = widget.options || (widget.options = {});

    const showLabelsField = document.createElement('div');
    showLabelsField.className = 'field';
    const showLabelsLabel = document.createElement('label');
    showLabelsLabel.className = 'checkbox-row';
    const showLabelsCheckbox = document.createElement('input');
    showLabelsCheckbox.type = 'checkbox';
    showLabelsCheckbox.checked = options.show_labels !== false;
    showLabelsCheckbox.addEventListener('change', () => {
      options.show_labels = showLabelsCheckbox.checked;
      markDirty();
      refreshWidgetBodies();
    });
    showLabelsLabel.append(
      showLabelsCheckbox,
      document.createTextNode('显示行标签')
    );
    showLabelsField.appendChild(showLabelsLabel);
    inspectorEl.appendChild(showLabelsField);

    const fontSizeField = makeField('字号档位');
    const fontSizeSelect = document.createElement('select');
    for (const size of ['S', 'M', 'L']) {
      const option = document.createElement('option');
      option.value = size;
      option.textContent = size;
      fontSizeSelect.appendChild(option);
    }
    fontSizeSelect.value = options.font_size || 'M';
    fontSizeSelect.addEventListener('change', () => {
      options.font_size = fontSizeSelect.value;
      markDirty();
      refreshWidgetBodies();
    });
    fontSizeField.appendChild(fontSizeSelect);
    inspectorEl.appendChild(fontSizeField);

    const rowHeightField = makeField('行高(px)');
    const rowHeightInput = document.createElement('input');
    rowHeightInput.type = 'number';
    rowHeightInput.min = '14';
    rowHeightInput.max = '40';
    rowHeightInput.value = String(options.row_height || 22);
    rowHeightInput.addEventListener('input', () => {
      const value = Number(rowHeightInput.value);
      if (Number.isFinite(value)) {
        options.row_height = clamp(value, 14, 40);
        markDirty();
        refreshWidgetBodies();
      }
    });
    rowHeightField.appendChild(rowHeightInput);
    inspectorEl.appendChild(rowHeightField);

    const labelWidthField = makeField('标签宽度(px)');
    const labelWidthInput = document.createElement('input');
    labelWidthInput.type = 'number';
    labelWidthInput.min = '30';
    labelWidthInput.max = '120';
    labelWidthInput.value = String(options.label_width || 60);
    labelWidthInput.addEventListener('input', () => {
      const value = Number(labelWidthInput.value);
      if (Number.isFinite(value)) {
        options.label_width = clamp(value, 30, 120);
        markDirty();
        refreshWidgetBodies();
      }
    });
    labelWidthField.appendChild(labelWidthInput);
    inspectorEl.appendChild(labelWidthField);
  }

  /** 通用选项编辑器：按 widgets.json 里 def.options 的类型自动生成控件。
   *  string→文本框 number→数字框 bool→复选 select→下拉；title 由上方单独处理。 */
  function appendOptionsEditor(widget, def) {
    const options = widget.options || (widget.options = {});
    for (const opt of def.options || []) {
      if (opt.key === 'title') continue;
      const field = makeField(opt.label || opt.key);
      const commit = (value) => {
        options[opt.key] = value;
        markDirty();
        refreshWidgetBodies();
      };
      if (opt.type === 'bool') {
        const label = document.createElement('label');
        label.className = 'checkbox-row';
        const checkbox = document.createElement('input');
        checkbox.type = 'checkbox';
        checkbox.checked = options[opt.key] !== false;
        checkbox.addEventListener('change', () => commit(checkbox.checked));
        label.append(checkbox, document.createTextNode('启用'));
        field.appendChild(label);
      } else if (opt.type === 'number') {
        const input = document.createElement('input');
        input.type = 'number';
        if (opt.min !== undefined) input.min = String(opt.min);
        if (opt.max !== undefined) input.max = String(opt.max);
        input.value = String(
          Number.isFinite(Number(options[opt.key]))
            ? options[opt.key]
            : opt.default ?? 0
        );
        input.addEventListener('input', () => {
          const value = Number(input.value);
          if (!Number.isFinite(value)) return;
          let next = value;
          if (opt.min !== undefined) next = Math.max(opt.min, next);
          if (opt.max !== undefined) next = Math.min(opt.max, next);
          commit(next);
        });
        field.appendChild(input);
      } else if (opt.type === 'select') {
        const select = document.createElement('select');
        for (const choice of opt.options || []) {
          const option = document.createElement('option');
          option.value = choice;
          option.textContent = choice;
          select.appendChild(option);
        }
        select.value = String(options[opt.key] ?? opt.default ?? '');
        select.addEventListener('change', () => commit(select.value));
        field.appendChild(select);
      } else {
        const input = document.createElement('input');
        input.type = 'text';
        input.value =
          typeof options[opt.key] === 'string' ? options[opt.key] : '';
        input.placeholder = opt.default ? String(opt.default) : '';
        input.addEventListener('input', () => commit(input.value));
        field.appendChild(input);
      }
      inspectorEl.appendChild(field);
    }
  }

  function closeSelect() {
    state.selectOpen = false;
    if (!state.inspectorPending) return;
    state.inspectorPending = false;
    renderInspector();
  }

  function renderInspector() {
    if (state.selectOpen) {
      state.inspectorPending = true;
      return;
    }
    state.inspectorPending = false;
    if (isClassic()) {
      inspectorEl.innerHTML =
        '<p class="muted">传统框架为固定式界面，无组件属性可编辑。<br>' +
        '显示内容：模型名称、引擎/前缀缓存/草稿器三态、GPU/CPU/内存/KV 占用、缓存命中率、草稿接受率、并发数、排队数、GPU 温度与功率。</p>';
      return;
    }
    const widget = currentWidgets().find(
      (item) => item.id === state.selectedId
    );
    if (!widget) {
      inspectorEl.innerHTML =
        '<p class="muted">选中画布里的组件后在这里绑定指标。</p>';
      return;
    }
    const def = widgetDef(widget.type);
    if (!def) {
      inspectorEl.textContent = `未知组件类型 ${widget.type}`;
      return;
    }
    inspectorEl.innerHTML = '';

    const titleField = makeField('标题');
    const titleInput = document.createElement('input');
    titleInput.type = 'text';
    titleInput.value = (widget.options && widget.options.title) || '';
    titleInput.addEventListener('input', () => {
      widget.options = widget.options || {};
      widget.options.title = titleInput.value;
      markDirty();
      const titleEl = canvas.querySelector(
        `[data-id="${esc(widget.id)}"] .widget-title`
      );
      if (titleEl) titleEl.textContent = widgetTitle(widget);
    });
    titleField.appendChild(titleInput);
    inspectorEl.appendChild(titleField);

    if (def.resizable && sizeOptions(def).length > 1) {
      const sizeField = makeField('尺寸（网格）');
      const sizeSelect = document.createElement('select');
      for (const [w, h] of sizeOptions(def)) {
        const option = document.createElement('option');
        option.value = `${w}x${h}`;
        option.textContent = `${w} × ${h}`;
        sizeSelect.appendChild(option);
      }
      sizeSelect.value = `${widget.w}x${widget.h}`;
      sizeSelect.addEventListener('change', () => {
        const [w, h] = sizeSelect.value.split('x').map(Number);
        if (!fitsInBothOrientations(widget, w, h)) {
          toast('这个尺寸在当前布局里放不下');
          sizeSelect.value = `${widget.w}x${widget.h}`;
          return;
        }
        applyWidgetSize(widget, w, h);
        markDirty();
        renderCanvas();
        renderInspector();
      });
      sizeField.appendChild(sizeSelect);
      inspectorEl.appendChild(sizeField);
    }

    const toggleField = document.createElement('div');
    toggleField.className = 'field';
    const toggleLabel = document.createElement('label');
    toggleLabel.className = 'checkbox-row';
    const toggle = document.createElement('input');
    toggle.type = 'checkbox';
    toggle.checked = !(widget.options && widget.options.show_title === false);
    toggle.addEventListener('change', () => {
      widget.options = widget.options || {};
      widget.options.show_title = toggle.checked;
      markDirty();
      renderCanvas();
      if (!toggle.checked) {
        toast('已隐藏标题，可拖右下角缩小高度');
      }
    });
    toggleLabel.append(toggle, document.createTextNode('显示标题栏'));
    toggleField.appendChild(toggleLabel);
    inspectorEl.appendChild(toggleField);

    for (const binding of def.bindings) {
      const field = makeField(
        `绑定 · ${binding.label}${binding.required ? ' *' : ''}`
      );
      const select = document.createElement('select');
      const none = document.createElement('option');
      none.value = '';
      none.textContent = '— 不绑定 —';
      select.appendChild(none);
      let candidates = state.catalog.list.filter((metric) =>
        binding.types.includes(metric.type)
      );
      if (widget.type === 'server_card') {
        const allowed = SERVER_CARD_BINDINGS[binding.key];
        if (allowed) {
          candidates = candidates.filter((metric) => allowed.includes(metric.id));
        }
      }
      appendMetricOptions(select, candidates);
      const currentBinding = widget.bindings[binding.key] || '';
      if (
        currentBinding &&
        !Array.from(select.options).some((option) => option.value === currentBinding)
      ) {
        const fallback = document.createElement('option');
        fallback.value = currentBinding;
        fallback.textContent = `当前绑定 · ${currentBinding}（${bindingIssue(
          currentBinding,
          widget.type
        )}）`;
        select.appendChild(fallback);
      }
      select.value = currentBinding;
      select.addEventListener('change', () => {
        widget.bindings[binding.key] = select.value;
        markDirty();
        renderCanvas();
        renderInspector();
      });
      field.appendChild(select);

      if (widget.type === 'server_card' && binding.key !== 'ram_total') {
        const labelInput = document.createElement('input');
        labelInput.type = 'text';
        labelInput.className = 'label-override';
        const customLabels =
          widget.options && widget.options.labels ? widget.options.labels : {};
        labelInput.value = customLabels[binding.key] || '';
        const metric = metricDef(widget.bindings[binding.key]);
        labelInput.placeholder = `标签：${metric ? metric.label : binding.label}`;
        labelInput.addEventListener('input', () => {
          widget.options = widget.options || {};
          widget.options.labels = widget.options.labels || {};
          if (labelInput.value.trim()) {
            widget.options.labels[binding.key] = labelInput.value.trim();
          } else {
            delete widget.options.labels[binding.key];
          }
          markDirty();
          refreshWidgetBodies();
        });
        field.appendChild(labelInput);
      }

      inspectorEl.appendChild(field);
    }

    if (widget.type === 'server_card') {
      const note = document.createElement('p');
      note.className = 'meta';
      note.textContent =
        '服务器卡按字段限定 CPU/内存/GPU/文本 测点；其余任意指标请使用大数字或指标列表。';
      inspectorEl.appendChild(note);
    }
    if (def.rows) {
      if (widget.type === 'metric_list') appendMetricListEditor(widget, def);
      else appendRowEditor(widget, def);
    }
    if (widget.type === 'desktop_pet') {
      appendPetEditor(widget);
    } else if (widget.type !== 'metric_list') {
      appendOptionsEditor(widget, def);
    }

    const meta = document.createElement('p');
    meta.className = 'meta';
    const miniNote =
      def.generation === 2
        ? '卡片高度不足 60px 时自动隐藏标题栏（与固件同规则），把这格留给图形。'
        : '';
    meta.textContent = `${def.label} · ${def.description || ''}。${miniNote}拖动边框移动；隐藏标题后可把底边向上拖 1 格收紧高度。`;
    inspectorEl.appendChild(meta);

    const del = document.createElement('button');
    del.type = 'button';
    del.className = 'btn danger';
    del.textContent = '删除组件';
    del.addEventListener('click', () => {
      const widgets = currentWidgets();
      const index = widgets.findIndex((item) => item.id === widget.id);
      if (index >= 0) widgets.splice(index, 1);
      state.selectedId = null;
      markDirty();
      renderCanvas();
      renderInspector();
    });
    inspectorEl.appendChild(del);
  }

  function defaultBindings(def) {
    const result = {};
    for (const binding of def.bindings) {
      if (binding.default && state.catalog.byId.has(binding.default)) {
        result[binding.key] = binding.default;
        continue;
      }
      const fallback = state.catalog.list.find((metric) =>
        binding.types.includes(metric.type)
      );
      result[binding.key] = fallback ? fallback.id : '';
    }
    return result;
  }

  function defaultOptions(def) {
    const result = {};
    for (const option of def.options || []) {
      if (option.default !== undefined) result[option.key] = option.default;
    }
    if (def.rows && Array.isArray(def.rows.default)) {
      result.rows = def.rows.default.map((row) => ({ ...row }));
    }
    return result;
  }

  function addWidget(type, cell = null) {
    if (!state.layout) return;
    const def = widgetDef(type);
    if (!def) return;
    if (totalWidgets() >= 20) {
      toast('总组件数量超过 20（设备内存限制）');
      return;
    }
    if (currentWidgets().length >= 40) {
      toast('单页最多 40 个组件');
      return;
    }
    const profile = activeProfile();
    const [width, height] = widgetSize(def);
    const capacity = Math.min(
      ...Object.values(state.profiles).map((item) => item.cols * item.rows)
    );
    const area =
      currentWidgets().reduce((sum, item) => sum + item.w * item.h, 0) +
      width * height;
    if (area > capacity) {
      toast('单页放不下，请先删减组件');
      return;
    }
    const editingAlt = state.profile !== state.baseProfile;
    let baseRect = null;
    if (editingAlt) {
      baseRect = findFreeBaseSpot(def);
      if (!baseRect) {
        toast('主方向画布没有空位了，先删掉一些组件');
        return;
      }
    }
    let position;

    if (cell) {
      position = {
        x: clamp(cell.x, 0, profile.cols - width),
        y: clamp(cell.y, 0, profile.rows - height),
      };
      if (!isValidPlacement({ ...position, w: width, h: height })) {
        toast('这个位置放不下，请换个位置');
        return;
      }
    } else {
      position = findFreeSpot(def, profile);
      if (!position) {
        toast('画布没有空位了，先删掉一些组件');
        return;
      }
    }

    const widget = {
      id: `w-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 6)}`,
      type,
      x: position.x,
      y: position.y,
      w: width,
      h: height,
      bindings: defaultBindings(def),
      options: defaultOptions(def),
      _base: editingAlt ? baseRect : { x: position.x, y: position.y, w: width, h: height },
    };
    currentWidgets().push(widget);
    state.selectedId = widget.id;
    markDirty();
    renderCanvas();
    renderInspector();
  }

  function startMove(event, widget, el) {
    if (event.button !== undefined && event.button !== 0) return;
    event.preventDefault();
    event.stopPropagation();
    selectWidget(widget.id);

    const profile = activeProfile();
    const { cw, ch } = cellSize(profile);
    const scale = canvas.getBoundingClientRect().width / profile.width;
    const startX = event.clientX;
    const startY = event.clientY;
    const original = { x: widget.x, y: widget.y };
    let moved = false;

    state.interacting = true;
    el.classList.add('dragging');

    const move = (moveEvent) => {
      const dx = Math.round(
        (moveEvent.clientX - startX) / ((cw + profile.gap) * scale)
      );
      const dy = Math.round(
        (moveEvent.clientY - startY) / ((ch + profile.gap) * scale)
      );
      widget.x = clamp(original.x + dx, 0, profile.cols - widget.w);
      widget.y = clamp(original.y + dy, 0, profile.rows - widget.h);
      const rect = rectPx(widget, profile);
      el.style.left = `${rect.left}px`;
      el.style.top = `${rect.top}px`;
      el.classList.toggle(
        'invalid',
        !isValidPlacement(widget, widget.id)
      );
      moved = moved || dx !== 0 || dy !== 0;
    };

    const finish = () => {
      document.removeEventListener('pointermove', move);
      document.removeEventListener('pointerup', finish);
      document.removeEventListener('pointercancel', finish);
      el.classList.remove('dragging', 'invalid');
      state.interacting = false;
      if (!isValidPlacement(widget, widget.id)) {
        widget.x = original.x;
        widget.y = original.y;
        toast('这个位置被占用了');
      } else if (moved) {
        markDirty();
      }
      renderCanvas();
    };

    document.addEventListener('pointermove', move);
    document.addEventListener('pointerup', finish);
    document.addEventListener('pointercancel', finish);
  }

  function fitsInBothOrientations(widget, w, h) {
    if (!isValidPlacement({ ...widget, w, h }, widget.id)) return false;
    const base = state.profiles[state.baseProfile];
    if (!base) return true;
    const self = baseRectOf(widget);
    if (self.x + w > base.cols || self.y + h > base.rows) return false;
    const rect = { x: self.x, y: self.y, w, h };
    return !currentWidgets().some((other) => {
      if (other.id === widget.id) return false;
      const otherRect = baseRectOf(other);
      return intersects(rect, {
        x: otherRect.x,
        y: otherRect.y,
        w: otherRect.w,
        h: otherRect.h,
      });
    });
  }

  function applyWidgetSize(widget, w, h) {
    widget.w = w;
    widget.h = h;
    if (widget._base) {
      widget._base.w = w;
      widget._base.h = h;
    }
  }

  function startResize(event, widget, el) {
    if (event.button !== undefined && event.button !== 0) return;
    event.preventDefault();
    event.stopPropagation();
    selectWidget(widget.id);
    const def = widgetDef(widget.type);
    if (!def || !def.resizable) return;
    const profile = activeProfile();
    const { cw, ch } = cellSize(profile);
    const scale = canvas.getBoundingClientRect().width / profile.width;
    const startX = event.clientX;
    const startY = event.clientY;
    const original = { w: widget.w, h: widget.h };
    let changed = false;

    state.interacting = true;
    el.classList.add('resizing');

    const move = (moveEvent) => {
      const dw = Math.round(
        (moveEvent.clientX - startX) / ((cw + profile.gap) * scale)
      );
      const dh = Math.round(
        (moveEvent.clientY - startY) / ((ch + profile.gap) * scale)
      );
      const [w, h] = nearestSize(def, original.w + dw, original.h + dh);
      if (w === widget.w && h === widget.h) return;
      if (!fitsInBothOrientations(widget, w, h)) return;
      applyWidgetSize(widget, w, h);
      changed = true;
      renderCanvas();
      const next = canvas.querySelector(`[data-id="${esc(widget.id)}"]`);
      if (next) next.classList.add('resizing');
    };

    const finish = () => {
      document.removeEventListener('pointermove', move);
      document.removeEventListener('pointerup', finish);
      document.removeEventListener('pointercancel', finish);
      state.interacting = false;
      document.querySelectorAll('.widget.resizing').forEach((node) => {
        node.classList.remove('resizing');
      });
      if (changed) markDirty();
      renderCanvas();
      renderInspector();
    };

    document.addEventListener('pointermove', move);
    document.addEventListener('pointerup', finish);
    document.addEventListener('pointercancel', finish);
  }

  function collectHistory() {
    if (!state.layout) return;
    const ids = new Set();
    for (const widget of currentWidgets()) {
      if (widget.type === 'sparkline' && widget.bindings.value) {
        ids.add(widget.bindings.value);
      } else if (widget.type === 'dual_line') {
        if (widget.bindings.value) ids.add(widget.bindings.value);
        if (widget.bindings.value2) ids.add(widget.bindings.value2);
      } else if (widget.type === 'big_stat' && widget.bindings.value) {
        ids.add(widget.bindings.value);
      } else if (
        widget.type === 'spark_row' &&
        widget.options &&
        Array.isArray(widget.options.rows)
      ) {
        for (const row of widget.options.rows) {
          if (row.binding) ids.add(row.binding);
        }
      }
    }
    for (const id of ids) {
      const item = state.values[id];
      if (!item || !item.a || typeof item.v !== 'number') continue;
      let history = state.history.get(id);
      if (!history) {
        history = [];
        state.history.set(id, history);
      }
      history.push(item.v);
      if (history.length > 60) history.shift();
    }
  }

  function currentDevice() {
    return state.devices.find((device) => device.id === state.deviceId) || null;
  }

  function deviceAddr(device) {
    if (!device) return '';
    return device.port && device.port !== 80
      ? `${device.ip}:${device.port}`
      : device.ip;
  }

  function updateServerUrl() {
    const el = $('server-url');
    el.textContent = state.serverUrl || '—';
    el.title = state.serverUrl || '';
  }

  function updateDeviceBar() {
    const device = currentDevice();
    const status = $('device-status');
    if (!device) {
      status.textContent = state.devices.length ? '未选择屏幕' : '未连接屏幕';
      status.className = 'device-status';
      $('device-settings-btn').disabled = true;
      $('save-btn').disabled = true;
      return;
    }
    const parts = [device.online ? '在线' : '离线', deviceAddr(device)];
    parts.push(device.profile === '480x320' ? '横屏' : '竖屏');
    if (device.api && device.api.url) {
      if (deviceUsesForward(device)) {
        parts.push(state.forward.enabled ? '转发' : '转发未启用');
      }
      const provider = deviceRuntimeProvider(device);
      if (provider) parts.push(providerLabel(provider));
    }
    status.textContent = parts.join(' · ');
    status.className = `device-status ${device.online ? 'online' : 'offline'}`;
    $('device-settings-btn').disabled = false;
    $('save-btn').disabled = false;
  }

  function renderDeviceSelect() {
    // 下拉展开时不要重建选项，否则菜单会被定时刷新顶掉
    if (document.activeElement === deviceSelect) return;
    if (!state.devices.length) {
      deviceSelect.innerHTML = '<option value="">未连接屏幕</option>';
      deviceSelect.value = '';
      return;
    }
    deviceSelect.innerHTML = state.devices
      .map((device) => {
        const label = `${device.name} · ${device.online ? '在线' : '离线'} · ${deviceAddr(device)}`;
        return `<option value="${escHtml(device.id)}">${escHtml(label)}</option>`;
      })
      .join('');
    deviceSelect.value = state.deviceId || '';
  }

  async function refreshDevices() {
    const data = await api('/api/v1/devices');
    state.devices = Array.isArray(data.devices) ? data.devices : [];
    state.serverUrl = data.server_url || state.serverUrl;
    state.forward = { ...state.forward, ...(data.forward || {}) };
    updateServerUrl();
    if (state.deviceId && !state.devices.some((d) => d.id === state.deviceId)) {
      state.deviceId = null;
    }
    if (!state.deviceId && state.devices.length) {
      const online = state.devices.find((d) => d.online);
      state.deviceId = (online || state.devices[0]).id;
    }
    renderDeviceSelect();
    updateDeviceBar();
    const device = currentDevice();
    if (device) {
      const runtimeKey = `${device.runtime?.provider || ''}|${device.runtime?.api_ok ? 1 : 0}|${device.api?.url || ''}|${state.forward.enabled ? 1 : 0}|${state.forward.base_url || ''}`;
      if (runtimeKey !== state.lastRuntimeKey) {
        state.lastRuntimeKey = runtimeKey;
        if (state.selectedId && state.layout) renderInspector();
      }
    }
    return state.devices;
  }

  const PROVIDER_LABELS = {
    halogen: 'Halogen',
    gufo: 'Gufo',
    strata: 'Strata',
    lmstudio: 'LM Studio',
    ollama: 'Ollama',
    openai: 'OpenAI 兼容',
    panel: '自定义 API',
  };
  function providerLabel(provider) {
    return PROVIDER_LABELS[provider] || provider || '未识别';
  }
  function normUrl(value) {
    return String(value || '').replace(/\/+$/, '');
  }
  function deviceUsesForward(device) {
    const apiUrl = device && device.api ? device.api.url : '';
    const base = state.forward ? state.forward.base_url : '';
    return !!(apiUrl && base && normUrl(apiUrl) === normUrl(base));
  }
  function deviceRuntimeProvider(device) {
    if (!device) return '';
    const runtime = device.runtime || {};
    if (runtime.provider && runtime.provider !== 'unknown') return runtime.provider;
    const configured = device.api ? device.api.provider : '';
    return configured && configured !== 'auto' ? configured : '';
  }

  async function selectDevice(deviceId) {
    if ((deviceId || null) === state.deviceId) return;
    if (state.dirty && !confirm('切换屏幕会放弃未保存的修改，继续？')) {
      renderDeviceSelect();
      return;
    }
    state.deviceId = deviceId || null;
    updateDeviceBar();
    await loadLayout();
  }

  function openDeviceModal(mode) {
    const device = currentDevice();
    state.modalMode = mode;
    $('device-modal-error').textContent = '';
    $('device-modal-title').textContent = mode === 'connect' ? '连接屏幕' : '屏幕设置';
    $('device-modal-hint').textContent =
      mode === 'connect'
        ? '在设备屏幕上读取 IP 填入此处；设备需与电脑在同一局域网。连接后会下发服务端地址、API 设置和该屏幕的布局。'
        : '修改后会立即下发到屏幕；布局与配色请在画布中编辑。';
    $('device-modal-save').textContent = mode === 'connect' ? '连接并下发' : '保存并下发';
    const apiCfg = (device && device.api) || {};
    $('dev-ip').value = device ? deviceAddr(device) : '';
    $('dev-ip').disabled = mode === 'settings';
    $('dev-name').value = device ? device.name : '';
    const forwardBase = (state.forward && state.forward.base_url) || '';
    const isForward = !!(
      forwardBase && apiCfg.url && normUrl(apiCfg.url) === normUrl(forwardBase)
    );
    $('dev-mode').value = isForward ? 'forward' : 'direct';
    $('dev-api-url').value = isForward ? '' : apiCfg.url || '';
    $('dev-api-key').value = isForward ? '' : apiCfg.key || '';
    $('dev-provider').value = isForward ? 'auto' : apiCfg.provider || 'auto';
    $('dev-rot180').checked = !!(device && device.rot180);
    applyConnectMode();
    $('device-reboot-btn').classList.toggle('hidden', mode !== 'settings');
    $('device-delete-btn').classList.toggle('hidden', mode !== 'settings');
    deviceModal.classList.remove('hidden');
    setTimeout(() => {
      if (mode === 'connect') $('dev-ip').focus();
      else $('dev-name').focus();
    }, 30);
  }

  function closeDeviceModal() {
    deviceModal.classList.add('hidden');
    state.modalMode = null;
  }

  /** 连接模式切换：直连显示 API 地址/密钥/服务类型，转发隐藏并提示直连控制台。 */
  function applyConnectMode() {
    const forward = $('dev-mode').value === 'forward';
    const note = $('dev-forward-note');
    document
      .querySelectorAll('#device-modal .direct-only')
      .forEach((el) => el.classList.toggle('hidden', forward));
    if (!forward) {
      note.classList.add('hidden');
      return;
    }
    const base = (state.forward && state.forward.base_url) || '';
    if (!base) {
      note.textContent = '转发模式不可用：服务端未配置转发地址。';
    } else if (!state.forward.enabled) {
      note.textContent = `转发未启用：屏幕将连接 ${base}，请先在控制台「服务设置」启用转发。`;
    } else {
      note.textContent = `转发模式：屏幕直连控制台 ${base}，API 密钥保存在服务端。`;
    }
    note.classList.remove('hidden');
  }

  function apiFromForm() {
    if ($('dev-mode').value === 'forward') {
      return {
        url: (state.forward && state.forward.base_url) || '',
        key: '',
        provider: 'auto',
      };
    }
    return {
      url: $('dev-api-url').value.trim(),
      key: $('dev-api-key').value,
      provider: $('dev-provider').value,
    };
  }

  async function submitDeviceModal() {
    const button = $('device-modal-save');
    const errorEl = $('device-modal-error');
    errorEl.textContent = '';
    button.disabled = true;
    try {
      const name = $('dev-name').value.trim();
      const apiCfg = apiFromForm();
      const rot180 = $('dev-rot180').checked;
      let device;
      if (state.modalMode === 'connect') {
        const ip = $('dev-ip').value.trim();
        if (!ip) throw new Error('请填写设备 IP');
        const data = await api('/api/v1/devices/connect', {
          method: 'POST',
          body: JSON.stringify({ ip, name, api: apiCfg, rot180 }),
        });
        device = data.device;
        toast('已下发配置，等待屏幕上线');
      } else {
        if (!state.deviceId) throw new Error('未选择屏幕');
        const data = await api(
          `/api/v1/devices/${encodeURIComponent(state.deviceId)}`,
          {
            method: 'PATCH',
            body: JSON.stringify({ name, api: apiCfg, rot180 }),
          }
        );
        device = data.device;
        toast('设置已下发');
      }
      closeDeviceModal();
      await refreshDevices();
      if (device && device.id) {
        state.deviceId = device.id;
        renderDeviceSelect();
        await loadLayout();
      }
    } catch (error) {
      errorEl.textContent = error.message;
    } finally {
      button.disabled = false;
    }
  }

  async function rebootDevice() {
    if (!state.deviceId) return;
    if (!confirm('确定重启这块屏幕？')) return;
    try {
      await api(`/api/v1/devices/${encodeURIComponent(state.deviceId)}/reboot`, {
        method: 'POST',
      });
      toast('重启指令已发送');
    } catch (error) {
      $('device-modal-error').textContent = error.message;
    }
  }

  async function deleteDevice() {
    if (!state.deviceId) return;
    if (!confirm('从服务端移除这块屏幕？设备本身不会被重置。')) return;
    try {
      await api(`/api/v1/devices/${encodeURIComponent(state.deviceId)}`, {
        method: 'DELETE',
      });
      state.deviceId = null;
      closeDeviceModal();
      await refreshDevices();
      await loadLayout();
      toast('已移除屏幕');
    } catch (error) {
      $('device-modal-error').textContent = error.message;
    }
  }

  function connectWS() {
    if (state.ws) {
      try {
        state.ws.close();
      } catch {
        /* ignore */
      }
      state.ws = null;
    }
    clearTimeout(state.wsTimer);

    const protocol = location.protocol === 'https:' ? 'wss' : 'ws';
    const ws = new WebSocket(`${protocol}://${location.host}/ws`);
    state.ws = ws;
    setConn('连接中…', '');

    ws.addEventListener('open', () => {
      setConn('实时数据已连接', 'online');
      ws.send(JSON.stringify({ type: 'subscribe' }));
    });
    ws.addEventListener('message', (event) => {
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        return;
      }
      if (message.type === 'device_status') {
        const device = state.devices.find((d) => d.id === message.device_id);
        if (device) device.runtime = message.runtime || {};
        if (message.device_id === state.deviceId) {
          const runtimeKey = `${message.runtime?.provider || ''}|${message.runtime?.api_ok ? 1 : 0}|${device?.api?.url || ''}|${state.forward.enabled ? 1 : 0}|${state.forward.base_url || ''}`;
          if (runtimeKey !== state.lastRuntimeKey) {
            state.lastRuntimeKey = runtimeKey;
            updateDeviceBar();
            if (state.selectedId) renderInspector();
          }
        }
        return;
      }
      if (message.type === 'layout_changed') {
        if (message.device_id && message.device_id !== state.deviceId) return;
        if (state.lastSaveAt && Date.now() - state.lastSaveAt < 2000) return;
        if (state.dirty) {
          toast('服务端布局已更新，点击「重载」获取最新版本');
          return;
        }
        loadLayout().catch((error) =>
          toast(`自动重载失败：${error.message}`)
        );
        return;
      }
      if (message.type !== 'snapshot' || !message.values) return;
      const beforeKeys = Object.keys(state.values || {}).sort().join(',');
      state.values = message.values;
      const afterKeys = Object.keys(state.values).sort().join(',');
      // 测点清单只随可用集合变化刷新，且限流：避免实时数据把下拉菜单顶掉
      if (
        beforeKeys !== afterKeys &&
        state.selectedId &&
        Date.now() - state.lastInspectorRefresh > 5000
      ) {
        state.lastInspectorRefresh = Date.now();
        renderInspector();
      }
      collectHistory();
      if (!state.interacting) {
        if (isClassic()) classicApply();
        else refreshWidgetBodies();
      }
    });
    ws.addEventListener('close', () => {
      if (state.ws !== ws) return;
      state.ws = null;
      setConn('已断开，重连中…', 'offline');
      clearTimeout(state.wsTimer);
      state.wsTimer = setTimeout(connectWS, 2000);
    });
    ws.addEventListener('error', () => {
      try {
        ws.close();
      } catch {
        /* ignore */
      }
    });
  }

  async function loadLayout() {
    const device = currentDevice();
    const saved = device
      ? await api(`/api/v1/devices/${encodeURIComponent(device.id)}/layout`)
      : await api(`/api/v1/layout?profile=${encodeURIComponent(state.profile)}`);
    state.layout = normalizeLayout(saved);
    state.baseProfile = state.layout.profile;
    state.profile =
      device && state.profiles[device.profile] ? device.profile : state.layout.profile;
    applyOrientationView();
    frameworkSelect.value = isClassic() ? 'classic' : 'widgets';
    syncThemeOptions();
    renderPalette();
    profileSelect.value = state.profile;
    profileSelect.disabled = isClassic();
    state.selectedId = null;
    state.dirty = false;
    updateDirty();
    applyTheme();
    renderCanvas();
    renderInspector();
    updateDeviceBar();
  }

  function layoutPayload() {
    captureOrientation();
    const raw = JSON.parse(JSON.stringify(state.layout));
    raw.profile = state.baseProfile;
    raw.pages = (raw.pages || []).slice(0, 1);
    const editingAlt = state.profile !== state.baseProfile;
    const liveWidgets = currentWidgets();
    (raw.pages[0].widgets || []).forEach((widget, index) => {
      const live = liveWidgets[index];
      const base = baseRectOf(live);
      if (editingAlt) {
        widget.alt = { x: live.x, y: live.y };
        widget.x = base.x;
        widget.y = base.y;
      }
      delete widget._base;
    });
    return raw;
  }

  async function saveLayout() {
    if (!state.layout) return;
    const device = currentDevice();
    if (!device) {
      toast('请先连接屏幕，再保存布局');
      return;
    }
    const button = $('save-btn');
    button.disabled = true;
    button.textContent = '保存中…';
    try {
      const payload = layoutPayload();
      payload.v = 2;
      state.lastSaveAt = Date.now();
      await api(`/api/v1/devices/${encodeURIComponent(device.id)}/layout`, {
        method: 'PUT',
        body: JSON.stringify(payload),
      });
      state.dirty = false;
      updateDirty();
      toast(`布局已保存并下发给 ${device.name}`);
    } catch (error) {
      toast(`保存失败：${error.message}`);
    } finally {
      button.disabled = false;
      button.textContent = '保存布局';
      updateDeviceBar();
    }
  }

  function wireEvents() {
    $('save-btn').addEventListener('click', saveLayout);
    $('reload-btn').addEventListener('click', () => {
      if (state.dirty && !confirm('放弃未保存的修改？')) return;
      loadLayout().catch((error) => toast(error.message));
    });

    deviceSelect.addEventListener('change', () => {
      selectDevice(deviceSelect.value).catch((error) => toast(error.message));
    });
    deviceSelect.addEventListener('blur', () => renderDeviceSelect());
    $('device-connect-btn').addEventListener('click', () => openDeviceModal('connect'));
    $('device-settings-btn').addEventListener('click', () => openDeviceModal('settings'));
    $('device-modal-close').addEventListener('click', closeDeviceModal);
    deviceModal.addEventListener('click', (event) => {
      if (event.target === deviceModal) closeDeviceModal();
    });
    $('device-modal-save').addEventListener('click', submitDeviceModal);
    $('dev-mode').addEventListener('change', applyConnectMode);
    $('device-reboot-btn').addEventListener('click', rebootDevice);
    $('device-delete-btn').addEventListener('click', deleteDevice);
    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && !deviceModal.classList.contains('hidden')) {
        closeDeviceModal();
      }
    });

    profileSelect.addEventListener('change', async () => {
      const next = profileSelect.value;
      if (next === state.profile) return;
      if (state.dirty && !confirm('切换方向会放弃未保存的修改，继续？')) {
        profileSelect.value = state.profile;
        return;
      }
      const device = currentDevice();
      if (!device) {
        state.profile = next;
        loadLayout().catch((error) => toast(error.message));
        return;
      }
      try {
        await api(`/api/v1/devices/${encodeURIComponent(device.id)}`, {
          method: 'PATCH',
          body: JSON.stringify({ profile: next }),
        });
        state.devices = state.devices.map((item) =>
          item.id === device.id ? { ...item, profile: next } : item
        );
        await loadLayout();
        toast(next === '480x320' ? '已切换为横屏' : '已切换为竖屏');
      } catch (error) {
        profileSelect.value = state.profile;
        toast(`切换方向失败：${error.message}`);
      }
    });

    themeSelect.addEventListener('change', () => {
      if (!state.layout) return;
      if (isClassic()) state.layout.classic_theme = themeSelect.value;
      else state.layout.theme = themeSelect.value;
      applyTheme();
      renderCanvas();
      markDirty();
    });

    /* 框架切换：widgets=小组件网格，classic=固定式传统主题（横屏 480×320） */
    frameworkSelect.addEventListener('change', async () => {
      if (!state.layout) return;
      const next = frameworkSelect.value;
      if (next === (isClassic() ? 'classic' : 'widgets')) return;
      state.layout.framework = next;
      if (next === 'classic' && !state.layout.classic_theme) {
        state.layout.classic_theme = 'instrument';
      }
      const device = currentDevice();
      if (next === 'classic' && device && device.profile !== '480x320') {
        try {
          await api(`/api/v1/devices/${encodeURIComponent(device.id)}`, {
            method: 'PATCH',
            body: JSON.stringify({ profile: '480x320' }),
          });
          state.devices = state.devices.map((item) =>
            item.id === device.id ? { ...item, profile: '480x320' } : item
          );
          state.profile = '480x320';
          state.baseProfile = '480x320';
          state.layout.profile = '480x320';
          profileSelect.value = '480x320';
          toast('传统框架为横屏固定界面，屏幕方向已切为 480×320');
        } catch (error) {
          toast(`切换屏幕方向失败：${error.message}`);
        }
      }
      syncThemeOptions();
      applyTheme();
      renderCanvas();
      renderPalette();
      renderInspector();
      markDirty();
    });

    // 下拉打开时不要重建属性面板，否则菜单会被实时数据刷新顶掉
    inspectorEl.addEventListener('focusin', (event) => {
      if (event.target instanceof HTMLSelectElement) state.selectOpen = true;
    });
    inspectorEl.addEventListener('mousedown', (event) => {
      if (event.target instanceof HTMLSelectElement) state.selectOpen = true;
    });
    inspectorEl.addEventListener('change', (event) => {
      if (event.target instanceof HTMLSelectElement) setTimeout(closeSelect, 80);
    });
    inspectorEl.addEventListener('focusout', (event) => {
      if (event.target instanceof HTMLSelectElement) setTimeout(closeSelect, 200);
    });

    canvas.addEventListener('dragover', (event) => {
      event.preventDefault();
      event.dataTransfer.dropEffect = 'copy';
    });
    canvas.addEventListener('drop', (event) => {
      if (isClassic()) return;
      event.preventDefault();
      const type = event.dataTransfer.getData('text/plain');
      if (!type) return;
      addWidget(type, pointToCell(event.clientX, event.clientY));
    });
    canvas.addEventListener('pointerdown', (event) => {
      if (event.target === canvas) {
        state.selectedId = null;
        document
          .querySelectorAll('.widget')
          .forEach((el) => el.classList.remove('selected'));
        renderInspector();
      }
    });

    window.addEventListener('resize', () => requestAnimationFrame(fitCanvas));
    window.addEventListener('beforeunload', (event) => {
      if (!state.dirty) return;
      event.preventDefault();
      event.returnValue = '';
    });
  }

  async function init() {
    wireEvents();
    try {
      const data = await api('/api/v1/bootstrap');
      state.catalog.list = data.catalog.metrics;
      state.catalog.byId = new Map(
        state.catalog.list.map((metric) => [metric.id, metric])
      );
      state.widgets.list = data.widgets.widgets;
      state.widgets.byType = new Map(
        state.widgets.list.map((widget) => [widget.type, widget])
      );
      state.profiles = data.profiles.profiles;
      const themesDoc = data.themes || { themes: [] };
      state.themes.list = themesDoc.themes;
      state.themes.byId = new Map(
        state.themes.list.map((theme) => [theme.id, theme])
      );
      state.themeDefault = themesDoc.default || state.themes.list[0]?.id || 'neon-cyber';
      state.themeAliases = themesDoc.aliases || {};

      syncThemeOptions();

      profileSelect.innerHTML = '';
      for (const [key, profile] of Object.entries(state.profiles)) {
        const option = document.createElement('option');
        option.value = key;
        option.textContent = profile.label || key;
        profileSelect.appendChild(option);
      }
      state.profile = state.profiles['320x480']
        ? '320x480'
        : Object.keys(state.profiles)[0];
      profileSelect.value = state.profile;

      renderPalette();
      try {
        const snapshot = await api('/api/v1/snapshot');
        state.values = snapshot.values || {};
      } catch {
        state.values = {};
      }

      await refreshDevices();
      const requestedDevice = new URLSearchParams(location.search).get('device');
      if (requestedDevice && state.devices.some((d) => d.id === requestedDevice)) {
        state.deviceId = requestedDevice;
        renderDeviceSelect();
        updateDeviceBar();
      }
      try {
        await loadLayout();
      } catch (error) {
        if (!state.deviceId) throw error;
        state.deviceId = null;
        renderDeviceSelect();
        updateDeviceBar();
        await loadLayout();
        toast(`屏幕布局加载失败，已切到模板：${error.message}`);
      }
      connectWS();
      if (!state.devices.length) openDeviceModal('connect');

      setInterval(async () => {
        const before = state.deviceId;
        try {
          await refreshDevices();
        } catch {
          return;
        }
        if (!!before !== !!state.deviceId) {
          loadLayout().catch((error) => toast(error.message));
        }
      }, 5000);
    } catch (error) {
      toast(`初始化失败：${error.message}`);
      setConn('初始化失败', 'offline');
    }
  }

  init();
})();
