/**
 * Web 编辑器组件渲染回归测试（无头）
 *
 * 用最小 DOM 桩加载 server/web/app.js，逐个渲染目录里的全部组件类型 ×
 * 全部尺寸选项 × 四套主题，并跑一遍属性面板与历史采集，捕捉运行时错误
 * 与关键 DOM 结构退化（圆弧/刻度/柱体/图例/字号阶梯等）。
 *
 * 运行：node server/web/tests/widget-render-smoke.mjs
 * 退出码：0 全绿；1 有失败；2 加载失败。
 */
import { loadEditor } from './lib/headless-editor.mjs';

const { T, makeEl, metrics, widgetsDoc, themesDoc, seedHistory, clearHistory } =
  loadEditor();

/* ================= 断言工具 ================= */
let pass = 0;
let fail = 0;
const failures = [];
function check(label, fn) {
  try {
    fn();
    pass += 1;
  } catch (error) {
    fail += 1;
    const detail = error && error.stack
      ? error.stack.split('\n').slice(0, 3).join(' | ')
      : String(error);
    failures.push(`${label}: ${detail}`);
  }
}
function descendants(el, out = []) {
  for (const child of el.children) {
    out.push(child);
    descendants(child, out);
  }
  return out;
}
function byClass(el, name) {
  return descendants(el, []).filter((c) =>
    (c.attributes.class || '').split(/\s+/).includes(name)
  );
}
function one(el, name) {
  const hits = byClass(el, name);
  if (!hits.length) throw new Error(`找不到 .${name}`);
  return hits[0];
}
function ok(cond, message) {
  if (!cond) throw new Error(message);
}

/* ================= 测试矩阵 ================= */
const NEW_TYPES = [
  'gauge_ring', 'led_dot', 'led_matrix', 'bar_strip', 'vbar_gauge',
  'multi_bars', 'dual_line', 'big_stat', 'spark_row', 'text_badge',
];
const OLD_TYPES = [
  'status_pill', 'kpi', 'sparkline', 'server_card', 'metric_list', 'desktop_pet',
];
const firstMetricOf = (types) => {
  const hit = metrics.find((m) => types.includes(m.type));
  return hit ? hit.id : '';
};

function buildWidget(def, w, h) {
  const bindings = {};
  for (const b of def.bindings) {
    bindings[b.key] =
      b.default && T.state.values[b.default] ? b.default : firstMetricOf(b.types);
  }
  return {
    id: `w-${def.type}-${w}x${h}`,
    type: def.type, x: 0, y: 0, w, h,
    bindings,
    options: T.defaultOptions(def),
  };
}

for (const theme of themesDoc.themes) {
  T.state.layout.theme = theme.id;

  for (const type of [...NEW_TYPES, ...OLD_TYPES]) {
    const def = T.widgetDef(type);
    if (!def) {
      fail += 1;
      failures.push(`widgets.json 缺少组件定义: ${type}`);
      continue;
    }
    for (const [w, h] of def.size_options || [def.default_size]) {
      const widget = buildWidget(def, w, h);
      T.state.layout.pages[0].widgets = [widget];
      clearHistory();
      seedHistory();

      check(`渲染 ${type} ${w}x${h} @${theme.id}`, () => {
        const body = makeEl('div');
        T.renderWidgetBody(widget, body);
        ok(body.children.length > 0, 'body 无子节点');
      });

      check(`属性面板 ${type} ${w}x${h} @${theme.id}`, () => {
        T.state.selectedId = widget.id;
        T.renderInspector();
      });

      if (theme.id !== themesDoc.default) continue;
      check(`结构 ${type} ${w}x${h}`, () => {
        const body = makeEl('div');
        T.renderWidgetBody(widget, body);
        assertStructure(type, widget, body, w, h);
      });
    }
  }
}

