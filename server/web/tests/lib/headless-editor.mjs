/**
 * 无头编辑器加载器
 *
 * 在 Node 里用最小 DOM 桩加载 server/web/app.js，暴露内部渲染函数与
 * 序列化能力。供两个用途：
 *   1. 回归测试（server/web/tests/widget-render-smoke.mjs）
 *   2. 设计图鉴生成（docs/design/build_widget_gallery.mjs）——
 *      图鉴里的预览由真实渲染函数输出，不手抄视觉，避免与代码漂移。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const ROOT = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)), '../../../..'
);
const APP_JS = path.join(ROOT, 'server/web/app.js');
const CATALOG_DIR = path.join(ROOT, 'shared/catalog');

/* ================= 最小 DOM 桩 ================= */

function makeStyle() {
  const target = {};
  return new Proxy(target, {
    get(obj, prop) {
      if (prop === 'setProperty') return (k, v) => { obj[k] = v; };
      return obj[prop];
    },
    set(obj, prop, value) { obj[prop] = value; return true; },
  });
}

function syncClass(el, value) {
  const set = el.classList._set;
  set.clear();
  for (const name of String(value).split(/\s+/).filter(Boolean)) set.add(name);
}

let seq = 0;
export function makeEl(tag) {
  const el = {
    _tag: String(tag || '').toLowerCase(),
    _id: ++seq,
    children: [],
    attributes: {},
    style: makeStyle(),
    dataset: {},
    textContent: '',
    checked: false,
    disabled: false,
    type: '',
    value: '',
    placeholder: '',
    title: '',
    draggable: false,
    min: '',
    max: '',
    parentNode: null,
    classList: {
      _set: new Set(),
      add: (...names) => names.forEach((n) => el.classList._set.add(n)),
      remove: (...names) => names.forEach((n) => el.classList._set.delete(n)),
      toggle(name, force) {
        const want = force === undefined ? !el.classList._set.has(name) : force;
        if (want) el.classList._set.add(name); else el.classList._set.delete(name);
        return want;
      },
      contains: (name) => el.classList._set.has(name),
    },
    appendChild(child) { child.parentNode = el; el.children.push(child); return child; },
    append(...kids) { kids.forEach((k) => el.appendChild(k)); },
    removeChild(child) {
      const i = el.children.indexOf(child);
      if (i >= 0) el.children.splice(i, 1);
      return child;
    },
    insertBefore(child, ref) {
      const i = el.children.indexOf(ref);
      if (i < 0) el.children.push(child); else el.children.splice(i, 0, child);
      child.parentNode = el;
      return child;
    },
    setAttribute(k, v) {
      el.attributes[k] = String(v);
      if (k === 'class') syncClass(el, v);
    },
    getAttribute(k) { return k in el.attributes ? el.attributes[k] : null; },
    removeAttribute(k) { delete el.attributes[k]; },
    addEventListener() {},
    removeEventListener() {},
    querySelector() { return null; },
    querySelectorAll() { return []; },
    getBoundingClientRect() { return { left: 0, top: 0, width: 100, height: 100 }; },
    focus() {},
    blur() {},
    closest() { return null; },
  };
  let className = '';
  Object.defineProperty(el, 'className', {
    get: () => className,
    set: (v) => {
      className = String(v);
      el.attributes.class = className;
      syncClass(el, className);
    },
  });
  let html = '';
  Object.defineProperty(el, 'innerHTML', {
    get: () => html,
    set: (v) => {
      html = String(v);
      el.children.length = 0;
      if (html.trim()) {
        const parsed = makeEl('#parsed');
        parsed._html = html;
        el.appendChild(parsed);
      }
    },
  });
  if (el._tag === 'select') {
    Object.defineProperty(el, 'options', {
      get: () => el.children.filter((c) => c._tag === 'option'),
    });
  }
  return el;
}

/* ================= 加载 app.js ================= */

