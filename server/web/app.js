(() => {
  'use strict';

  const state = {
    catalog: { list: [], byId: new Map() },
    widgets: { list: [], byType: new Map() },
    profiles: {},
    themes: { list: [], byId: new Map() },
    profile: '320x480',
    activePage: 0,
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
    modalMode: null,
  };

  const $ = (id) => document.getElementById(id);
  const canvas = $('canvas');
  const canvasWrap = $('canvas-wrap');
  const canvasScroll = document.querySelector('.canvas-scroll');
  const paletteEl = $('palette');
  const inspectorEl = $('inspector');
  const profileSelect = $('profile-select');
  const themeSelect = $('theme-select');
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
    return state.layout ? state.profiles[state.layout.profile] : null;
  }
  function currentPage() {
    if (!state.layout || !Array.isArray(state.layout.pages)) return null;
    return state.layout.pages[state.activePage] || state.layout.pages[0] || null;
  }
  function currentWidgets() {
    const page = currentPage();
    return page && Array.isArray(page.widgets) ? page.widgets : [];
  }
  const THEME_VARS = {
    bg: '--bg',
    surface: '--surface',
    surface_2: '--surface-2',
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
  };
  function applyTheme() {
    if (!state.layout) return;
    const theme =
      state.themes.byId.get(state.layout.theme) ||
      state.themes.byId.get('classic');
    if (theme && state.layout.theme !== theme.id) state.layout.theme = theme.id;
    const tokens = (theme && theme.tokens) || {};
    for (const [token, cssVar] of Object.entries(THEME_VARS)) {
      if (tokens[token]) canvas.style.setProperty(cssVar, tokens[token]);
    }
    const radius = Number(tokens.radius);
    canvas.style.setProperty('--radius', `${Number.isFinite(radius) ? radius : 12}px`);
    if (themeSelect.value !== state.layout.theme) {
      themeSelect.value = state.layout.theme;
    }
  }
  function totalWidgets() {
    if (!state.layout || !Array.isArray(state.layout.pages)) return 0;
    return state.layout.pages.reduce(
      (total, page) => total + (Array.isArray(page.widgets) ? page.widgets.length : 0),
      0
    );
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
        layout.pages = [{ name: '第 1 页', widgets: [] }];
      }
      if (!['slide', 'fade', 'none', 'stack', 'zoom', 'flip', 'rotate'].includes(layout.page_animation)) {
        layout.page_animation = 'slide';
      }
      if (typeof layout.theme !== 'string' || !layout.theme) {
        layout.theme = 'classic';
      }
      return layout;
    }
    return {
      v: 2,
      profile: layout.profile,
      theme: layout.theme || 'classic',
      page_animation: layout.page_animation || 'slide',
      pages: [
        {
          name: '第 1 页',
          widgets: Array.isArray(layout.widgets) ? layout.widgets : [],
        },
      ],
    };
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
  function effectiveMinSize(widget, def) {
    const minSize = (def && def.min_size) || [1, 1];
    const minW = Number(minSize[0]) || 1;
    const minH = Number(minSize[1]) || 1;
    const titleHidden = widget.options && widget.options.show_title === false;
    return [minW, Math.max(1, minH - (titleHidden ? 1 : 0))];
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
    const [width, height] = def.default_size;
    for (let y = 0; y <= profile.rows - height; y += 1) {
      for (let x = 0; x <= profile.cols - width; x += 1) {
        if (isValidPlacement({ x, y, w: width, h: height })) {
          return { x, y };
        }
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
    const page = currentPage();
    $('canvas-title').textContent = `${profile.label}${
      page ? ` · ${page.name}` : ''
    } · ${profile.cols}×${profile.rows} 网格`;
  }

  function renderPalette() {
    paletteEl.innerHTML = '';
    for (const def of state.widgets.list) {
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
    body.append(main, label);
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
    addRow(labelFor('ip', 'IP'), valueFor('ip'));
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
    return `<svg class="pet-svg pet-state-${state}" viewBox="0 0 100 100" preserveAspectRatio="xMidYMid meet" aria-hidden="true">
      <ellipse class="pet-shadow" cx="50" cy="91" rx="24" ry="5"></ellipse>
      <g class="pet-actor">
        <circle class="pet-ear" cx="29" cy="18" r="9"></circle>
        <circle class="pet-ear" cx="71" cy="18" r="9"></circle>
        <ellipse class="pet-torso" cx="50" cy="73" rx="30" ry="18"></ellipse>
        <ellipse class="pet-head" cx="50" cy="45" rx="29" ry="24"></ellipse>
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
    const state = petState(widget);
    const cls = `pet-svg pet-state-${state}`;
    if (svg.getAttribute('class') !== cls) svg.setAttribute('class', cls);
    const fx = svg.querySelector('.pet-fx');
    if (fx) fx.textContent = PET_FX[state];
    const badge = body.querySelector('.pet-badge');
    if (badge) badge.textContent = PET_LABELS[state];
    const valueEl = body.querySelector('.pet-value');
    if (valueEl) valueEl.textContent = petValueText(widget);
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
    else body.textContent = def.label;
  }

  function buildWidget(widget, profile) {
    const def = widgetDef(widget.type);
    const el = document.createElement('div');
    const classes = ['widget'];
    if (widget.id === state.selectedId) classes.push('selected');
    if (widget.options && widget.options.show_title === false) {
      classes.push('title-hidden');
    }
    el.className = classes.join(' ');
    el.dataset.id = widget.id;
    const rect = rectPx(widget, profile);
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

    const handle = document.createElement('div');
    handle.className = 'resize-handle';

    el.append(header, body, handle);
    el.addEventListener('pointerdown', (event) => {
      if (event.target === handle) return;
      selectWidget(widget.id);
      if (isNearBorder(event, el)) {
        startMove(event, widget, el);
      }
    });
    el.addEventListener('pointermove', (event) => {
      if (event.buttons) return;
      el.style.cursor = isNearBorder(event, el) ? 'move' : '';
    });
    el.addEventListener('pointerleave', () => {
      el.style.cursor = '';
    });
    header.addEventListener('pointerdown', (event) =>
      startMove(event, widget, el)
    );
    handle.addEventListener('pointerdown', (event) =>
      startResize(event, widget, el)
    );

    if (!def) {
      body.textContent = `未知组件 ${widget.type}`;
    }
    return el;
  }

  function renderCanvas() {
    if (!state.layout) return;
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
  const SOURCE_GROUPS = [
    ['api', '传统 API · LLM 服务直连'],
    ['server', '专用 Server · 服务端本机'],
  ];
  function appendMetricOptions(select, metrics) {
    const buckets = new Map(SOURCE_GROUPS.map(([id]) => [id, []]));
    for (const metric of metrics) {
      const source = metric.source === 'server' ? 'server' : 'api';
      buckets.get(source).push(metric);
    }
    for (const [source, label] of SOURCE_GROUPS) {
      const items = buckets.get(source);
      if (!items.length) continue;
      const group = document.createElement('optgroup');
      group.label = label;
      for (const metric of items) {
        const option = document.createElement('option');
        option.value = metric.id;
        option.textContent = `${metric.label} · ${metric.id}`;
        group.appendChild(option);
      }
      select.appendChild(group);
    }
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

  function appendMetricListEditor(widget, def) {
    const options = widget.options || (widget.options = {});
    if (!Array.isArray(options.rows)) options.rows = [];
    const rows = options.rows;
    const spec = def.rows || {};
    const minRows = Number(spec.min) || 1;
    const maxRows = Number(spec.max) || 8;

    const head = document.createElement('div');
    head.className = 'row-editor-head';
    const heading = document.createElement('div');
    heading.className = 'row-editor-title';
    heading.textContent = '行配置';
    const addButton = document.createElement('button');
    addButton.type = 'button';
    addButton.className = 'btn small';
    addButton.textContent = '添加行';
    addButton.disabled = rows.length >= maxRows;
    addButton.addEventListener('click', () => {
      const fallback =
        state.catalog.list.find((metric) => metric.type === 'number') ||
        state.catalog.list[0];
      rows.push({
        label: '指标',
        binding: fallback ? fallback.id : '',
        kind: 'auto',
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
        const candidates = metricOptionsForKind(row.kind);
        if (
          !candidates.some((metric) => metric.id === row.binding) &&
          candidates.length
        ) {
          row.binding = candidates[0].id;
        }
        markDirty();
        renderCanvas();
        renderInspector();
      });

      const metricSelect = document.createElement('select');
      const candidates = metricOptionsForKind(row.kind || 'auto');
      appendMetricOptions(metricSelect, candidates);
      if (!candidates.some((metric) => metric.id === row.binding)) {
        const option = document.createElement('option');
        option.value = row.binding || '';
        option.textContent = `不兼容：${row.binding || '未绑定'}`;
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

  function renderInspector() {
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
        toast('已隐藏标题，可拖动底边向上收紧 1 格');
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
      appendMetricOptions(
        select,
        state.catalog.list.filter((metric) => binding.types.includes(metric.type))
      );
      select.value = widget.bindings[binding.key] || '';
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

    if (widget.type === 'metric_list') {
      appendMetricListEditor(widget, def);
    }
    if (widget.type === 'desktop_pet') {
      appendPetEditor(widget);
    }

    const meta = document.createElement('p');
    meta.className = 'meta';
    meta.textContent = `${def.label} · ${def.description || ''}。拖动边框移动；隐藏标题后可把底边向上拖 1 格收紧高度。`;
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
      renderPageTabs();
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
    const [width, height] = def.default_size;
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
    };
    currentWidgets().push(widget);
    state.selectedId = widget.id;
    markDirty();
    renderPageTabs();
    renderCanvas();
    renderInspector();
  }

  function isNearBorder(event, el) {
    const rect = el.getBoundingClientRect();
    const hit = 10;
    return (
      event.clientX - rect.left <= hit ||
      rect.right - event.clientX <= hit ||
      event.clientY - rect.top <= hit ||
      rect.bottom - event.clientY <= hit
    );
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

  function startResize(event, widget, el) {
    if (event.button !== undefined && event.button !== 0) return;
    event.preventDefault();
    event.stopPropagation();
    selectWidget(widget.id);

    const profile = activeProfile();
    const { cw, ch } = cellSize(profile);
    const scale = canvas.getBoundingClientRect().width / profile.width;
    const startX = event.clientX;
    const startY = event.clientY;
    const original = { x: widget.x, y: widget.y, w: widget.w, h: widget.h };
    const def = widgetDef(widget.type);
    const [minW, minH] = effectiveMinSize(widget, def);
    let resized = false;

    state.interacting = true;

    const move = (moveEvent) => {
      const dw = Math.round(
        (moveEvent.clientX - startX) / ((cw + profile.gap) * scale)
      );
      const dh = Math.round(
        (moveEvent.clientY - startY) / ((ch + profile.gap) * scale)
      );
      widget.w = clamp(original.w + dw, minW, profile.cols - widget.x);
      widget.h = clamp(original.h + dh, minH, profile.rows - widget.y);
      const rect = rectPx(widget, profile);
      el.style.width = `${rect.width}px`;
      el.style.height = `${rect.height}px`;
      el.classList.toggle(
        'invalid',
        !isValidPlacement(widget, widget.id)
      );
      resized = resized || dw !== 0 || dh !== 0;
    };

    const finish = () => {
      document.removeEventListener('pointermove', move);
      document.removeEventListener('pointerup', finish);
      document.removeEventListener('pointercancel', finish);
      el.classList.remove('invalid');
      state.interacting = false;
      if (!isValidPlacement(widget, widget.id)) {
        widget.w = original.w;
        widget.h = original.h;
        toast('缩放后会和别的组件重叠');
      } else if (resized) {
        markDirty();
      }
      renderCanvas();
    };

    document.addEventListener('pointermove', move);
    document.addEventListener('pointerup', finish);
    document.addEventListener('pointercancel', finish);
  }

  function collectHistory() {
    if (!state.layout || !Array.isArray(state.layout.pages)) return;
    const ids = new Set();
    for (const page of state.layout.pages) {
      for (const widget of page.widgets) {
        if (widget.type === 'sparkline' && widget.bindings.value) {
          ids.add(widget.bindings.value);
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

  function renderPageTabs() {
    const pageTabs = $('page-tabs');
    pageTabs.innerHTML = '';
    if (!state.layout) return;
    state.layout.pages.forEach((page, index) => {
      const tab = document.createElement('div');
      tab.className = `page-tab${index === state.activePage ? ' active' : ''}`;
      tab.setAttribute('role', 'button');
      tab.tabIndex = 0;
      tab.title = '单击切换，双击重命名';
      const name = document.createElement('span');
      name.className = 'page-name';
      name.textContent = page.name;
      const count = document.createElement('span');
      count.className = 'page-count';
      count.textContent = String(page.widgets.length);
      tab.append(name, count);
      tab.addEventListener('click', () => {
        if (state.activePage !== index) switchPage(index);
      });
      tab.addEventListener('dblclick', (event) => {
        event.preventDefault();
        beginRenamePage(index);
      });
      pageTabs.appendChild(tab);
    });
    const animationSelect = $('page-animation');
    if (animationSelect) {
      animationSelect.value = state.layout.page_animation || 'slide';
    }
    $('add-page').disabled = state.layout.pages.length >= 4;
    $('copy-page').disabled = state.layout.pages.length >= 4;
    $('delete-page').disabled = state.layout.pages.length <= 1;
  }

  function switchPage(index) {
    if (!state.layout || index < 0 || index >= state.layout.pages.length) {
      return;
    }
    state.activePage = index;
    state.selectedId = null;
    renderPageTabs();
    renderCanvas();
    renderInspector();
  }

  function beginRenamePage(index) {
    if (!state.layout) return;
    const page = state.layout.pages[index];
    if (!page) return;
    const tab = $('page-tabs').children[index];
    if (!tab) return;
    tab.innerHTML = '';
    const input = document.createElement('input');
    input.className = 'page-rename';
    input.value = page.name;
    tab.appendChild(input);
    let done = false;
    const commit = () => {
      if (done) return;
      done = true;
      const name = input.value.trim();
      if (name) page.name = name;
      markDirty();
      renderPageTabs();
      renderCanvas();
    };
    input.addEventListener('keydown', (event) => {
      if (event.key === 'Enter') {
        event.preventDefault();
        commit();
      } else if (event.key === 'Escape') {
        event.preventDefault();
        done = true;
        renderPageTabs();
      }
    });
    input.addEventListener('blur', commit);
    input.addEventListener('click', (event) => event.stopPropagation());
    input.addEventListener('dblclick', (event) => event.stopPropagation());
    input.focus();
    input.select();
  }

  function cloneWidget(widget) {
    const clone = JSON.parse(JSON.stringify(widget));
    clone.id = `w-${Date.now().toString(36)}-${Math.random()
      .toString(36)
      .slice(2, 7)}`;
    return clone;
  }

  function addPage() {
    if (!state.layout) return;
    if (state.layout.pages.length >= 4) {
      toast('最多 4 页');
      return;
    }
    state.layout.pages.push({
      name: `第 ${state.layout.pages.length + 1} 页`,
      widgets: [],
    });
    state.activePage = state.layout.pages.length - 1;
    state.selectedId = null;
    markDirty();
    renderPageTabs();
    renderCanvas();
    renderInspector();
  }

  function copyPage() {
    if (!state.layout) return;
    if (state.layout.pages.length >= 4) {
      toast('最多 4 页');
      return;
    }
    const page = currentPage();
    if (!page) return;
    const copiedWidgets = page.widgets.map((widget) => cloneWidget(widget));
    if (totalWidgets() + copiedWidgets.length > 20) {
      toast('总组件数量超过 20（设备内存限制）');
      return;
    }
    const insertAt = state.activePage + 1;
    state.layout.pages.splice(insertAt, 0, {
      name: `${page.name} 副本`,
      widgets: copiedWidgets,
    });
    state.activePage = insertAt;
    state.selectedId = null;
    markDirty();
    renderPageTabs();
    renderCanvas();
    renderInspector();
  }

  function deletePage() {
    if (!state.layout) return;
    if (state.layout.pages.length <= 1) {
      toast('至少保留 1 页');
      return;
    }
    const page = currentPage();
    if (!page) return;
    if (!confirm(`删除「${page.name}」？`)) return;
    state.layout.pages.splice(state.activePage, 1);
    if (state.activePage >= state.layout.pages.length) {
      state.activePage = state.layout.pages.length - 1;
    }
    state.selectedId = null;
    markDirty();
    renderPageTabs();
    renderCanvas();
    renderInspector();
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
    status.textContent = `${device.online ? '在线' : '离线'} · ${deviceAddr(device)}`;
    status.className = `device-status ${device.online ? 'online' : 'offline'}`;
    $('device-settings-btn').disabled = false;
    $('save-btn').disabled = false;
  }

  function renderDeviceSelect() {
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
    return state.devices;
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
    $('dev-api-url').value = apiCfg.url || '';
    $('dev-api-key').value = apiCfg.key || '';
    $('dev-provider').value = apiCfg.provider || 'auto';
    $('dev-host-agent').value = apiCfg.host_agent || '';
    $('dev-rot180').checked = !!(device && device.rot180);
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

  function apiFromForm() {
    return {
      url: $('dev-api-url').value.trim(),
      key: $('dev-api-key').value,
      provider: $('dev-provider').value,
      host_agent: $('dev-host-agent').value.trim(),
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
      state.values = message.values;
      collectHistory();
      if (!state.interacting) refreshWidgetBodies();
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
    state.profile = state.layout.profile;
    profileSelect.value = state.profile;
    profileSelect.disabled = !!device;
    state.activePage = 0;
    state.selectedId = null;
    state.dirty = false;
    updateDirty();
    renderPageTabs();
    applyTheme();
    renderCanvas();
    renderInspector();
    updateDeviceBar();
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
      state.layout.v = 2;
      state.lastSaveAt = Date.now();
      await api(`/api/v1/devices/${encodeURIComponent(device.id)}/layout`, {
        method: 'PUT',
        body: JSON.stringify(state.layout),
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
    $('device-connect-btn').addEventListener('click', () => openDeviceModal('connect'));
    $('device-settings-btn').addEventListener('click', () => openDeviceModal('settings'));
    $('device-modal-close').addEventListener('click', closeDeviceModal);
    deviceModal.addEventListener('click', (event) => {
      if (event.target === deviceModal) closeDeviceModal();
    });
    $('device-modal-save').addEventListener('click', submitDeviceModal);
    $('device-reboot-btn').addEventListener('click', rebootDevice);
    $('device-delete-btn').addEventListener('click', deleteDevice);
    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && !deviceModal.classList.contains('hidden')) {
        closeDeviceModal();
      }
    });

    profileSelect.addEventListener('change', () => {
      if (state.deviceId) {
        profileSelect.value = state.profile;
        return;
      }
      if (state.dirty && !confirm('切换画布会放弃未保存的修改，继续？')) {
        profileSelect.value = state.profile;
        return;
      }
      state.profile = profileSelect.value;
      loadLayout().catch((error) => toast(error.message));
    });

    themeSelect.addEventListener('change', () => {
      if (!state.layout) return;
      state.layout.theme = themeSelect.value;
      applyTheme();
      markDirty();
    });

    $('add-page').addEventListener('click', addPage);
    $('copy-page').addEventListener('click', copyPage);
    $('delete-page').addEventListener('click', deletePage);
    $('page-animation').addEventListener('change', () => {
      if (!state.layout) return;
      state.layout.page_animation = $('page-animation').value;
      markDirty();
    });

    canvas.addEventListener('dragover', (event) => {
      event.preventDefault();
      event.dataTransfer.dropEffect = 'copy';
    });
    canvas.addEventListener('drop', (event) => {
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

      themeSelect.innerHTML = '';
      for (const theme of state.themes.list) {
        const option = document.createElement('option');
        option.value = theme.id;
        option.textContent = theme.label || theme.id;
        if (theme.description) option.title = theme.description;
        themeSelect.appendChild(option);
      }

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