function assertStructure(type, widget, body, w, h) {
  if (type === 'gauge_ring') {
    const svg = body.children[0];
    ok((svg.attributes.class || '').includes('gauge-svg'), '根节点不是 gauge-svg');
    ok(byClass(svg, 'gauge-track').length === 1, '缺轨道');
    ok(byClass(svg, 'gauge-arc').length === 1, '缺数值弧');
    const compact = w <= 2 || h <= 2;
    const ticks = byClass(svg, 'gauge-tick').length;
    ok(compact ? ticks === 0 : ticks === 13, `刻度数异常: ${ticks}`);
    ok(byClass(svg, 'gauge-value').length === 1, '缺中心数值');
    ok(compact ? byClass(svg, 'gauge-unit').length === 0 : byClass(svg, 'gauge-unit').length === 1,
      '单位显示与紧凑规则不符');
  }
  if (type === 'led_dot') {
    ok(byClass(body, 'led-dot').length === 1, '缺 LED 点');
    const tiny = w <= 2 && h <= 2;
    ok(tiny ? byClass(body, 'led-text').length === 0 : byClass(body, 'led-text').length === 1,
      '文案显示与 2x2 规则不符');
  }
  if (type === 'led_matrix') {
    const grid = one(body, 'matrix-grid');
    const rows = (widget.options.rows || []).length;
    ok(byClass(grid, 'matrix-cell').length === rows, '单元格数与行配置不符');
    const cols = Number((grid.style.gridTemplateColumns.match(/repeat\((\d+)/) || [])[1]);
    const gridRows = Number((grid.style.gridTemplateRows.match(/repeat\((\d+)/) || [])[1]);
    ok(cols > 0 && gridRows > 0, '缺网格模板');
    ok(cols * gridRows >= rows, '网格容量不足');
    const expectedCols = Math.max(1, Math.min(Math.max(1, rows), Math.ceil(Math.sqrt(Math.max(1, rows) * (w / h)))));
    ok(cols === expectedCols, `列数 ${cols} != ${expectedCols}`);
    ok(byClass(grid, 'matrix-dot').length === rows, '点数量与行数不符');
  }
  if (type === 'bar_strip') {
    const fill = one(body, 'bar-fill');
    const pct = parseFloat(fill.style.width || fill.style.height || '0');
    ok(pct >= 2 && pct <= 100, `填充比例越界: ${pct}`);
    if (h <= 2) ok(byClass(body, 'bar-inline-label').length === 1, 'inline 缺标题');
    if (h === 3) ok(byClass(body, 'bar-stack-top').length === 1, '堆叠布局缺顶部行');
    if (h >= 4) ok(byClass(body, 'bar-hero-left').length === 1, 'hero 布局缺左栏');
  }
  if (type === 'vbar_gauge') {
    ok(byClass(body, 'vbar-track').length === 1, '缺轨道');
    const fill = one(body, 'vbar-fill');
    const pct = parseFloat(fill.style.height || '0');
    ok(pct >= 2 && pct <= 100, `填充比例越界: ${pct}`);
    ok(byClass(body, 'vbar-value').length === 1, '缺数值');
  }
  if (type === 'multi_bars') {
    const wrap = one(body, 'mbars');
    const cols = byClass(wrap, 'mbar-col');
    ok(cols.length === (widget.options.rows || []).length, '柱数与行配置不符');
    const bars = cols.map((c) => parseFloat(one(c, 'mbar-bar').style.height || '0'));
    if (cols.length > 1) {
      const marked = cols.filter((c) => c.classList.contains('max'));
      ok(marked.length === 1, '高亮列不唯一');
      const maxBar = Math.max(...bars);
      const markedBar = bars[cols.indexOf(marked[0])];
      // 并列最大值时固件取最后一根（>= 比较），这里只校验高亮列确实最高
      ok(Math.abs(markedBar - maxBar) < 0.01, `高亮列 ${markedBar}% 不是最高 ${maxBar}%`);
    }
  }
  if (type === 'dual_line') {
    ok(byClass(body, 'dual-item').length === 2, '图例应两项');
    const svg = one(body, 'dual-chart');
    ok(byClass(svg, 'dual-line').length === 2, '应有两条折线');
    ok(byClass(svg, 'dual-fill').length === 1, '缺主线面积填充');
  }
  if (type === 'big_stat') {
    const value = one(body, 'big-value');
    const size = parseFloat(value.style.fontSize || '0');
    ok(size >= 20 && size <= 48, `字号阶梯越界: ${size}`);
    ok(byClass(body, 'big-spark').length === 1, '缺背景曲线');
    ok(byClass(body, 'big-trend').length === 1, '缺趋势行');
    const trend = one(body, 'big-trend').textContent;
    ok(/[▲▼]/.test(trend) || trend.includes('采样中'), `趋势文案异常: ${trend}`);
  }
  if (type === 'spark_row') {
    const cols = byClass(body, 'srow-col');
    ok(cols.length === (widget.options.rows || []).length, '列数与行配置不符');
    for (const col of cols) {
      ok(byClass(col, 'srow-value').length === 1, '列缺数值');
      ok(byClass(col, 'srow-svg').length === 1, '列缺曲线');
      ok(byClass(col, 'srow-label').length === 1, '列缺标签');
      ok(byClass(col, 'srow-line').length === 1, '列缺折线');
    }
  }
  if (type === 'text_badge') {
    const wrap = one(body, 'tbadge');
    if (h >= 2) ok(byClass(wrap, 'tbadge-bar').length === 1, '缺左侧装饰条');
    const text = one(wrap, 'tbadge-text');
    const size = parseFloat(text.style.fontSize || '0');
    ok(size >= 11 && size <= 26, `字号阶梯越界: ${size}`);
  }
}

/* ================= 历史采集覆盖 ================= */
check('collectHistory 覆盖 sparkline / dual_line / big_stat / spark_row', () => {
  const nums = metrics.filter((m) => m.type === 'number').slice(0, 4);
  ok(nums.length === 4, '目录数值指标不足 4 个');
  const mk = (type, bindings, options) => ({
    id: `h-${type}`, type, x: 0, y: 0, w: 6, h: 4, bindings, options,
  });
  T.state.history.clear();
  T.state.layout.pages[0].widgets = [
    mk('sparkline', { value: nums[0].id }, {}),
    mk('dual_line', { value: nums[1].id, value2: nums[2].id }, {}),
    mk('big_stat', { value: nums[3].id }, {}),
    mk('spark_row', {}, { rows: [{ label: 'A', binding: nums[0].id, kind: 'number' }] }),
  ];
  T.collectHistory();
  for (const m of nums) {
    ok(T.state.history.has(m.id), `未采集历史: ${m.id}`);
    ok(T.state.history.get(m.id).length === 1, `重复采样: ${m.id}`);
  }
});

/* ================= mini helper 与固件逻辑一致性 ================= */
check('miniUnit：覆盖优先、回落指标单位', () => {
  const metric = { unit: 'MB' };
  ok(T.miniUnit({ unit: ' GB ' }, metric) === 'GB', '覆盖值未 trim');
  ok(T.miniUnit({}, metric) === 'MB', '未回落指标单位');
  ok(T.miniUnit({}, null) === '', '空指标应返回空串');
});

check('miniRatio：显式量程 / % / 软缩放 / 钳位', () => {
  const close = (a, b) => Math.abs(a - b) < 1e-9;
  ok(close(T.miniRatio(62, { max_value: 0 }, '%'), 0.62), '% 单位应按 100 量程');
  ok(close(T.miniRatio(50, { max_value: 200 }, 'x'), 0.25), '显式量程失效');
  ok(close(T.miniRatio(61, {}, '°C'), 0.61), '°C 应按 100 量程');
  ok(close(T.miniRatio(1, {}, 'tok'), 0.5), '软缩放 v/(v+1) 失效');
  ok(T.miniRatio(-5, {}, 'tok') === 0, '负数未钳到 0');
  ok(T.miniRatio(null, {}, 'tok') === 0, '空值未钳到 0');
  ok(T.miniRatio(999, { max_value: 100 }, '%') === 1, '超量程未钳到 1');
});

check('miniValueColor：越线着色优先级', () => {
  const opts = { warn_above: 80, danger_above: 90 };
  ok(T.miniValueColor(95, opts) === 'var(--err)', 'danger 分支失效');
  ok(T.miniValueColor(85, opts) === 'var(--warn)', 'warn 分支失效');
  ok(T.miniValueColor(10, opts) === null, '正常区不应内联着色');
  ok(T.miniValueColor(10, {}) === null, '无阈值不应内联着色');
  ok(T.miniValueColor(10, { warn_above: 80, danger_above: 90 }) === null, '边界内不应着色');
});

check('阈值越线时圆弧内联 err 色', () => {
  const def = T.widgetDef('gauge_ring');
  const num = firstMetricOf(['number']);
  T.state.values[num] = { a: true, v: 95 };
  const widget = {
    id: 'w-thresh', type: 'gauge_ring', x: 0, y: 0, w: 4, h: 4,
    bindings: { value: num },
    options: { ...T.defaultOptions(def), warn_above: 80, danger_above: 90 },
  };
  const body = makeEl('div');
  T.renderWidgetBody(widget, body);
  const arc = one(body, 'gauge-arc');
  ok(arc.style.stroke === 'var(--err)', `圆弧未着色: ${arc.style.stroke}`);
  const text = one(body, 'gauge-value');
  ok(text.style.fill === 'var(--err)', `数值未着色: ${text.style.fill}`);
});

/* ================= 汇总 ================= */
console.log(`\n===== 组件渲染冒烟测试：${pass} 通过 / ${fail} 失败 =====`);
if (fail) {
  console.log('失败明细：');
  for (const item of failures.slice(0, 25)) console.log(`  ✗ ${item}`);
  if (failures.length > 25) console.log(`  …还有 ${failures.length - 25} 条`);
  process.exit(1);
}
