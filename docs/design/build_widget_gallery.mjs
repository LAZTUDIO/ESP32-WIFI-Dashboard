/**
 * 小组件设计图鉴生成器（generation 2）
 *
 * 关键点：图鉴里的每一个组件预览都不是手绘的，而是用无头 DOM 加载
 * server/web/app.js 后调用真实的 renderWidgetBody 输出，再序列化成
 * HTML 字符串嵌入页面。因此图鉴永远与代码一致，不会出现「文档好看、
 * 界面走样」的漂移。
 *
 * 输出：docs/design/widget-gallery-v2.html（单文件，离线可开）
 * 运行：node docs/design/build_widget_gallery.mjs
 */
import fs from 'node:fs';
import path from 'node:path';
import { loadEditor, serialize, makeEl, ROOT } from '../../server/web/tests/lib/headless-editor.mjs';

const OUT_FILE = path.join(ROOT, 'docs/design/widget-gallery-v2.html');
const NEW_TYPES = [
  'gauge_ring', 'led_dot', 'led_matrix', 'bar_strip', 'vbar_gauge',
  'multi_bars', 'dual_line', 'big_stat', 'spark_row', 'text_badge',
];

const { T, metrics, widgetsDoc, themesDoc, seedHistory, clearHistory } = loadEditor();

/* 整屏拼版：直接读仓库里的示例布局，图鉴与示例文件同源 */
const SHOWCASE_LAYOUT = JSON.parse(
  fs.readFileSync(path.join(ROOT, 'examples/widgets-showcase-320x480.json'), 'utf8')
);

/* 指标值：数值型给 42.5，便于看刻度/阈值/柱高 */
const SAMPLE_OVERRIDES = {
  'host.cpu_pct': 74.5,
  'host.gpu_util_pct': 86.2,
  'llm.decode_tps': 42.5,
  'llm.prefill_tps': 128.3,
  'llm.decode_avg': 38.2,
  'llm.prefill_avg': 110.6,
  'llm.kv_pct': 38.1,
};
T.state.values = {
  ...T.state.values,
  ...Object.fromEntries(
    Object.entries(SAMPLE_OVERRIDES).map(([id, v]) => [id, { a: true, v }])
  ),
};

/* 每个组件的展示参数：标题、采样绑定、阈值，让预览有信息量 */
const SHOWCASE = {
  gauge_ring: { title: 'CPU 占用', bindings: { value: 'host.cpu_pct' }, options: { unit: '%', max_value: 100, warn_above: 70, danger_above: 88 } },
  led_dot: { title: '推理服务', bindings: { value: 'llm.online', subtitle: 'llm.phase' }, options: { on_text: '在线', off_text: '离线' } },
  led_matrix: { title: '服务状态墙', bindings: {}, options: {} },
  bar_strip: { title: '显存占用', bindings: { value: 'llm.kv_pct' }, options: { unit: '%', max_value: 100, warn_above: 75, danger_above: 92 } },
  vbar_gauge: { title: 'GPU 利用率', bindings: { value: 'host.gpu_util_pct' }, options: { unit: '%', max_value: 100, warn_above: 80, danger_above: 95 } },
  multi_bars: { title: '吞吐对比', bindings: {}, options: {} },
  dual_line: { title: '流量趋势', bindings: { value: 'llm.decode_tps', value2: 'llm.prefill_tps' }, options: {} },
  big_stat: { title: '解码速度', bindings: { value: 'llm.decode_tps' }, options: { unit: 'tok/s', warn_above: 60, danger_above: 100 } },
  spark_row: { title: '关键曲线', bindings: {}, options: {} },
  text_badge: { title: '当前模型', bindings: { value: 'llm.model' }, options: {} },
};

