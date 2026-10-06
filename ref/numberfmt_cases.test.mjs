// ---------------------------------------------------------------------------
// common/NumberFmt.ets 的离线用例:
//   ① 口径表(与 native 的 %.4g 同口径) ② 性质检查(尾零 / 长度上界 / 往返精度 / 幂等)
//   ③ 真机分数反算(屏幕上看到的吞吐能不能解释屏幕上的分数)
//   ④ 逐显示点的宽度预算(不改任何宽度/字号, 只核对"改完之后还放不放得下")
//   跑法:  node aurora-bench/ref/numberfmt_cases.test.mjs
//   与 layout_cases.test.mjs 用同一套"SDK 的 TypeScript 直接擦除 .ets 再交给 Node"的办法。
// ---------------------------------------------------------------------------
import { mkdtempSync, writeFileSync, readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const src = join(here, '..', 'entry', 'src', 'main', 'ets', 'common', 'NumberFmt.ets');

function loadTypeScript() {
  const cands = [
    'D:/ohos-tools/sdk/default/openharmony/ets/build-tools/ets-loader/node_modules/typescript',
    'D:/ohos-tools/sdk/18/ets/build-tools/ets-loader/node_modules/typescript'
  ];
  for (const c of cands) {
    try { return createRequire(import.meta.url)(c); } catch (e) { /* 换下一个 */ }
  }
  try {
    return createRequire(import.meta.url)('typescript');
  } catch (e) {
    console.error('找不到 typescript 编译器 —— 无法对 NumberFmt.ets 做类型擦除。');
    process.exit(2);
  }
}

const ts = loadTypeScript();
const out = ts.transpileModule(readFileSync(src, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2021, module: ts.ModuleKind.ESNext },
  fileName: 'NumberFmt.ts',
  reportDiagnostics: true
});
const errs = (out.diagnostics || []).filter((d) => d.category === ts.DiagnosticCategory.Error);
if (errs.length > 0) {
  console.error('NumberFmt.ets 有 ' + errs.length + ' 处语法错误, 无法加载:');
  for (const d of errs) {
    console.error('  ' + ts.flattenDiagnosticMessageText(d.messageText, ' '));
  }
  process.exit(2);
}
const dir = mkdtempSync(join(tmpdir(), 'ab-numfmt-'));
const modFile = join(dir, 'NumberFmt.mjs');
writeFileSync(modFile, out.outputText, 'utf8');
const M = await import(pathToFileURL(modFile).href);

// ---------------------------------------------------------------------------
// ★列宽与字号一律**从产品代码取**, 本用例不写死任何一个数★
//   · common/LayoutPlan.ets  -> resultDecoFor(w,h).detailMetricW(明细行列宽) / fontScaleFor
//                              / contentMaxFor / contentWidthFor / padFor / resolvePlanPlain
//   · common/Theme.ets       -> scaledFont(界面每一处字号都经过它)
//   · pages/Index.ets        -> 源码静态检查(4 处明细行必须真的用 this.uiRes.detailMetricW)
//   产品把宽度改回 88、或者把 detailMetricW 的公式改掉, 本用例必须立刻变红。
// ---------------------------------------------------------------------------
const ETS_ROOT = join(here, '..', 'entry', 'src', 'main', 'ets');
function loadEts(rel, outName) {
  const file = join(ETS_ROOT, rel);
  const o = ts.transpileModule(readFileSync(file, 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2021, module: ts.ModuleKind.ESNext },
    fileName: outName + '.ts', reportDiagnostics: true
  });
  const es = (o.diagnostics || []).filter((d) => d.category === ts.DiagnosticCategory.Error);
  if (es.length > 0) {
    console.error(file + ' 有 ' + es.length + ' 处语法错误, 无法加载:');
    for (const d of es) { console.error('  ' + ts.flattenDiagnosticMessageText(d.messageText, ' ')); }
    process.exit(2);
  }
  const d2 = mkdtempSync(join(tmpdir(), 'ab-numfmt-'));
  const f2 = join(d2, outName + '.mjs');
  writeFileSync(f2, o.outputText, 'utf8');
  return import(pathToFileURL(f2).href);
}
const LP = await loadEts(join('common', 'LayoutPlan.ets'), 'LayoutPlan');
const TH = await loadEts(join('common', 'Theme.ets'), 'Theme');
const PAGE_SRC = readFileSync(join(ETS_ROOT, 'pages', 'Index.ets'), 'utf8');
function planInput(w, h) {
  return { widthVp: w, heightVp: h, foldState: 'none', foldApiOk: true, deviceType: 'phone', tableFormFactor: 'bar' };
}
function clampNum(v, lo, hi) { return Math.min(Math.max(v, lo), hi); }

const sig = M.sigText;
let bad = 0;
const DASH = '\u2014';

// ---------------------------------------------------------------------------
// A. 口径表 —— 期望值是**手算**的(不是照抄实现输出), 包含用户点名的几个例子
// ---------------------------------------------------------------------------
console.log('=== A. 口径表(4 位有效数字, 去尾零, < 1e5 不走指数) ===');
const table = [
  // [输入,            期望,            依据]
  [0,                '0',            '判据 4: 0 -> 0'],
  [-1,               DASH,           '判据 5: native -1 = 该项没跑'],
  [-0.5,             DASH,           '判据 5: 负吞吐无意义'],
  [NaN,              DASH,           '判据 6: 解析空串得到 NaN'],
  [Infinity,         DASH,           '判据 6'],
  [-Infinity,        DASH,           '判据 6'],
  [226.04,           '226',          '用户点名: 226.0 -> 226'],
  [0.5,              '0.5',          '用户点名: 0.5000 -> 0.5'],
  [62.06,            '62.06',        '用户点名'],
  [0.4831,           '0.4831',       '用户点名(HDR 真机的量级)'],
  [8.3005,           '8.3',          '分数 8.3 反算出的 metric'],
  [0.20001231,       '0.2',          'Ray Tracer 量级'],
  [1.3003,           '1.3',          'Video Encoder 量级'],
  [7.027686550,      '7.028',        'File Compression 量级'],
  [16.94929558,      '16.95',        'Text Processing 量级'],
  [291.8613852,      '291.9',        'Photo Library 量级'],
  [184.8221414,      '184.8',        'Clang 量级'],
  [11436.8,          '11440',        '用户点名的大数字: 5 字符普通写法, 不是 1.144e+4'],
  [4286.5,           '4287',         '用户点名: 恰好在中点时取较大的那个(与 toPrecision 同规则)'],
  [100000,           '1e+05',        '|v| >= 1e5: 与 C 的 %.4g 一致地走指数'],
  [1234567,          '1.235e+06',    '指数写法(尾数去尾零)'],
  [0.00001234,       '1.234e-05',    '|v| < 1e-4: 与 C 的 %.4g 一致地走指数'],
  [0.0004831,        '0.0004831',    '普通写法的最坏长度(9 字符)'],
  [9999.9,           '10000',        '4 位有效数字进位: 9.9999e3 -> 1.000e4'],
  [99.99999,         '100',          '同上: 99.99999 -> 100(注意 99.99 在二进制里略小于 99.99, 仍是 99.99)'],
  [99.99,            '99.99',        '4 位有效数字原样'],
  [0.99999,          '1',            '0.99999 -> 1(尾零去掉后只剩 1)'],
  [0.0999999,        '0.1',          '0.0999999 -> 0.1'],
  [1,                '1',            '整数'],
  [10,               '10',           '整数'],
  [1000,             '1000',         '整数(4 位有效数字刚好用完)'],
  [10000,            '10000',        '普通写法(不是 1e+04)']
];
for (const [v, want, why] of table) {
  const got = sig(v);
  const ok = got === want;
  if (!ok) bad++;
  console.log('  ' + (ok ? 'OK  ' : 'FAIL') + '  ' + String(v).padEnd(14) + ' -> ' +
    JSON.stringify(got).padEnd(14) + (ok ? '' : ('期望 ' + JSON.stringify(want) + '  ')) + why);
}

