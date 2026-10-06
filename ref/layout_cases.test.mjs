// 形态判定离线用例: 把 entry/src/main/ets/common/LayoutPlan.ets(零 import 的纯函数)
// 直接交给 Node 做类型擦除后加载, 逐条核对判定表与布局参数。
//   跑法:  node aurora-bench/ref/layout_cases.test.mjs
import { mkdtempSync, writeFileSync, readFileSync, readdirSync } from 'node:fs';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const src = join(here, '..', 'entry', 'src', 'main', 'ets', 'common', 'LayoutPlan.ets');
const dir = mkdtempSync(join(tmpdir(), 'ab-layout-'));
const modFile = join(dir, 'LayoutPlan.mjs');

// 用 SDK 自带的 TypeScript 做类型擦除, 再交给 Node 加载。
// 为什么不直接把 .ets 复制成 .mts 让 Node 自己擦: Node 的类型擦除
// (--experimental-strip-types) 要 22.6+, 本工程的 Node 是 20.18.1, 会直接报
// ERR_UNKNOWN_FILE_EXTENSION —— 那样这个用例在本机根本跑不起来。
function loadTypeScript() {
  const cands = [
    'D:/ohos-tools/sdk/default/openharmony/ets/build-tools/ets-loader/node_modules/typescript',
    'D:/ohos-tools/sdk/18/ets/build-tools/ets-loader/node_modules/typescript'
  ];
  for (const c of cands) {
    try {
      return createRequire(import.meta.url)(c);
    } catch (e) { /* 换下一个 */ }
  }
  try {
    return createRequire(import.meta.url)('typescript');
  } catch (e) {
    console.error('找不到 typescript 编译器 —— 无法对 LayoutPlan.ets 做类型擦除。');
    console.error('请安装 typescript, 或把 SDK 里 ets-loader/node_modules/typescript 的路径加进上面的 cands。');
    process.exit(2);
  }
}

const ts = loadTypeScript();
const out = ts.transpileModule(readFileSync(src, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2021, module: ts.ModuleKind.ESNext },
  fileName: 'LayoutPlan.ts',
  reportDiagnostics: true
});
const errs = (out.diagnostics || []).filter((d) => d.category === ts.DiagnosticCategory.Error);
if (errs.length > 0) {
  console.error('LayoutPlan.ets 有 ' + errs.length + ' 处语法错误, 无法加载:');
  for (const d of errs) {
    console.error('  ' + ts.flattenDiagnosticMessageText(d.messageText, ' '));
  }
  process.exit(2);
}
writeFileSync(modFile, out.outputText, 'utf8');
const M = await import(pathToFileURL(modFile).href);

function inp(w, h, fold, apiOk, dtype, table) {
  return { widthVp: w, heightVp: h, foldState: fold, foldApiOk: apiOk,
    deviceType: dtype, tableFormFactor: table };
}
const NONE = 'none', EXP = 'expanded', FOLDED = 'folded', UNK = 'unknown';

// ---- A. 判定表 ----
//  ★ 2026-10 重写: 编号现在是**纯几何**的(只看宽/高/宽高比), 与机型、折叠状态、内置表都无关。
//    每一档的含义见 common/LayoutPlan.ets 的 shapeIdOf; 名字只是给人看的标签。
//    「折叠状态」列仍然照实打印, 但它**不参与**判定 —— 这一条由本文件 K/L 两节断言。
const cases = [
  ['1  极窄竖长 360x780',     inp(360, 780, NONE, true, 'phone', 'bar'),        'S3'],
  ['2  窄窗·阔直 430x700',    inp(430, 700, NONE, true, 'phone', 'bar'),        'S1'],
  ['3  极窄竖长 340x800',     inp(340, 800, FOLDED, true, 'phone', 'foldable'), 'S3'],
  ['4  近方形 700x760',       inp(700, 760, EXP, true, 'phone', 'foldable'),    'S4'],
  ['5  竖长宽窗 700x900',     inp(700, 900, EXP, true, 'phone', 'foldable'),    'S5'],
  // ---- 展开态横向窗口(比例 1.5~1.6 这一档): 编号由比例定, 与"是不是折叠屏"无关 ----
  ['5a Pura X 2120x1320@3.0',  inp(706, 440, EXP, true, 'phone', 'foldable'),   'S7'],
  ['5b 举例 2400x1500@3.0',    inp(800, 500, EXP, true, 'phone', 'foldable'),   'S7'],
  ['5c Pura X 2120x1320@2.0',  inp(1060, 660, EXP, true, 'phone', 'foldable'),  'S7'],
  ['5d 举例 2400x1500@2.0',    inp(1200, 750, EXP, true, 'phone', 'foldable'),  'S11'],
  // ---- 1:sqrt(2) 竖长(目标机型真机比例) ----
  //   ★ 真机设备行实测: 1828x2584px ÷ 密度 2.750 = 664x940vp; 1828/2584 = 0.7074 ≈ 1/sqrt(2)
  ['5f 真机 1828x2584px@2.750', inp(665, 940, EXP, true, 'phone', 'foldable'), 'S5'],
  ['5g 真机同尺寸的另一口径',      inp(664, 940, EXP, true, 'phone', 'foldable'), 'S5'],
  ['5h 同比例横放 940x665',    inp(940, 665, EXP, true, 'phone', 'foldable'),  'S7'],
  ['5i 同比例放大 566x800',     inp(566, 800, EXP, true, 'phone', 'foldable'), 'S1'],
  ['5e 大折叠 2496x2224@3.0', inp(832, 741, EXP, true, 'phone', 'foldable'),   'S6'],
  ['6  近方形宽窗 760x820',    inp(760, 820, EXP, true, 'phone', 'foldable'),   'S6'],
  ['7  横向 1000x700',        inp(1000, 700, NONE, true, 'tablet', 'tablet'),  'S7'],
  ['8  横向宽窗 1400x900',     inp(1400, 900, NONE, true, '2in1', ''),          'S11'],
  ['9a 极窄 300x800',         inp(300, 800, NONE, true, 'phone', 'bar'),       'S9'],
  ['9b 极扁 900x300',         inp(900, 300, NONE, true, 'phone', 'bar'),       'S10'],
  ['B1 正方形 800x800',       inp(800, 800, EXP, true, 'phone', 'foldable'),   'S6'],
  ['B2 超宽 2000x600(2in1)',  inp(2000, 600, NONE, true, '2in1', ''),          'S10'],
  ['B3 超宽 2000x600(未知)',   inp(2000, 600, NONE, true, 'default', ''),       'S10'],
  ['B4 竖长 800x1280',        inp(800, 1280, NONE, true, 'tablet', 'tablet'),  'S5'],
  ['B5 扁宽 780x360',         inp(780, 360, NONE, true, 'phone', 'bar'),       'S8'],
  ['B6 折叠接口不可用 340x800',  inp(340, 800, UNK, false, 'phone', 'foldable'), 'S3'],
  ['B7 折叠接口不可用 700x900',  inp(700, 900, UNK, false, 'phone', 'foldable'), 'S5'],
  ['B8 窗口未取到',            inp(0, 0, UNK, false, 'phone', ''),               'S0'],
  ['B9 临界 320x780',         inp(320, 780, NONE, true, 'phone', 'bar'),       'S3'],
  ['B10 临界 319x780',        inp(319, 780, NONE, true, 'phone', 'bar'),       'S9'],
  ['B11 分屏 600x900',        inp(600, 900, NONE, true, 'phone', 'bar'),       'S5'],
  ['B12 分屏 599x900',        inp(599, 900, NONE, true, 'phone', 'bar'),       'S1'],
  ['B13 折叠接口挂+表非折叠',    inp(340, 800, UNK, false, 'phone', 'bar'),      'S3'],
  ['B14 很宽 1400x900',        inp(1400, 900, NONE, true, 'tablet', 'tablet'),  'S11'],
  // ---- 任意比例专项(用户点名要求覆盖的比例; 断言见 L 节) ----
  ['R1 1:sqrt(2) 竖长 665x940', inp(665, 940, NONE, true, 'phone', 'bar'),      'S5'],
  ['R2 16:10 横 1060x665',     inp(1060, 665, NONE, true, 'phone', 'bar'),     'S7'],
  ['R3 9:16 竖 360x640',       inp(360, 640, NONE, true, 'phone', 'bar'),      'S1'],
  ['R4 1:1 800x800',          inp(800, 800, NONE, true, 'phone', 'bar'),      'S6'],
  ['R5 21:9 2100x900',        inp(2100, 900, NONE, true, 'phone', 'bar'),     'S8'],
  ['R6 3:1 极扁 1500x500',     inp(1500, 500, NONE, true, 'phone', 'bar'),     'S10'],
  ['R7 1:3 极窄 400x1200',     inp(400, 1200, NONE, true, 'phone', 'bar'),     'S9'],
  ['R8 小方窗 400x400',        inp(400, 400, NONE, true, 'phone', 'bar'),      'S4'],
  ['R9 超大 2000x1400',        inp(2000, 1400, NONE, true, 'tablet', 'tablet'),'S11'],
  // 同一窗口、不同折叠状态与不同设备表: 编号必须**完全相同**(证明判定不看这些)
  ['R10 同窗口·系统报展开',      inp(1060, 665, EXP, true, 'phone', 'foldable'), 'S7'],
  ['R11 同窗口·内置表说折叠',     inp(1060, 665, NONE, false, 'phone', 'foldable'), 'S7'],
  ['R12 同窗口·deviceType电视', inp(1060, 665, NONE, true, 'tv', ''),          'S7']
];
console.log('=== A. 形态判定表(离线用例, resolvePlanPlain 纯函数) ===');
console.log('用例'.padEnd(26) + '窗口(vp)'.padEnd(13) + '比例  '.padEnd(8) +
  '折叠/设备'.padEnd(22) + '期望  实际  编号 形态名');
let bad = 0;
for (const [label, i, want] of cases) {
  const p = M.resolvePlanPlain(i);
  const ar = i.heightVp > 0 ? (i.widthVp / i.heightVp) : 0;
  const ok = p.id === want;
  if (!ok) bad++;
  const foldTxt = (i.foldApiOk ? i.foldState : 'apiX:' + i.foldState) + '/' + i.deviceType;
  console.log(label.padEnd(26) +
    (i.widthVp + 'x' + i.heightVp).padEnd(13) +
    (ar > 0 ? ar.toFixed(3) : '--').padEnd(8) +
    foldTxt.padEnd(22) +
    (want + (ok ? '  OK  ' : '  FAIL')).padEnd(10) +
    p.id.padEnd(6) + p.index.toString().padEnd(5) + p.name);
}
console.log(bad === 0 ? '\n判定表全部通过 (' + cases.length + ' 条)' : '\n有 ' + bad + ' 条不符合期望');

// ---------------------------------------------------------------------------
//  J. ★比例专项★ —— 用真机比例(1:sqrt(2))与它容易混淆的比例(16:10 等)把"比例 -> 编号"
//     钉死。**注意**: 这里断言的是"这个比例落到哪个编号区间", 不是"这个比例应该是某种形态" ——
//     判定与布局都不认比例的名字, 只认宽高比落在哪两条阈值之间(见 G 节)。
//
//  事实(逐条给出处; 判定本身**不用**这些数字, 只用实时窗口宽高比):
//    · Pura X Max (HOP-AL00/AL10, 目标机型): 内屏 2584 x 1828 px -> 2584/1828 = 1.4136 ≈ sqrt(2),
//      即宽高比 0.707 —— 竖长。出处 ref/consumer.huawei.com_cn_phones_pura-x-max_specs_
//      第 1933-1937 行("内屏：2584 × 1828 像素"), 抄录 ref/DEVICE_MODELS.md:7。
//    · 真机设备行实测: 窗口=1828x2584px ÷ 密度 2.750 = 664x940vp; 1828/2584 = 0.7074
//      (与 1/sqrt(2) = 0.7071 吻合), 见 service/BenchRunner.ets 的 windowTagOf(换算依据也落在设备行里)。
//    · Pura X (VDE-AL00/AL10): 内屏 2120 x 1320 px = 1.606 -> **这一台才是 16:10**。
//      出处 ref/consumer.huawei.com_cn_phones_pura-x_specs_, 抄录 ref/DEVICE_MODELS.md:129。
//    · UX 参考: 1:sqrt(2) 正是 A4 纸的比例, 也是"竖长内屏"最常见的来源。
// ---------------------------------------------------------------------------
const AR_WIDE_MIN = 1.25;   // 与 LayoutPlan 的 AR_WIDE 同值(这里只为断言方向, 不复算判定)
console.log('\n=== J. 1:sqrt(2) / 16:10 / 9:16 … 比例专项 ===');
console.log('窗口(vp)'.padEnd(26) + '宽高比'.padEnd(10) + '已知比例'.padEnd(10) +
  '期望  实际  形态名');
const ratioCases = [
  ['1828x2584px@2.750 (真机)', 664, 940, EXP, 'S5'],
  ['1828x2584px@2.750 取整', 665, 940, EXP, 'S5'],
  ['1:sqrt(2) 放大 566x800', 566, 800, EXP, 'S1'],
  ['1:sqrt(2) 缩小 424x600', 424, 600, EXP, 'S1'],
  ['2584x1828px@2.750 横放', 940, 665, EXP, 'S7'],
  ['Pura X 2120x1320@2.0', 1060, 660, EXP, 'S7'],
  ['外屏 1848x1264@2.750', 672, 460, FOLDED, 'S7'],
  ['9:16 360x640', 360, 640, NONE, 'S1'],
  ['1:1 800x800', 800, 800, NONE, 'S6'],
  ['21:9 2100x900', 2100, 900, NONE, 'S8'],
  ['3:1 1500x500', 1500, 500, NONE, 'S10'],
  ['1:3 400x1200', 400, 1200, NONE, 'S9']
];
let jBad = 0;
for (const [tag, w, h, fold, want] of ratioCases) {
  const i = inp(w, h, fold, true, 'phone', 'foldable');
  const p = M.resolvePlanPlain(i);
  const ar = w / h;
  const hint = M.aspectHint(w, h);
  // ① 编号必须与期望一致; ② **竖长(ar<1)的窗口绝不许落到横向档**(S7/S8/S10/S11/S12);
  //    ③ 横向(ar>1.25)的窗口绝不许落到竖长档(S1/S2/S3/S5); ④ 已知比例要能被标注出来
  const noLandscape = !(ar < 1.0 && ['S7', 'S8', 'S10', 'S11', 'S12'].indexOf(p.id) >= 0);
  const noPortrait = !(ar > AR_WIDE_MIN && ['S1', 'S2', 'S3', 'S5'].indexOf(p.id) >= 0);
  const hintWant = Math.abs(ar - 1 / Math.sqrt(2)) <= 0.008 ? '（≈1:√2）'
    : (Math.abs(ar - 1.6) <= 0.008 ? '（≈16:10）'
      : (Math.abs(ar - 0.5625) <= 0.008 ? '（≈9:16）'
        : (Math.abs(ar - 1.77777778) <= 0.008 ? '（≈16:9）' : '')));
  const hintOk = hintWant === '' || hint === hintWant;
  const ok = p.id === want && noLandscape && noPortrait && hintOk;
  if (!ok) jBad++;
  console.log((tag + ' (' + w + 'x' + h + ')').padEnd(26) +
    ar.toFixed(3).padEnd(10) + (hint.length > 0 ? hint : '-').padEnd(10) +
    (want + (ok ? '  OK  ' : '  FAIL')).padEnd(10) +
    p.id.padEnd(6) + p.name);
}
// 同一比例、不同折叠状态: 判定只认窗口比例(展开/折叠只影响文案, 见 G 节末的同窗口对比)
const portraitFold = M.resolvePlanPlain(inp(665, 940, EXP, true, 'phone', 'foldable'));
const landscapeFold = M.resolvePlanPlain(inp(940, 665, EXP, true, 'phone', 'foldable'));
const checks = [
  ['1:sqrt(2) -> S5', portraitFold.id === 'S5'],
  ['1:sqrt(2) 编号 = 5', portraitFold.index === 5],
  ['1:sqrt(2) 名字含"竖长宽窗"', portraitFold.name.indexOf('竖长宽窗') >= 0],
  ['1:sqrt(2) 名字印出实时宽高比 0.707', portraitFold.name.indexOf('0.707') >= 0],
  ['1:sqrt(2) 名字不含面板断言', portraitFold.name.indexOf('16:10') < 0 &&
    portraitFold.name.indexOf('阔折叠') < 0 && portraitFold.name.indexOf('内屏') < 0],
  ['1:sqrt(2) 依据写明宽高比 < 0.8', portraitFold.basis.indexOf('< 0.8') >= 0],
  ['1:sqrt(2) 依据标注 ≈1:√2', portraitFold.basis.indexOf('≈1:√2') >= 0],
  ['1:sqrt(2) 两栏(宽 665 >= 600)', portraitFold.split === true],
  ['1:sqrt(2) 结果列 1 列(右栏 373vp 放不下 2 张 300vp)', portraitFold.columns === 1],
  ['横放 940x665 -> S7', landscapeFold.id === 'S7'],
  ['横放名字印出 1.414', landscapeFold.name.indexOf('1.414') >= 0],
  ['横放名字不含面板断言', landscapeFold.name.indexOf('16:10') < 0 &&
    landscapeFold.name.indexOf('阔折叠') < 0],
  ['横放依据是纯比例条件', landscapeFold.basis.indexOf('宽高比') >= 0 &&
    landscapeFold.basis.indexOf('16:10') < 0],
  ['aspectHint 认 1:sqrt(2)', M.aspectHint(665, 940) === '（≈1:√2）'],
  ['aspectHint 认 16:10', M.aspectHint(1060, 660) === '（≈16:10）'],
  ['aspectHint 认 9:16', M.aspectHint(360, 640) === '（≈9:16）'],
  ['aspectHint 不硬认普通手机 0.462', M.aspectHint(360, 780) === ''],
  ['aspectHint 尺寸未取到返回空', M.aspectHint(0, 0) === '']
];
for (const [what, ok] of checks) {
  if (!ok) jBad++;
  console.log('  ' + (ok ? 'OK  ' : 'FAIL') + '  ' + what);
}
console.log(jBad === 0
  ? '  ' + ratioCases.length + ' 条比例 + ' + checks.length + ' 条断言全部符合预期(编号 / 命名 / 依据 / 比例标注)'
  : ('  有 ' + jBad + ' 条不符合预期'));

console.log('\n=== B. 每种形态的布局参数表(典型窗口) ===');
const head = ['形态', '典型窗口', '分栏', '栅格', '结果卡列', '内容max', '基准宽', '最小宽',
  '开始钮', '按钮高', '字号x', '间距', '内边距', '圆角', '宽高比'];
console.log(head[0].padEnd(6) + head[1].padEnd(12) + head[2].padEnd(6) + head[3].padEnd(7) +
  head[4].padEnd(10) + head[5].padEnd(9) + head[6].padEnd(8) + head[7].padEnd(8) +
  head[8].padEnd(8) + head[9].padEnd(8) + head[10].padEnd(7) + head[11].padEnd(6) +
  head[12].padEnd(8) + head[13].padEnd(6) + head[14]);
