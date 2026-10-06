// ---------------------------------------------------------------------------
// 有效数字排版 —— 与 native 侧 metric 的 "%.4g" 同口径(纯函数, 零依赖, 无 IO)
//
// 为什么需要它
//   GB7 的单项分是 native 用 metric 字符串算出来的: 单项分 = k x (metric x conv)。
//   native 侧 16 处 metric 格式串已统一成 "%.4g"(4 位有效数字, 量化误差 < 0.1%)。
//   如果 ArkTS 显示层再把 metric 四舍五入成 1~2 位小数(旧代码就是 toFixed(1)/toFixed(2)),
//   屏幕上就会出现"吞吐 0.5 / 分数 8.3"这种算不回去的组合 —— 用户投诉的正是这个:
//   两台不同芯片(HDR / Ray Tracer / Audio Encoder / Video Encoder)分数逐位相同,
//   而屏幕上看到的吞吐却解释不了那个分数。
//   所以显示层必须与 native 同口径: 4 位有效数字, 一位不多、一位不少。
//
// 口径(逐条写清, 可机械核对; 边界用例见 ref/numberfmt_cases.test.mjs)
//   1) 一律 4 位有效数字(SIG_DIGITS)。位数用完即止, 不留无意义的尾随零:
//        226.04 -> '226'    0.5 -> '0.5'    62.06 -> '62.06'    0.4831 -> '0.4831'
//      这与 C 的 printf("%.4g", x) 去尾零的行为一致。
//   2) |v| < 1e5 一律普通十进制写法, 不出现 '1.234e+4' 这种指数形式
//      (那正是 JS 的 toPrecision(4) 会干的事, 也是本函数存在的第二个理由)。
//      与 C 的 %.4g 的唯一差别: C 在 |v| >= 1e4 就转指数, 本函数按项目要求放宽到 1e5。
//      这么写既好读, 也更短: 4 位有效数字在 1e4~1e5 之间最多 5 个字符('11440'),
//      而 %.4g 的 '1.144e+04' 要 8 个字符。
//   3) 只有 |v| >= 1e5 或 0 < |v| < 1e-4 时才用指数写法(与 C 的 %.4g 一致):
//        '1.235e+06' / '4.831e-05'(尾数同样去尾零, 指数至少两位)。
//      这一条的作用是让输出长度有上界: 任何取值下最长 9 个字符
//      ('0.0004831' 或 '4.831e-05'), 不会随着数值趋近 0 而无限制变长(1e-9 也还是 9 个字符)。
//   4) 0 -> '0'。
//   5) 负数 -> '—'。判据: native 约定 -1 = "该项没跑 / 无此读数", 负吞吐没有物理意义;
//      若原样显示成 '-1.0', 会被当成一个真实读数。
//   6) 非有限值(NaN / ±Infinity) -> '—'。判据: native 用空串表示"没有读数",
//      ArkTS 侧 parseFloat('') 得到的是 NaN(见 BenchRunner 解析 metricRaw 的那几处);
//      NaN 既不是 0 也不是一个可显示的数 —— 不写 0 冒充读数。
//   '—'(U+2014)与 service/FullRun.ets 里 numOrDash 的"无读数"标记是同一个字符。
//
// 排版约束 本模块只产出字符串, 不碰任何宽度 / 字号 / 内外边距。
//   最坏情况长度: 普通写法 <= 9 字符, 指数写法 <= 9 字符; 日常区间(0.1 <= v < 1e5)<= 6 字符。
// ---------------------------------------------------------------------------

// 有效数字位数: 与 native 的 %.4g 同值。改这一个数就同时改了所有显示点。
export const SIG_DIGITS: number = 4;

// "没有读数"标记(见上面第 5 / 6 条判据)
export const NO_READING: string = '—';

// 补 n 个 '0'(n <= 0 时返回空串)
function zeros(n: number): string {
  let s: string = '';
  for (let i = 0; i < n; i++) {
    s += '0';
  }
  return s;
}

// 去掉小数部分无意义的尾随零; 若小数部分被去空, 连小数点一起去掉。
//   不能拿去处理整数写法: '10000' 的尾零是有效数字, 去掉就变成 '1' 了。
function trimFrac(s: string): string {
  const di: number = s.indexOf('.');
  if (di < 0) {
    return s;                       // 没有小数点 = 整数写法, 一个字符都不动
  }
  let end: number = s.length;
  while (end > di + 1 && s.charAt(end - 1) === '0') {
    end--;
  }
  if (end === di + 1) {
    end = di;                       // 小数部分全是零 -> 连小数点一起去掉
  }
  return s.substring(0, end);
}