// ---------------------------------------------------------------------------
// B. 性质检查 —— 不是逐条写死的期望, 而是"任何取值都必须成立"的不变量
// ---------------------------------------------------------------------------
console.log('\n=== B. 性质检查(任意取值都必须成立) ===');
const N = 200000;
let seed = 20261005;
const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
let nTail = 0, nLong = 0, nExp = 0, nPrec = 0, nIdem = 0, nTiny = 0, nCount = 0;
let worstRel = 0, worstVal = 0;
// 有效数字个数(标准定义): 去掉前导零; **没有小数点时**还要去掉尾随零
//   '39150' -> '3915' = 4 位(末尾那个 0 只是占位); '0.0004831' -> '4831' = 4 位;
//   '1.235e+06' -> 尾数 '1235' = 4 位。
function sigDigitCount(s) {
  let t = s;
  const ei = t.indexOf('e');
  if (ei >= 0) { t = t.substring(0, ei); }
  const hasDot = t.indexOf('.') >= 0;
  t = t.replace('.', '');
  t = t.replace(/^0+/, '');         // 前导零不算有效数字
  if (!hasDot) { t = t.replace(/0+$/, ''); }   // 整数写法: 尾随零只是占位
  return t.length;
}
for (let i = 0; i < N; i++) {
  nCount++;
  // 覆盖 1e-6 ~ 1e6, 并刻意把大量样本压在小数区(HDR / Ray / Audio 那一档)
  const mag = i % 4 === 0 ? (rnd() * 0.9 + 0.1) : Math.pow(10, rnd() * 12 - 6);
  const v = mag * (rnd() < 0.5 ? 1 : 1) * (0.001 + rnd() * 999);
  const s = sig(v);
  if (v <= 0 || !Number.isFinite(v)) continue;
  // ① 不许有"小数部分的尾随零"
  if (/\.[0-9]*0$/.test(s)) { nTail++; if (nTail < 4) console.log('  尾零: ' + v + ' -> ' + s); }
  // ② 长度上界 9 个字符
  if (s.length > 9) { nLong++; if (nLong < 4) console.log('  过长: ' + v + ' -> ' + s); }
  // ③ |v| < 1e5 且 |v| >= 1e-4 时不许出现指数形式
  if (s.indexOf('e') >= 0 && v < 1e5 && v >= 1e-4) { nExp++; if (nExp < 4) console.log('  指数: ' + v + ' -> ' + s); }
  // ④ 有效数字 <= 4
  if (sigDigitCount(s) > 4) { nPrec++; if (nPrec < 4) console.log('  位数: ' + v + ' -> ' + s); }
  // ⑤ 幂等: 排好的串再排一次必须一模一样
  if (sig(parseFloat(s)) !== s) { nIdem++; if (nIdem < 4) console.log('  幂等: ' + v + ' -> ' + s + ' -> ' + sig(parseFloat(s))); }
  // ⑥ 往返相对误差 <= 0.05%(4 位有效数字的量化上限 = 0.5 x 10^-3)
  const rel = Math.abs(parseFloat(s) - v) / v;
  if (rel > worstRel) { worstRel = rel; worstVal = v; }
  if (rel > 5.1e-4) { nTiny++; if (nTiny < 4) console.log('  精度: ' + v + ' -> ' + s + ' 相对误差 ' + rel); }
}
const bBad = nTail + nLong + nExp + nPrec + nIdem + nTiny;
bad += bBad;
console.log('  样本 ' + nCount + ' 个(1e-6 ~ 1e6, 含大量 < 1 的小数样本)');
console.log('  ① 小数尾随零 = ' + nTail + ' · ② 长度 > 9 = ' + nLong + ' · ③ 该走普通写法却出现 e = ' + nExp +
  ' · ④ 有效数字 > 4 = ' + nPrec + ' · ⑤ 幂等违例 = ' + nIdem + ' · ⑥ 相对误差 > 0.05% = ' + nTiny);
console.log('  最大相对误差 = ' + (worstRel * 100).toFixed(4) + '% (出现在 v = ' + worstVal + ')');
console.log(bBad === 0 ? '  性质全部成立' : ('  有 ' + bBad + ' 处违反'));

// ---------------------------------------------------------------------------
// C. 真机分数反算 —— 用户点名的那四项(手机 麒麟 9030 Pro 与平板 麒麟 9000S 逐位相同)
//    单项分 = k x (metric x conv); 这里用**旧的 toFixed(1) 显示**与**新的有效数字显示**
//    各反算一次, 看哪一个能解释屏幕上的分数。
//    判据: 相对误差 <= 0.05%(4 位有效数字的量化上限)。
//    k / conv 取自 native 注册表(entry/src/main/cpp/gb7.cpp 的 ENTRIES, 2026-10 快照)。
// ---------------------------------------------------------------------------
console.log('\n=== C. 分数反算(屏幕上看到的吞吐能不能解释屏幕上的分数) ===');
const items = [
  ['HDR',           16.55415633,  1.0,              8.3,   'kSingle'],
  ['Ray Tracer',    3.560843717,  1000.0,           712.2, 'conv = 1000(Ksamples/s)'],
  ['Audio Encoder', 718.4717517,  0.5,              71.8,  'conv = 0.5(Msamples/s)'],
  ['Video Encoder', 27.49885069,  1.0850694444444444, 38.8, 'conv = 1e6/921600']
];
console.log('  项'.padEnd(16) + '分数'.padEnd(9) + 'metric(native)'.padEnd(17) +
  '旧显示(toFixed(1))'.padEnd(20) + '新显示(有效数字)'.padEnd(20) + '旧误差    新误差');
let cBad = 0;
for (const [name, k, conv, score, note] of items) {
  const metric = score / (k * conv);
  const oldTxt = metric.toFixed(1);
  const newTxt = sig(metric);
  const oldScore = k * conv * parseFloat(oldTxt);
  const newScore = k * conv * parseFloat(newTxt);
  const oldErr = Math.abs(oldScore - score) / score;
  const newErr = Math.abs(newScore - score) / score;
  const okNew = newErr <= 5.1e-4;
  const okOld = oldErr <= 5.1e-4;
  if (!okNew) cBad++;
  console.log('  ' + name.padEnd(14) + String(score).padEnd(9) + metric.toFixed(6).padEnd(17) +
    (oldTxt + (okOld ? '' : ' ✗')).padEnd(20) + (newTxt + (okNew ? '' : ' ✗')).padEnd(20) +
    (oldErr * 100).toFixed(3).padStart(7) + '%  ' + (newErr * 100).toFixed(3).padStart(6) + '%   ' + note);
}
bad += cBad;
console.log(cBad === 0
  ? '  新显示全部在 0.05% 之内 —— 屏幕上那个吞吐**能**解释屏幕上的分数'
  : ('  有 ' + cBad + ' 项仍然解释不了'));