for (const [label, i] of cases.slice(0, 10)) {
  const p = M.resolvePlanPlain(i);
  const ar = i.heightVp > 0 ? (i.widthVp / i.heightVp) : 0;
  console.log(
    p.id.padEnd(6) +
    (i.widthVp + 'x' + i.heightVp).padEnd(12) +
    (p.split ? '是' : '否').padEnd(6) +
    ((p.leftSpan + ':' + p.rightSpan)).padEnd(7) +
    p.columns.toString().padEnd(10) +
    (p.contentMaxVp > 0 ? p.contentMaxVp.toString() : '不限').padEnd(9) +
    p.cardBasis.padEnd(8) +
    (p.minCardVp > 0 ? p.minCardVp.toString() : '-').padEnd(8) +
    (p.heroDia + 'vp').padEnd(8) +
    (p.btnH + 'vp').padEnd(8) +
    ('x' + p.fontScale.toFixed(2)).padEnd(7) +
    p.gutter.toString().padEnd(6) +
    p.pad.toString().padEnd(8) +
    p.radius.toString().padEnd(6) +
    (ar > 0 ? ar.toFixed(3) : '--'));
}

console.log('\n=== C. 判定依据 / 降级说明(S1..S10 + 兜底) ===');
for (const [label, i] of cases.slice(0, 10).concat([cases[17]])) {
  const p = M.resolvePlanPlain(i);
  console.log('[' + p.id + '] ' + p.name);
  console.log('    依据: ' + p.basis);
  console.log('    降级: ' + (p.degrade.length > 0 ? p.degrade : '无'));
}

console.log('\n=== D. 抖动抑制(边界 ±8vp) ===');
function hyst(w, h, prev) {
  return M.resolveShapeId(inp(w, h, NONE, true, 'phone', 'bar'), prev);
}
// S1(竖长) / S2(阔直板) 的边界在 宽高比 0.55 上: 600x1080 = 0.5556
console.log('600x1080 上一次 S1: ' + hyst(600, 1080, 'S1') + ' (期望保持 S1, 阈值附近不抖动)');
console.log('600x1060 上一次 S1: ' + hyst(600, 1060, 'S1') + ' (宽 600vp 起就分栏, 宽高比 < 0.80 -> S5, 与上一形态不同则切换)');
// S10(极扁) / 后面的边界在 高 340vp 上
console.log('900x341 上一次 S10: ' + hyst(900, 341, 'S10') + ' / 900x350 上一次 S10: ' + hyst(900, 350, 'S10') +
  '  (期望 341..349 保持 S10, 350 起 S8)');
console.log('幂等性(同一输入+同一上一次形态连算 3 次): ' +
  hyst(600, 1080, 'S1') + ',' + hyst(600, 1080, hyst(600, 1080, 'S1')) + ',' +
  hyst(600, 1080, hyst(600, 1080, hyst(600, 1080, 'S1'))));

// 结果区宽度: **直接用计划自己算出来的那个数**(geometry.resultZoneVp)。
//  ★ 为什么不再在用例里独立复算一遍: 以前用例用的是 content*rightSpan/12 - gutter 这个近似,
//    而计划用的是"内容列宽 - 左栏宽(按 spans 取整)"。7/12 与 60% 差 8.33% 的宽度 ——
//    近似公式会让"计划说不溢出、用例说溢出"这种假警报出现。现在的口径是:
//    用例拿**计划自己的** 列数 / 最小宽 / 间距 / 结果区宽度 四个数复算, 四个数必须自洽。
//    (结果区宽度的公式本身另有断言: 见 M 节, 它会用 contentMaxVp 与 spans 独立复算一遍。)
function rightZoneOf(p) {
  return p.geometry.resultZoneVp;
}

console.log('\n=== E. 横向不溢出校验 ===');
console.log('  两条规则, 覆盖面如实统计(单列形态不再被"跳过", 而是被明确断言):');
console.log('   (a) 多列形态: 列数 x 最小宽 + 间距 <= 结果区宽度');
console.log('   (b) 单列形态: 必须 minCardVp = 0 且 基准宽 = 100%  （设了下限反而会撑破窄屏）');
let overflow = 0;
let ruleA = 0, ruleB = 0;
for (const [label, i] of cases) {
  const p = M.resolvePlanPlain(i);
  const right = rightZoneOf(p);
  if (p.columns >= 2) {
    ruleA++;
    const need = p.columns * p.minCardVp + (p.columns - 1) * p.gutter;
    const ok = p.minCardVp > 0 && need <= right + 0.5;
    if (!ok) overflow++;
    console.log('  (a) ' + (i.widthVp + 'x' + i.heightVp).padEnd(11) + p.id.padEnd(5) +
      ' 结果区 ' + right.toFixed(1).padStart(7) + 'vp, ' + p.columns + ' 列 x ' + p.minCardVp +
      'vp + 间距 = ' + need.toFixed(1).padStart(7) + 'vp -> ' + (ok ? 'OK' : '溢出'));
  } else {
    ruleB++;
    const ok = p.minCardVp === 0 && p.cardBasis === '100%';
    if (!ok) overflow++;
    console.log('  (b) ' + (i.widthVp + 'x' + i.heightVp).padEnd(11) + p.id.padEnd(5) +
      ' 结果区 ' + right.toFixed(1).padStart(7) + 'vp, 单列 -> minCardVp=' + p.minCardVp +
      ' 基准宽=' + p.cardBasis + ' -> ' + (ok ? 'OK' : '异常'));
  }
}
console.log('  覆盖: 规则(a) ' + ruleA + ' 条, 规则(b) ' + ruleB + ' 条, 合计 ' + (ruleA + ruleB) +
  '/' + cases.length + ' 条 —— 没有一条被跳过');
console.log(overflow === 0 ? '  无溢出' : ('  有 ' + overflow + ' 条溢出'));

console.log('\n=== F. 可复制文本样例(两台机器各一份) ===');
//  F1: 目标机型 Pura X Max 展开态 664x940vp(1:sqrt(2) 竖长) -> S5
const fCase = cases.find((c) => c[0].startsWith('5g '));
console.log('---- F1. 目标机型展开态 664x940vp(1:sqrt(2), 竖长内屏) ----');
console.log(M.planLines(fCase[1], M.resolvePlanPlain(fCase[1]), ''));
//  F2: 16:10 的横向展开窗口(纯比例兜底档) -> S12
const fCase2 = cases.find((c) => c[0].startsWith('5c '));
console.log('---- F2. 展开态横向宽窗 1060x660(16:10, 比例兜底档) ----');
console.log(M.planLines(fCase2[1], M.resolvePlanPlain(fCase2[1]), ''));

// ---------------------------------------------------------------------------
//  G. 比例 / 宽度边界专项
//   ★ 2026-10 重写: 编号现在是**纯几何**的(只看宽/高/宽高比), 所以这一节不再围绕
//     "折叠屏展开"组织, 而是围绕**阈值**组织 —— 每一条都是"这个宽度/比例落在哪一档"。
//     期望值由规则推出来(不是照抄实际结果); 名字里不许出现面板断言(16:10 / 阔折叠)。
// ---------------------------------------------------------------------------
console.log('\n=== G. 阈值边界专项(比例 / 宽度) ===');
console.log('窗口'.padEnd(26) + '比例  '.padEnd(8) + '期望/实际 编号 名称'.padEnd(46) +
  '分栏  栅格'.padEnd(12) + '列 内容max  开始钮 按钮高 字号x');
// 每条带期望编号 —— 期望值由 shapeIdOf 的规则推出来, 不是照抄实际结果:
//   极窄: 宽<320 或 ar<0.40 -> S9   极扁/矮: ar>=3.0 或 (高<400 且 ar>=2.4) -> S10
//   扁宽: 1.70~3.0 -> S8            横向: 1.25~1.70(宽>=1100 -> S11, 否则 S7)
//   近方形: 0.80~1.25(最小边<730 -> S4, 否则 S6)
//   竖长: <0.80, 宽>=600 -> S5; 窄窗里 <=0.50 -> S3, <0.80 -> S1, 否则 S2
const boundary = [
  // ---- 上边界 / 下边界各取一点 ----
  ['ar 0.40 下界 400x1000', 400, 1000, 'S3'],
  ['ar 0.399 399x1000', 399, 1000, 'S9'],
  ['ar 0.500 边界 400x800', 400, 800, 'S3'],
  ['ar 0.501 401x800', 401, 800, 'S1'],
  ['ar 0.799 599x750', 599, 750, 'S1'],
  ['ar 0.80 下界 600x750', 600, 750, 'S4'],
  ['ar 1.249 749x600', 749, 600, 'S4'],
  ['ar 1.25 上界 750x600', 750, 600, 'S7'],
  ['ar 1.699 1359x800', 1359, 800, 'S11'],
  ['ar 1.70 下界 1360x800', 1360, 800, 'S8'],
  ['ar 2.999 1199x400', 1199, 400, 'S8'],
  ['ar 3.0 下界 1200x400', 1200, 400, 'S10'],
  ['宽 319 319x700', 319, 700, 'S9'],
  ['宽 320 320x700', 320, 700, 'S3'],   // 320vp 是"极窄"的**闭**下界(W_TINY: 宽 < 320 -> S9)
  ['宽 1099 1099x800', 1099, 800, 'S7'],
  ['宽 1100 1100x800', 1100, 800, 'S11'],
  ['高 399 且 ar 2.4 960x399', 960, 399, 'S10'],
  ['高 401 且 ar 2.4 964x401', 964, 401, 'S8'],
  ['最小边 729 729x729', 729, 729, 'S4'],
  ['最小边 730 730x730', 730, 730, 'S6']
];
let gBad = 0;
for (const [tag, w, h, want] of boundary) {
  const i = inp(w, h, EXP, true, 'phone', 'foldable');
  const p = M.resolvePlanPlain(i);
  const ar = w / h;
  // 名字必须说"实话": 不许出现 16:10 / "阔折叠内屏" 这种**面板断言**,
  // 也不许出现"系统未按大屏设备上报"(那是在替系统下结论);
  // 名字里的比例必须与实测一致(不是写死的)。
  const clean = p.name.indexOf('系统未按大屏') < 0 && p.name.indexOf('16:10') < 0 &&
    p.name.indexOf('阔折叠') < 0 && p.name.indexOf('内屏') < 0;
  const heroOk = p.heroDia >= 72 && p.heroDia <= 300 && p.heroDia <= w;
  const touchOk = p.btnH >= 40;
  const ok = p.id === want && clean && heroOk && touchOk;
  if (!ok) gBad++;
  console.log((tag + ' (' + w + 'x' + h + ')').padEnd(26) +
    ar.toFixed(3).padEnd(8) +
    (want + (p.id === want ? '/OK ' : '/' + p.id) + ' ' + p.index + ' ' + p.name).padEnd(46) +
    (p.split ? '是' : '否').padEnd(7) +
    (p.leftSpan + ':' + p.rightSpan).padEnd(10) +
    p.columns.toString().padEnd(3) + p.contentMaxVp.toString().padEnd(9) +
    (p.heroDia + 'vp').padEnd(8) + (p.btnH + 'vp').padEnd(7) + ('x' + p.fontScale.toFixed(2)));
}
console.log(gBad === 0
  ? ('  ' + boundary.length + ' 条边界全部落在期望编号; 名字无面板断言; 主按钮与触摸目标都在下限之上')
  : ('  有 ' + gBad + ' 条不符合预期'));
// 同尺寸、不同折叠状态 / 不同设备类型 -> 编号必须完全相同(判定只看几何)
const same = [[1060, 660], [340, 800], [665, 940], [2000, 600]];
let sameBad = 0;
for (const [w, h] of same) {
  const ids = [
    M.resolvePlanPlain(inp(w, h, NONE, true, 'phone', 'bar')).id,
    M.resolvePlanPlain(inp(w, h, EXP, true, 'phone', 'foldable')).id,
    M.resolvePlanPlain(inp(w, h, FOLDED, true, 'phone', 'foldable')).id,
    M.resolvePlanPlain(inp(w, h, UNK, false, 'phone', 'foldable')).id,
    M.resolvePlanPlain(inp(w, h, NONE, true, '2in1', '')).id,
    M.resolvePlanPlain(inp(w, h, NONE, true, 'tv', 'tablet')).id
  ];
  const uniq = [...new Set(ids)];
  const ok = uniq.length === 1;
  if (!ok) sameBad++;
  console.log('  同一窗口不同折叠状态/设备类型 ' + w + 'x' + h + ': ' + uniq.join('/') +
    ' -> ' + (ok ? 'OK（判定与它们无关）' : '不一致!'));
}
console.log(sameBad === 0
  ? '  折叠状态 / deviceType / 内置机型表**都不影响编号**(只影响文案)'
  : ('  有 ' + sameBad + ' 条不一致'));

// ---------------------------------------------------------------------------
//  H. 竖向空间预算 —— 高度是稀缺资源的窗口靠它兜底。
//     ★ 目标机型展开态是**竖长**的(664x940vp, 高 940vp), 走"放得下"那一条, 不是被压的。
// ---------------------------------------------------------------------------
console.log('\n=== H. 竖向空间预算(顶栏+标签栏 96 + 设备卡 150 + 主按钮 + 2 行结果) ===');
console.log('窗口'.padEnd(14) + '形态  可用高  需要高  fit  压主按钮 压间距 设备卡紧凑 只能滚动');
let hBad = 0;
const checkBudget = (tag, i) => {
  const p = M.resolvePlanPlain(i);
  const b = p.height;
  const avail = i.heightVp > 0 ? Math.round(i.heightVp) : 780;
  // needVp 必须能被"各项之和"复算出来: 顶栏+标签栏 + 设备卡(压缩后) + 主按钮块 + 2 行结果
  const need = b.chromeVp + b.topInfoVp + b.heroBlockVp + b.resultRowsVp;
  // 行高由窗口高算出(下限 28), 压缩档再 -4(仍有下限)
  const rowBase = Math.round(Math.min(44, Math.max(28, 28 + (avail - 500) * 0.06)));
  const rowWant = b.compactY ? Math.max(28, rowBase - 4) : rowBase;
  const inv = [
    b.availVp === avail,
    b.needVp === need,                       // 逐项相加必须精确等于 needVp
    b.scrollOnly === !b.fits,
    b.fits ? b.needVp <= b.availVp : b.needVp > b.availVp,
    p.heroDia >= 72 && p.heroDia <= 300 && p.heroDia <= i.widthVp,   // 有下限、不越上限、不超窗口宽
    b.topInfoVp === 150 || b.topInfoVp === 70,
    b.topInfoCompressed === (b.topInfoVp === 70),
    p.gutter >= 6 && p.pad >= 10,            // 间距/内边距被压过也仍有下限
    b.heroBlockVp === p.heroDia + (b.compactY ? 12 : 16),
    b.resultRowsVp === 2 * rowWant,
    p.btnH >= 40                             // 触摸目标下限
  ];
  const ok = inv.every(Boolean);
  if (!ok) hBad++;
  const why = ok ? '' : (' 失败项#' + inv.map((v, ix) => v ? '' : ix).filter((s) => s !== '').join(','));
  console.log((tag + ' ' + i.widthVp + 'x' + i.heightVp).padEnd(16) +
    p.id.padEnd(5) + b.availVp.toString().padEnd(8) + b.needVp.toString().padEnd(8) +
    (b.fits ? '是' : '否').padEnd(6) +
    (b.heroCompressed ? '是' : '否').padEnd(10) +
    (b.compactY ? '是' : '否').padEnd(8) +
    (b.topInfoCompressed ? '是' : '否').padEnd(12) +
    (b.scrollOnly ? '是' : '否').padEnd(6) + (ok ? 'OK' : 'INVFAIL') + why +
    (ok ? '' : (' [hero=' + p.heroDia + ' btnH=' + p.btnH + ' gut=' + p.gutter +
      ' pad=' + p.pad + ' heroBlk=' + b.heroBlockVp + ' rows=' + b.resultRowsVp +
      ' top=' + b.topInfoVp + ' cmp=' + b.compactY + ']')));
};
for (const [tag, w, h, want] of boundary) checkBudget('边界', inp(w, h, EXP, true, 'phone', 'foldable'));
// 目标机型: 展开态竖长(1:sqrt(2))与它横放时的预算各查一遍
checkBudget('展开竖', inp(664, 940, EXP, true, 'phone', 'foldable'));
checkBudget('展开竖', inp(665, 940, EXP, true, 'phone', 'foldable'));
checkBudget('展开竖', inp(566, 800, EXP, true, 'phone', 'foldable'));
checkBudget('外屏', inp(672, 460, FOLDED, true, 'phone', 'foldable'));
checkBudget('手机', inp(360, 780, NONE, true, 'phone', 'bar'));
checkBudget('阔直板', inp(430, 700, NONE, true, 'phone', 'bar'));
checkBudget('平板', inp(1000, 700, NONE, true, 'tablet', 'tablet'));
checkBudget('电脑', inp(1400, 900, NONE, true, '2in1', ''));
checkBudget('极扁', inp(900, 300, NONE, true, 'phone', 'bar'));
checkBudget('极窄', inp(300, 800, NONE, true, 'phone', 'bar'));
checkBudget('小方窗', inp(400, 400, NONE, true, 'phone', 'bar'));
checkBudget('超大', inp(2000, 1400, NONE, true, 'tablet', 'tablet'));
console.log(hBad === 0 ? '  预算不变量全部成立(需要高/可用高/压缩标志/滚动标志/各分项)' : ('  有 ' + hBad + ' 条不成立'));

// ---------------------------------------------------------------------------
//  I. 全尺寸扫描 —— 任意窗口下都不许溢出、不许出现未知形态/非法列数
// ---------------------------------------------------------------------------
console.log('\n=== I. 全尺寸扫描 ===');
const dtypes = ['phone', 'tablet', '2in1', 'tv', 'default'];
const folds = [[NONE, true], [EXP, true], [FOLDED, true], [UNK, false]];
let n = 0, oA = 0, oB = 0, badCol = 0, badShape = 0;
const seen = {};
const shapesOk = ['S0','S1','S2','S3','S4','S5','S6','S7','S8','S9','S10','S11','S12'];
for (let w = 280; w <= 2400; w += 20) {
  for (let h = 280; h <= 1600; h += 20) {
    for (const dt of dtypes) {
      for (const [fs, ok] of folds) {
        const i = inp(w, h, fs, ok, dt, dt === 'tablet' ? 'tablet' : (fs === EXP ? 'foldable' : 'bar'));
        const p = M.resolvePlanPlain(i);
        n++;
        seen[p.id] = (seen[p.id] || 0) + 1;
        if (p.columns < 1 || p.columns > 3) badCol++;
        if (shapesOk.indexOf(p.id) < 0) badShape++;
        const right = rightZoneOf(p);
        if (p.columns >= 2) {
          if (p.minCardVp <= 0 || p.columns * p.minCardVp + (p.columns - 1) * p.gutter > right + 0.5) oA++;
        } else {
          if (p.minCardVp !== 0 || p.cardBasis !== '100%') oB++;
        }
      }
    }
  }
}
console.log('  采样点 ' + n + ' 个 (宽 280~2400 step20 x 高 280~1600 step20 x 5 deviceType x 4 折叠状态)');
console.log('  规则(a) 多列不溢出 违例 = ' + oA);
console.log('  规则(b) 单列无下限 违例 = ' + oB);
console.log('  非法列数 = ' + badCol + ' · 未知形态编号 = ' + badShape);
console.log('  编号可达情况: ' + shapesOk.filter((s) => seen[s]).map((s) => s + '=' + seen[s]).join(' '));
console.log((oA + oB + badCol + badShape) === 0 ? '  扫描干净' : '  扫描发现问题');