/* 布局分层规则（对照 ui/src/ui_mini.c 的 build_* 实现逐条整理） */
const LAYOUT_RULES = {
  gauge_ring: [
    '270° 圆弧：135° 起画、扫过 270°，轨道用 border 色，数值弧用主题主色',
    '2×2 紧凑模式只留弧 + 中心数值，刻度与单位整组隐藏；3×3 起完整显示',
    '13 道刻度（每 3 道一道主刻度），刻度环半径随直径 40→132px 递增',
    'warn / danger 阈值在弧外画短标记；越线时弧、数值、单位同步换成 warn / err 色',
    '量程优先级：max_value > 0 → %/°C 按 100 → v/(v+1) 软缩放（永不溢出）',
  ],
  led_dot: [
    '2×2 只留一颗呼吸灯（1s 正弦，光晕透明度 60→220）',
    '3×3 增加在线/离线文案；4×4 再挂一行副文本（绑定 subtitle，单行省略）',
    '在线 = ok 色 + 光晕呼吸；离线 = err 色 1Hz 硬闪；未就绪 = text_mut 常亮',
    '直径按短边取档：14 / 22 / 30 / 40px；复古主题变方灯（radius 2）',
  ],
  led_matrix: [
    '列数 = ceil(√(行数 × 宽高比))，行列自适应，行数上限 16',
    '单元格偏高（高/宽 ≥ 0.8）用「点上文下」堆叠，偏宽用「点左文右」',
    '离线点 1Hz 闪烁；无数据点用 text_mut 颜色区分「未知」与「故障」',
    '标签在单元格小于 36×15px 时自动整组隐藏，避免挤成色块',
  ],
  bar_strip: [
    '高度 2 → inline：标题 ｜ 细条 ｜ 数值 三段横排，最省纵向空间（此形态无标题栏，标题由行内标签承担）',
    '高度 3 → 堆叠：指标名与 17px 大数同一行，满宽 10px 粗条压在底部（组件名在标题栏，不重复显示）',
    '高度 4 → hero：左侧大数值 + 单位，右侧 30px 竖条自底向上填充',
    '阈值在轨道上画 warn / err 竖线（hero 形态转为横线），一眼看出余量',
  ],
  vbar_gauge: [
    '轨道宽 = 组件宽 40%，上限 22px、下限 8px，垂直居中偏左留刻度位',
    '数值与单位分层置底：轨道吃满剩余高度（最小 16px）',
    '阈值横线横跨轨道，左侧另有 5 档小刻度（长 4px / 短 2px）',
    '字号按高度取档 12 / 16 / 20px；律动主题轨道与填充变胶囊圆角',
  ],
  multi_bars: [
    '柱高按「最大值 × 1.15」归一，留 15% 顶部余量给数值帽',
    '背景 3 条 25% 网格线 + 1 条基线，弱到只看得到趋势',
    '最大值柱用 accent_2 高亮（复古主题反色：其余柱 accent_2、最大柱 accent）',
    '数值 ≥ 1000 自动 k 缩写；柱顶数值需要宽 ≥ 48px 且柱数 ≤ 6 才显示',
  ],
  dual_line: [
    '图例两列：色点 + 指标名 + 实时值，实时值用对应线的颜色',
    '两条线各自独立纵轴（min/max 自适应，跨度 < 1 时补 1），互不压扁',
    '主线下方 20% 透明度面积填充：按 2px 列线性插值，双线不打结',
    'show_area 可关；数据不足 2 点时退化成一条基线，不画假的折线',
  ],
  big_stat: [
    'hero 数值三档字号 48 / 28 / 20px，按可用宽高自动降档防溢出',
    '背景铺一条 20% 透明度趋势曲线（move_background，不抢文字）',
    '底部趋势行：▲/▼ + 60s 涨跌幅，涨 ok 色、跌 err 色',
    '采样不足时显示「◆ 采样中」，不做无数据依据的涨跌判断',
  ],
  spark_row: [
    '1–4 列横排，每列独立纵轴与独立历史环形缓冲（不互相缩放）',
    '列内三段：数值置顶 12px、曲线吃满中部、标签置底 10px',
    '列宽 ≥ 40px 才带单位，否则只留数字避免换行',
    '长条规格专用：12×2 一屏放下 4 条趋势，比四个 sparkline 省 3 倍面积',
  ],
  text_badge: [
    '左侧 3px 装饰竖条（accent 60% 透明），高度 < 30px 时省略',
    '字号阶梯按「文本长度 × 字宽 ≤ 可用宽度 × 行数」自动降档',
    'ASCII 与 CJK 分别用不同阶梯（26→11 / 22→11），中英混排不炸框',
    '高度 ≥ 40px 转多行换行，否则单行省略；align 支持左对齐与居中',
  ],
};

/* ================= 尺寸换算 ================= */
const PROFILE = '320x480';
const CELL = (() => {
  const p = T.state.profiles[PROFILE];
  return {
    cw: (p.width - 2 * p.padding - (p.cols - 1) * p.gap) / p.cols,
    gap: p.gap,
  };
})();
const pxOf = (units) => units * CELL.cw + (units - 1) * CELL.gap;
function shapeOf(w, h) {
  if (w === h) return '正方形';
  if (w >= h * 2) return '长条';
  if (h >= w * 2) return '竖条';
  return w > h ? '横矩形' : '竖矩形';
}
const SHAPE_ORDER = ['正方形', '长条', '竖条', '横矩形', '竖矩形'];