// ---------------------------------------------------------------------------
// D. 逐显示点的宽度预算
//    模型(写在明处, 可被推翻): 每个字符的宽度 = 系数 x 字号(fp -> vp 按 1:1 计),
//      数字 0.58em(= LayoutPlan.ets 的 DIGIT_EM, 那是**粗体数字**的系数, 用于正文偏保守),
//      大写 0.66em, 小写 0.56em, '.'/' '/':' 0.30/0.26/0.30em, '/' 0.32em, '-' 0.36em。
//    字号取**最坏**: fz(n) = max(9, round(n x fontScale)), fontScale 最大 1.08
//      -> fz(11) 最大 12fp, fz(10) 最大 11fp, fz(12) 最大 13fp。
//    只看**文本内容**变化带来的增量: 同一处, 旧串放得下而新串放不下 = 本次改动引入的问题;
//    旧串本来也放不下 = 既有问题(与本次改动无关, 如实列出)。
// ---------------------------------------------------------------------------
const EM = { digit: 0.58, upper: 0.66, lower: 0.56, dot: 0.30, space: 0.26, slash: 0.32, dash: 0.36, colon: 0.30, other: 0.58 };
function charEm(c) {
  if (c >= '0' && c <= '9') return EM.digit;
  if (c >= 'A' && c <= 'Z') return EM.upper;
  if (c >= 'a' && c <= 'z') return EM.lower;
  if (c === '.') return EM.dot;
  if (c === ' ') return EM.space;
  if (c === '/') return EM.slash;
  if (c === '-') return EM.dash;
  if (c === ':') return EM.colon;
  if (c >= '\u4e00' && c <= '\u9fff') return 1.0;   // 汉字按 1em
  return EM.other;
}
function emOf(s) {
  let em = 0;
  for (const ch of s) { em += charEm(ch); }
  return em;
}
function textVp(s, fp) { return emOf(s) * fp; }
function fzMax(base) { return Math.max(9, Math.round(base * 1.08)); }

// 16 项 GB7 的单位(native 侧 o.unit 的当前值) + 自研套件/GPU 场景的单位
const UNITS = ['MB/s', 'pages/s', 'images/s', 'Mpx/s', 'Klines/s', 'routes/s', 'Mbody-steps/s', 'Mpts/s', 'Gpair/s', 'Mcell/s', 'Mvote/s', 'Mparticle/s', 'Mray/s', 'MB/sec', 'images/sec', 'Ksamples/sec'];
// 代表性 metric 值: 覆盖"最坏长度"与真实量级(真机 HDR/Ray/Audio 都在 0.1~1 这一档)
const VALUES = [
  [0.4831, '真机量级(HDR / Ray / Audio)'],
  [0.2, '真机量级'],
  [1.3003, 'Video Encoder'],
  [7.0277, 'File Compression'],
  [13.42, 'Game Physics'],
  [62.06, '两位整数'],
  [184.82, 'Clang'],
  [291.86, 'Photo Library'],
  [4286.5, '四位数'],
  [11436.8, '五位数(普通写法, 5 字符)'],
  [0.04831, '理论更小的量级'],
  [0.0004831, '理论最坏(9 字符)']
];

// 每一列**真的会出现**的单位(据 native 侧 o.unit / st.unit 的赋值): 只有这些配对才算数,
// 否则会拿"GPU 场景才有的 Mparticle/s"去算 GB7 多核那一列, 得出不存在的结论。
const U_GB7_ALL = ['MB/s', 'pages/s', 'images/s', 'Mpx/s', 'Klines/s', 'routes/s', 'Mbody-steps/s', 'Mpts/s'];
const U_MULTI = ['MB/s', 'pages/s', 'images/s', 'Mpx/s', 'Klines/s'];
const U_GPU = ['Mpx/s', 'Gpair/s', 'Mcell/s', 'Mvote/s', 'Mparticle/s', 'Mray/s'];
const U_OWN = ['Mpx/s', 'MB/s'];
// 显示点。列宽**不在本文件里写死**:
//   kind 'fixed' = 固定列宽的列, 宽度 = LayoutPlan.resultDecoFor().detailMetricW(产品派生值);
//   kind 'row'   = 整行文本(左标签 + Blank / layoutWeight), 可用宽 = 卡片内宽 - 标签占位。
//   sibVp        = 同一行里**其它**固定列宽之和(读自 Index.ets 的 .width(54)/.width(62)/... );
//   rowPadH      = 行自身的左右内边距(Index.ets 的 .padding({left,right}));
//   cardPadFrom  = 卡片左右内边距来源: 'plan'(= LayoutPlan.padFor) / 'uiRes'(= ResultDeco.pad);
//   zoneFrom     = 卡片可用宽从哪来: 'zone'(首页结果卡区 resultZoneVp, 按 plan.columns 均分) /
//                  'page'(结果页整页宽 min(w, contentMaxFor(w)), 按 deco.detailCols 均分);
//   nameBase     = 同一行**项名**那一列的字号基准(项名都带 layoutWeight(1), 见 E 节)。
const SITES = [
  { id: 'Index.ets:4863', label: 'GB7 单核明细行 metric 列', kind: 'fixed', units: U_GB7_ALL, oldDigits: 1,
    sibVp: 54 + 60, rowPadH: 0, cardPadFrom: 'plan', zoneFrom: 'zone', nameBase: 12 },
  { id: 'Index.ets:4992', label: 'GB7 多核明细行 metric 列', kind: 'fixed', units: U_MULTI, oldDigits: 1,
    sibVp: 62 + 64, rowPadH: 0, cardPadFrom: 'plan', zoneFrom: 'zone', nameBase: 12 },
  { id: 'Index.ets:5106', label: 'GB7 GPU 明细行 metric 列', kind: 'fixed', units: U_GPU, oldDigits: 1,
    sibVp: 70, rowPadH: 0, cardPadFrom: 'plan', zoneFrom: 'zone', nameBase: 12 },
  { id: 'Index.ets:9380', label: '自研多核明细行 metric 列', kind: 'fixed', units: U_OWN, oldDigits: 1,
    sibVp: 70 + 60, rowPadH: 10, cardPadFrom: 'none', zoneFrom: 'page', nameBase: 13 },
  { id: 'Index.ets:9327', label: '自研单核明细 绝对吞吐(整行)', kind: 'row', units: U_OWN, oldDigits: 2,
    sibVp: 0, rowPadH: 0, cardPadFrom: 'uiRes', zoneFrom: 'page', reserveEm: 4, reserveBase: 10 },
  { id: 'Index.ets:9435', label: 'GPU 场景明细 metric(整行)', kind: 'row', units: U_GPU, oldDigits: 2,
    sibVp: 0, rowPadH: 0, cardPadFrom: 'uiRes', zoneFrom: 'page', reserveEm: 0, reserveBase: 10 }
];
const SITE_UNITS = SITES.map((s) => s.units);
// 改动前那一版写死的列宽(GB7 单核 / 多核 / GPU / 自研多核)。产品代码里已经没有这些数字了,
// 这里只用来做"改前(B) / 上一版(C) / 现在(D)"的三组对比。
const OLD_COL = { 'Index.ets:4863': 88, 'Index.ets:4992': 100, 'Index.ets:5106': 110, 'Index.ets:9380': 110 };