// ---------------------------------------------------------------------------
//  K. ★"把形态名字全删掉, 布局结果是否完全不变"★ —— 这一节就是那件事的验证办法。
//
//  做法(不是嘴上保证, 而是**改掉被测代码本身**): 把转译产物里的两个纯文本函数
//    nameOf()(形态名) 与 indexOf()(需求表编号) 整体替换成恒返回空值/0,
//    重新加载一份模块, 然后把全部用例、边界用例与 I 节扫描点逐个跑两遍,
//    逐个字段比对整份计划 —— **除了 name / id / basis / note 这四个纯文本字段,
//    其余每一个数都必须逐位相同**。
//
//  另外用**源码静态检查**兜底: LayoutPlan.ets 的布局算术里不许出现 'S0'.. 'S12' 之类的
//    编号比较、"形态名"、deviceType / tableFormFactor / foldState 这些只用于显示的输入。
// ---------------------------------------------------------------------------
console.log('\n=== K. 名字剥离复现(删掉形态名与编号, 布局是否不变) ===');
// 把**源码**里的 nameOf / indexOf 整块换成常量(按行匹配花括号配平, 不靠正则赌边界):
//  这样拿到的是"名字/编号在这份文件里彻底不存在"的版本, 而不是"名字恰好拼成空串"。
function stripFnByBraces(src, header) {
  const lines = src.split(String.fromCharCode(10));
  const out = [];
  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    if (line.trim().indexOf(header) === 0) {
      let depth = 0;
      let closed = false;
      while (i < lines.length) {
        const t = lines[i];
        for (let c = 0; c < t.length; c++) {
          if (t.charAt(c) === '{') depth++;
          else if (t.charAt(c) === '}') {
            depth--;
            if (depth === 0) { closed = true; }
          }
        }
        i++;
        if (closed) break;
      }
      // ★ 保留**原同签名**的桩(不能只留 header — 转译会把它当成另一份函数定义而整块删掉,
      //   结果调用处变成未定义引用, 反而测不出东西)。桩体里没有一行判定逻辑。
      const isIdx = header.indexOf('indexOf') >= 0;
      const args = isIdx ? 'id' : 'id, inp';
      out.push(header + ' ' + args + '): ' + (isIdx ? 'number' : 'string') + ' { return ' +
        (isIdx ? '0' : "''") + '; }');
      continue;
    }
    out.push(line);
    i++;
  }
  return out.join(String.fromCharCode(10));
}
function loadStrippedModule() {
  let srcText = readFileSync(src, 'utf8');
  srcText = stripFnByBraces(srcText, 'function nameOf(');
  srcText = stripFnByBraces(srcText, 'function indexOf(');
  const stripped = ts.transpileModule(srcText, {
    compilerOptions: { target: ts.ScriptTarget.ES2021, module: ts.ModuleKind.ESNext },
    fileName: 'LayoutPlanStripped.ts'
  });
  const dir2 = mkdtempSync(join(tmpdir(), 'ab-strip-'));
  const f2 = join(dir2, 'Stripped.mjs');
  writeFileSync(f2, stripped.outputText, 'utf8');
  return import(pathToFileURL(f2).href);
}
// 这几个字段是"只给人看"的标签, 剥离后本来就该变 —— 比对时排除它们, 其余**每一个数**都必须相同。
//   exclude: name(形态名) / index(需求表编号) / basis(判定依据文案) / note(形态说明文案) / degrade(降级文案)
//   ★ 注意 id 不在排除之列: 编号本身由几何决定(与名字无关), 剥离后 id 必须一模一样。
const TEXT_FIELDS = ['name', 'index', 'basis', 'note', 'degrade'];
function numericFields(p) {
  const o = {};
  for (const k of Object.keys(p)) {
    if (TEXT_FIELDS.indexOf(k) >= 0) continue;
    o[k] = p[k];
  }
  return o;
}
let kBad = 0, kChecked = 0, kSweep = 0, kStripWorked = false;
// 加载"剥离版"必须在 async 里(不能用顶层 await): 本文件在满足条件下会被当 CommonJS 解析,
//  顶层 await 会直接语法报错。IIFE 立刻执行, 外面紧接着就用到结果 —— 顺序不会乱。
const kPromise = (async () => {
  let S = null;
  try {
    S = await loadStrippedModule();
  } catch (e) {
    kBad++;
    console.log('  剥离复现无法执行: ' + e.message);
    return;
  }
  const probe = inp(665, 940, EXP, true, 'phone', 'foldable');
  const sp = S.resolvePlanPlain(probe);
  kStripWorked = sp.name === '' && sp.index === 0;
  const kPoints = cases.concat(boundary.map((b) => ['K ' + b[0],
    inp(b[1], b[2], EXP, true, 'phone', 'foldable'), b[3]]));
  for (const [label, i] of kPoints) {
    const a = numericFields(M.resolvePlanPlain(i));
    const b = numericFields(S.resolvePlanPlain(i));
    kChecked++;
    if (JSON.stringify(a) !== JSON.stringify(b)) {
      kBad++;
      if (kBad < 4) {
        console.log('  字段变了: ' + label);
        console.log('    原: ' + JSON.stringify(a));
        console.log('    剥离后: ' + JSON.stringify(b));
      }
    }
  }
  // 再扫一遍全尺寸网格(step 60), 覆盖任意比例
  for (let w = 280; w <= 2400; w += 60) {
    for (let h = 280; h <= 1600; h += 60) {
      const i = inp(w, h, EXP, true, 'phone', 'foldable');
      const a = numericFields(M.resolvePlanPlain(i));
      const b = numericFields(S.resolvePlanPlain(i));
      kSweep++;
      if (JSON.stringify(a) !== JSON.stringify(b)) {
        kBad++;
        if (kBad < 4) console.log('  扫描点字段变了: ' + w + 'x' + h);
      }
    }
  }
})();

// ---------------------------------------------------------------------------
//  L. ★任意比例专项★ —— 每条断言: 不溢出 / 不裁切 / 关键内容可见 / 触摸目标达标 /
//     并且 geometry 里的每个数都能由窗口宽高**独立复算**出来。
// ---------------------------------------------------------------------------
console.log('\n=== L. 任意比例: 不溢出 / 不裁切 / 关键内容可见 / 触摸达标 ===');
console.log('窗口(vp)'.padEnd(16) + '比例'.padEnd(8) + '编号'.padEnd(6) + '分栏  栅格'.padEnd(12) +
  '列'.padEnd(4) + '内容/结果区'.padEnd(16) + '开始钮 按钮高 字号x 预算');
const ratioList = [
  ['1:sqrt(2) 竖长', 665, 940], ['16:10 横', 1060, 665], ['9:16 竖', 360, 640],
  ['1:1 方形', 800, 800], ['21:9 超宽', 2100, 900], ['3:1 极扁', 1500, 500],
  ['1:3 极窄', 400, 1200], ['小方窗', 400, 400], ['超大', 2000, 1400],
  ['真机展开态', 664, 940], ['真机横放', 940, 665], ['手机竖', 360, 780],
  ['平板横', 1000, 700], ['极扁窗', 900, 300], ['窄分屏', 300, 800]
];
// 伪随机比例(固定种子, 结果可复现 —— 不用 Math.random, 免得每次跑的数不一样)
let seed = 20261005;
const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
for (let k = 0; k < 12; k++) {
  ratioList.push(['随机#' + (k + 1), Math.round(280 + rnd() * 1720), Math.round(280 + rnd() * 1320)]);
}
let lBad = 0;
for (const [tag, w, h] of ratioList) {
  const i = inp(w, h, NONE, true, 'phone', 'bar');
  const p = M.resolvePlanPlain(i);
  const g = p.geometry;
  const content = p.contentMaxVp > 0 ? Math.min(p.contentMaxVp, w) : w;
  // ① 几何字段必须能由宽高独立复算(证明这些数不是拍脑袋来的)
  const geoOk = g.widthVp === w && g.heightVp === h &&
    Math.abs(g.aspect - w / h) < 1e-9 &&
    g.usedWidthVp === w && g.usedHeightVp === h &&
    g.shortSideVp === Math.min(w, h) && g.longSideVp === Math.max(w, h) &&
    g.contentVp === content;
  // ② 结果区宽度: 分栏时 = 内容列宽 - 左栏宽(按 spans 取整), 不分栏时 = 内容列宽
  const leftWant = Math.round(content * p.leftSpan / 12);
  const zoneWant = p.split ? (content - leftWant) : content;
  const zoneOk = Math.abs(g.resultZoneVp - zoneWant) < 1e-9 &&
    (p.split ? (Math.abs(g.leftColVp - leftWant) < 1e-9 &&
      Math.abs(g.rightColVp - (content - leftWant)) < 1e-9) : (g.leftColVp === 0 && g.rightColVp === 0));
  // ③ 不溢出: 多列时 列数 x 最小宽 + 间距 <= 结果区宽度; 单列时 minCardVp = 0 且基准宽 100%
  const overflowOk = p.columns >= 2
    ? (p.minCardVp > 0 && p.columns * p.minCardVp + (p.columns - 1) * p.gutter <= g.resultZoneVp + 0.5)
    : (p.minCardVp === 0 && p.cardBasis === '100%');
  const colOk = p.columns >= 1 && p.columns <= 3;
  // ④ 关键内容可见: 放不下时必须是"靠纵向滚动"(scrollOnly), 而不是把内容裁掉
  const keyOk = p.height.fits ? (p.height.needVp <= p.height.availVp)
    : (p.height.scrollOnly === true);
  // ⑤ 触摸目标: 按钮高 >= 40vp, 主按钮直径 >= 88vp 且不超过窗口宽
  const touchOk = p.btnH >= 40 && p.heroDia >= 72 && p.heroDia <= 300 && p.heroDia <= w;
  // ⑥ 内容不裁切: 单列卡必须有整行宽度可用(基准宽 100%), 多列卡不低于最小可读宽
  const cardOk = p.columns === 1 ? p.cardBasis === '100%' : (p.minCardVp >= 300 && p.cardBasis !== '100%');
  const ok = geoOk && zoneOk && overflowOk && colOk && keyOk && touchOk && cardOk;
  if (!ok) lBad++;
  console.log(tag.padEnd(16) + (w + 'x' + h).padEnd(12) + (w / h).toFixed(3).padEnd(8) +
    p.id.padEnd(6) +
    (p.split ? '是 ' : '否 ') .padEnd(6) + (p.leftSpan + ':' + p.rightSpan).padEnd(6) +
    p.columns.toString().padEnd(4) +
    (Math.round(g.contentVp) + '/' + Math.round(g.resultZoneVp)).padEnd(16) +
    (p.heroDia + 'vp').padEnd(7) + (p.btnH + 'vp').padEnd(7) + ('x' + p.fontScale.toFixed(2)).padEnd(7) +
    (p.height.fits ? '放得下' : '纵向滚动') + (ok ? '  OK' : '  FAIL' + (geoOk ? '' : ' geo') +
      (zoneOk ? '' : ' zone') + (overflowOk ? '' : ' overflow') + (colOk ? '' : ' col') +
      (keyOk ? '' : ' key') + (touchOk ? '' : ' touch') + (cardOk ? '' : ' card')));
}
console.log(lBad === 0
  ? ('  ' + ratioList.length + ' 个任意比例窗口: 全部不溢出、不裁切、关键内容可见、按钮达标,' +
    ' 且 geometry 每个数都由宽高复算一致')
  : ('  有 ' + lBad + ' 条不符合'));

// ---------------------------------------------------------------------------
//  N. ★结果页首屏: 分块估算 + "三个大数字卡不许吃满屏幕" 的不变量★(2026-10 真机缺陷回归)
//
//  背景(用户真机 7.3 截图): 一键跑分结果页第一层的三个大数字卡(单核 / 多核 / GPU)被**纵向
//  拉伸到几乎占满整屏**, 卡与卡之间隔着几百 vp, 底下的关键得分列表被挤到最下面一小条。
//  根因两处(都在源码里, N3 会把它们钉死):
//    ① Index.ets 的 bigScoreCard() 根 Column 上挂着 .layoutWeight(1)。它的父节点是 **Column**,
//       主轴是**竖直**的 —— ArkUI 线性布局对 layoutWeight 子节点是按"父容器理想主轴尺寸 -
//       已分配"给尺寸的(linear_layout_utils.cpp: remainSize -> SetIdealMainSize),
//       所以卡高 = "父容器给多少吃多少", 与内容无关; 窗口越高空块越大。
//    ② 三张卡的 flex 基准宽取自**首页结果卡**的列数(竖屏手机上 = 1 → 基准宽 100%),
//       三张卡于是各占一行 —— 行数一多, 上面那个 layoutWeight 就有了成倍的纵向空间可以吃。
//  同时暴露的方法论问题: 上一轮自检页显示 firstScreenVp = 833/896 通过, 真机却完全不是那样 ——
//  因为那份估算按"三张卡一行"算, 而界面按"每张卡一行 + 拉伸"渲染, 两套模型对不上。
//  本节把"估算 == 渲染"写成可执行的断言:
//    N1 若干典型窗口(含真机 665x940)上, 三个大数字块总高 <= 可用高度的 30%(不变量);
//    N2 分块账本自洽: 逐块相加 == firstScreenVp; 只有"结果列表 / 滚动区"是拉伸块(grow > 0),
//       其它块一律固定高度(grow === 0) —— "多余高度给谁"是可核对的;
//    N3 源码静态检查: bigScoreCard 里不许再有 layoutWeight; 三处调用点都必须写固定高度,
//       且三处 Flex 都必须显式 alignItems: ItemAlign.Start(交叉轴不拉伸)。
// ---------------------------------------------------------------------------
console.log('\n=== N. 结果页首屏分块估算 + 大数字卡不变量(<= 可用高度 30%) ===');
const l1Windows = [
  ['真机 665x940 (1:√2)', 665, 940],
  ['手机 360x780', 360, 780],
  ['平板 1000x700', 1000, 700],
  ['PC 1400x900', 1400, 900],
  ['超宽 2000x600', 2000, 600],
  ['小方窗 400x400', 400, 400]
];
let nBad = 0;
console.log('窗口'.padEnd(21) + '可用高'.padEnd(8) + '30%上限'.padEnd(9) + '大数字块'.padEnd(10) +
  '占比'.padEnd(8) + '每张卡'.padEnd(8) + '一行/行数'.padEnd(11) + '首屏'.padEnd(7) + '多余给谁');
for (const [tag, w, h] of l1Windows) {
  const p = M.resolvePlanPlain(inp(w, h, NONE, true, 'phone', 'bar'));
  const r = p.result;
  const cap = Math.floor(r.availVp * 0.3);
  const blocks = r.blocks;
  // ① ★不变量★: 三个大数字块的总高 <= 可用高度的 30%
  const invOk = r.l1BlockH <= cap && r.l1InvariantOk === true;
  // ② 卡高是**固定值**: 整块 = 上下内边距 + 行数 x 卡高(逐项对得上, 没有"账外"的加法)
  const idOk = r.l1BlockH === 2 * r.pad + r.l1Lines * r.l1CardH;
  // ③ 行数与每行张数自洽, 且卡高不低于可读下限
  const colOk = r.l1Cards >= 1 && r.l1Cards <= 3 && r.l1Lines === Math.ceil(3 / r.l1Cards) &&
    r.l1CardH >= 48;
  // ④ 基准宽与"一行几张"一一对应(单列 100%; 多列必须小于 100% 且不大于均分)
  const basisNum = r.l1Basis === '100%' ? 100 : parseInt(r.l1Basis, 10);
  const basisOk = r.l1Cards === 1
    ? (r.l1Basis === '100%' && r.l1MinW === 0)
    : (basisNum > 0 && basisNum <= Math.floor(100 / r.l1Cards) && r.l1MinW >= 104);
  // ⑤ 分块账本: 固定块相加 == firstScreenVp(并排时 = 两栏共有块 + 较高那一栏的块 + 下内边距)
  let fixedSum = 0;
  let growCount = 0;
  let badBlocks = 0;
  for (const b of blocks) {
    if (b.key === r.growBlockKey) {
      growCount++;
      if (b.fixed !== false || b.grow <= 0) { badBlocks++; }
      continue;
    }
    if (b.fixed !== true || b.grow !== 0) { badBlocks++; }
    //  单列时: 除滚动区外的每一块都直接相加; 并排时留给下面的"较高栏"复算
    if (!r.side) { fixedSum += b.vp; }
  }
  //  并排时 firstScreenVp = 较高栏 + 下内边距; 这里按"两栏共有 + 左栏"或"两栏共有 + 右栏"复算一次,
  //  取与 firstScreenVp 相等的那一种(证明账目闭合, 且能看到是哪一栏更高)
  let ledgerOk = false;
  if (!p.result.side) {
    ledgerOk = fixedSum === r.firstScreenVp;
  } else {
    for (const tall of ['左栏', '右栏']) {
      let s = 0;
      for (const b of blocks) {
        if (b.key === r.growBlockKey) { continue; }
        if (b.col === '两栏' || b.col === tall) { s += b.vp; }
      }
      if (s === r.firstScreenVp) { ledgerOk = true; }
    }
  }
  const oneGrowOk = growCount === 1 && badBlocks === 0;
  const fitOk = r.fits === (r.firstScreenVp <= r.availVp) &&
    (r.slackVp === r.availVp - r.firstScreenVp);
  const ok = invOk && idOk && colOk && basisOk && ledgerOk && oneGrowOk && fitOk;
  if (!ok) nBad++;
  console.log(tag.padEnd(21) + (r.availVp + 'vp').padEnd(8) + (cap + 'vp').padEnd(9) +
    (r.l1BlockH + 'vp').padEnd(10) +
    ((r.l1Share * 100).toFixed(1) + '%').padEnd(8) + (r.l1CardH + 'vp').padEnd(8) +
    (r.l1Cards + ' x ' + r.l1Lines).padEnd(11) + (r.firstScreenVp + 'vp').padEnd(7) +
    (r.slackVp > 0 ? ('滚动区 ' + r.slackVp + 'vp') : '无（列表要滚动）') +
    (ok ? '  OK' : ('  FAIL' + (invOk ? '' : ' 不变量') + (idOk ? '' : ' 卡高恒等式') +
      (colOk ? '' : ' 行数/卡高') + (basisOk ? '' : ' 基准宽') + (ledgerOk ? '' : ' 分块账目') +
      (oneGrowOk ? '' : ' 拉伸块') + (fitOk ? '' : ' 首屏/余量'))));
  //  逐块打印(这就是自检页上那几行): 块名 / 在哪一栏 / 预测 vp / 固定还是拉伸 / 凭什么这么高
  for (const b of blocks) {
    console.log('    · ' + b.label.padEnd(22) + (b.vp + 'vp').padEnd(8) + ('[' + b.col + ']').padEnd(7) +
      (b.fixed ? '固定高度' : ('拉伸 grow=' + b.grow)).padEnd(12) + b.detail);
  }
}
//  N1-b: 全尺寸扫描 —— 不变量在**任意**窗口上都成立(不是只在上面 6 个窗口上成立)
let nSweep = 0, nSweepBad = 0;
for (let w = 280; w <= 2400; w += 40) {
  for (let h = 320; h <= 1600; h += 40) {
    nSweep++;
    const r = M.resolvePlanPlain(inp(w, h, NONE, true, 'phone', 'bar')).result;
    if (!(r.l1BlockH <= r.availVp * 0.3 + 0.5)) { nSweepBad++; }
    if (!(r.l1BlockH === 2 * r.pad + r.l1Lines * r.l1CardH)) { nSweepBad++; }
    if (r.l1Cards < 1 || r.l1Cards > 3 || r.l1Lines !== Math.ceil(3 / r.l1Cards)) { nSweepBad++; }
  }
}
//  N1-c: "卡高不随窗口高按比例长大" —— 同一宽度把窗口高翻倍, 大数字块**不许**跟着翻倍
//  (改前正是这个毛病: 拉得越高, 卡越像个吃满屏的空块; 现在有 30% 上限 + 字号封顶双重约束)
const baseL1 = M.resolvePlanPlain(inp(665, 940, NONE, true, 'phone', 'bar')).result;
const tallL1 = M.resolvePlanPlain(inp(665, 1880, NONE, true, 'phone', 'bar')).result;
const noGrowOk = tallL1.l1BlockH <= baseL1.l1BlockH * 1.6 &&
  tallL1.l1BlockH <= Math.floor(tallL1.availVp * 0.3);