/* ================= 渲染 ================= */
const THEME_VARS = {
  bg: '--bg', surface: '--surface', surface_2: '--surface-2',
  surface_grad_from: '--surface-grad-from', surface_grad_to: '--surface-grad-to',
  border: '--border', text: '--text', text_dim: '--dim', text_mut: '--muted',
  accent: '--accent', accent_2: '--accent-2', cyan: '--cyan', ok: '--ok',
  warn: '--warn', err: '--err', pink: '--pink', glow: '--glow',
};
function themeStyle(theme) {
  const tokens = theme.tokens || {};
  const parts = [];
  for (const [token, cssVar] of Object.entries(THEME_VARS)) {
    if (tokens[token]) parts.push(`${cssVar}:${tokens[token]}`);
  }
  const radius = Number(tokens.radius);
  parts.push(`--radius:${Number.isFinite(radius) ? radius : 12}px`);
  parts.push(`--bw:${Number.isFinite(Number(tokens.border_width)) ? tokens.border_width : 1}px`);
  parts.push(`--corner-len:${Number.isFinite(Number(tokens.corner_len)) ? tokens.corner_len : 0}px`);
  parts.push(`--glow-opa:${Number.isFinite(Number(tokens.glow_opa)) ? tokens.glow_opa : 0}%`);
  parts.push(`--tracking:${Number.isFinite(Number(tokens.tracking)) ? tokens.tracking : 0}px`);
  return parts.join(';');
}

function widgetFor(type, w, h) {
  const def = T.widgetDef(type);
  const show = SHOWCASE[type] || {};
  const options = { ...T.defaultOptions(def), ...(show.options || {}) };
  if (show.title) options.title = show.title;
  return {
    id: `g-${type}-${w}x${h}`,
    type, x: 0, y: 0, w, h,
    bindings: { ...(show.bindings || {}) },
    options,
  };
}

function frameHtml(widget, scale, pos = { left: 0, top: 0 }) {
  const width = pxOf(widget.w);
  const height = pxOf(widget.h);
  const def = T.widgetDef(widget.type);
  // 与固件 / 编辑器同规则：mini 卡片高度不足 60px 不画标题栏
  const showHeader = !(def && def.generation === 2 && height < 60);

  const frame = makeEl('div');
  frame.className = showHeader ? 'widget' : 'widget title-hidden';
  frame.style.position = 'absolute';
  frame.style.left = `${pos.left}px`;
  frame.style.top = `${pos.top}px`;
  frame.style.width = `${width}px`;
  frame.style.height = `${height}px`;
  frame.style.transform = `scale(${scale})`;
  frame.style.transformOrigin = 'top left';

  const header = makeEl('div');
  header.className = 'widget-header';
  const title = makeEl('div');
  title.className = 'widget-title';
  title.textContent = T.widgetTitle(widget);
  header.appendChild(title);

  const body = makeEl('div');
  body.className = 'widget-body';
  T.renderWidgetBody(widget, body);

  const deco = makeEl('div');
  deco.className = 'widget-deco';
  for (const cls of ['dc tl', 'dc tr', 'dc bl', 'dc br', 'dc ticks', 'dc pulse']) {
    const node = makeEl('i');
    node.className = cls;
    deco.appendChild(node);
  }
  frame.append(...(showHeader ? [header, body, deco] : [body, deco]));
  return {
    html: serialize(frame),
    width, height,
    showHeader,
    scaledW: Math.round(width * scale),
    scaledH: Math.round(height * scale),
  };
}

function themePanel(theme, widget, scale, extraClass = '') {
  const frame = frameHtml(widget, scale);
  return `<div class="theme-scope ${extraClass}" data-theme="${theme.id}" style="${themeStyle(theme)}">
      <div class="zoom" style="width:${frame.scaledW}px;height:${frame.scaledH}px">${frame.html}</div>
    </div>`;
}

/* 320×480 整屏：按示例布局的格子坐标换算像素，10 个组件绝对定位 */
const SCREEN = T.state.profiles['320x480'];
function rectOf(cell) {
  const cw = (SCREEN.width - 2 * SCREEN.padding - (SCREEN.cols - 1) * SCREEN.gap) / SCREEN.cols;
  const ch = (SCREEN.height - 2 * SCREEN.padding - (SCREEN.rows - 1) * SCREEN.gap) / SCREEN.rows;
  return {
    left: SCREEN.padding + cell.x * (cw + SCREEN.gap),
    top: SCREEN.padding + cell.y * (ch + SCREEN.gap),
    width: cell.w * cw + (cell.w - 1) * SCREEN.gap,
    height: cell.h * ch + (cell.h - 1) * SCREEN.gap,
  };
}
function screenHtml(theme) {
  T.state.layout.theme = theme.id;
  clearHistory();
  seedHistory([14, 19, 16, 24, 28, 26, 33]);
  const frames = SHOWCASE_LAYOUT.pages[0].widgets
    .map((cell) => {
      const rect = rectOf(cell);
      const widget = {
        ...cell,
        bindings: cell.bindings || {},
        options: cell.options || {},
      };
      return frameHtml(widget, 1, { left: rect.left, top: rect.top }).html;
    })
    .join('\n');
  return `<div class="theme-scope screen" data-theme="${theme.id}" style="${themeStyle(theme)};width:${SCREEN.width}px;height:${SCREEN.height}px">${frames}</div>`;
}