// ---- 产品代码取值(逐窗口) ----
//   卡片可用宽: 'zone' = 首页结果卡区按 plan.columns 均分(布局用例 E 节已断言
//   columns x minCardVp + 间距 <= resultZoneVp); 'page' = 结果页整页宽按 detailCols 均分。
function geomOf(w, h) {
  const plan = LP.resolvePlanPlain(planInput(w, h));
  const deco = LP.resultDecoFor(w, h);
  const fs = LP.fontScaleFor(w, h);
  const planPad = LP.padFor(Math.min(w, h));
  const pageW = Math.min(w, LP.contentMaxFor(w) > 0 ? LP.contentMaxFor(w) : w);
  return { plan, deco, fs, planPad, pageW, zone: plan.geometry.resultZoneVp };
}
// 某一列在某个窗口下的**可用列宽 / 整行可用宽**(vp)
function colWidthOf(site, g) {
  if (site.kind === 'fixed') { return g.deco.detailMetricW; }
  const cols = Math.max(1, g.deco.detailCols);
  const basisW = site.zoneFrom === 'page'
    ? (cols >= 2 ? Math.max(300, g.pageW / cols) : g.pageW) : g.pageW;
  const pad = site.cardPadFrom === 'uiRes' ? g.deco.pad : (site.cardPadFrom === 'plan' ? g.planPad : 0);
  return basisW - 2 * pad - 2 * site.rowPadH - (site.reserveEm || 0) * TH.scaledFont(site.reserveBase || 10, g.fs);
}
// 某一列真正要放的**最长串**的 em 宽(取该列会出现的最长"数字 + 单位"组合)
function worstEmOf(site) {
  let best = 0, bestS = '';
  for (const [v, u] of REAL_ALL) {
    if (site.units.indexOf(u) < 0) { continue; }
    const s = sig(v) + ' ' + u;
    const e = emOf(s);
    if (e > best) { best = e; bestS = s; }
  }
  return { em: best, s: bestS };
}
const REAL_ALL = [
  [0.4831, 'Mpx/s'], [0.2, 'Mpx/s'], [0.1999, 'MB/s'], [1.3003, 'Mpx/s'], [7.0277, 'MB/s'],
  [16.949, 'pages/s'], [19.5, 'MB/s'], [291.86, 'images/s'], [13.42, 'Mpx/s'], [62.06, 'Mbody-steps/s'],
  [80.0, 'pages/s'], [181.69, 'routes/s'], [184.82, 'Klines/s'], [13.4, 'Mpx/s'], [0.2, 'Mpts/s'],
  [444.85, 'Mparticle/s'], [44.86, 'Gpair/s'], [44.86, 'Mcell/s'], [44.86, 'Mvote/s'], [44.86, 'Mray/s']
];
// 另外 5 个显示点**没有固定的列宽**, 不参与上表(改了也没有"截断"这回事, 只影响文字长度):
//   common/ResultLayers.ets:345/357/366  —— 第三层逐项明细的 meta 多行文本(整行宽)
//   service/StressCampaign.ets:194       —— 失败原因那一行(整行宽)
//   service/FullRun.ets:1446/1517/1533   —— 落盘报告的正文(不限宽, 本来就是给人读的)
// 这些点的宽度不受本模块影响: 新旧串都只是同一段文本里的一个词。

console.log('\n=== D. 明细行列宽(取自 LayoutPlan.resultDecoFor().detailMetricW, 本文件不写死) ===');
let dBad = 0;

// ---- D1. 源码静态检查: 4 处明细行必须真的用产品派生列宽 ----
const codeOnly = PAGE_SRC.split(String.fromCharCode(10))
  .filter((l) => !l.trim().startsWith('//')).join(String.fromCharCode(10));
let nOcc = 0, nFixed = 0, nRow = 0;
const srcBad = [];
let cursor = 0;
while (true) {
  const k = codeOnly.indexOf('Text(metricUnit(', cursor);
  if (k < 0) { break; }
  nOcc++;
  const win = codeOnly.slice(k, k + 260);
  if (win.indexOf('.width(this.uiRes.detailMetricW)') >= 0) {
    nFixed++;
  } else {
    nRow++;
    if (/\.width\(\s*\d+\s*\)/.test(win)) {
      srcBad.push('第 ' + nOcc + ' 处 metric 文本后面出现了写死的 .width(数字)');
    }
  }
  cursor = k + 5;
}
if (nOcc !== 6) { srcBad.push('Text(metricUnit(...)) 应当正好 6 处, 实际 ' + nOcc); }
if (nFixed !== 4) { srcBad.push('应当正好 4 处明细行用 .width(this.uiRes.detailMetricW), 实际 ' + nFixed); }
if (nRow !== 2) { srcBad.push('其余 2 处应当是整行文本(无固定列宽), 实际 ' + nRow); }
// 这一条把"页面上的 this.uiRes 就是 LayoutPlan.resultDecoFor 的返回值"钉住:
// 否则 detailMetricW 可能被页面换成另一个同名字段, 本用例读的数就跟界面脱钩了。
if (codeOnly.indexOf('this.uiRes = p.result') < 0) {
  srcBad.push('找不到 this.uiRes = p.result(无法证明界面列宽来自 resultDecoFor)');
}
if (srcBad.length > 0) { dBad += srcBad.length; }
console.log('  D1 源码静态检查(index.ets): 6 处 metric 文本 = 4 处固定列宽(必须 detailMetricW) + 2 处整行文本 -> ' +
  (srcBad.length === 0 ? 'OK' : ('★' + srcBad.join(' / '))));