console.log('  窗口高翻倍(665x940 -> 665x1880): 大数字块 ' + baseL1.l1BlockH + 'vp -> ' +
  tallL1.l1BlockH + 'vp（<= 1.6 倍且仍在 30% 之内: ' + (noGrowOk ? '是' : '否') + '）');
console.log('  全尺寸扫描 ' + nSweep + ' 个窗口: 不变量 / 卡高恒等式 / 行数自洽 违例 = ' + nSweepBad);
if (nSweepBad > 0) { nBad += nSweepBad; }
if (!noGrowOk) { nBad++; }
console.log(nBad === 0
  ? ('  ' + l1Windows.length + ' 个典型窗口 + ' + nSweep + ' 个扫描窗口: 三个大数字块**从不超过可用高度的 30%**, ' +
    '卡高是固定值(账目逐项对得上), 多余高度只给结果列表 / 滚动区')
  : ('  有 ' + nBad + ' 条不符合'));

// ---------------------------------------------------------------------------
//  P. ★开始页(无结果状态)必须拿到正数宽高★(2026-10 真机缺陷回归: 用户报的 5 条里的 1/2/3/5)
//
//  背景: 开始页在"一次都没跑过"时也要画控件 —— 首页分数卡(上次成绩)、三大数字卡的样子、
//  块间距、字号全都来自 resultDecoFor(实测窗口宽高 -> ResultDeco, 也就是界面里的 this.uiRes)。
//  用户报的现象(按钮成黑块、文字重影、三个数字一位一行)有一半的共同特征就是
//  "某个宽/高被压到极小或 0": 一旦这些字段里出现 0 / 负数 / NaN, ArkUI 就会按"最小内容"
//  去排 —— 大字数字一位一行、圆环被容器切掉、两行文字挤在一起, 全都能这么来。
//
//  本节把"开始页要用的每一个字段都必须是正数"写成**可执行断言**, 三条:
//    P1 关键窗口(含"窗口未取到"的 0x0 兜底 + 用户报的 460x672 与 1440x960)逐个字段核对;
//    P2 全尺寸扫描: 任意窗口上都不许出现非正数(不是只在几个样例上成立);
//    P3 源码静态检查(Index.ets): uiRes 的初值必须与其余布局状态同源、三大数字卡与小数字块
//       必须有"绝不折行"的字号策略、圆环容器不许小于外圈直径、结果区不许没有空态。
//       ★ 这一条会在有人把 uiRes 写回与窗口无关的默认值时**变红** ★
// ---------------------------------------------------------------------------
console.log('\n=== P. 开始页(无结果状态)使用的每一个 uiRes 字段都必须是正数 ===');
//  开始页会用到的字段(逐条对应界面上的写法):
//   · bigScoreCard(首页分数卡 / 结果浮层共用): l1CardH(固定高) l1Basis(基准宽) l1MinW(最小宽)
//     keyName(标签) huge(大数字) keyUnit(单位与条件) l1CondLines(条件行数)
//   · miniScore(首页「上次成绩」卡里的三个小数字块): keyName keyNum
//   · 上次成绩卡: giant(总分字号) gap(块间距) pad(内边距)
//   · 首页三张 GB7 卡与结果页明细: detailMetricW(「数字 + 单位」列) detailCols(列数)
//   · 结果浮层: metricW condW inlineCond rowPy side leftSpan rightSpan
const PD_POSITIVE = ['giant', 'huge', 'keyNum', 'keyName', 'keyUnit', 'gap', 'pad', 'rowPy',
  'metricW', 'detailMetricW', 'l1CardH', 'l1BlockH', 'l1Share', 'availVp', 'firstScreenVp'];
const PD_ATLEAST1 = ['l1Cards', 'l1Lines', 'l1CondLines', 'keyCols', 'detailCols',
  'leftSpan', 'rightSpan'];
function pdComplaints(r, tag) {
  const bad = [];
  for (const f of PD_POSITIVE) {
    const v = r[f];
    if (typeof v !== 'number' || !isFinite(v) || v <= 0) {
      bad.push(tag + ' ' + f + '=' + String(v));
    }
  }
  for (const f of PD_ATLEAST1) {
    const v = r[f];
    if (typeof v !== 'number' || !isFinite(v) || v < 1) {
      bad.push(tag + ' ' + f + '=' + String(v));
    }
  }
  // 基准宽必须是可解析的百分比, 且落在 (0, 100] —— '0%' / '1000%' / 空串都是废值
  const b = r.l1Basis;
  const num = (typeof b === 'string' && b.endsWith('%')) ? Number(b.slice(0, -1)) : NaN;
  if (!isFinite(num) || !(num > 0) || num > 100) {
    bad.push(tag + ' l1Basis=' + String(b));
  }
  // 最小宽只有"一行一张"时才允许是 0(那一档按设计不设下限)
  if (!(r.l1MinW > 0) && !(r.l1MinW === 0 && r.l1Cards === 1)) {
    bad.push(tag + ' l1MinW=' + String(r.l1MinW));
  }
  // 取得条件列: 只有"条件另起一行"时才允许是 0
  if (!(r.condW > 0) && r.inlineCond === true) {
    bad.push(tag + ' condW=' + String(r.condW) + '(inlineCond=true)');
  }
  if (typeof r.slackVp !== 'number' || !isFinite(r.slackVp)) {
    bad.push(tag + ' slackVp=' + String(r.slackVp));
  }
  if (!Array.isArray(r.blocks) || r.blocks.length === 0) {
    bad.push(tag + ' blocks 不是非空数组');
  } else {
    for (const bl of r.blocks) {
      if (typeof bl.vp !== 'number' || !isFinite(bl.vp)) {
        bad.push(tag + ' blocks[' + bl.key + '].vp=' + String(bl.vp));
      }
    }
  }
  if (typeof r.text !== 'string' || r.text.length === 0) {
    bad.push(tag + ' text 为空');
  }
  return bad;
}
const pWindows = [
  ['窗口未取到(0x0 兜底)', 0, 0],
  ['窄窗 460x672(用户报的)', 460, 672],
  ['平板宽窗 1440x960(用户报的)', 1440, 960],
  ['真机 1:√2 665x940', 665, 940],
  ['手机 360x780', 360, 780],
  ['极窄 300x800', 300, 800],
  ['小方窗 400x400', 400, 400],
  ['极扁 2000x600', 2000, 600]
];
let pBad = 0;
for (const [tag, w, h] of pWindows) {
  const r = M.resolvePlanPlain(inp(w, h, NONE, true, 'phone', 'bar')).result;
  const bad = pdComplaints(r, tag);
  if (bad.length > 0) pBad += bad.length;
  console.log('  ' + (bad.length === 0 ? 'OK  ' : 'FAIL') + '  ' + tag.padEnd(26) +
    ('大数字卡 ' + r.l1CardH + 'vp x 基准 ' + r.l1Basis + ' / 最小 ' + r.l1MinW + 'vp').padEnd(46) +
    ('字号 ' + r.giant + '/' + r.huge + '/' + r.keyNum + '/' + r.keyName + '/' + r.keyUnit).padEnd(28) +
    ('间距 ' + r.gap + '/' + r.pad + '/' + r.rowPy).padEnd(14) +
    (bad.length === 0 ? '' : bad.join(' | ')));
}
//  P2: 全尺寸扫描 —— 非正数在**任意**窗口上都不许出现
let pSweep = 0, pSweepBad = 0;
for (let w = 240; w <= 2400; w += 40) {
  for (let h = 280; h <= 1700; h += 40) {
    pSweep++;
    const r = M.resolvePlanPlain(inp(w, h, NONE, true, 'tablet', 'tablet')).result;
    if (pdComplaints(r, w + 'x' + h).length > 0) pSweepBad++;
  }
}
console.log('  全尺寸扫描 ' + pSweep + ' 个窗口(含 0x0 那一档之外的任意比例): 非正数违例 = ' + pSweepBad);
pBad += pSweepBad;
console.log(pBad === 0
  ? ('  ' + pWindows.length + ' 个关键窗口 + ' + pSweep + ' 个扫描窗口: 开始页用到的每一个宽高/字号/间距' +
    '都是有限正数(与"有没有跑分结果"无关 —— 它们只由实测窗口宽高算出)')
  : ('  有 ' + pBad + ' 处非正数/废值 —— 开始页会按"最小内容"排版(用户报的 1/2/3/5 条就是这么来的)'));

// ---------------------------------------------------------------------------
//  M. 源码静态检查(函数定义; 调用在 Z 之前, 见 K 的 .then)
//     —— "布局里不许出现形态编号 / 机型 / 比例预期" 这条要能机械核对。
//     它是给 K 节那条结论兜底的第二重证据: K 证明"删掉名字结果不变",
//     M 证明"布局算术段里压根没有能影响它的东西"。
// ---------------------------------------------------------------------------
function printSourceCheck(srcText) {
  const lines = srcText.split(String.fromCharCode(10));
  // 用源码里那两个**显式标记**切段(比按标题猜稳): LAYOUT-CORE-BEGIN ~ LAYOUT-CORE-END。
  //  这一段里不许出现: 形态编号(SHAPE_Sx)、显示层函数(nameOf/indexOf/aspectHint)、
  //  设备/折叠字段(deviceType/tableFormFactor/foldState/foldApiOk)、任何写死的比例或机型。
  let start = 0, end = lines.length;
  for (let k = 0; k < lines.length; k++) {
    if (lines[k].indexOf('LAYOUT-CORE-END') >= 0) end = k;
    if (lines[k].indexOf('LAYOUT-CORE-BEGIN') >= 0 && start === 0) start = k;
  }
  const bad = [];
  for (let k = start; k < end; k++) {
    const t = lines[k];
    if (t.trim().startsWith('//')) continue;                 // 注释不算
    if (/SHAPE_S\d+/.test(t)) bad.push('布局段引用了形态编号(第 ' + (k + 1) + ' 行)');
    if (/nameOf|indexOf|aspectHint/.test(t)) bad.push('布局段引用了显示层函数(第 ' + (k + 1) + ' 行)');
    if (/deviceType|tableFormFactor|foldState|foldApiOk/.test(t)) {
      bad.push('布局段引用了设备/折叠字段(第 ' + (k + 1) + ' 行)');
    }
    if (/16:10|16：10|Pura|HOP-AL|Kirin/.test(t)) bad.push('布局段写死了机型/面板(第 ' + (k + 1) + ' 行)');
  }
  console.log('\n=== M. 源码静态检查(布局算术段) ===');
  console.log('  检查范围: 第 ' + (start + 1) + ' ~ ' + end + ' 行(' + (end - start) + ' 行)');
  console.log('  结果: ' + (bad.length === 0
    ? '干净 —— 无形态编号、无设备字段、无写死的比例/机型'
    : (bad.length + ' 处: ' + bad.join(' / '))));

  // ---- N3: 结果页第一层"不许纵向拉伸"的源码静态检查(Index.ets) ----
  //  把"数字卡不许吃满屏幕"这句话变成**机械可核对**的事实(而不是靠肉眼看截图):
  //   ① bigScoreCard() 的根 Column 上不许再有 layoutWeight —— 它的父节点是 Column(主轴竖直),
  //      layoutWeight 会让卡高变成"父容器给多少吃多少", 这就是真机那次拉伸的直接原因;
  //   ② 三处调用点都必须写固定高度 .height(this.uiRes.l1CardH), 基准宽必须用结果页自己的
  //      this.l1Basis()(不许再借首页结果卡的 this.cardBasis()), 最小宽同理;
  //   ③ 三处 Flex 必须显式 alignItems: ItemAlign.Start(交叉轴不拉伸)。
  const page = readFileSync(join(here, '..', 'entry', 'src', 'main', 'ets', 'pages', 'Index.ets'), 'utf8');
  //  ★ 只看代码, 不看注释: 这一节的注释里就写着".layoutWeight(1) 是根因", 若连注释一起搜,
  //    检查会被自己写的说明文字绊倒(那是假阳性, 会让这条断言失去意义)。
  const pageCode = page.split(String.fromCharCode(10))
    .filter((l) => !l.trim().startsWith('//')).join(String.fromCharCode(10));
  const bad2 = [];
  const bStart = pageCode.indexOf('bigScoreCard(c: ScoreCard) {');
  const bEnd = bStart < 0 ? -1 : pageCode.indexOf('\n  @Builder', bStart);
  if (bStart < 0 || bEnd < 0) {
    bad2.push('找不到 bigScoreCard 的定义(静态检查失效, 必须修用例本身)');
  } else {
    const body = pageCode.slice(bStart, bEnd);
    if (body.indexOf('.layoutWeight(') >= 0) {
      bad2.push('bigScoreCard 里仍有 layoutWeight(父节点是 Column, 主轴竖直 -> 会纵向拉伸)');
    }
    if (body.indexOf('.height(this.uiRes.l1CardH)') < 0) {
      bad2.push('bigScoreCard 没有写固定高度 .height(this.uiRes.l1CardH)');
    }
    if (body.indexOf(".width('100%')") < 0) {
      bad2.push("bigScoreCard 没有写 .width('100%')");
    }
  }
  let useN = 0;
  for (let k = 0; k < pageCode.length; k++) {
    if (pageCode.startsWith('this.bigScoreCard(c)', k)) {
      useN++;
      const win = pageCode.slice(Math.max(0, k - 500), k + 700);
      if (win.indexOf('.height(this.uiRes.l1CardH)') < 0) {
        bad2.push('第 ' + useN + ' 处大数字卡没有固定高度');
      }
      if (win.indexOf('.flexBasis(this.l1Basis())') < 0) {
        bad2.push('第 ' + useN + ' 处大数字卡没有用结果页自己的基准宽 this.l1Basis()');
      }
      if (win.indexOf('.flexBasis(this.cardBasis())') >= 0) {
        bad2.push('第 ' + useN + ' 处大数字卡还在借首页结果卡的基准宽 this.cardBasis()');
      }
      if (win.indexOf('alignItems: ItemAlign.Start') < 0) {
        bad2.push('第 ' + useN + ' 处大数字卡的 Flex 没有显式 alignItems: ItemAlign.Start(交叉轴可能被拉伸)');
      }
    }
  }
  if (useN !== 3) {
    bad2.push('大数字卡的调用点应当正好 3 处(一键跑分结果页 / 单次跑分结果浮层 / 首页分数卡), 实际 ' + useN);
  }
  console.log('  结果页第一层"不许纵向拉伸"源码检查: ' + (bad2.length === 0
    ? '干净 —— bigScoreCard 无 layoutWeight, 3 处调用点都有固定高度 / 结果页基准宽 / alignStart'
    : (bad2.length + ' 处: ' + bad2.join(' / '))));
  // ---- P3: 开始页(无结果状态)的四条源码断言(见 P 节) ----
  const bad3 = startPageSourceCheck(page);
  // ---- Q: 「历史」页(新增的第二个标签)的源码静态检查(见 Q 节) ----
  histPageBad = historyPageSourceCheck(page);
  return bad.length + bad2.length + bad3;
}