/* ================= 卡片内容 ================= */
function esc(text) {
  return String(text).replace(/[&<>"]/g, (ch) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;',
  }[ch]));
}

function bindingsTable(def) {
  if (!def.bindings || !def.bindings.length) {
    return '<p class="none">无直接绑定 —— 数据来自下方行配置（rows）。</p>';
  }
  const rows = def.bindings.map((b) => `<tr>
      <td><code>${esc(b.key)}</code></td>
      <td>${esc(b.label)}</td>
      <td>${b.types.map((t) => `<span class="ty">${t}</span>`).join(' ')}</td>
      <td>${b.required ? '<b class="req">必填</b>' : '可选'}</td>
      <td>${b.default ? `<code class="dim">${esc(b.default)}</code>` : '—'}</td>
    </tr>`).join('');
  return `<table class="spec-table">
      <thead><tr><th>键</th><th>含义</th><th>类型</th><th>必填</th><th>默认</th></tr></thead>
      <tbody>${rows}</tbody></table>`;
}

function optionsTable(def) {
  const list = (def.options || []).filter((o) => o.key !== 'title');
  if (!list.length) return '<p class="none">除标题外无可调选项。</p>';
  const rows = list.map((o) => {
    let type = o.type || 'string';
    if (o.type === 'select') type = `select(${(o.options || []).join(' / ')})`;
    let def2 = o.default;
    if (def2 === true) def2 = '开';
    else if (def2 === false) def2 = '关';
    else if (def2 === '') def2 = '空';
    return `<tr>
      <td><code>${esc(o.key)}</code></td>
      <td>${esc(o.label)}</td>
      <td>${esc(type)}</td>
      <td>${esc(def2 ?? '—')}</td>
    </tr>`;
  }).join('');
  return `<table class="spec-table">
      <thead><tr><th>键</th><th>含义</th><th>类型</th><th>默认</th></tr></thead>
      <tbody>${rows}</tbody></table>`;
}

function rowsPreview(def) {
  if (!def.rows) return '';
  const defaults = def.rows.default || [];
  const chips = defaults.map((r) => `<span class="rowchip"><b>${esc(r.label)}</b>${esc(r.binding)}</span>`).join('');
  return `<div class="rows">
      <div class="rows-head">行配置 · ${def.rows.min}–${def.rows.max} 行<span class="rows-note">默认 ${defaults.length} 行</span></div>
      <div class="rowchips">${chips}</div>
    </div>`;
}

function sizeTable(def) {
  const rows = (def.size_options || [def.default_size]).map(([w, h]) => {
    const shape = shapeOf(w, h);
    return `<tr>
      <td><code>${w} × ${h}</code></td>
      <td>${Math.round(pxOf(w))} × ${Math.round(pxOf(h))}</td>
      <td><span class="shape s-${SHAPE_ORDER.indexOf(shape)}">${shape}</span></td>
      <td>${(w * h / (12 * 18) * 100).toFixed(1)}%</td>
    </tr>`;
  }).join('');
  return `<table class="spec-table compact">
      <thead><tr><th>网格</th><th>像素</th><th>形态</th><th>占屏</th></tr></thead>
      <tbody>${rows}</tbody></table>`;
}

/* ================= 组装 ================= */
const screens = themesDoc.themes
  .map((theme) => `<figure class="screen-item">
      <div class="screen-frame">${screenHtml(theme)}</div>
      <figcaption><b>${esc(theme.label)}</b><span>${esc(theme.description || '')}</span></figcaption>
    </figure>`)
  .join('\n');
const cards = [];
const stats = { widgets: 0, sizes: 0, shapes: new Map() };