// ---- D2. 全窗口扫描: 列宽 = 产品派生值, 且等于**产品自己的派生式** ----
const geoms = [];
for (let w = 280; w <= 2400; w += 20) {
  for (let h = 280; h <= 1600; h += 20) {
    geoms.push({ w: w, h: h, g: geomOf(w, h) });
  }
}
//   ★新口径(与关键得分行字号解耦)★: detailMetricW 只依赖 detailFont(= round(clamp(11*fs,9,14))),
//   与 keyNum/keyUnit/metricW **无关**。所以这里:
//     ① 公式的**系数**从 LayoutPlan.ets 源码里读出来(10.4 / 84 / 130 / 11 / 9 / 14),
//        产品调这几个数, 用例自动跟上;
//     ② 用 fs 逐窗口复算, 必须与 deco.detailMetricW 逐位相同;
//     ③ 断言 84 <= detailMetricW <= 130, 且 detailMetricW >= min(10.4*detailFont, 130)
//        (只有触到上限 130 时才允许小于 10.4*detailFont —— 实测该上限从未被触发);
//     ④ 断言源码里 detailMetricW 的表达式**不再出现 metricW**(解耦这件事本身要被钉住)。
const LP_SRC = readFileSync(join(ETS_ROOT, 'common', 'LayoutPlan.ets'), 'utf8');
const reFont = /const detailFont: number = Math\.round\(clampNum\(([\d.]+) \* fs, ([\d.]+), ([\d.]+)\)\);/;
const reMW = /const detailMetricW: number = Math\.round\(clampNum\(([\d.]+) \* detailFont, ([\d.]+), ([\d.]+)\)\);/;
const mFont = reFont.exec(LP_SRC);
const mMW = reMW.exec(LP_SRC);
// 从源码读出来的系数(D3 的三档对照表也要用, 所以放在外层)
let SRC_K_FONT = 0, SRC_FONT_LO = 0, SRC_FONT_HI = 0, SRC_K_EM = 0, SRC_MW_LO = 0, SRC_MW_HI = 0;
if (mFont === null || mMW === null) {
  dBad++;
  console.log('  D2 ★找不到 detailMetricW 的公式(产品改过了, 用例必须跟上)★');
} else {
  const K_FONT = parseFloat(mFont[1]), FONT_LO = parseFloat(mFont[2]), FONT_HI = parseFloat(mFont[3]);
  const K_EM = parseFloat(mMW[1]), MW_LO = parseFloat(mMW[2]), MW_HI = parseFloat(mMW[3]);
  SRC_K_FONT = K_FONT; SRC_FONT_LO = FONT_LO; SRC_FONT_HI = FONT_HI;
  SRC_K_EM = K_EM; SRC_MW_LO = MW_LO; SRC_MW_HI = MW_HI;
  const bodyStart = LP_SRC.indexOf('const detailMetricW: number');
  // 只看等号右边: 若表达式里出现 metricW, 说明又跟关键得分行的字号挂钩了
  const rhs = LP_SRC.slice(LP_SRC.indexOf('=', bodyStart) + 1, LP_SRC.indexOf(';', bodyStart));
  const decoupledOk = rhs.indexOf('metricW') < 0;
  console.log('  D2 公式(从 LayoutPlan.ets 源码读出): detailFont = round(clamp(' + K_FONT + ' * fs, ' +
    FONT_LO + ', ' + FONT_HI + ')) · detailMetricW = round(clamp(' + K_EM + ' * detailFont, ' +
    MW_LO + ', ' + MW_HI + '))');
  if (!decoupledOk) {
    dBad++;
    console.log('       ★detailMetricW 又引用回 metricW 了(口径要求与关键得分行字号解耦)★');
  } else {
    console.log('       与关键得分行解耦: detailMetricW 的表达式里不含 metricW -> OK');
  }
  console.log('  D2 列宽来源: ' + geoms.length + ' 个窗口 (宽 280~2400 x 高 280~1600 step20)');
  let minDetail = Infinity, minDetailAt = '', maxDetail = 0;
  let formulaBad = 0, rangeBad = 0, ratioBad = 0, ceilingHit = 0;
  for (const c of geoms) {
    const d = c.g.deco;
    const detailFont = Math.round(clampNum(K_FONT * c.g.fs, FONT_LO, FONT_HI));
    const want = Math.round(clampNum(K_EM * detailFont, MW_LO, MW_HI));
    if (d.detailMetricW !== want) { formulaBad++; }
    if (!(d.detailMetricW >= MW_LO && d.detailMetricW <= MW_HI)) { rangeBad++; }
    if (K_EM * detailFont > MW_HI) { ceilingHit++; }
    //  产品最后做了 Math.round, 所以允许半个单位的舍入(实测最多差 0.4vp, 例如 fs=0.96 -> detailFont=11
    //  -> 10.4*11 = 114.4 -> 列宽 114)。不允许出现"比 round 之后还小"的值。
    if (d.detailMetricW + 0.5 < Math.min(K_EM * detailFont, MW_HI)) { ratioBad++; }
    if (d.detailMetricW < minDetail) { minDetail = d.detailMetricW; minDetailAt = c.w + 'x' + c.h; }
    if (d.detailMetricW > maxDetail) { maxDetail = d.detailMetricW; }
  }
  // 字号一致性: 列宽是按 detailFont 定的, 而明细行那一段文本用的是界面上的 fz(11) = scaledFont(11, fs)。
  //   两者必须逐窗口相同, 否则"按 detailFont 算的列宽"与"实际渲染的字号"就是两码事。
  let fontBad = 0;
  for (const c of geoms) {
    if (TH.scaledFont(11, c.g.fs) !== Math.round(clampNum(K_FONT * c.g.fs, FONT_LO, FONT_HI))) { fontBad++; }
  }
  if (formulaBad > 0 || rangeBad > 0 || ratioBad > 0 || fontBad > 0) {
    dBad += formulaBad + rangeBad + ratioBad + fontBad;
  }
  console.log('       detailFont == 界面实际字号 Theme.scaledFont(11, fs): ' +
    (fontBad === 0 ? '全部一致(' + geoms.length + ' 个窗口)' : ('★' + fontBad + ' 个窗口不一致')));
  console.log('       detailMetricW 取值域 = ' + minDetail + ' ~ ' + maxDetail + 'vp (最小在 ' + minDetailAt + ')');
  console.log('       == round(clamp(' + K_EM + ' * detailFont, ' + MW_LO + ', ' + MW_HI +
    '))(用 fs 逐窗口复算): ' + (formulaBad === 0 ? '全部一致' : ('★' + formulaBad + ' 个窗口不一致')));
  console.log('       ' + MW_LO + ' <= detailMetricW <= ' + MW_HI + ': ' +
    (rangeBad === 0 ? '全部成立' : ('★' + rangeBad + ' 个窗口越界')));
  console.log('       detailMetricW >= min(' + K_EM + ' * detailFont, ' + MW_HI + ') - 0.5(允许 Math.round 舍入): ' +
    (ratioBad === 0 ? '全部成立' : ('★' + ratioBad + ' 个窗口偏小')) +
    ' (上限 ' + MW_HI + ' 被触发的窗口 = ' + ceilingHit + ')');
  let roundDown = 0, worstRound = 0;
  for (const c of geoms) {
    const detailFont = Math.round(clampNum(K_FONT * c.g.fs, FONT_LO, FONT_HI));
    const raw = Math.min(K_EM * detailFont, MW_HI);
    const diff = raw - c.g.deco.detailMetricW;
    if (diff > 0) { roundDown++; worstRound = Math.max(worstRound, diff); }
  }
  console.log('       其中被 Math.round 向下取整的窗口 = ' + roundDown + ' 个, 最多比 ' + K_EM +
    '*detailFont 少 ' + worstRound.toFixed(1) + 'vp(仍然是"列宽 >= 真实串"的下限之上, 见 D3)');
}

// ---- D3. 逐窗口: 该列最长的真实串(按该窗口**实际字号**)必须放得下 ----
console.log('\n  逐窗口核对(字号 = Theme.scaledFont(11, fontScaleFor(w,h)) —— 产品同一个函数):');
console.log('    ' + '显示点'.padEnd(30) + '列宽来源'.padEnd(22) + '最长真实串'.padEnd(24) +
  '需求/列宽(最紧处)'.padEnd(24) + '结论');
const siteWorst = new Map();
for (const site of SITES) {
  const worst = worstEmOf(site);
  let minMargin = Infinity, minInfo = null, viol = 0, minCol = Infinity, maxCol = 0;
  for (const c of geoms) {
    const have = colWidthOf(site, c.g);
    const fp = TH.scaledFont(11, c.g.fs);
    const need = worst.em * fp;
    if (have < minCol) { minCol = have; }
    if (have > maxCol) { maxCol = have; }
    if (have - need < minMargin) {
      minMargin = have - need;
      minInfo = { w: c.w, h: c.h, have: have, need: need, fp: fp };
    }
    if (need > have) { viol++; }
  }
  if (viol > 0) { dBad += viol; }
  siteWorst.set(site.id, { minCol: minCol, maxCol: maxCol, worst: worst });
  const src = site.kind === 'fixed' ? 'detailMetricW' : '整行(卡片内宽-标签)';
  console.log('    ' + (site.id + ' ' + site.label).padEnd(30) + src.padEnd(22) +
    (worst.s + ' (' + worst.em.toFixed(2) + 'em)').padEnd(24) +
    (minInfo.need.toFixed(1) + ' / ' + minInfo.have.toFixed(1) + ' @' + minInfo.fp + 'fp').padEnd(24) +
    (viol === 0 ? ('OK 余量 ' + minMargin.toFixed(1) + 'vp @' + minInfo.w + 'x' + minInfo.h)
      : ('★' + viol + ' 个窗口放不下(最紧 ' + minMargin.toFixed(1) + 'vp @' + minInfo.w + 'x' + minInfo.h + ')')));
  console.log('        ' + '列宽范围 ' + minCol.toFixed(1) + '~' + maxCol.toFixed(1) + 'vp' +
    ' · 字号范围 ' + Math.min.apply(null, geoms.map((c) => TH.scaledFont(11, c.g.fs))) + '~' +
    Math.max.apply(null, geoms.map((c) => TH.scaledFont(11, c.g.fs))) + 'fp');
}