// ---------------------------------------------------------------------------
//  Q. 源码静态检查(Index.ets): 「历史」页
//  这一节回答的是"这一页有没有真的按要求排得对", 十条:
//    Q1 底部导航等分: tabBarItem 用 layoutWeight(1) 分剩余宽度,
//       **不许**再用 width('100%')(那样每个标签都按整个底栏要位置,
//       标签多于一个时后面的会被挤出屏幕)。
//    Q2 历史页存在, 且只在切到本页 / 整轮跑完后读盘(按需渲染)。
//    Q3 每一条必须有的东西 —— 2026-10-06 用户看了结果页截图后定稿的形态:
//       ① 时间(人话) + 设备(短名); ② **上半部分: 三个大数字**(单核 / 多核 / GPU)一组;
//       ③ **下半部分: 一行一项的清单**; ④ 有异常才有的一行小字。
//       2026-10-05 真机反馈之后卡片重做成"大字、少数字"; 2026-10-06 上午把 CS1 三个分加成
//       第二行大数字; 用户随后要求"精简一点, 只保留项目名和分数" —— 于是 CS1 三个分从
//       "第二行大数字"变成清单里的三行, 下半部分只剩"项目名 + 分数"。这一条跟着改形状。
//       2026-10 / 10.0 位置对调: 上面那三个大数字换成 **CS1**, 下面清单前三行换成 **自研**
//       (用户:"把分数的主体突出部位变成 CS1…把这几个字眼的组件放到现在 CS1 的位置")。
//    Q3b 三个大数字必须与结果页那三个**同一套**: 走同一个 buildBigThree(),
//        histBigNum() 用到的结果页密度令牌不许超出 bigScoreCard() 用到的集合。
//    Q3c 三个大数字的取数入口只从账本已有的键里取(不新建数据源)。
//    Q3d ★新★ 下半部分清单: 六项齐全(自研 单核 / 自研 多核 / 自研 GPU / GPU-SNL / NPU 推理 /
//        存储 I/O), **顺序照结果页**; (结果页清单第一行「自研套件总分」不进卡片 —— 用户说它与
//        上面三个大数字重复, 而且它正好会把 460x672 上"一屏 2 张"挤掉, 见 Q9 那笔账)
//        取数同样只许取账本已有的键; 自研那三行走 buildBigThree(), GPU-SNL / NPU / 存储
//        走 num1() / num2()(与结果页那三行逐字同源)。
//    Q3f ★新★ 新加的三个账本键(SNL / NPU / 存储)必须"接口声明 + 写账本 + 解析行"三处齐全 ——
//        只在读的一侧加键, 卡片上那三行会永远显示 '--'。
//    Q3e ★新★ 清单那一行里**只许有"项目名 + 分数"**: 整行正好两个 Text, 项名白(TEXT)、
//        分数蓝(ACCENT); 单位(row.unit) / 取得条件(row.cond) / 灰色小字(FAINT / SUB) /
//        单位那一档字号(keyUnit)一个都不许出现在这一行(用户原话: "只保留项目名和分数")。
//    Q4 备注**只在有异常时**才出现(没异常就什么都不显示, 也不写"正常")。
//    Q4b 历史页不许出现"表格式的密集行": 任何一处 Text(...) 的实参里
//        数字产出表达式(toString / toFixed / Math.round / histNum)不得超过 7 个
//        (>= 8 直接失败); 旧那三个排版函数 histVerText / histNum / histScoreLine
//        以及耗时列 / 版本列都不许回来(CS1 三分是**以清单里的三行**回来的, 不在此列)。
//    Q5 空态就一句话(不留白, 也不写成说明书)。
//    Q6 这一页里不准有动画调用(跑分期间不能有逐帧重绘)。
//    Q8 口径那句必须还在(列表最后极淡的一行 historyCalibLine), 不许回到表头;
//       并且必须按 10.0 位置对调后的形态说清楚: 上面三个大数字是 CS1、其中只有单核能跨设备比、
//       CS1 多核 8.2 之前口径不同不要跨版本比、下面的自研三项与 GPU-SNL / NPU / 存储只适合同机前后比、
//       GPU-SNL / NPU / 存储不计分; 仍然只许是一句话(句号最多一个)。
//    Q9 定稿后的卡片更高了(三个大数字 + 六行清单), 但"一屏至少看得见 2 张"必须仍然成立 ——
//       按实测窗口算出来的卡片高度(与源码写法逐项对应)翻倍之后仍要放得下。
//       (窗口本身不到 600vp 高的极端方形窗只做"至少 1 张"的承诺, 见 Q9 末尾那条。)
//  (与 M / P3 同样的做法: 只看代码, 不看注释。)
// ---------------------------------------------------------------------------
let histPageBad = 0;
function bodyOf(codeText, marker) {
  const at = codeText.indexOf(marker);
  if (at < 0) {
    return null;
  }
  const end = codeText.indexOf(String.fromCharCode(10) + '  @Builder', at);
  return codeText.slice(at, end < 0 ? undefined : end);
}
// ---- Q 节用的小工具(都只看代码, 不看注释) ----
// 取一个函数/Builder 的**括号配平**函数体(字符串字面量里的括号不算)。
// 为什么不用上面的 bodyOf(): 它是"切到下一个 @Builder 为止", 会把中间的辅助函数一起吞进来,
// 用来比对"某一段用了哪些令牌"时会误判。
function fnBodyOf(codeText, marker) {
  const at = codeText.indexOf(marker);
  if (at < 0) {
    return null;
  }
  const open = codeText.indexOf('{', at);
  if (open < 0) {
    return null;
  }
  let depth = 0;
  let inStr = false;
  let q = '';
  for (let i = open; i < codeText.length; i++) {
    const ch = codeText[i];
    if (inStr) {
      if (ch === '\\') { i++; continue; }
      if (ch === q) { inStr = false; }
      continue;
    }
    if (ch === String.fromCharCode(39) || ch === '"') { inStr = true; q = ch; continue; }
    if (ch === '{') { depth++; continue; }
    if (ch === '}') {
      depth--;
      if (depth === 0) {
        return codeText.slice(open + 1, i);
      }
    }
  }
  return null;
}

// 一段源码里用到的结果页密度令牌(去重, 保持出现顺序)
function uiResTokens(body) {
  const out = [];
  const re = /this\.uiRes\.([A-Za-z0-9_]+)/g;
  let m = re.exec(body);
  while (m !== null) {
    if (out.indexOf(m[1]) < 0) {
      out.push(m[1]);
    }
    m = re.exec(body);
  }
  return out;
}

// 历史页那一段源码(historyRowItem .. historyTab 结束) —— 密集行检查只看这一段
function histPageSource(codeText) {
  const from = codeText.indexOf('historyRowItem(');
  if (from < 0) {
    return '';
  }
  const at = codeText.indexOf('historyTab() {');
  const body = at < 0 ? null : fnBodyOf(codeText, 'historyTab() {');
  if (body === null) {
    return codeText.slice(from);
  }
  const end = codeText.indexOf(body, at);
  return codeText.slice(from, end < 0 ? undefined : (end + body.length));
}

// 取出源码里每一处 Text(...) 的实参文本(按括号配平, 跳过字符串字面量)
function textArgs(codeText) {
  const out = [];
  const key = 'Text(';
  let i = codeText.indexOf(key);
  while (i >= 0) {
    const open = i + key.length - 1;
    let depth = 0;
    let inStr = false;
    let q = '';
    let k = open;
    for (; k < codeText.length; k++) {
      const ch = codeText[k];
      if (inStr) {
        if (ch === '\\') { k++; continue; }
        if (ch === q) { inStr = false; }
        continue;
      }
      if (ch === String.fromCharCode(39) || ch === '"') { inStr = true; q = ch; continue; }
      if (ch === '(') { depth++; continue; }
      if (ch === ')') {
        depth--;
        if (depth === 0) {
          break;
        }
      }
    }
    out.push(codeText.slice(open + 1, k));
    i = codeText.indexOf(key, k + 1);
  }
  return out;
}