for (const type of NEW_TYPES) {
  const def = T.widgetDef(type);
  const sizes = def.size_options || [def.default_size];
  stats.widgets += 1;
  stats.sizes += sizes.length;
  for (const [w, h] of sizes) {
    const shape = shapeOf(w, h);
    stats.shapes.set(shape, (stats.shapes.get(shape) || 0) + 1);
  }

  const [dw, dh] = def.default_size;
  const heroWidget = widgetFor(type, dw, dh);
  clearHistory();
  seedHistory([14, 19, 16, 24, 28, 26, 33]);

  /* hero 缩放：让最大边落在 190–230px，小格子放大、长条保持 1:1 */
  const maxPx = Math.max(pxOf(dw), pxOf(dh));
  const heroScale = Math.max(1, Math.min(4, Math.round((205 / maxPx) * 20) / 20));

  const heroPanels = themesDoc.themes
    .map((theme) => {
      const widget = widgetFor(type, dw, dh);
      T.state.layout.theme = theme.id;
      return themePanel(theme, widget, heroScale, 'hero-panel');
    })
    .join('\n');

  /* 尺寸矩阵：每个尺寸 1:1 实测，主题固定为默认主题（几何与主题无关） */
  const defaultTheme = themesDoc.themes.find((t) => t.id === themesDoc.default);
  T.state.layout.theme = defaultTheme.id;
  const stripItems = sizes.map(([w, h]) => {
    const widget = widgetFor(type, w, h);
    const frame = frameHtml(widget, 1);
    return `<figure class="strip-item">
        <div class="theme-scope strip-canvas" data-theme="${defaultTheme.id}" style="${themeStyle(defaultTheme)}">
          <div class="zoom" style="width:${frame.width}px;height:${frame.height}px">${frame.html}</div>
        </div>
        <figcaption><b>${w}×${h}</b><span>${Math.round(frame.width)}×${Math.round(frame.height)}px</span></figcaption>
      </figure>`;
  }).join('\n');

  cards.push(`<article class="card" data-cat="${esc(def.category)}" data-type="${type}">
    <div class="prev">
      ${heroPanels}
      <div class="prev-meta">
        <span class="default-size">${dw} × ${dh} 格</span>
        <span class="zoom-tag">×${heroScale.toFixed(2)}</span>
      </div>
    </div>
    <div class="cbody">
      <div class="tline">
        <span class="tid">${type}</span>
        <h3>${esc(def.label)}</h3>
        <em>${esc(def.category)}</em>
      </div>
      <p class="desc">${esc(def.description || '')}</p>
      <div class="tags">
        <span class="tag">generation 2</span>
        <span class="tag">${sizes.length} 种尺寸</span>
        <span class="tag">${def.bindings.length ? `${def.bindings.length} 个绑定` : `${(def.rows && def.rows.min) || 0}–${(def.rows && def.rows.max) || 0} 行`}</span>
        ${def.resizable ? '<span class="tag">可缩放</span>' : ''}
        ${def.generation === 2 && pxOf(dh) < 60 ? '<span class="tag tag-hl">默认尺寸无标题栏</span>' : ''}
      </div>
      <div class="strip-wrap">
        <div class="strip-label">尺寸矩阵 · ${esc(defaultTheme.label)}实测（1:1 像素）</div>
        <div class="strip">${stripItems}</div>
      </div>
      ${rowsPreview(def)}
      <details class="spec" open>
        <summary>布局分层与实现约束</summary>
        <ul class="rules-list">${(LAYOUT_RULES[type] || []).map((r) => `<li>${esc(r)}</li>`).join('')}</ul>
      </details>
      <details class="spec">
        <summary>绑定与选项</summary>
        <h4 class="sub">指标绑定</h4>
        ${bindingsTable(def)}
        <h4 class="sub">可调选项</h4>
        ${optionsTable(def)}
      </details>
      <details class="spec">
        <summary>尺寸规格（${sizes.length} 项）</summary>
        ${sizeTable(def)}
      </details>
    </div>
  </article>`);
}

/* ================= 样式（真实编辑器 CSS + 图鉴外壳） ================= */
let editorCss = fs.readFileSync(path.join(ROOT, 'server/web/style.css'), 'utf8');
// 主题规则原本挂在 #canvas 上，图鉴里改挂到每个预览面板的 .theme-scope
editorCss = editorCss.replaceAll('#canvas[data-theme=', '.theme-scope[data-theme=');
editorCss = editorCss.split('\n')
  .map((line) => (line.trim() === '#canvas {' ? '.theme-scope {' : line))
  .join('\n');

const shapeCounts = SHAPE_ORDER
  .filter((s) => stats.shapes.get(s))
  .map((s) => `<span class="stat"><b>${stats.shapes.get(s)}</b>${s}</span>`)
  .join('');