// 三档字号 x 列宽 的对照(用户点名要确认的那三组: 98.2@10fp / 107.9@11fp / 117.8@12fp vs 104/114/125)
console.log('\n  D3b 三档字号对照(Game Physics 那一行, 最长串 62.06 Mbody-steps/s = ' +
  emOf('62.06 Mbody-steps/s').toFixed(2) + 'em):');
for (const fs3 of [0.92, 1.00, 1.08]) {
  const fp3 = TH.scaledFont(11, fs3);
  const col3 = Math.round(clampNum(SRC_K_EM * Math.round(clampNum(SRC_K_FONT * fs3, SRC_FONT_LO, SRC_FONT_HI)),
    SRC_MW_LO, SRC_MW_HI));
  const need3 = emOf('62.06 Mbody-steps/s') * fp3;
  console.log('      fs=' + fs3.toFixed(2) + ' -> 字号 ' + fp3 + 'fp · 列宽 ' + col3 + 'vp · 需求 ' +
    need3.toFixed(1) + 'vp · 余量 ' + (col3 - need3).toFixed(1) + 'vp' +
    (col3 >= need3 ? '  OK' : '  ★放不下★'));
}

// ---- D4. 理论长度上界(只打印, 不计失败) ----
//   这一节故意用**现实中不存在**的 9 字符 metric('0.0004831', |v|<1e-4 那一档才可能)去压列宽,
//   目的是把"这一列的长度上界"写清楚: 列宽按 10.4em 定的(D 版), 而 9 字符 metric + 13 字符单位 = 145.7vp,
//   永远放不进 130vp 的上限 —— 但 GB7 16 项的真实 metric 都在 0.2~300 之间(D3 已逐窗口证明放得下)。
//   比法: 旧串(toFixed)对**旧列宽**、新串(有效数字)对**新列宽**, 各比各的。
console.log('\n  D4 理论长度上界(只打印不计失败; 各比各的列宽):');
let dPre = 0, dNew = 0;
for (const site of SITES) {
  const sw = siteWorst.get(site.id);
  const fp = Math.max.apply(null, geoms.map((c) => TH.scaledFont(11, c.g.fs)));
  let oldS = null, newS = null, ow = -1, nw = -1;
  for (const u of site.units) {
    for (const [v, tag] of VALUES) {
      const o = v.toFixed(site.oldDigits) + ' ' + u;
      const n = sig(v) + ' ' + u;
      if (emOf(o) > ow) { ow = emOf(o); oldS = o; }
      if (emOf(n) > nw) { nw = emOf(n); newS = n; }
    }
  }
  const oldCol = OLD_COL[site.id] || sw.minCol;
  const oldOver = ow * fp > oldCol;
  const newOver = nw * fp > sw.minCol;
  if (newOver && oldOver) { dPre++; }
  if (newOver && !oldOver) { dNew++; }
  console.log('    ' + site.id.padEnd(16) + '新列宽 ' + String(sw.minCol.toFixed(1)).padEnd(7) + 'vp(旧列宽 ' +
    String(oldCol).padEnd(5) + ', ' + fp + 'fp 下) ' +
    ('旧 ' + oldS + ' = ' + (ow * fp).toFixed(1)).padEnd(32) +
    ('新 ' + newS + ' = ' + (nw * fp).toFixed(1)).padEnd(32) +
    (newOver ? (oldOver ? '新旧都超(理论串)' : '★只有新的超★') : '放得下'));
}
// 真实量级单独核一遍(上面那行是理论最坏, 这里回答"真机上会不会截断")
console.log('\n  真实量级(0.1 ~ 300, 用户点名的四项 + 其余 12 项的量级)逐点核:');
//   第 4 个字段 true = "理论压力组合(该负载现实中不会出现这个量级)", 只打印不计失败:
//   它的用途是**主动找出**本次改动在理论上唯一可能引入截断的组合, 而不是等别人来问。
const REAL = [
  [0.4831, 'Mpx/s', 'HDR'], [0.2, 'Mpx/s', 'Ray Tracer'], [0.1999, 'MB/s', 'Audio Encoder'],
  [1.3003, 'Mpx/s', 'Video Encoder'], [7.0277, 'MB/s', 'File Compression'],
  [16.949, 'pages/s', 'Text Processing'], [19.5, 'MB/s', 'Asset Compression'],
  [291.86, 'images/s', 'Photo Library'], [13.42, 'Mpx/s', 'Photo Editor'],
  [62.06, 'Mbody-steps/s', 'Game Physics'], [80.0, 'pages/s', 'HTML5 Browser'],
  [181.69, 'routes/s', 'Navigation'], [184.82, 'Klines/s', 'Clang'],
  [13.4, 'Mpx/s', 'PDF Viewer'], [0.2, 'Mpts/s', 'Structure from Motion'],
  [444.85, 'Mparticle/s', 'GPU 粒子(最长单位)', true],
  [44.86, 'Mparticle/s', 'GPU 粒子(最长单位)', true],
  [4.486, 'Mparticle/s', 'GPU 粒子 x 个位数 metric', true],
  [0.4486, 'Mparticle/s', 'GPU 粒子 x 小数 metric', true]
];
//   列宽同样取自产品: 固定列宽那一列 = 该窗口的 detailMetricW(逐窗口算), 整行那一列 = 卡内宽。
//   第 4 个字段 true = "理论压力组合(该负载现实中不会出现这个量级)", 只打印不计失败。
let realBad = 0;
for (const [v, u, name, probe] of REAL) {
  const s = sig(v) + ' ' + u;
  const cells = [];
  for (const site of SITES) {
    if (site.units.indexOf(u) < 0) { continue; }   // 这个单位不会出现在这一列
    let worst = null;
    for (const c of geoms) {
      const have = colWidthOf(site, c.g);
      const fp = TH.scaledFont(11, c.g.fs);
      const need = emOf(s) * fp;
      if (worst === null || (have - need) < worst.margin) {
        worst = { margin: have - need, have: have, need: need, w: c.w, h: c.h };
      }
    }
    const oldNeed = emOf(v.toFixed(site.oldDigits) + ' ' + u) * TH.scaledFont(11, LP.fontScaleFor(worst.w, worst.h));
    const flag = worst.margin >= 0 ? 'OK' : (oldNeed > worst.have ? '既有截断' : '★新增截断★');
    if (flag === '★新增截断★' && !probe) { realBad++; }
    cells.push(site.id.split(':')[1] + ':' + worst.need.toFixed(0) + '/' + worst.have.toFixed(0) + ' ' + flag);
  }
  console.log('    ' + name.padEnd(22) + JSON.stringify(s).padEnd(26) + cells.join('  ') +
    (probe ? '   <- 理论压力组合(只打印, 不计失败)' : ''));
}
bad += dBad;
console.log('\n  理论长度上界: 新旧都超 ' + dPre + ' 处(都是那个 9 字符 metric 的理论串, 现实不存在); ' +
  '只有新版超 ' + dNew + ' 处; 真实量级的越界见 D3(0 处)。');
console.log('    注 1: 两列 GPU(5106 / 9435)的 metric 是 native 侧先量化过的 —— ' +
  'gpu7/finish() 用 fixed(st.metric, 1~2)(gpu7_renderer.cpp:132/147), ' +
  'gpu/gpu_renderer.cpp 用 num(st.metricValue, 2)(:801):');
console.log('          也就是说 ArkTS 拿到的本来就是 1~2 位小数, 按 4 位有效数字重排只会更短或等长, ' +
  '这两列**不可能**因为本次改动变长。');