export function loadEditor() {
  let source = fs.readFileSync(APP_JS, 'utf8');
  const marker = '\n  init();\n})();';
  if (!source.includes(marker)) {
    throw new Error('app.js 结尾结构变化：找不到 init(); 标记');
  }
  source = source.replace(
    marker,
    '\n  init();\n  globalThis.__EDITOR__ = { state, renderWidgetBody, renderInspector, ' +
    'collectHistory, widgetDef, metricDef, defaultOptions, widgetTitle, ' +
    'miniRatio, miniValueColor, miniUnit, activeProfile, cellSize, applyTheme };\n})();'
  );

  const byId = new Map();
  const documentStub = {
    createElement: (tag) => makeEl(tag),
    createElementNS: (_ns, tag) => makeEl(tag),
    createTextNode: (text) => {
      const el = makeEl('#text');
      el.textContent = text;
      return el;
    },
    getElementById: (id) => {
      if (!byId.has(id)) byId.set(id, makeEl(`div#${id}`));
      return byId.get(id);
    },
    querySelector: () => null,
    querySelectorAll: () => [],
    addEventListener() {},
    body: makeEl('body'),
    activeElement: null,
  };
  const windowStub = {
    document: documentStub,
    addEventListener() {},
    matchMedia: () => ({ matches: false }),
    innerHeight: 900,
    location: { search: '', protocol: 'http:', host: 'localhost' },
    requestAnimationFrame: (fn) => setTimeout(fn, 0),
    CSS: { escape: (s) => s },
    setTimeout,
    clearTimeout,
    setInterval: () => 0,
  };
  const localStorageStub = {
    _data: new Map(),
    getItem(k) { return this._data.has(k) ? this._data.get(k) : null; },
    setItem(k, v) { this._data.set(k, String(v)); },
    removeItem(k) { this._data.delete(k); },
  };
  class FakeWebSocket {
    constructor() { this.readyState = 0; }
    addEventListener() {}
    close() {}
    send() {}
  }

  new Function(
    'window', 'document', 'localStorage', 'WebSocket', 'fetch', 'location',
    'URLSearchParams', 'requestAnimationFrame', 'getComputedStyle', source
  )(
    windowStub, documentStub, localStorageStub, FakeWebSocket,
    async () => ({ ok: false, status: 500, text: async () => '' }),
    windowStub.location, URLSearchParams, windowStub.requestAnimationFrame,
    () => ({})
  );

  const T = globalThis.__EDITOR__;
  if (!T) throw new Error('app.js 内部符号导出失败');

  const readJson = (name) =>
    JSON.parse(fs.readFileSync(path.join(CATALOG_DIR, name), 'utf8'));
  const metricsRaw = readJson('metrics.json');
  const metrics = Array.isArray(metricsRaw) ? metricsRaw : metricsRaw.metrics || [];
  const widgetsDoc = readJson('widgets.json');
  const profilesDoc = readJson('profiles.json');
  const themesDoc = readJson('themes.json');

  T.state.catalog.list = metrics;
  T.state.catalog.byId = new Map(metrics.map((m) => [m.id, m]));
  T.state.widgets.list = widgetsDoc.widgets;
  T.state.widgets.byType = new Map(widgetsDoc.widgets.map((w) => [w.type, w]));
  T.state.profiles = profilesDoc.profiles;
  T.state.themes.list = themesDoc.themes;
  T.state.themes.byId = new Map(themesDoc.themes.map((t) => [t.id, t]));
  T.state.themeDefault = themesDoc.default;
  T.state.themeAliases = themesDoc.aliases || {};
  T.state.layout = {
    v: 2,
    profile: profilesDoc.profiles['320x480'] ? '320x480' : Object.keys(profilesDoc.profiles)[0],
    theme: themesDoc.default,
    pages: [{ name: '主页', widgets: [] }],
  };

  /** 假快照：每个指标都给一个可用值（数值型给 42.5，便于看刻度与阈值） */
  function fakeValues(overrides = {}) {
    const out = {};
    for (const m of metrics) {
      out[m.id] = {
        a: true,
        v: m.type === 'number' ? 42.5 : m.type === 'bool' ? true : 'Qwen2.5-7B',
      };
    }
    Object.assign(out, overrides);
    return out;
  }
  T.state.values = fakeValues();

  return {
    T,
    makeEl,
    metrics,
    widgetsDoc,
    profilesDoc,
    themesDoc,
    fakeValues,
    /** 预置历史采样，让折线/面积/趋势走完整数据路径 */
    seedHistory(values = [12, 18, 15, 22, 26, 24, 30]) {
      for (const m of metrics) {
        if (m.type === 'number') T.state.history.set(m.id, values.slice());
      }
    },
    clearHistory() { T.state.history.clear(); },
  };
}

/* ================= 序列化 ================= */

const VOID_LIKE = new Set(['line', 'path', 'polyline', 'polygon', 'circle', 'rect', 'use', 'image']);

function styleToInline(style) {
  const parts = [];
  for (const [key, value] of Object.entries(style)) {
    if (value === '' || value === null || value === undefined) continue;
    const prop = key.includes('-')
      ? key
      : key.replace(/[A-Z]/g, (ch) => `-${ch.toLowerCase()}`);
    parts.push(`${prop}:${value}`);
  }
  return parts.join(';');
}

function escapeHtml(text) {
  return String(text).replace(/[&<>"]/g, (ch) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;',
  }[ch]));
}

/** 把桩元素序列化成 HTML 字符串（属性 + 内联样式 + 文本） */
export function serialize(el) {
  if (el._tag === '#text') return escapeHtml(el.textContent);
  if (el._tag === '#parsed') return el._html || '';
  const attrs = [];
  const cls = el.attributes.class;
  if (cls) attrs.push(`class="${escapeHtml(cls)}"`);
  const style = styleToInline(el.style);
  if (style) attrs.push(`style="${escapeHtml(style)}"`);
  for (const [key, value] of Object.entries(el.attributes)) {
    if (key === 'class') continue;
    attrs.push(`${key}="${escapeHtml(value)}"`);
  }
  const tag = el._tag.replace(/^div#.*/, 'div');
  const open = `<${tag}${attrs.length ? ` ${attrs.join(' ')}` : ''}>`;
  if (!el.children.length && !el.textContent) {
    return VOID_LIKE.has(tag) ? `${open.slice(0, -1)} />` : `${open}</${tag}>`;
  }
  const inner = el.textContent
    ? escapeHtml(el.textContent)
    : el.children.map((c) => serialize(c)).join('');
  return `${open}${inner}</${tag}>`;
}