const galleryCss = `
/* ---------- 图鉴外壳（覆盖编辑器页面级样式） ---------- */
body{
  background:#f4f6fa;color:#1b2432;margin:0;
  font-family:-apple-system,"PingFang SC","Hiragino Sans GB","Microsoft YaHei","Noto Sans SC",sans-serif;
  line-height:1.6;font-size:14px;
}
.wrap{max-width:1480px;margin:0 auto;padding:26px 22px 70px}
.hero{background:#fff;border:1px solid #e3e8f0;border-radius:16px;padding:24px 26px;margin-bottom:16px}
.hero .badge{display:inline-block;font-size:11.5px;letter-spacing:2px;color:#2f6bd8;background:#e9f0fc;border-radius:999px;padding:3px 12px;font-weight:700}
.hero h1{font-size:25px;margin:10px 0 8px;letter-spacing:.4px}
.hero p{color:#5b6b84;font-size:13.5px;max-width:1000px;margin:0}
.hero .stats{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px}
.stat{background:#f3f6fa;border:1px solid #e3e8f0;border-radius:8px;padding:5px 12px;font-size:12px;color:#5b6b84}
.stat b{color:#1b2432;font-size:14px;margin-right:3px}
.rules{display:grid;grid-template-columns:repeat(auto-fit,minmax(258px,1fr));gap:12px;margin-bottom:18px}
.rule{background:#fff;border:1px solid #e3e8f0;border-radius:12px;padding:14px 16px}
.rule h4{font-size:13.5px;margin:0 0 8px}
.rule p{font-size:12.5px;color:#5b6b84;margin:0}
.rule ul{list-style:none;margin:0;padding:0}
.rule li{font-size:12px;color:#5b6b84;padding-left:14px;position:relative;margin-bottom:4px}
.rule li:before{content:"";position:absolute;left:0;top:8px;width:6px;height:6px;border-radius:2px;background:#2f6bd8}
.showcase{background:#fff;border:1px solid #e3e8f0;border-radius:16px;padding:20px 22px 22px;margin-bottom:18px}
.showcase-head h2{font-size:16px;margin:0 0 6px}
.showcase-head p{font-size:12.5px;color:#5b6b84;margin:0 0 16px;max-width:900px}
.showcase-head code{font-size:11.5px;background:#eef2f8;border-radius:4px;padding:1px 5px;color:#3c4a5f}
.screens{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:16px;justify-items:center}
.screen-item{margin:0;display:flex;flex-direction:column;gap:9px;align-items:center}
.screen-frame{border-radius:18px;overflow:hidden;border:5px solid #10131a;box-shadow:0 10px 26px rgba(16,24,40,.22);line-height:0}
.screen{position:relative;overflow:hidden}
.screen-item figcaption{display:flex;flex-direction:column;align-items:center;gap:1px;font-size:11px;color:#8794a8;max-width:330px;text-align:center}
.screen-item figcaption b{color:#3c4a5f;font-size:12.5px}
.toolbar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:16px;position:sticky;top:0;z-index:20;background:rgba(244,246,250,.95);padding:10px 0;border-bottom:1px solid #e3e8f0;backdrop-filter:blur(6px)}
.toolbar .label{font-size:11.5px;color:#8794a8;letter-spacing:.5px;margin-right:2px}
.chip{border:1px solid #e3e8f0;background:#fff;color:#5b6b84;border-radius:999px;padding:5px 13px;font-size:12.5px;cursor:pointer;transition:all .15s}
.chip:hover{border-color:#2f6bd8;color:#2f6bd8}
.chip.on{background:#2f6bd8;border-color:#2f6bd8;color:#fff}
.chip .dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:-1px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(370px,1fr));gap:16px}
.card{background:#fff;border:1px solid #e3e8f0;border-radius:14px;overflow:hidden;display:flex;flex-direction:column;transition:box-shadow .2s,transform .2s}
.card:hover{box-shadow:0 8px 24px rgba(27,36,50,.10);transform:translateY(-2px)}
.card.hide{display:none}
.prev{position:relative;display:flex;align-items:center;justify-content:center;padding:16px;min-height:170px;background:#0b0d10;border-bottom:1px solid #e3e8f0}
.hero-panel{display:none;background:#0b0d10;border-radius:0;position:relative;overflow:hidden}
body[data-theme="neon-cyber"] .hero-panel[data-theme="neon-cyber"],
body[data-theme="minimal-air"] .hero-panel[data-theme="minimal-air"],
body[data-theme="vintage-gauge"] .hero-panel[data-theme="vintage-gauge"],
body[data-theme="vital-pulse"] .hero-panel[data-theme="vital-pulse"]{display:flex;align-items:center;justify-content:center;padding:14px}
.zoom{position:relative}
.prev-meta{position:absolute;left:10px;bottom:8px;display:flex;gap:6px;font-size:10px;color:#c6cede;letter-spacing:.4px}
.prev-meta span{background:rgba(10,12,16,.62);border-radius:5px;padding:1px 7px}
.cbody{padding:13px 15px 15px;display:flex;flex-direction:column;gap:9px;flex:1}
.tline{display:flex;align-items:baseline;gap:8px;flex-wrap:wrap}
.tid{font-family:Consolas,Menlo,monospace;font-size:11px;color:#2f6bd8;background:#e9f0fc;border-radius:5px;padding:1px 6px;font-weight:700}
.tline h3{font-size:16px;margin:0}
.tline em{font-style:normal;font-size:11px;color:#8794a8;letter-spacing:1px}
.desc{font-size:12.5px;color:#5b6b84;margin:0}
.tags{display:flex;flex-wrap:wrap;gap:6px}
.tag{font-size:11px;border:1px solid #e3e8f0;border-radius:6px;padding:1px 8px;background:#f5f7fa;color:#5b6b84}
.tag-hl{color:#b45309;border-color:#fcd9a5;background:#fff8ec}
.strip-wrap{display:flex;flex-direction:column;gap:5px}
.strip-label{font-size:10.5px;color:#8794a8;letter-spacing:.3px}
.strip{display:flex;flex-wrap:wrap;gap:8px;padding:9px;background:#f5f7fa;border:1px solid #e9edf4;border-radius:10px}
.strip-item{margin:0;display:flex;flex-direction:column;gap:4px;align-items:center}
.strip-canvas{display:block;background:#0b0d10;border-radius:5px;overflow:hidden;line-height:0}
.strip-item figcaption{display:flex;flex-direction:column;align-items:center;font-size:9.5px;color:#8794a8;line-height:1.3}
.strip-item figcaption b{color:#3c4a5f;font-size:10px}
.rows{border:1px solid #e9edf4;border-radius:10px;padding:9px 11px;background:#fbfcfe}
.rows-head{font-size:11.5px;color:#3c4a5f;font-weight:600;margin-bottom:6px;display:flex;justify-content:space-between}
.rows-note{font-weight:400;color:#8794a8}
.rowchips{display:flex;flex-wrap:wrap;gap:5px}
.rowchip{font-size:10.5px;color:#5b6b84;background:#fff;border:1px solid #e3e8f0;border-radius:5px;padding:1px 7px}
.rowchip b{color:#1b2432;margin-right:5px;font-weight:600}
details.spec{border-top:1px dashed #e3e8f0;padding-top:8px}
details.spec summary{font-size:12px;color:#2f6bd8;cursor:pointer;list-style:none;user-select:none}
details.spec summary::-webkit-details-marker{display:none}
details.spec summary:before{content:"▸ ";font-size:10px}
details.spec[open] summary:before{content:"▾ "}
.rules-list{list-style:none;margin:8px 0 0;padding:0}
.rules-list li{font-size:11.5px;color:#5b6b84;padding-left:13px;position:relative;margin-bottom:4px;line-height:1.55}
.rules-list li:before{content:"";position:absolute;left:0;top:7px;width:5px;height:5px;border-radius:50%;background:#9db4d8}
h4.sub{font-size:11.5px;color:#3c4a5f;margin:10px 0 4px}
.none{font-size:11.5px;color:#8794a8;margin:6px 0 0}
.spec-table{width:100%;border-collapse:collapse;font-size:11px;margin-top:4px}
.spec-table th{text-align:left;color:#8794a8;font-weight:600;padding:4px 6px;border-bottom:1px solid #e3e8f0;font-size:10.5px;white-space:nowrap}
.spec-table td{padding:4px 6px;border-bottom:1px solid #f2f5f9;color:#3c4a5f;vertical-align:top}
.spec-table code{font-family:Consolas,Menlo,monospace;font-size:10.5px;color:#2f6bd8}
.spec-table code.dim{color:#8794a8}
.spec-table.compact td{font-size:10.5px}
.ty{font-size:10px;border-radius:4px;padding:0 5px;background:#eef2f8;color:#5b6b84;border:1px solid #e3e8f0}
.req{font-size:10px;color:#b45309}
.shape{font-size:10px;border-radius:4px;padding:0 6px;border:1px solid}
.s-0{color:#0f766e;border-color:#99f6e4;background:#effcf9}
.s-1{color:#b45309;border-color:#fcd9a5;background:#fff8ec}
.s-2{color:#7c3aed;border-color:#ddd0fb;background:#f6f2ff}
.s-3{color:#2563eb;border-color:#c9d9f5;background:#eff4fd}
.s-4{color:#be123c;border-color:#fdc9d4;background:#fff1f4}
.foot{color:#8794a8;font-size:11.5px;margin-top:26px;line-height:1.7}
.foot code{color:#5b6b84;background:#eef2f8;border-radius:4px;padding:0 5px;font-size:11px}
@media (max-width:520px){
  .wrap{padding:14px 12px 50px}
  .grid{grid-template-columns:1fr}
  .prev{min-height:150px;padding:10px}
}
`;