console.log('    注 2: 最后两条"GPU 粒子 x 小数 metric"是**理论压力组合**: ' +
  '只有当 native 也把这两处改成 %.4g 之后才需要重新核一遍。');
console.log('    注 3: 列宽本身由 LayoutPlan.detailMetricW 给(明细行 120vp 起), 不再写死 88/100/110 —— ' +
  '旧写法下 Game Physics 那一行(62.1 Mbody-steps/s ≈ 115vp)是被截断的。');
console.log(realBad === 0
  ? '  真实量级下 6 个显示点一个都不越界'
  : ('  真实量级下有 ' + realBad + ' 处新增越界'));

// ---------------------------------------------------------------------------
//  E. ★顺带回答: 同一条明细行里"项名列"(layoutWeight(1))还剩多少 vp★
//
//  项名列拿到多少 = 行内宽 - (其它固定列之和, **含新改的 metric 列**):
//      行内宽 = 卡片可用宽 - 2 x 卡片内边距 - 2 x 行内边距
//      卡片可用宽: 首页 GB7 卡 = 结果卡区 resultZoneVp 按 plan.columns 均分;
//                  结果页明细行 = 整页宽 min(w, contentMaxFor(w))
//  判据用**实数**: 项名要完整显示, 该列至少要有"该行项名字号下的最长项名"那么宽。
//      GB7 明细行最长项名 = 'Structure from Motion'(21 字符);
//      自研套件明细行项名 = 4 个汉字(如 '图像处理')。
//  ★2026-10 起这一节是硬断言★: 项名列 <= 0 表示"固定列之和已经超过行内宽" —— 那是**内容溢出卡片**,
//  不是"显示得挤一点", 属于硬缺陷, 任何窗口出现都判失败(bad += eZeroD)。
//  "项名列小于最长项名"(只是显示省略号)仍然只计数不判失败。
// ---------------------------------------------------------------------------
function cardAvailW(site, g) {
  if (site.zoneFrom === 'zone') {
    const cols = Math.max(1, g.plan.columns);
    return cols >= 2 ? (g.zone - (cols - 1) * g.plan.gutter) / cols : g.zone;
  }
  return g.pageW;
}
function nameColW(site, g) {
  const pad = site.cardPadFrom === 'uiRes' ? g.deco.pad : (site.cardPadFrom === 'plan' ? g.planPad : 0);
  return cardAvailW(site, g) - 2 * pad - 2 * site.rowPadH - (g.deco.detailMetricW + site.sibVp);
}
const NAME_FIXED = SITES.filter((s) => s.kind === 'fixed');
const LONGEST = { gb7: 'Structure from Motion', own: '图像处理' };
function longestNameOf(site) {
  return site.units === U_OWN ? LONGEST.own : LONGEST.gb7;
}
// 三版口径的对比(只为把"改前 / 上一版 / 现在"三组数摆在一起; 产品代码里只有最后一版):
//   B = 改动前的写死列宽 88/100/110(GB7 单核 / 多核 / GPU / 自研多核)
//   C = 上一版 clamp(max(metricW,120),84,260) —— 已证伪, 用返回的 keyNum/keyUnit 复算
//   D = 现在这一版 round(clamp(10.4 * detailFont, 84, 130))
const VARIANTS = [
  { k: 'B', label: '改前 写死 88/100/110' },
  { k: 'C', label: '上一版 max(metricW,120) 夹 84~260' },
  { k: 'D', label: '现在 10.4 x detailFont 夹 84~130' }
];
function metricColWFor(site, g, variant) {
  if (variant === 'B') { return OLD_COL[site.id]; }
  if (variant === 'C') {
    const metricW = Math.round(clampNum(g.deco.keyNum * 3.2 + g.deco.keyUnit * 7.0, 84, 260));
    return Math.round(clampNum(Math.max(metricW, 120), 84, 260));
  }
  return g.deco.detailMetricW;
}
function rowInnerW(site, g) {
  const pad = site.cardPadFrom === 'uiRes' ? g.deco.pad : (site.cardPadFrom === 'plan' ? g.planPad : 0);
  return cardAvailW(site, g) - 2 * pad - 2 * site.rowPadH;
}
function nameWFor(site, g, variant) {
  return rowInnerW(site, g) - (metricColWFor(site, g, variant) + site.sibVp);
}
function nameNeedOf(site, g) {
  return emOf(longestNameOf(site)) * TH.scaledFont(site.nameBase, g.fs);
}
console.log('\n=== E. 明细行"项名列"剩余宽度(★硬断言: 任何窗口 <= 0 = 内容溢出卡片 = 失败★) ===');
console.log('    卡片可用宽 = 首页结果卡区/结果页整页宽均分; 行内宽 = 卡宽 - 2x卡片内边距 - 2x行内边距;');
console.log('    项名列 = 行内宽 - (metric 列 + 同行其它固定列)。三组口径: B 改前写死 / C 上一版 / D 现在。');
//  1400x900 / 2000x1400 是"三列 + 大字号"那一档, 1420x1100 是上一版被压成负数的那个窗口(必须列出来)
const E_WINS = [[280, 800], [280, 280], [320, 780], [360, 780], [600, 900], [665, 940], [1060, 665],
  [1400, 900], [1420, 1100], [2000, 1400]];
console.log('    ' + '窗口'.padEnd(11) + '显示点'.padEnd(22) + '行内宽'.padEnd(8) + '其它列'.padEnd(8) +
  'B(88/100/110)'.padEnd(15) + 'C(上一版)'.padEnd(12) + 'D(现在)'.padEnd(12) + '最长项名需'.padEnd(11) + 'D 的结论');