// "数字产出表达式": 一行里出现几个, 就是这一行要塞几个数进去
const NUM_EXPR = /(?:\.toString\s*\(|\.toFixed\s*\(|Math\.round\s*\(|\bhistNum\s*\()/g;

function historyPageSourceCheck(pageText) {
  const pageCode = pageText.split(String.fromCharCode(10))
    .filter((l) => !l.trim().startsWith('//')).join(String.fromCharCode(10));
  const bad = [];
  // Q1 底部导航等分
  const bar = bodyOf(pageCode, 'tabBarItem(idx: number, label: string) {');
  if (bar === null) {
    bad.push('找不到 tabBarItem 的定义(静态检查失效, 必须修用例本身)');
  } else {
    if (bar.indexOf('.layoutWeight(1)') < 0) {
      bad.push('标签项没有 layoutWeight(1) —— 标签不会等分, 多于一个时后面的会被挤出屏幕');
    }
    if (bar.indexOf('.width(' + String.fromCharCode(39) + '100%' + String.fromCharCode(39) + ')') >= 0) {
      bad.push('标签项又写回了 width(100%) —— 那是"只看得到第一个标签"的原因');
    }
  }
  const tabsUsed = (pageCode.match(/this\.tabBarItem\(\d+, /g) || []).length;
  if (tabsUsed < 2) {
    bad.push('底部导航不到两个标签(实际 ' + tabsUsed + ' 个)');
  }
  // Q2 历史页按需渲染
  const tab = bodyOf(pageCode, 'historyTab() {');
  if (tab === null) {
    bad.push('找不到 historyTab()');
  }
  const reload = bodyOf(pageCode, 'private reloadHistory(): void {');
  if (reload === null) {
    bad.push('找不到 reloadHistory()');
  } else if (reload.indexOf('RunLog.get().historyRowsNewestFirst(') < 0) {
    bad.push('reloadHistory() 没有读账本(RunLog.historyRowsNewestFirst) —— 数据源必须与历史成绩同源');
  } else if (reload.indexOf('BenchStore') >= 0) {
    bad.push('reloadHistory() 又去读 BenchStore 了 —— 那是另一份数据(自研套件的旧记录)');
  }
  const switchAt = pageCode.indexOf('this.tab = idx;');
  if (switchAt < 0 || pageCode.slice(switchAt, switchAt + 400).indexOf('this.reloadHistory()') < 0) {
    bad.push('切到历史页时没有读一次账本');
  }
  // Q3 每一条必须有的东西(2026-10-06 定稿的形状: 时间 + 设备 + **三个大数字** + **一行一项的清单**)
  //    上半部分: 三个大数字(单核 / 多核 / GPU)—— 一行, 颜色字体照旧;
  //    下半部分: 一行一项的清单 —— 每行只有"项目名(白) + 分数(蓝)", 六项齐全 + 结果页第一行"自研套件总分"。
  const Q1 = String.fromCharCode(39);
  const item = bodyOf(pageCode, 'historyRowItem(r: RunHistoryRow) {');
  const rowPart = fnBodyOf(pageCode, 'histBigRow(tag: string, cards: ScoreCard[]) {');
  const linePart = fnBodyOf(pageCode, 'histKeyLine(row: KeyRow) {');
  const listPart = fnBodyOf(pageCode, 'private histKeyRows(r: RunHistoryRow): KeyRow[] {');
  if (item === null) {
    bad.push('找不到 historyRowItem()');
  } else {
    const need = [
      ['histTimeText(', '时间'],
      ['histDeviceText(', '设备'],
      //  ★ 2026-10(10.0)位置对调: 行标签从「自研」改成「CS1」—— 大数字那一组现在是 CS1,
      //    下面清单前三行才是自研。标签必须跟着换, 否则读的人会把两组数看串。
      ["this.histBigRow('CS1', this.histBig(r))", '上半部分的三个大数字(单核 / 多核 / GPU)'],
      ['this.histKeyRows(r)', '下半部分一行一项的清单'],
      ['this.histKeyLine(row)', '清单里"项目名 + 分数"那一行的零件']
    ];
    for (const pair of need) {
      if (item.indexOf(pair[0]) < 0) {
        bad.push('历史每一条缺少' + pair[1]);
      }
    }
    // 上半部分只许一行三个大数字(2026-10 / 10.0 位置对调后: 上面是 CS1, 下面清单前三行是自研 ——
    //  两组都只出现一次, 不许再摆第二行大字)
    const rowUses = (item.match(/this\.histBigRow\(/g) || []).length;
    if (rowUses !== 1) {
      bad.push('卡片上半部分有 ' + rowUses + ' 组大数字 —— 定稿的形态是"三个大数字"一组');
    }
    if (rowPart === null) {
      bad.push('找不到 histBigRow() —— 大数字的排版零件不见了(静态检查失效, 必须修用例本身)');
    } else {
      if (rowPart.indexOf('this.histBigNum(c)') < 0) {
        bad.push('histBigRow() 没有走 histBigNum() —— 大数字会另画一套样式');
      }
      if (rowPart.indexOf('Text(tag)') < 0) {
        bad.push('histBigRow() 没有画行标签 —— 下面清单前三行是自研, 上面那一组是 CS1, 不写清会看串');
      }
      if (rowPart.indexOf('.fontSize(this.uiRes.keyUnit)') < 0 ||
        rowPart.indexOf('.fontColor(FAINT)') < 0) {
        bad.push('行标签不是"小字(keyUnit) + 浅色(FAINT)" —— 它会跟三个大数字抢位置');
      }
      if (rowPart.indexOf('.lineHeight(Math.round(this.uiRes.keyUnit * 1.25))') < 0) {
        bad.push('行标签没有显式行高(1.25 x keyUnit) —— 卡片高度就变成猜的, Q9 那笔账不算数');
      }
      // Q3g ★新★(2026-10-06, 真机截图上抓到的): 三个大数字**必须排在一行里, 绝不许换行**。
      //   原来的写法是 Flex(wrap: Wrap) + flexBasis(l1Basis) + constraintSize(minWidth: l1MinW),
      //   而历史页在 665x940 上是**分栏**的, 一张卡只有 ~310vp 宽; l1MinW 比 310/3 大,
      //   于是三个数字换行成三行, 卡片高度是 Q9 估算值的 3 倍 —— 一屏连一张都放不下,
      //   用户截图里就是"单核 / 多核 / GPU 竖着排"。Q9 当时算的是"一行大数字"的高度,
      //   可它没有把"这一行会不会换行"钉住, 所以用例是绿的、机器上是坏的。
      //   现在的写法是等分三列的 Row + layoutWeight(1) + minWidth 0, 数字放不下时由
      //   histBigNum 的 minFontSize/maxLines(1) 缩字号, 排版上不可能换行。
      //   这条断言就是"不许再改回会换行的容器"。
      if (rowPart.indexOf('FlexWrap.Wrap') >= 0 || rowPart.indexOf('flexBasis(this.uiRes.l1Basis)') >= 0 ||
        rowPart.indexOf('constraintSize({ minWidth: this.uiRes.l1MinW })') >= 0) {
        bad.push('histBigRow() 用了会换行的容器(wrap / flexBasis(l1Basis) / minWidth(l1MinW)) —— ' +
          '分栏窗口下一张卡只有 ~310vp 宽, 三个大数字会竖排成三行, 卡片高度翻三倍(真机上出过)');
      }
      if (rowPart.indexOf('layoutWeight(1)') < 0) {
        bad.push('histBigRow() 的三个大数字没有等分宽度(layoutWeight(1)) —— 它们会挤在一边或换行');
      }
    }
    // Q3d ★新★ 下半部分那六行: 项名与顺序照结果页的关键得分清单, 一行都不许少、不许乱序
    //  六项, 顺序照结果页的关键得分清单(结果页第一行「自研套件总分」刻意不放进卡片:
    //  用户说它与上面三个大数字重复, 而且它正好会把 460x672 上的"一屏 2 张"挤掉 —— Q9)
    //  ★ 2026-10(10.0)位置对调: 清单前三行从 CS1 换成了**自研套件**(CS1 现在是上面那三个大数字)。
    //    项名与顺序照结果页的关键得分清单的新形态; 后三项没动。
    const keyNames = ['自研 单核', '自研 多核', '自研 GPU', 'GPU-SNL', 'NPU 推理', '存储 I/O'];
    if (listPart === null) {
      bad.push('找不到 histKeyRows() —— 下半部分那一列清单不见了');
    } else {
      let prev = -1;
      for (const n of keyNames) {
        const at = listPart.indexOf(Q1 + n + Q1);
        if (at < 0) {
          bad.push('历史卡片的清单里缺少「' + n + '」这一行');
        } else if (at < prev) {
          bad.push('清单里「' + n + '」的顺序与结果页的关键得分清单不一致');
        } else {
          prev = at;
        }
      }
      // 取数只许取账本行里已有的键(不新建数据源); 自研那三行必须经过 buildBigThree()
      //  ★ 2026-10(10.0)位置对调: 前三行现在取 own*(它们原来是 CS1 的那三行, 取的是 gb8*)。
      for (const k of ['ownSingle', 'ownMulti', 'ownGpu',
        'snlScore', 'npuGops', 'storeWriteMbps']) {
        if (listPart.indexOf('r.' + k) < 0) {
          bad.push('清单没有取账本里的 ' + k + ' —— 数据源必须是 runhistory.jsonl 已有的键');
        }
      }
      if (listPart.indexOf('buildBigThree(') < 0) {
        bad.push('清单里自研那三行没有走 ResultLayers.buildBigThree() —— "没拿到写 --" 的口径会分叉');
      }
      // GPU-SNL / NPU / 存储 必须用结果页那两个排版函数(整数分 / 一位小数 / 两位小数)
      if (listPart.indexOf('num1(') < 0 || listPart.indexOf('num2(') < 0) {
        bad.push('清单里 GPU-SNL / NPU / 存储 没有用结果页的 num1() / num2()');
      }
      if (listPart.indexOf('readText(') >= 0 || listPart.indexOf('runhistory') >= 0) {
        bad.push('histKeyRows() 在自己读盘 —— 历史页的数据源只许是 RunLog.historyRowsNewestFirst');
      }
    }
    // Q3f ★新★ 新加的三个键必须真的被写进账本 —— 只加在"读"的一侧的话, 卡片上那三行
    //  会永远显示 '--'(这正是本次改动里差点漏掉的一处: 构造 RunHistoryExtra 的地方没填)。
    //  这条断言把"声明 / 写入 / 解析"三头都钉住, 少一头就变红。
    let runLogSrc = '';
    try {
      runLogSrc = readFileSync(join(here, '..', 'entry', 'src', 'main', 'ets', 'service',
        'RunLog.ets'), 'utf8');
    } catch (e) {
      bad.push('读不到 service/RunLog.ets(静态检查失效)');
    }
    if (runLogSrc.length > 0) {
      const rlCode = runLogSrc.split(String.fromCharCode(10))
        .filter((l) => !l.trim().startsWith('//')).join(String.fromCharCode(10));
      for (const k of ['snlScore', 'npuGops', 'storeWriteMbps']) {
        const hits = (rlCode.match(new RegExp(k, 'g')) || []).length;
        if (hits < 3) {
          bad.push('账本键 ' + k + ' 在 RunLog.ets 里只有 ' + hits + ' 处 —— ' +
            '新增的可选键必须"接口声明 + 写账本 + 解析行"三处齐全');
        }
      }
      //  ★ 2026-10-10 增补(真机缺陷回归): 上面那条"数出现次数"的断言**不够** ——
      //    这三个键在 RunLog.ets 里当时确实出现了 3 次以上(接口声明 / summary 里搬进
      //    RunHistoryInput / 解析行), 但真正落盘的那一行是在另一个函数 historyAdd()
      //    里逐字段拼的, 那里**没有搬这三个键** -> 账本行里根本没有它们 -> 历史卡三行永远 '--'。
      //    次数够了、数据仍然丢了, 所以那条断言绿着、机器上是坏的。
      //    这一条把"最后一跳"钉死: historyAdd() 的函数体里必须真的把三个键赋给 line。
      const haAt = rlCode.indexOf('historyAdd(h: RunHistoryInput): void {');
      const haEnd = haAt < 0 ? -1 : rlCode.indexOf(String.fromCharCode(10) + '  }', haAt);
      if (haAt < 0 || haEnd < 0) {
        bad.push('找不到 historyAdd() 的函数体(静态检查失效, 必须修用例本身)');
      } else {
        const haBody = rlCode.slice(haAt, haEnd);
        for (const k of ['snlScore', 'npuGops', 'storeWriteMbps']) {
          if (haBody.indexOf('line.' + k + ' = h.' + k + ';') < 0) {
            bad.push('historyAdd() 没有把 ' + k + ' 搬进真正落盘的 line —— ' +
              '账本行里不会有这个键, 历史卡片那一行永远是 --(真机上出过)');
          }
        }
      }
    }
    const hexAt = pageCode.indexOf('const hex: RunHistoryExtra = {');
    if (hexAt < 0) {
      bad.push('找不到 Index.ets 里构造 RunHistoryExtra 的那一处 —— 账本行不会被填上新键');
    } else {
      const hexBlock = pageCode.slice(hexAt, hexAt + 1500);
      for (const k of ['snlScore:', 'npuGops:', 'storeWriteMbps:']) {
        if (hexBlock.indexOf(k) < 0) {
          bad.push('写账本那一处没有填 ' + k + ' —— 历史卡片上那一行会永远是 --');
        }
      }
    }
    // Q3e ★新★ 清单那一行里**只许有"项目名 + 分数"**: 单位 / 括号条件 / 灰色小字一个都不许有
    if (linePart === null) {
      bad.push('找不到 histKeyLine() —— 清单那一行的零件不见了(静态检查失效, 必须修用例本身)');
    } else {
      const nText = (linePart.match(/Text\(/g) || []).length;
      if (nText !== 2) {
        bad.push('清单一行里有 ' + nText + ' 个 Text —— 只许"项目名 + 分数"两样');
      }
      if (linePart.indexOf('row.name') < 0 || linePart.indexOf('.fontColor(TEXT)') < 0) {
        bad.push('清单行的项目名不是白色(TEXT)');
      }
      if (linePart.indexOf('row.value') < 0 || linePart.indexOf('.fontColor(ACCENT)') < 0) {
        bad.push('清单行的分数不是结果页清单那个蓝(ACCENT)');
      }
      for (const b2 of [['row.unit', '单位'], ['row.cond', '取得条件(括号里那串灰字)'],
        ['FAINT', '灰色小字'], ['.fontColor(SUB)', '灰色小字'], ['keyUnit', '单位/条件那一档字号']]) {
        if (linePart.indexOf(b2[0]) >= 0) {
          bad.push('清单行里又出现了' + b2[1] + '(' + b2[0] + ') —— 用户要的是"只保留项目名和分数"');
        }
      }
      if (linePart.indexOf('this.uiRes.keyName') < 0 || linePart.indexOf('this.uiRes.keyNum') < 0) {
        bad.push('清单行的项名 / 分数没有用结果页清单那两个字号令牌(keyName / keyNum)');
      }
      if (linePart.indexOf('.maxLines(1)') < 0) {
        bad.push('清单行没有 maxLines(1) —— 窄卡片上会折成两行');
      }
    }
    // 仍然不许回来的东西: 旧的一行三个数的小字密集行 / App 内的版本列 / 自己拼数字 / 耗时列。
    //  (2026-10 / 10.0 位置对调: 这一列前三行现在是自研; CS1 已经变成上面那三个大数字)
    //  (buildBigThree 取数 + histKeyLine 排版), 不是那种小字密集行; 由上面的 Q3d 钉住。
    const banned = [
      ['histScoreLine(', '旧的一行三个数的密集行 histScoreLine()'],
      ['histVerText(', 'App 内的版本列 histVerText()'],
      ['histNum(', '自己拼数字的 histNum()'],
      ['durationMs', '耗时列'],
      ['appVer', 'App 内的版本列']
    ];
    for (const pair of banned) {
      if (item.indexOf(pair[0]) >= 0) {
        bad.push('历史卡片里又出现了' + pair[1] + '(用户要的是"大字、少数字")');
      }
    }
    // Q4 备注只在有异常时出现
    if (item.indexOf('if (this.histNoteText(r).length > 0) {') < 0) {
      bad.push('备注行没有被"有异常才显示"包住 —— 正常也会占一行');
    }
  }
  // Q3b 三个大数字必须与结果页那三个同一套(buildBigThree + 同一组结果页密度令牌)
  const big = fnBodyOf(pageCode, 'bigScoreCard(c: ScoreCard) {');
  const hbn = fnBodyOf(pageCode, 'histBigNum(c: ScoreCard) {');
  const hbig = fnBodyOf(pageCode, 'private histBig(r: RunHistoryRow): ScoreCard[] {');
  if (big === null || hbn === null || hbig === null) {
    bad.push('找不到 bigScoreCard() / histBigNum() / histBig() 的定义(静态检查失效, 必须修用例本身)');
  } else {
    if (hbig.indexOf('buildBigThree(') < 0) {
      bad.push('histBig() 没有走 ResultLayers.buildBigThree() —— 历史卡片与结果页会各算一套口径');
    }
    // Q3c 上半部分那一组三个大数字的取数入口: 只许取账本里**已经有**的那三个键。
    //  ★ 2026-10(10.0)位置对调: 上半部分那三个大数字从自研套件换成了 **CS1**,
    //    所以这里期望的键从小写 own* 换成小写 gb8*(**键名本身一个都没改**, 老记录照常读)。
    for (const k of ['gb8Single', 'gb8Multi', 'gb8Gpu']) {
      if (hbig.indexOf('r.' + k) < 0) {
        bad.push('histBig() 没有取账本里的 ' + k + ' —— 上半部分必须用 runhistory.jsonl 已有的键');
      }
    }
    // 没拿到的数一律是 '--' 而不是 0 分: 上半部分与清单里那三行都必须走 buildBigThree()
    if (pageCode.indexOf('buildBigThree(r.gb8Single, r.gb8Multi, r.gb8Gpu)') < 0) {
      bad.push('上半部分那三个数没有经过 buildBigThree() —— "没拿到分写 --" 的口径会分叉');
    }
    const bigT = uiResTokens(big);
    const hbnT = uiResTokens(hbn);
    for (const t of hbnT) {
      if (bigT.indexOf(t) < 0) {
        bad.push('histBigNum() 用了结果页大数字卡没有的密度令牌 this.uiRes.' + t +
          ' —— 三个大数字必须与结果页同一套字号/间距');
      }
    }
    for (const t of ['huge', 'keyName', 'keyUnit']) {
      if (hbnT.indexOf(t) < 0) {
        bad.push('histBigNum() 少了 this.uiRes.' + t + ' —— 字号不再与结果页那三个大数字同源');
      }
    }
    if (hbn.indexOf('this.cardTone(c.key)') < 0) {
      bad.push('histBigNum() 没有用 cardTone() 上色 —— 单核/多核/GPU 的颜色会与结果页不一致');
    }
    if (hbn.indexOf('.maxLines(1)') < 0 || hbn.indexOf('.minFontSize(') < 0) {
      bad.push('histBigNum() 缺 maxLines(1) / minFontSize —— 窄卡片上大数字会换行或溢出');
    }
  }
  // Q4b 历史页不许出现"表格式的密集行": 任何一处 Text(...) 实参里的数字产出表达式 <= 7 个
  const histRegion = histPageSource(pageCode);
  const args = textArgs(histRegion);
  let worst = 0;
  let worstText = '';
  for (const a of args) {
    const n = (a.match(NUM_EXPR) || []).length;
    if (n > worst) {
      worst = n;
      worstText = a.replace(/\s+/g, ' ').slice(0, 90);
    }
  }
  if (args.length === 0) {
    bad.push('历史页里一个 Text(...) 都没扫到(静态检查失效, 必须修用例本身)');
  } else if (worst >= 8) {
    bad.push('历史页有一行 Text 里塞了 ' + worst + ' 个数字(>= 8): ' + worstText +
      ' —— 那就是用户说的"密集表", 请拆成大字卡片');
  }
  const note = bodyOf(pageCode, 'private histNoteText(r: RunHistoryRow): string {');
  if (note === null) {
    bad.push('找不到 histNoteText()');
  } else if (note.indexOf('正常') >= 0) {
    bad.push('histNoteText() 里出现了"正常" —— 没异常就该什么都不显示');
  }
  // Q5 空态就一句话
  const empty = bodyOf(pageCode, 'historyEmpty() {');
  if (empty === null) {
    bad.push('找不到 historyEmpty()');
  } else {
    const texts = (empty.match(/Text\(/g) || []).length;
    if (texts === 0) {
      bad.push('空态里一句话都没有(会留白)');
    } else if (texts > 2) {
      bad.push('空态写了 ' + texts + ' 句 —— 它只应该是一句话');
    }
  }
  // Q7 历史成绩表必须标出口径变更点(多核列 8.2 之前不同)
  let histFile = '';
  try {
    histFile = readFileSync(join(here, '..', 'entry', 'src', 'main', 'ets', 'service',
      'ScoreHistory.ets'), 'utf8');
  } catch (e) {
    bad.push('读不到 service/ScoreHistory.ets(静态检查失效)');
  }
  if (histFile.length > 0) {
    const hcode = histFile.split(String.fromCharCode(10))
      .filter((l) => !l.trim().startsWith('//')).join(String.fromCharCode(10));
    if (hcode.indexOf("'版本'") < 0) {
      bad.push('历史成绩表没有版本列 —— 无法看出哪一行是哪个口径');
    }
    if (hcode.indexOf('8.2 之前口径不同') < 0 || hcode.indexOf('不要跨版本比多核') < 0) {
      bad.push('表头口径里没写"多核列 8.2 之前口径不同, 不要跨版本比多核"');
    }
    if (hcode.indexOf('r.appVer') < 0) {
      bad.push('数据行没有写版本(版本列会是空的)');
    }
  }
  // Q6 没有动画
  for (const pair of [[tab, 'historyTab'], [item, 'historyRowItem'], [empty, 'historyEmpty'],
    [bodyOf(pageCode, 'historyReportOverlay() {'), 'historyReportOverlay']]) {
    if (pair[0] === null) {
      continue;
    }
    if (pair[0].indexOf('animateTo(') >= 0 || pair[0].indexOf('.animation(') >= 0) {
      bad.push(pair[1] + ' 里出现了动画调用 —— 跑分期间不能有逐帧重绘');
    }
  }
  // 点一条 -> 打开那一次的报告
  const open = bodyOf(pageCode, 'private openHistoryReport(r: RunHistoryRow): void {');
  if (open === null) {
    bad.push('找不到 openHistoryReport()');
  } else if (open.indexOf('reportStampName(') < 0 || open.indexOf('filesDir()') < 0) {
    bad.push('openHistoryReport() 没有按时间去对应那一份报告文件');
  }
  // Q8 口径那句: 必须还在(列表最后极淡的一行), 而且要按**定稿后的形态**说清楚
  //  卡片现在是"上面三个大数字 + 下面一行一项的清单", 里面混着四个小节的数,
  //  "哪个能跨设备比、哪个只能同机前后比"必须写在界面上, 否则用户会把不该比的数直接比;
  //  但用户明确要求"一句话", 所以句号只许有一个。
  const calib = fnBodyOf(pageCode, 'historyCalibLine() {');
  if (calib === null) {
    bad.push('找不到 historyCalibLine() —— 卡片上这些数"能不能跨设备比"就没地方说了');
  } else {
    //  ★ 2026-10(10.0)位置对调之后这一句的语义整个反了: 上面三个大数字现在是 CS1,
    //    所以"只有 CS1 单核能跨设备比"变成"只有单核能跨设备比"(前提是上一句已经点名 CS1)。
    if (calib.indexOf('只有单核能跨设备比') < 0) {
      bad.push('口径那句没说清"上面那三个数里只有单核能跨设备比"');
    }
    if (calib.indexOf('只适合同一台机器前后对比') < 0) {
      bad.push('口径那句没说清"自研那一组只适合同一台机器前后对比"');
    }
    if (calib.indexOf('不要跨版本比') < 0) {
      bad.push('口径那句丢了"CS1 多核在 8.2 之前口径不同, 不要跨版本比"');
    }
    if (calib.indexOf('CS1') < 0 || calib.indexOf('自研') < 0) {
      bad.push('口径那句没有点名 CS1 / 自研 —— 上面三个数与下面清单会分不清');
    }
    if (calib.indexOf('不计分') < 0) {
      bad.push('口径那句没说 GPU-SNL / NPU / 存储 不计分 —— 这三个数会被加进总分里比');
    }
    const stops = (calib.match(/。/g) || []).length;
    if (stops === 0) {
      bad.push('口径那句一个句号都没有(静态检查可能失效, 必须修用例本身)');
    } else if (stops > 1) {
      bad.push('口径那句被写成了 ' + stops + ' 句 —— 用户要的是"一句话", 不是说明书');
    }
  }
  // Q9 定稿后的卡片更高了(三个大数字 + 六行清单) —— "一屏至少看得见 2 张"必须仍然成立。
  //  卡片高度不是估的: 逐项对应源码里实际写的那几行 ——
  //    卡内边距 x2 + 时间行 + (行标签 + 三个大数字) + 清单 7 行 + 有异常时那一行(按最坏情况算)
  //  字号与间距全部来自 resultDecoFor(实测窗口) —— 与界面上 this.uiRes 用的是同一份计划;
  //  行高系数沿用 LayoutPlan 自己的那几个(1.3 / 1.2 / 1.25), 行标签另有显式 lineHeight(Q3 已钉住)。
  //  可用高度按**偏保守**的算法扣: 底栏 52vp(TAB_H 就是 52) + 顶栏 46vp + 上下系统栏 48vp。
  //  也就是说"这里算出来放得下" -> 真机上一定放得下。
  //  (Q3d 断言"六项项名齐全、顺序照结果页", CH_KEY_ROWS 就是那六行 —— 估算不会与源码各说各话。)
  const CH_TAB_H = 52, CH_HEADER_H = 46, CH_SAFE_H = 48, CH_KEY_ROWS = 6;
  function histKeyRowVp(res) {
    return Math.max(Math.round(res.keyName * 1.3), Math.round(res.keyNum * 1.2));
  }
  function histCardHeightVp(res, pad) {
    const gapHalf = Math.round(res.gap / 2);
    const nameLine = Math.round(res.keyName * 1.3);   // 时间行 / 清单行的项名
    const tagLine = Math.round(res.keyUnit * 1.25);   // 行标签(源码里就是这一个显式行高)
    const bigLine = Math.round(res.huge * 1.2);       // 三个大数字
    const noteLine = Math.round(res.keyUnit * 1.3);   // 有异常时那一行(最坏情况)
    return 2 * pad + nameLine + gapHalf + (tagLine + nameLine + bigLine) +
      gapHalf + CH_KEY_ROWS * histKeyRowVp(res) + gapHalf + noteLine;
  }
  const qWindows = [
    ['窄窗 460x672(用户报的)', 460, 672],
    ['真机 1:√2 665x940', 665, 940],
    ['手机 360x780', 360, 780],
    ['极窄 300x800', 300, 800],
    ['平板宽窗 1440x960(用户报的)', 1440, 960],
    ['极扁 2000x600', 2000, 600]
  ];
  console.log('  · 定稿后的历史卡片高度(三个大数字 + 6 行清单; 一屏必须看得见 2 张)');
  for (const pair of qWindows) {
    const tag = pair[0], w = pair[1], h = pair[2];
    const pl = M.resolvePlanPlain(inp(w, h, NONE, true, 'phone', 'bar'));
    const res = pl.result;
    const cardH = histCardHeightVp(res, pl.pad);
    const avail = h - CH_TAB_H - CH_HEADER_H - CH_SAFE_H;
    //  分栏(宽窗)时历史列表自己就是两列(每张 flexBasis 45%), 一行就已经是 2 张;
    //  单列(窄窗)时要两张上下叠, 那才是真的"一屏 2 张"。
    const need = pl.split ? (cardH + pl.gutter) : (cardH * 2 + pl.gutter);
    const ok = need <= avail;
    if (!ok) {
      bad.push(tag + ': 一屏要 ' + need + 'vp, 可用只有 ' + avail +
        'vp —— 一屏看不见 2 张(卡片要再压矮一点)');
    }
    console.log('    ' + (ok ? 'OK  ' : 'FAIL') + ' ' + tag.padEnd(26) +
      ((pl.split ? '分栏: 一行两张 ' : '单列: 两张上下 ') + need + 'vp').padEnd(30) +
      ('可用 ' + avail + 'vp').padEnd(14) +
      ('卡片 ' + cardH + 'vp · 大数字 ' + res.huge + 'fp · 清单 ' + CH_KEY_ROWS + ' 行 x ' +
        histKeyRowVp(res) + 'vp'));
  }
  //  窗口本身不到 600vp 高的极端方形窗(400x400): 不承诺 2 张, 但一张都不能少
  const sqPlan = M.resolvePlanPlain(inp(400, 400, NONE, true, 'phone', 'bar'));
  const sqH = histCardHeightVp(sqPlan.result, sqPlan.pad);
  const sqAvail = 400 - CH_TAB_H - CH_HEADER_H - CH_SAFE_H;
  if (sqH > sqAvail) {
    bad.push('小方窗 400x400 连一张历史卡都放不下(' + sqH + 'vp > ' + sqAvail + 'vp)');
  } else {
    console.log('    OK   小方窗 400x400(只承诺 1 张)     卡片 ' + sqH + 'vp / 可用 ' + sqAvail + 'vp');
  }
  console.log('');
  console.log('=== Q. 历史页源码静态检查 ===');
  console.log('  历史页源码检查: ' + (bad.length === 0
    ? ('干净 —— 底部导航等分; 历史页按需读盘且与账本同源; 每条 = 时间/设备 + 三个大数字' +
      '(与结果页同源、小字行标签) + 一行一项的清单(六行: 自研三个 / GPU-SNL / ' +
      'NPU 推理 / 存储 I/O, 顺序照结果页; 每行只有"白名 + 蓝数", 没有单位/条件/灰色小字); ' +
      '卡片里没有密集表/耗时/版本列(一行最多 ' + worst + ' 个数字); 备注只在有异常时出现; ' +
      '空态一句话; 无动画; 口径一句话说清能不能比; 一屏仍看得见 2 张; ' +
      '落盘的成绩表仍标出了 8.2 的口径变更点')
    : (bad.length + ' 处: ' + bad.join(' / '))));
  return bad.length;
}

// ---------------------------------------------------------------------------
//  P3. 源码静态检查(Index.ets): 开始页在"没有任何结果"时也必须排得像样
//  这一节回答的是"模型算出来的值有没有真的被用到界面上":
//   ① uiRes 的初值必须与其余布局状态**同源**(DeviceProfile 里那份唯一的计划)。
//      写回与窗口无关的常量(例如 resultDecoFor(0, 0))就会**变红** —— 那正是"某个宽高被压到极小"
//      最常见的来源: 结果页密度按兜底窗口算, 而内容列宽 / 按钮直径 / 分栏按实测窗口算。
//   ② syncProfile() 必须把计划里的 result 交给 uiRes(否则实测尺寸到达后开始页仍是旧值)。
//   ③ 首页那三个小数字块(单核 / 多核 / GPU)的数字必须 maxLines(1) + minFontSize:
//      缺了就会"一位数字一行"(用户报的第 3 条)。
//   ④ 圆环容器(Stack)不许小于外圈直径 —— 否则圆环被自己的容器切掉、圆又压到右边的说明文字上
//      (用户报的第 1、2 条)。
//   ⑤ 结果区必须有"空态": 宽窗口首页右栏占一多半屏宽, 没有结果时不能留一大片空(用户报的第 4 条)。
//  (与 M 节同样的做法: 只看代码, 不看注释 —— 注释里就写着这些结论, 连注释一起搜会被自己绊倒。)
// ---------------------------------------------------------------------------
function startPageSourceCheck(pageText) {
  const pageCode = pageText.split(String.fromCharCode(10))
    .filter((l) => !l.trim().startsWith('//')).join(String.fromCharCode(10));
  const bad = [];
  // ① uiRes 的初值
  const decl = pageCode.indexOf('@State uiRes');
  if (decl < 0) {
    bad.push('找不到 @State uiRes 的声明(静态检查失效, 必须修用例本身)');
  } else {
    const lineEnd = pageCode.indexOf(String.fromCharCode(10), decl);
    const declLine = pageCode.slice(decl, lineEnd < 0 ? undefined : lineEnd);
    if (/resultDecoFor\s*\(\s*0\s*,\s*0\s*\)/.test(declLine)) {
      bad.push('uiRes 的初值又写回 resultDecoFor(0, 0) —— 它与实测窗口无关, 首帧会与其余布局状态自相矛盾');
    }
    if (declLine.indexOf('DeviceProfile') < 0) {
      bad.push('uiRes 的初值不是从 DeviceProfile 的计划里取的(与其余布局状态不同源)');
    }
  }
  // ② 实测窗口到达后必须整份换掉
  if (pageCode.indexOf('this.uiRes = p.result;') < 0) {
    bad.push('syncProfile() 里没有 this.uiRes = p.result —— 实测窗口到达后开始页仍会用旧密度');
  }
  // ③ 小数字块绝不许"一位数字一行"
  const mini = pageCode.indexOf('miniScore(label: string, value: number, color: string) {');
  if (mini < 0) {
    bad.push('找不到 miniScore 的定义');
  } else {
    const miniEnd = pageCode.indexOf(String.fromCharCode(10) + '  @Builder', mini);
    const body = pageCode.slice(mini, miniEnd < 0 ? undefined : miniEnd);
    if (body.indexOf('.maxLines(1)') < 0) {
      bad.push('首页三个小数字块的数字没有 maxLines(1) —— 卡一窄就会一位数字一行');
    }
    if (body.indexOf('.minFontSize(') < 0) {
      bad.push('首页三个小数字块的数字没有 minFontSize —— 放不下时既不缩字号也不截断');
    }
  }
  // ④ 圆环容器 >= 外圈直径
  const hero = pageCode.indexOf('startCircleInner() {');
  if (hero < 0) {
    bad.push('找不到 startCircleInner 的定义');
  } else {
    const heroEnd = pageCode.indexOf(String.fromCharCode(10) + '  @Builder', hero);
    const body = pageCode.slice(hero, heroEnd < 0 ? undefined : heroEnd);
    //  外圈画笔必须**由 startDia 派生**(允许减去描边宽: 描边骑在路径上, 减掉它圆环才完全落在容器里)
    const circleUses = (body.match(/\.width\(this\.startDia/g) || []).length >= 1 &&
      (body.match(/\.height\(this\.startDia/g) || []).length >= 1;
    //  容器可以有**两种合法写法**: 直接写 this.startDia(与外圈同值), 或写 this.startRingDia()
    //  —— 后者的正确性由下面那条"startRingDia() 里不许有减法"的断言保证。
    const viaDirect = (body.match(/\.width\(this\.startDia\)/g) || []).length >= 2 &&
      (body.match(/\.height\(this\.startDia\)/g) || []).length >= 2;
    const viaHelper = body.indexOf('.width(this.startRingDia())') >= 0 &&
      body.indexOf('.height(this.startRingDia())') >= 0;
    if (!circleUses || (!viaDirect && !viaHelper)) {
      bad.push('圆环容器(Stack)与外圈没有共用同一个直径 —— 容器小于内容会把圆环切掉');
    }
  }
  const ring = pageCode.indexOf('private startRingDia(): number {');
  if (ring >= 0) {
    const ringEnd = pageCode.indexOf('}', ring);
    const body = pageCode.slice(ring, ringEnd < 0 ? undefined : ringEnd);
    if (body.indexOf('this.startDia') < 0 || /-\s*[0-9]/.test(body)) {
      bad.push('startRingDia() 里又出现了比 startDia 小的写法(容器会小于圆环)');
    }
  }
  // ⑤ 结果区空态
  if (pageCode.indexOf('resultZoneEmptyCard') < 0) {
    bad.push('结果区没有空态卡 —— 宽窗口首页右栏在"还没有结果"时会留一大片空');
  }
  console.log('  开始页(无结果态)源码检查: ' + (bad.length === 0
    ? '干净 —— uiRes 与计划同源; 小数字块有 maxLines(1)+minFontSize; 圆环容器 >= 外圈; 结果区有空态'
    : (bad.length + ' 处: ' + bad.join(' / '))));
  return bad.length;
}
// ---------------------------------------------------------------------------
//  FX. 美化层(沉浸光感 / 粒子消散)的源码静态检查(Index.ets)
//
//  这一节把"跑分期间美化必须根本不存在"这句话变成**机械可核对**的事实。
//  用户的原话是"跑分的时候就不要用了, 影响性能"; 而"不播放动画"是不够的 ——
//  只要 Canvas 节点还挂在树上、只要还有一个 setInterval 在跑, 代价就已经付掉了。
//  所以这里钉住的不是"动画会不会播", 而是下面五条:
//    ① 全工程只有一个判据函数 fxOn(), 而且它的函数体里必须读既有的跑分标志位
//       (anyBenchRunning) 与运行时能力探测(FX_CAPABLE);
//    ② 界面上不许出现"裸的 this.fxOn"(字段式访问) —— 判据只许是函数;
//    ③ 全工程只有一处 setInterval(粒子 tick), 它落在 fxStart() 里,
//       而 fxStart() 的第一道门就是 fxOn();
//    ④ 全工程只有一处 Canvas(粒子层), 它落在 fxParticleLayer() 里,
//       而且唯一的调用点上写着 if (this.fxOn() && this.fxLive) ——
//       跑分期间这个节点根本不会被创建;
//    ⑤ 所有阴影 / 渐变 / 径向辉光都必须走 fxAura() / fxSheen() / fxHalo(),
//       而 fxHalo() 的每一个调用点都被 if (this.fxOn()) 包着(节点根本不存在),
//       fxAura() / fxSheen() 在关掉时返回的是"没有效果"的规格(半径 0 / 两端同色)。
//  另外: 粒子的四个时刻(按下消散 / 收工聚拢 / 切标签消散 / 结果出现聚拢)必须都在,
//        而且粒子的唯一入口 fxFire() 的第一句就是 fxOn() 的早退。
//  (与 M / P3 / Q 同样的做法: 只看代码, 不看注释。)
// ---------------------------------------------------------------------------
function decorativeFxSourceCheck() {
  const bad = [];
  let raw = '';
  try {
    raw = readFileSync(join(here, '..', 'entry', 'src', 'main', 'ets', 'pages', 'Index.ets'), 'utf8');
  } catch (e) {
    bad.push('读不到 pages/Index.ets(静态检查失效, 必须修用例本身)');
    return bad.length;
  }
  const code = raw.split(String.fromCharCode(10))
    .filter((l) => !l.trim().startsWith('//')).join(String.fromCharCode(10));
  // 把单引号字符串的**内容**换成空(引号保留, 换行也算字符串结束 —— 于是引号数不平衡
  //  也绝不会跨行泄漏)。为什么要这一步: 这一节要数"代码里调了几次 hdsEffect",
  //  而「关于」页的说明文字里也写着 hdsEffect 这几个字 —— 不剥字符串就是假阳性。
  const stripStrings = (src) => {
    let out = '';
    let inStr = false;
    for (let i = 0; i < src.length; i++) {
      const ch = src[i];
      if (!inStr) {
        out += ch;
        if (ch === "'") { inStr = true; }
        continue;
      }
      if (ch === '\\') { i++; continue; }
      if (ch === "'") { out += ch; inStr = false; continue; }
      if (ch === String.fromCharCode(10)) { out += ch; inStr = false; }
    }
    return out;
  };
  const code2 = stripStrings(code);
  const count = (s) => (code.split(s).length - 1);
  const count2 = (s) => (code2.split(s).length - 1);
  const bodyIn = (src, marker) => {
    const at = src.indexOf(marker);
    if (at < 0) {
      return null;
    }
    const open = src.indexOf('{', at);
    let depth = 0;
    for (let i = open; i < src.length; i++) {
      const ch = src[i];
      if (ch === '{') { depth++; continue; }
      if (ch === '}') {
        depth--;
        if (depth === 0) { return src.slice(open, i + 1); }
      }
    }
    return null;
  };
  const bodyOfFn = (marker) => bodyIn(code, marker);
  const bodyOfFn2 = (marker) => bodyIn(code2, marker);
  // ① 唯一判据
  const nFxOn = count('private fxOn(): boolean {');
  if (nFxOn !== 1) {
    bad.push('美化判据 fxOn() 应当正好 1 处, 实际 ' + nFxOn + ' 处 —— 判据必须唯一');
  }
  const fxOnBody = bodyOfFn('private fxOn(): boolean {');
  if (fxOnBody === null) {
    bad.push('找不到 fxOn() 的函数体(静态检查失效, 必须修用例本身)');
  } else {
    if (fxOnBody.indexOf('this.anyBenchRunning()') < 0) {
      bad.push('fxOn() 没有读既有的跑分标志位(anyBenchRunning) —— 跑分期间它可能不为 false');
    }
    if (fxOnBody.indexOf('FX_CAPABLE') < 0) {
      bad.push('fxOn() 没有过运行时能力探测(FX_CAPABLE) —— 能力不可用时效果会照挂上去');
    }
  }
  // ② 不许有"裸的 this.fxOn"
  const bare = (code.match(/this\.fxOn(?![(\w])/g) || []).length;
  if (bare > 0) {
    bad.push('界面上出现了 ' + bare + ' 处裸的 this.fxOn —— 判据只许是函数 fxOn()');
  }
  // ③ 只有一处 setInterval, 且在 fxStart() 里, 且第一道门是 fxOn()
  const nTimer = count('setInterval(');
  if (nTimer !== 1) {
    bad.push('setInterval( 应当正好 1 处(粒子 tick), 实际 ' + nTimer + ' 处');
  }
  const startBody = bodyOfFn('private fxStart(): void {');
  if (startBody === null) {
    bad.push('找不到 fxStart() —— 粒子 tick 的注册点不见了');
  } else {
    if (startBody.indexOf('setInterval(') < 0) {
      bad.push('粒子 tick 没有注册在 fxStart() 里');
    }
    if (startBody.indexOf('this.fxOn()') < 0) {
      bad.push('fxStart() 没有先问 fxOn() —— 跑分期间可能照样注册 tick');
    }
  }
  // ④ 只有一处 Canvas, 且在 fxParticleLayer() 里, 且被 fxOn() + fxLive 双重包住
  const nCanvas = count('Canvas(');
  if (nCanvas !== 1) {
    bad.push('Canvas( 应当正好 1 处(粒子层), 实际 ' + nCanvas + ' 处');
  }
  if (code.indexOf('if (this.fxOn() && this.fxLive) {') < 0) {
    bad.push('粒子 Canvas 的调用点没有写 if (this.fxOn() && this.fxLive) —— 跑分期间节点会留在树上');
  }
  const layerBody = bodyOfFn('fxParticleLayer() {');
  if (layerBody === null || layerBody.indexOf('Canvas(') < 0) {
    bad.push('Canvas 不在 fxParticleLayer() 里 —— 粒子节点可能被别的路径创建');
  }
  // ⑤ 阴影 / 渐变 / 辉光全部走三个受控入口
  const nShadow = count('.shadow(');
  const nShadowOk = count('.shadow(this.fxAura(');
  if (nShadow !== nShadowOk) {
    bad.push('有 ' + (nShadow - nShadowOk) + ' 处 .shadow() 没走 fxAura() —— 关掉美化时它们不会退化');
  }
  //  ★ 2026-10 起 .linearGradient() 有**两种合法写法**, 期望值跟着改(断言没删, 只是多认一类):
  //    (a) .linearGradient(this.fxSheen(...))    —— 美化层的光带, 由 fxOn() 管;
  //    (b) .linearGradient(this.appBackdropGrad(...)) —— **App 画布本身**(2026-10 背景重做:
  //        自己的深色底 + 暗红->深紫的静态渐变)。它不是装饰效果, 是页面底色,
  //        所以它**故意不受 fxOn() 管**; 代价上也说得通 —— 一次静态填充, 与它替换掉的
  //        纯色同档(无离屏层、无模糊、不重绘)。下面第 ⑧ 条会单独钉住这两件事。
  //    除了这两类, 任何第三种 .linearGradient() 仍然算失败。
  const nLin = count('.linearGradient(');
  //  注意画布这一头要按**前缀**数: 不透明版是 appBackdropGrad(), 磨砂浮层版是 appBackdropGradSoft()
  const nLinOk = count('.linearGradient(this.fxSheen(') +
    count('.linearGradient(this.appBackdropGrad');
  if (nLin !== nLinOk) {
    bad.push('有 ' + (nLin - nLinOk) + ' 处 .linearGradient() 既没走 fxSheen() 也不是画布渐变');
  }
  const haloBody = bodyOfFn('fxHalo(color: string, dia: number, alpha: number) {');
  const nRadAll = count('radialGradient(');
  const nRadHalo = haloBody === null ? -1 : (haloBody.split('radialGradient(').length - 1);
  if (haloBody === null || nRadHalo !== 1 || nRadAll !== 1) {
    bad.push('径向辉光应当只在 fxHalo() 里画一次(全文件 ' + nRadAll + ' 处 / fxHalo 内 ' +
      nRadHalo + ' 处) —— 辉光可能绕过了唯一入口');
  }
  let haloCalls = 0;
  let at = code.indexOf('this.fxHalo(');
  while (at >= 0) {
    haloCalls++;
    const guard = code.slice(Math.max(0, at - 160), at);
    if (guard.indexOf('if (this.fxOn())') < 0) {
      bad.push('第 ' + haloCalls + ' 处 this.fxHalo() 没有被 if (this.fxOn()) 包住 —— ' +
        '跑分期间辉光节点会留在树上');
    }
    at = code.indexOf('this.fxHalo(', at + 1);
  }
  if (haloCalls < 1) {
    bad.push('一处 this.fxHalo() 调用都没有 —— 光感节点根本没挂上去');
  }
  // ⑥ 粒子四个时刻 + 唯一入口的早退
  const moments = ['private fxHeroStart(): void {', 'private fxFinishBurst(): void {',
    'private fxTabBurst(idx: number): void {', 'private fxResultBurst(): void {'];
  for (const m of moments) {
    if (code.indexOf(m) < 0) {
      bad.push('找不到 ' + m + ' —— 四个粒子时刻少了一个');
    }
  }
  const fireBody = bodyOfFn('private fxFire(cx: number, cy: number, n: number, mode: number, ' +
    'spread: number, color: string): void {');
  if (fireBody === null) {
    bad.push('找不到 fxFire() —— 粒子的唯一入口不见了');
  } else if (fireBody.indexOf('if (!this.fxOn()) {') < 0) {
    bad.push('fxFire() 的第一道门不是 fxOn() —— 跑分期间粒子数组会被建出来');
  }
  // ⑦ ★新★ 真正的沉浸光感(HDS 点光源 @kit.UIDesignKit 的 hdsEffect.pointLight):
  //    跑分期间不是"不播放", 而是**连效果都不许被构造** —— 所以钉三件事:
  //    (a) 全工程调用 hdsEffect 的地方只有 fxLight() 一处, 而 fxLight() 的第一句就是
  //        "fxOn() 或 HDS_CAPABLE 不过 -> 直接返回空效果(早退)";
  //    (b) 每个 .visualEffect() 都必须是 .visualEffect(this.fxLight(...)) —— 不许绕开这道门;
  //    (c) 早退时给的那只"空效果"必须是官方 uiEffect.createEffect(), **不许**用 hdsEffect 现造
  //        (用 hdsEffect 造就等于"跑分期间还在调 hdsEffect", 这条正是本轮要防的事)。
  //    另外 HDS_CAPABLE 必须真的由 canIUse() 探出来 —— 门槛抬到 API 20 也不代表每台机器都有。
  //  这两个数都在"剥掉字符串"的代码上数(见 stripStrings 的说明)
  const fxLightBody = bodyOfFn('private fxLight(color: string, intensity: number, ' +
    'bloom: number): VisualEffect {');
  const fxLightBody2 = bodyOfFn2('private fxLight(color: string, intensity: number, ' +
    'bloom: number): VisualEffect {');
  const nHdsAll = count2('hdsEffect.');
  const nHdsInLight = fxLightBody2 === null ? -1 : (fxLightBody2.split('hdsEffect.').length - 1);
  if (fxLightBody === null) {
    bad.push('找不到 fxLight() —— 点光源的唯一构造入口不见了');
  } else {
    if (nHdsAll !== nHdsInLight) {
      bad.push('有 ' + (nHdsAll - nHdsInLight) + ' 处 hdsEffect 调用跑到 fxLight() 外面去了' +
        ' —— 那些地方绕过了 fxOn() 的门');
    }
    if (fxLightBody.indexOf('if (!this.fxOn()) {') < 0) {
      bad.push('fxLight() 的第一道门不是 fxOn() —— 跑分期间点光源会被构造出来');
    }
    //  第二道门必须是**原地写的** if (canIUse(...)): 除了"拿不到能力就静默降级"这件事本身,
    //  它还是本轮唯一能压住那 10 条 syscap 告警的写法(编译器的 syscap 检查只认这种形态;
    //  官方的 @SuppressWarnings 装饰器在本 SDK 上编译不过 —— 试过, BUILD FAILED)。
    //  把它挪走 / 改成 HDS_CAPABLE 之类的间接判断, 告警会立刻从 53 弹回 63。
    if (fxLightBody.indexOf("if (canIUse('SystemCapability.UIDesign.HDSComponent.Core')) {") < 0) {
      bad.push('fxLight() 里没有原地写的 if (canIUse(...)) —— 一是拿不到能力时不会静默降级,' +
        ' 二是那 10 条 syscap 告警会全部回来');
    }
    if (fxLightBody.indexOf('return fxNoLight();') < 0) {
      bad.push('fxLight() 早退时没有返回空效果 —— 关掉美化时会挂上真点光');
    }
  }
  const nVE = count('.visualEffect(');
  const nVEOk = count('.visualEffect(this.fxLight(');
  if (nVE !== nVEOk) {
    bad.push('有 ' + (nVE - nVEOk) + ' 处 .visualEffect() 没走 this.fxLight()' +
      ' —— 点光可能绕过唯一判据直接挂上去');
  }
  if (nVEOk < 1) {
    bad.push('一处 .visualEffect(this.fxLight()) 都没有 —— 点光根本没挂上去');
  }
  if (code.indexOf("canIUse('SystemCapability.UIDesign.HDSComponent.Core')") < 0) {
    bad.push('没有用 canIUse() 探 HDS 点光能力 —— 门槛抬到 API 20 也不等于每台机器都给得出这个能力');
  }
  // ⑨ ★新★ 可读性硬标准(2026-10-10 用户真机反馈:「你的光不是有质感而是很糊, 影响阅读」)。
  //    用户说的不是"不好看", 是**影响阅读** —— 所以这是可读性缺陷, 不是审美。三条:
  //      (a) 点光源只许点亮**边框**(illuminatedType = BORDER); BORDER_CONTENT 会把光打进
  //          内容里, 卡片里的长段说明文字第一个糊 —— 这一条不许再翻回去;
  //      (b) 有文字的元件上 bloom <= 0.30(光晕散得越开越糊);
  //      (c) 标签项(tabBarItem)里**不许**出现 .shadow() 或 .visualEffect():
  //          前者是贴着一行字画辉光(就是"字糊"), 后者会把每一格的边框点亮 ——
  //          真机上被用户看成"底部导航被白色的框框起来, 非常丑"。
  if (code.indexOf('PointLightIlluminatedType.BORDER_CONTENT') >= 0) {
    bad.push('点光源用回了 BORDER_CONTENT —— 光会打进内容里, 卡片里的长段说明文字会被糊住');
  }
  let fxBloomBad = 0;
  let fxBloomMax = 0;
  const bloomRe = /this\.fxLight\([^)]*?,\s*[0-9.]+\s*,\s*([0-9.]+)\s*\)/g;
  let bm = bloomRe.exec(code);
  while (bm !== null) {
    const v = Number(bm[1]);
    if (v > fxBloomMax) { fxBloomMax = v; }
    if (v > 0.30) { fxBloomBad++; }
    bm = bloomRe.exec(code);
  }
  if (fxBloomBad > 0) {
    bad.push('有 ' + fxBloomBad + ' 处点光源的 bloom > 0.30(最大 ' + fxBloomMax +
      ') —— 光散得太开就是"糊", 有文字的元件上必须 <= 0.30');
  }
  const barBody = bodyOfFn('tabBarItem(idx: number, label: string) {');
  if (barBody === null) {
    bad.push('找不到 tabBarItem()(静态检查失效, 必须修用例本身)');
  } else {
    if (barBody.indexOf('.shadow(') >= 0) {
      bad.push('标签项上又挂了 .shadow() —— 辉光贴着一行字画, 字就糊了(用户报过)');
    }
    if (barBody.indexOf('.visualEffect(') >= 0) {
      bad.push('标签项上又挂了点光源 —— 每一格的边框被点亮, 看起来就是"底部导航被白色的框框起来"(用户报过)');
    }
  }
  const noLightBody = bodyOfFn('function fxNoLight(): VisualEffect {');
  if (noLightBody === null) {
    bad.push('找不到 fxNoLight() —— 关掉美化时那个"没有效果"的取值没了');
  } else {
    if (noLightBody.indexOf('uiEffect.createEffect()') < 0) {
      bad.push('fxNoLight() 不是用官方 uiEffect.createEffect() 造的 —— 关掉美化时可能仍在调 hdsEffect');
    }
    if (noLightBody.indexOf('hdsEffect') >= 0) {
      bad.push('fxNoLight() 里出现了 hdsEffect —— 关掉美化(含跑分期间)时仍然会构造点光');
    }
  }
  // ⑧ ★新★ App 画布(2026-10 背景重做): 「不跟随系统浅色主题 + 每个整屏容器都用同一张底」。
  //    背景重做的两条, 一条都不能只落在首页:
  //      (a) 不跟系统的**那一条决定**写在 EntryAbility.onCreate 的
  //          applicationContext.setColorMode(ColorMode.COLOR_MODE_DARK) —— 少了它,
  //          系统是浅色时本 App 会被当浅色应用处理(组件默认前景/背景全翻浅色),
  //          整套深色设计连同点光/光晕一起废掉;
  //      (b) 画布渐变本身: 令牌在 common/Theme.ets 的 brandGradient(), 出口在 Index.ets 的
  //          appBackdropGrad() / appBackdropGradSoft(); **首页 + 5 个浮层 + 根容器**
  //          每一个整屏容器都必须挂上它(只改首页就是"首页有底、翻页露白")。
  //    另外这两条出口里不许出现 fxOn(): 它们是页面底色, 不是美化层的装饰效果。
  let entrySrc = '';
  try {
    entrySrc = readFileSync(join(here, '..', 'entry', 'src', 'main', 'ets', 'entryability',
      'EntryAbility.ets'), 'utf8');
  } catch (e) {
    bad.push('读不到 entryability/EntryAbility.ets(静态检查失效, 必须修用例本身)');
  }
  if (entrySrc.length > 0 &&
    entrySrc.indexOf('setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_DARK)') < 0) {
    bad.push('EntryAbility 没有把颜色模式钉在深色(setColorMode(ColorMode.COLOR_MODE_DARK))' +
      ' —— 系统处于浅色时整页会被当成浅色应用处理, 深色设计与点光/光晕一起废掉');
  }
  let themeSrc = '';
  try {
    themeSrc = readFileSync(join(here, '..', 'entry', 'src', 'main', 'ets', 'common',
      'Theme.ets'), 'utf8');
  } catch (e) {
    bad.push('读不到 common/Theme.ets(静态检查失效, 必须修用例本身)');
  }
  if (themeSrc.length > 0) {
    for (const tok of ['brandRed', 'brandViolet', 'brandDeep', 'export function brandGradient(']) {
      if (themeSrc.indexOf(tok) < 0) {
        bad.push('Theme.ets 少了画布令牌 ' + tok + ' —— 品牌底色没有单一来源');
      }
    }
  }
  const bgBody = bodyOfFn('private appBackdropGrad(): LinearGradient {');
  const bgSoftBody = bodyOfFn('private appBackdropGradSoft(): LinearGradient {');
  if (bgBody === null || bgSoftBody === null) {
    bad.push('找不到 appBackdropGrad() / appBackdropGradSoft() —— 画布渐变的出口不见了');
  } else {
    if (bgBody.indexOf('brandGradient()') < 0 || bgSoftBody.indexOf('brandGradient(') < 0) {
      bad.push('画布出口没有走 Theme.brandGradient() —— 底色会变成第二个来源');
    }
    if (bgBody.indexOf('fxOn') >= 0 || bgSoftBody.indexOf('fxOn') >= 0) {
      bad.push('画布出口里出现了 fxOn() —— 它是页面底色, 不是美化层的装饰效果, 不该被跑分关掉');
    }
  }
  //  每个整屏容器都得挂上: 根 Stack + 5 个浮层(历史报告 / 对比 / 一键全跑 / 跑分进度 / 结果)
  const wholeScreens = [
    ['build() {', '根容器(首页 / 历史 / 关于 三个标签页共用这一张底)'],
    ['historyReportOverlay() {', '历史报告浮层'],
    ['cmpOverlay() {', '两份报告对比浮层'],
    ['fullRunOverlay() {', '一键全部跑分浮层'],
    ['runningOverlay() {', '单次跑分进度浮层'],
    ['fullReportOverlay() {', '一键全跑结果浮层'],
    ['resultOverlay() {', '单次跑分结果浮层']
  ];
  let bgScreens = 0;
  for (const pair of wholeScreens) {
    const body = bodyOfFn(pair[0]);
    if (body === null) {
      bad.push('找不到 ' + pair[0] + '(静态检查失效, 必须修用例本身)');
      continue;
    }
    if (body.indexOf('.linearGradient(this.appBackdropGrad') < 0) {
      bad.push(pair[1] + '没有挂上画布渐变 —— 翻到这一屏会露出另一块底色');
    } else {
      bgScreens++;
    }
  }
  const hdsOnBody = bodyOfFn('private hdsLightOn(): boolean {');
  if (hdsOnBody === null) {
    bad.push('找不到 hdsLightOn() —— "两层里让谁上场"没有唯一出口');
  } else if (hdsOnBody.indexOf('this.fxOn()') < 0 || hdsOnBody.indexOf('HDS_CAPABLE') < 0) {
    bad.push('hdsLightOn() 没有同时读 fxOn() 与 HDS_CAPABLE');
  }
  console.log('  美化层(光感 / 粒子)源码检查: ' + (bad.length === 0
    ? '干净 —— 判据唯一(fxOn 读 anyBenchRunning + FX_CAPABLE, 界面无裸 this.fxOn); ' +
      'setInterval 1 处且在 fxStart() 里; Canvas 1 处且在 fxParticleLayer() 里、被 if (fxOn() && fxLive) 包住; ' +
      'shadow / linearGradient / radialGradient 全部走 fxAura() / fxSheen() / fxHalo()(辉光节点被 if (fxOn()) 包着); ' +
      'App 画布(EntryAbility 钉 COLOR_MODE_DARK + Theme.brandGradient)挂在根容器与 5 个浮层上、共 ' + bgScreens + ' 屏, ' +
      '且画布出口里没有 fxOn(); ' +
      '粒子四个时刻齐全, fxFire() 第一句就是 fxOn(); ' +
      'HDS 点光只在 fxLight() 里构造、第一句就是 fxOn() 早退返回空效果、' +
      '再原地用 if (canIUse(HDS syscap)) 包住真正的构造, ' +
      '每个 .visualEffect() 都走 this.fxLight(), 空效果由 uiEffect.createEffect() 造'
    : (bad.length + ' 处: ' + bad.join(' / '))));
  return bad.length;
}

// ---------------------------------------------------------------------------
//  R. 运行时能力可用性静态检查(build-profile.json5 的 runtimeOS 门禁 + 全 ets 源码)
//
//  为什么要有这一节(2026-10-05 真机缺陷回归):
//    首页「开始跑分」大圆在真机上渲染成一个纯黑矩形(直角), 圆与蓝色圆环完全不可见,
//    而它下面的说明文字『CPU 单核 + 多核 · 7 项负载 + GPU 4 场景』完全正常。
//    根因是挂在圆环 Stack 上的 .lightUpEffect(this.heroLift()):
//      · 官方文档(zh-cn/.../arkui-ts/ts-universal-attributes-image-effect.md, lightUpEffect):
//        "如果 value 等于 0 则图像为全黑, 如果 value 等于 1 则图像为全亮效果"。
//        Rosen 的实现(rosen/modules/render_service_base/src/render/rs_light_up_effect_filter.cpp)
//        是一张 4x5 颜色矩阵, 偏移项 = value - 1.0 **加到 R/G/B 上**;
//        RSProperties 里默认值就是 1.0f(不设 = 原样), IsLightUpEffectValid() = 0 <= v < 1。
//      · 旧代码传 0.18(平时)/ 0(跑分期间) —— 两个值都落在 [0,1) 里, 都会把按钮压成黑/近黑,
//        而且都会点亮一次离屏滤镜(needFilter_ / needHwcFilter_ 被置真)。
//        「0 = 不改变图像」这个写在注释里的理解是**反的**。
//      · 真机实测(HUAWEI Pura X Max / HOP-AL00 / OpenHarmony-7.0.0.105): 按钮位置是
//        638x650px 的纯 #000000 矩形, 连内切圆之外的四个角也是纯黑 —— 不只是"变暗"。
//    按钮本来就不需要滤镜: 它是实心圆 + 一圈描边。所以把它钉成永久断言:
//    只要 build-profile.json5 的 runtimeOS 还是 OpenHarmony, entry/src/main/ets 下
//    就不许再出现下面四个 token, 谁加回来这一节就变红。
//    (与 M / P3 节同样的做法: 只看代码, 不看注释 —— 注释里就写着这些结论, 连注释一起搜会被自己绊倒。)
// ---------------------------------------------------------------------------
const BANNED_CAPS = ['lightUpEffect', 'hdsEffect', '@kit.UIDesignKit', 'pointLight'];

// 去掉整行注释与行尾注释; 保护字符串字面量里的 "://"(例如 'https://...')
function stripLineComments(text) {
  const nl = String.fromCharCode(10);
  const out = [];
  for (const line of text.split(nl)) {
    if (line.trim().startsWith('//')) { out.push(''); continue; }
    let cut = line.length;
    for (let i = 0; i + 1 < line.length; i++) {
      if (line[i] === '/' && line[i + 1] === '/' && !(i > 0 && line[i - 1] === ':')) { cut = i; break; }
    }
    out.push(line.slice(0, cut));
  }
  return out.join(nl);
}

// 递归收集 .ets / .ts(不用 glob: 本用例只依赖 node:fs, 不引任何第三方)
function etsFilesUnder(root) {
  const acc = [];
  const walk = (d) => {
    let ents = [];
    try { ents = readdirSync(d, { withFileTypes: true }); } catch (e) { return; }
    for (const e of ents) {
      const p = join(d, e.name);
      if (e.isDirectory()) { walk(p); }
      else if (/\.(ets|ts)$/.test(e.name)) { acc.push(p); }
    }
  };
  walk(root);
  return acc;
}

function runtimeCapabilitySourceCheck(projectRoot) {
  const bad = [];
  let bp = '';
  try { bp = readFileSync(join(projectRoot, 'build-profile.json5'), 'utf8'); }
  catch (e) { bad.push('读不到 build-profile.json5(静态检查失效, 必须修用例本身)'); }
  const m = /"runtimeOS"\s*:\s*"([A-Za-z]+)"/.exec(bp);
  if (bp.length > 0 && m === null) {
    bad.push('build-profile.json5 里找不到 runtimeOS(静态检查失效, 必须修用例本身)');
  }
  const runtimeOS = m === null ? '' : m[1];
  if (runtimeOS !== 'OpenHarmony') {
    // 门禁只在 OpenHarmony 运行时下成立。换到别的 runtimeOS 时这些能力到底可不可用
    // 是另一件事(要重新查文档), 所以这里**不静默放过**, 而是明确打出来让人复核。
    console.log('  R. 运行时能力静态检查: runtimeOS = ' + (runtimeOS === '' ? '(读不到)' : runtimeOS) +
      ' —— OpenHarmony 门禁不适用, 未扫描; 这几个能力在该运行时下是否可用需另行确认');
    return bad.length;
  }
  const etsRoot = join(projectRoot, 'entry', 'src', 'main', 'ets');
  const files = etsFilesUnder(etsRoot);
  if (files.length === 0) {
    bad.push('扫不到 entry/src/main/ets 下的 .ets 源码(静态检查失效, 必须修用例本身)');
  }
  let hits = 0;
  for (const f of files) {
    let text = '';
    try { text = readFileSync(f, 'utf8'); } catch (e) { continue; }
    const lines = stripLineComments(text).split(String.fromCharCode(10));
    for (let i = 0; i < lines.length; i++) {
      for (const tok of BANNED_CAPS) {
        if (lines[i].indexOf(tok) >= 0) {
          hits++;
          bad.push(f.slice(projectRoot.length + 1) + ':' + (i + 1) + ' 又出现了 ' + tok +
            '(runtimeOS = OpenHarmony, 该能力不可用/会把节点画成黑块)');
        }
      }
    }
  }
  console.log('  R. 运行时能力静态检查: runtimeOS = ' + runtimeOS + ' · 扫描 ' + files.length +
    ' 个 .ets/.ts · 禁用 token ' + BANNED_CAPS.join(' / ') + ' · 命中 ' + hits);
  console.log(hits === 0
    ? '    干净 —— 没有 lightUpEffect / hdsEffect / @kit.UIDesignKit / pointLight'
    : ('    ' + hits + ' 处命中: ' + bad.join(' / ')));
  return bad.length;
}

// ---------------------------------------------------------------------------
//  Z. 汇总 —— 各节失败计数汇总成一句, 并给进程退出码(0 = 全绿)。
//   ★ 它写在 K 的 .then 里: K 要 await 一个动态 import, 只有等它跑完, kChecked/kSweep/kBad
//     才是最终值 —— 否则汇总行会打印 0(这也是这条断言以前"看起来通过"的原因)。
// ---------------------------------------------------------------------------
kPromise.then(() => {
  if (!kStripWorked) {
    kBad++;
  }
  console.log('  逐字段比对: 用例 ' + kChecked + ' 条 + 扫描 ' + kSweep + ' 点 = ' +
    (kChecked + kSweep) + ' 次');
  console.log('  剥离是否真的生效: ' + (kStripWorked
    ? '是（剥离版形态名为空串、编号为 0 —— 不是空转）' : '否（剥离没生效, 这条断言不算数）'));
  console.log(kBad === 0
    ? '  结论: 布局结果与形态名/编号**完全无关** —— 名字全删掉, 每一个布局参数逐位相同'
    : ('  有 ' + kBad + ' 处不一致'));

  const srcText = readFileSync(join(here, '..', 'entry', 'src', 'main', 'ets', 'common',
    'LayoutPlan.ets'), 'utf8');
  const sourceBad = printSourceCheck(srcText);
  const capBad = runtimeCapabilitySourceCheck(join(here, '..'));
  // ---- FX: 美化层(光感 / 粒子)的源码静态检查(见 FX 节) ----
  const fxBad = decorativeFxSourceCheck();
  const totalBad = bad + jBad + gBad + sameBad + hBad + overflow + lBad + kBad + sourceBad + nBad + pBad +
    histPageBad + capBad + fxBad + (oA + oB + badCol + badShape);
  console.log('\n=== Z. 汇总 ===');
  console.log('  用例数: 判定表 ' + cases.length + ' 条 · 比例专项 ' + (ratioCases.length + checks.length) +
    ' 条 · 边界 ' + boundary.length + ' 条 · 任意比例 ' + ratioList.length +
    ' 条 · 全尺寸扫描 ' + n + ' 个采样点 · 名字剥离比对 ' + (kChecked + kSweep) + ' 次');
  console.log('  维度: 判定表 ' + bad + ' · 比例 ' + jBad + ' · 边界 ' + gBad + ' · 同窗口多状态 ' + sameBad +
    ' · 竖向预算 ' + hBad + ' · 溢出 ' + overflow + ' · 任意比例 ' + lBad + ' · 名字剥离 ' + kBad +
    ' · 扫描 ' + (oA + oB + badCol + badShape) + ' · 源码静态检查 ' + sourceBad +
    ' · 结果页首屏分块/大数字卡不变量 ' + nBad + ' · 开始页(无结果态)正数宽高 ' + pBad +
    ' · 历史页源码静态检查 ' + histPageBad +
    ' · 运行时能力静态检查(OpenHarmony 禁用 token) ' + capBad +
    ' · 美化层(光感 / 粒子)源码静态检查 ' + fxBad);
  if (totalBad === 0) {
    console.log('  全部通过: 判定只看实测窗口; 任意比例都不溢出、不裁切、关键内容可见、按钮达标;' +
      ' 把形态名与编号全删掉, 布局逐位不变;' +
      ' 结果页三个大数字块 <= 可用高度 30%(固定高度, 绝不纵向拉伸), 分块账目逐项对得上;' +
      ' 开始页(无结果态)用到的每一个宽高/字号/间距都是有限正数, uiRes 与实测窗口同源;' +
      ' 历史页底部导航等分、按需读盘、每条都带时间/设备 + 三个大数字 + 一行一项的清单' +
      '(白名 + 蓝数、六项齐全、顺序照结果页)、 没有密集表、口径一句话说清能不能比、' +
      ' 空态一句话、无动画、一屏仍看得见 2 张;' +
      ' runtimeOS = OpenHarmony 时 entry/src/main/ets 下不含 lightUpEffect / hdsEffect /' +
      ' @kit.UIDesignKit / pointLight;' +
      ' 美化(光感 / 粒子)只有一个判据 fxOn(), 它读既有的跑分标志位与运行时能力;' +
      ' 全工程只有一处 Canvas 与一处 setInterval, 两者都在 fxOn() 的门后 ——' +
      ' 跑分期间粒子节点根本不会被创建、tick 根本不会被注册');
    process.exit(0);
  }
  console.log('  有 ' + totalBad + ' 处不符合预期');
  process.exit(1);
});