// 去掉尾数(指数写法的 4 位数字, 一定不含小数点)上的尾随零: '1200' -> '12'
function trimMantissa(digits: string): string {
  let end: number = digits.length;
  while (end > 1 && digits.charAt(end - 1) === '0') {
    end--;
  }
  return digits.substring(0, end);
}

/**
 * 4 位有效数字的十进制写法(与 native 的 %.4g 同口径, 见文件头口径表)。
 *
 * @param v 待排版的数值(吞吐 / metric)
 * @returns 见文件头第 1~6 条; 0 -> '0'; 负数与非有限值 -> '—'
 */
export function sigText(v: number): string {
  if (!Number.isFinite(v)) {
    return NO_READING;      // NaN / ±Infinity: 没有读数(判据 6)
  }
  if (v < 0) {
    return NO_READING;      // native 的 -1 = 该项没跑(判据 5)
  }
  if (v === 0) {
    return '0';             // 判据 4
  }
  // toExponential 已经把"4 位有效数字"定死(与 toPrecision 同一套舍入规则:
  //   恰好在中间时取较大的那个), 并且无损地给出十进制指数 —— 比用 log10 判断
  //   指数稳(1000 这种整十次幂上 log10 会有浮点毛刺)。
  const exp: string = v.toExponential(SIG_DIGITS - 1);
  const ei: number = exp.indexOf('e');
  const mant: string = exp.substring(0, ei);          // 形如 '4.831'
  const x: number = parseInt(exp.substring(ei + 1), 10);  // 十进制指数
  const digits: string = mant.substring(0, 1) + mant.substring(2); // 4 位数字, 无小数点

  // (3) 指数写法: |v| >= 1e5 或 |v| < 1e-4
  if (x >= 5 || x <= -5) {
    let m: string = trimMantissa(digits);
    if (m.length > 1) {
      m = m.substring(0, 1) + '.' + m.substring(1);
    }
    const absX: string = Math.abs(x).toString();
    return m + 'e' + (x < 0 ? '-' : '+') + (absX.length < 2 ? ('0' + absX) : absX);
  }

  // (1)(2) 普通写法: 按指数把小数点放回去, 再去尾零
  let s: string = '';
  if (x >= 0) {
    if (x + 1 >= SIG_DIGITS) {
      s = digits + zeros(x + 1 - SIG_DIGITS);   // 整数, 后面补零(1144 x10^1 -> '11440')
    } else {
      s = digits.substring(0, x + 1) + '.' + digits.substring(x + 1);  // '226.0'
    }
  } else {
    s = '0.' + zeros(-x - 1) + digits;          // 0.4831 / 0.04831 / 0.0004831
  }
  return trimFrac(s);
}

/**
 * '数字 单位' 一组的整体写法(单位为空时不带多余空格)。
 * 例: metricUnit(0.4831, 'Mpx/s') -> '0.4831 Mpx/s'; metricUnit(0, '') -> '0'
 */
export function metricUnit(v: number, unit: string): string {
  const n: string = sigText(v);
  return unit.length > 0 ? (n + ' ' + unit) : n;
}

/**
 * 结果页(common/ResultLayers.ets)口径: 取不到就写 '--'。
 * 判据与那个文件里的 num0/num1/num2 完全一致(它们只对 v > 0 排版):
 *   v 非有限 / v <= 0 -> '--';否则 4 位有效数字。
 * 这样"运行失败(metric=0)"不会被显示成 '0 Mpx/s' 冒充一个真实读数。
 */
export function metricOrDash(v: number): string {
  if (!Number.isFinite(v) || v <= 0) {
    return '--';
  }
  return sigText(v);
}

/**
 * 完整报告(service/FullRun.ets)口径: 把 native 的两种"没有读数"分开写。
 * 判据与那个文件里的 numOrDash(v, 4) 一致, 只把小数位口径换成有效数字:
 *   NaN        -> '—'          (native 给了空串: 解析不出来)
 *   v < 0      -> '—（未跑）'   (native 用 -1 表示该项没跑)
 *   其它(含 0) -> 4 位有效数字
 */
export function metricUnrun(v: number): string {
  if (!Number.isFinite(v)) {
    return NO_READING;
  }
  if (v < 0) {
    return NO_READING + '（未跑）';
  }
  return sigText(v);
}