const now = new Date();
const stamp = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, '0')}-${String(now.getDate()).padStart(2, '0')} ${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
const themeChips = themesDoc.themes.map((t) => {
  const tokens = t.tokens || {};
  return `<button class="chip theme-chip" type="button" data-theme-id="${t.id}">
      <span class="dot" style="background:${tokens.accent || '#888'}"></span>${esc(t.label)}
    </button>`;
}).join('');

const categories = [...new Set(NEW_TYPES.map((t) => T.widgetDef(t).category))];
const catChips = categories.map((c) => `<button class="chip cat-chip" type="button" data-cat="${esc(c)}">${esc(c)}</button>`).join('');

const html = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>小组件图鉴 · generation 2（10 类 × 4 主题）</title>
<style>
${editorCss}
${galleryCss}
</style>
</head>
<body data-theme="${themesDoc.default}">
<div class="wrap">

  <section class="hero">
    <span class="badge">GENERATION 2 · WIDGET GALLERY</span>
    <h1>小组件图鉴 · 10 类形态 × 4 套主题</h1>
    <p>这不是手绘稿。每张卡片里的预览都由 <code>server/web/app.js</code> 的真实渲染函数
      (<code>renderWidgetBody</code>) 在无头 DOM 中跑出来后序列化，与编辑器、与固件
      <code>ui/src/ui_mini.c</code> 同一套布局与数值规则。切换主题，预览会一起变色。</p>
    <div class="stats">
      <span class="stat"><b>${stats.widgets}</b>个组件</span>
      <span class="stat"><b>${stats.sizes}</b>个尺寸规格</span>
      <span class="stat"><b>${themesDoc.themes.length}</b>套主题</span>
      ${shapeCounts}
    </div>
  </section>

  <div class="rules">
    <div class="rule">
      <h4>尺寸：从 2×2 到 6×6，不止正方形</h4>
      <p>竖屏 12×18 网格、单元格 18.67px、间距 8px。正方规格覆盖 2×2 → 6×6；
        另配 12×2 / 12×3 / 12×4 长条、2×4 / 2×6 竖条、4×3 / 8×6 横竖矩形。
        卡片高度不足 60px 时自动省略标题栏，把整格留给图形。</p>
    </div>
    <div class="rule">
      <h4>形态：同一组件按宽高比换布局</h4>
      <ul>
        <li>bar_strip 按高度 2 / 3 / 4 切换 inline / 堆叠 / hero</li>
        <li>led_matrix 单元格偏高堆叠、偏宽横排</li>
        <li>gauge_ring 紧凑模式丢掉刻度与单位</li>
      </ul>
    </div>
    <div class="rule">
      <h4>数值：量程与阈值统一口径</h4>
      <ul>
        <li>量程：max_value → %/°C 归 100 → v/(v+1) 软缩放</li>
        <li>越 warn 线转琥珀、越 danger 线转红</li>
        <li>极值柱用第二主色高亮，一眼定位瓶颈</li>
      </ul>
    </div>
    <div class="rule">
      <h4>动效：只在有含义处动</h4>
      <ul>
        <li>在线呼吸 1s 正弦，离线 1Hz 硬闪</li>
        <li>dual_line 主线面积填充 20% 透明度</li>
        <li>big_stat 背景曲线 20% 透明度不抢文字</li>
      </ul>
    </div>
  </div>

  <section class="showcase">
    <div class="showcase-head">
      <h2>整屏拼版 · 320 × 480（12 × 18 格）</h2>
      <p>10 个组件按真实网格拼一屏，布局取自 <code>examples/widgets-showcase-320x480.json</code>，
        含横屏 alt 坐标（480×320 走另一套排布）。四套主题并排对比。</p>
    </div>
    <div class="screens">
${screens}
    </div>
  </section>

  <div class="toolbar">
    <span class="label">主题</span>
    ${themeChips}
    <span class="label" style="margin-left:12px">分类</span>
    <button class="chip cat-chip on" type="button" data-cat="all">全部</button>
    ${catChips}
  </div>

  <div class="grid" id="grid">
${cards.join('\n')}
  </div>

  <p class="foot">
    生成时间 ${stamp} · 数据源 <code>shared/catalog/widgets.json</code> +
    <code>shared/catalog/themes.json</code> · 渲染源 <code>server/web/app.js</code><br>
    重新生成：<code>node docs/design/build_widget_gallery.mjs</code> ·
    回归测试：<code>node server/web/tests/widget-render-smoke.mjs</code>
  </p>
</div>
<script>
  // 主题切换：只切预览面板，配色变量来自 themes.json 注入
  document.querySelectorAll('.theme-chip').forEach(function (chip) {
    chip.addEventListener('click', function () {
      document.querySelectorAll('.theme-chip').forEach(function (c) { c.classList.remove('on'); });
      chip.classList.add('on');
      document.body.dataset.theme = chip.dataset.themeId;
    });
  });
  (function () {
    var active = document.querySelector('.theme-chip[data-theme-id="' + document.body.dataset.theme + '"]');
    if (active) active.classList.add('on');
  })();

  // 分类筛选
  document.querySelectorAll('.cat-chip').forEach(function (chip) {
    chip.addEventListener('click', function () {
      document.querySelectorAll('.cat-chip').forEach(function (c) { c.classList.remove('on'); });
      chip.classList.add('on');
      var want = chip.dataset.cat;
      document.querySelectorAll('.card').forEach(function (card) {
        var hit = want === 'all' || card.dataset.cat === want;
        card.classList.toggle('hide', !hit);
      });
    });
  });
</script>
</body>
</html>
`;

fs.writeFileSync(OUT_FILE, html, 'utf8');
const kb = (fs.statSync(OUT_FILE).size / 1024).toFixed(0);
console.log(`生成完成: ${OUT_FILE}`);
console.log(`  卡片 ${stats.widgets} 张 · 尺寸规格 ${stats.sizes} 项 · 体积 ${kb} KB`);
console.log('  形态分布:', [...stats.shapes.entries()].map(([k, v]) => `${k}${v}`).join(' / '));