for (const [w, h] of E_WINS) {
  const g = geomOf(w, h);
  for (const site of NAME_FIXED) {
    const inner = rowInnerW(site, g);
    const b = nameWFor(site, g, 'B'), c2 = nameWFor(site, g, 'C'), dd = nameWFor(site, g, 'D');
    const need = nameNeedOf(site, g);
    console.log('    ' + (w + 'x' + h).padEnd(11) + site.id.padEnd(22) + inner.toFixed(0).padEnd(8) +
      String(site.sibVp).padEnd(8) + b.toFixed(1).padEnd(15) + c2.toFixed(1).padEnd(12) + dd.toFixed(1).padEnd(12) +
      need.toFixed(1).padEnd(11) +
      (dd <= 0 ? '★<=0 溢出★' : (dd >= need ? '够' : '不足(省略号)')));
  }
}
const eStat = new Map();
for (const site of NAME_FIXED) {
  eStat.set(site.id, { perVar: {} });
  for (const v of VARIANTS) {
    eStat.get(site.id).perVar[v.k] = { min: Infinity, minAt: '', zero: 0, short: 0 };
  }
}
const zeroWins = [];
let eZeroD = 0, eShortD = 0, eZeroC = 0, eZeroB = 0;
// ★硬断言的适用范围(2026-10)★: 只在产品**自己承认支持的窗口宽度**上判失败。
// 阈值直接从 LayoutPlan.ets 源码读 W_TINY(极窄档阈值), 不在这里另写一个数 ——
// 产品把它调了, 这里跟着动。
// 为什么必须这么切: 本文件的扫描下界是 280vp, 比 W_TINY 还低 40vp; 在 280~319vp 上断言
// 「项名列必须 > 0」等于断言一个产品从未承诺过的窗口。那一段照样逐条计数并打印
// (eZeroDSub), 只是不判失败 —— 既不掩盖, 也不越界判。
const W_TINY_N = Number((LP_SRC.match(/const W_TINY: number = (\d+)/) || [])[1] || 320);
let eZeroDSub = 0;
for (const c of geoms) {
  for (const site of NAME_FIXED) {
    const st = eStat.get(site.id);
    const need = nameNeedOf(site, c.g);
    for (const v of VARIANTS) {
      const nw = nameWFor(site, c.g, v.k);
      const pv = st.perVar[v.k];
      if (nw < pv.min) { pv.min = nw; pv.minAt = c.w + 'x' + c.h; }
      if (nw <= 0) { pv.zero++; }
      if (nw < need) { pv.short++; }
    }
    const nwD = nameWFor(site, c.g, 'D');
    if (nwD <= 0 && c.w < W_TINY_N) { eZeroDSub++; }
    if (nwD <= 0 && c.w >= W_TINY_N) {
      eZeroD++;
      if (zeroWins.length < 24) {
        zeroWins.push(site.id + ' @' + c.w + 'x' + c.h + ' 行内宽 ' + rowInnerW(site, c.g).toFixed(1) +
          ', 固定列 ' + (c.g.deco.detailMetricW + site.sibVp) + ' -> 项名列 ' + nwD.toFixed(1) + 'vp');
      }
    }
    if (nwD < need) { eShortD++; }
    if (nameWFor(site, c.g, 'C') <= 0) { eZeroC++; }
    if (nameWFor(site, c.g, 'B') <= 0) { eZeroB++; }
  }
}
console.log('    全窗口扫描(' + geoms.length + ' 个窗口)逐行三版对比:');
for (const site of NAME_FIXED) {
  const st = eStat.get(site.id);
  const parts = VARIANTS.map((v) => {
    const pv = st.perVar[v.k];
    return v.k + ': 最小 ' + pv.min.toFixed(1) + 'vp@' + pv.minAt + ' · <=0 ' + pv.zero + '/' + geoms.length;
  });
  console.log('      ' + site.id.padEnd(22) + parts.join('   |   '));
}
// 集合比较: 相对**改前(B)**这一版, 哪些组是新坏的、哪些是修好的 —— 这比"计数相等"更能说明问题
let newBad = 0, fixedBad = 0;
const newBadList = [];
for (const c of geoms) {
  for (const site of NAME_FIXED) {
    const b = nameWFor(site, c.g, 'B'), dd = nameWFor(site, c.g, 'D');
    if (b > 0 && dd <= 0) {
      newBad++;
      if (newBadList.length < 6) {
        newBadList.push(site.id + ' @' + c.w + 'x' + c.h + ' 改前 ' + b.toFixed(1) + 'vp -> 现在 ' + dd.toFixed(1) + 'vp');
      }
    }
    if (b <= 0 && dd > 0) { fixedBad++; }
  }
}
console.log('    相对改前(B)的集合比较: 新坏掉的 = ' + newBad + ' 组 · 修好的 = ' + fixedBad + ' 组');
if (newBad > 0) {
  for (const s of newBadList) { console.log('      · 新坏: ' + s); }
  const nw = [];
  for (const c of geoms) {
    for (const site of NAME_FIXED) {
      if (nameWFor(site, c.g, 'B') > 0 && nameWFor(site, c.g, 'D') <= 0) { nw.push(c.w); break; }
    }
  }
  nw.sort((a, b) => a - b);
  console.log('        新坏掉的窗口宽度区间: ' + nw[0] + ' ~ ' + nw[nw.length - 1] + 'vp');
}
console.log('    合计(窗口 x 明细行 = ' + (geoms.length * NAME_FIXED.length) + ' 组):');
console.log('      B 改前写死  : 项名列 <= 0 的 ' + eZeroB + ' 组');
console.log('      C 上一版    : 项名列 <= 0 的 ' + eZeroC + ' 组');
console.log('      D 现在      : 项名列 <= 0 的 ' + eZeroD + ' 组' +
  (eZeroD > 0 ? '   ★这一版仍然把项名列压到 <= 0(硬缺陷, 下面给窗口)★' : '   (0 -> 硬缺陷已消掉)'));
console.log('      D 项名列 < 最长项名的组(只是会显示省略号, 不计失败): ' + eShortD + '/' +
  (geoms.length * NAME_FIXED.length));
if (eZeroD > 0) {
  console.log('    D 版仍然 <= 0 的窗口(最多列 24 条; 完整集合见"按行统计"的数字):');
  for (const z of zeroWins) { console.log('      · ' + z); }
  const ws = [];
  for (const c of geoms) {
    for (const site of NAME_FIXED) {
      if (nameWFor(site, c.g, 'D') <= 0) { ws.push(c.w); break; }
    }
  }
  ws.sort((a, b) => a - b);
  console.log('      出现问题的窗口宽度区间: ' + ws[0] + ' ~ ' + ws[ws.length - 1] +
    'vp (共 ' + ws.length + ' 个宽度档; 高度覆盖 280~1600 step20)');
  // 精扫(步长 1vp): 给出"从多宽开始四行全部 > 0"的阈值 —— 这是决定"抬最小卡宽 / 去 ms 列"时的关键数
  let lastBadW = -1, firstOkW = -1;
  const badRowsAt = {};
  for (let w = 260; w <= 380; w++) {
    let rowBad = false;
    for (const h of [280, 600, 900, 1200, 1600]) {
      const g = geomOf(w, h);
      for (const site of NAME_FIXED) {
        if (nameWFor(site, g, 'D') <= 0) {
          rowBad = true;
          badRowsAt[site.id] = w;
        }
      }
    }
    if (rowBad) { lastBadW = w; } else if (firstOkW < 0) { firstOkW = w; }
  }
  console.log('      精扫(宽 260~380 step 1vp, 高 280/600/900/1200/1600): 最后一个仍然 <=0 的宽度 = ' +
    lastBadW + 'vp; 从 ' + firstOkW + 'vp 起全部 > 0');
  const perRow = NAME_FIXED.filter((s) => eStat.get(s.id).perVar.D.zero > 0)
    .map((s) => s.id + '(最后出问题的宽度 ' + badRowsAt[s.id] + 'vp)');
  console.log('      仍然 <=0 的行: ' + (perRow.length > 0 ? perRow.join(' / ') : '无'));
}
bad += eZeroD;   // ★硬断言: 项名列 <= 0 = 内容溢出卡片 = 失败★

// ---------------------------------------------------------------------------
// Z. 汇总
// ---------------------------------------------------------------------------
console.log('\n=== Z. 汇总 ===');
if (bad === 0) {
  console.log('  全部通过: 口径表 ' + table.length + ' 条 · 性质 ' + nCount + ' 个样本 · 分数反算 ' +
    items.length + ' 项 · 明细行列宽(取自 LayoutPlan.detailMetricW)' + SITES.length + ' 个显示点 x ' +
    geoms.length + ' 个窗口 x ' + VALUES.length + ' 个理论量级');
  console.log('  E 节(硬断言, 只算窗口宽 >= ' + W_TINY_N + 'vp 的产品支持区间): 明细行项名列 <= 0 = ' + eZeroD + ' 个 · 支持区间外(< ' + W_TINY_N + 'vp, 只计数不判失败) = ' + eZeroDSub + ' 个 · ' +
    '仅"不足(省略号)"的 = ' + eShortD + ' 个');
  process.exit(0);
}
console.log('  有 ' + bad + ' 处不符合预期');
console.log('    分节: D 节(明细行 metric 列) ' + dBad + ' 处 · 真实量级表 ' + realBad +
  ' 处 · E 节(明细行项名列 <= 0 = 溢出卡片) ' + eZeroD + ' 处');
process.exit(1);
