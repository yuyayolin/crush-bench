// ---------------------------------------------------------------------------
// 报告文本的 UTF-8 合法性守卫(2026-10 真机报告缺陷 4 修)
//
// 现场(真机 8.0, Pura X Max, 2026-10-05 16:04):
//   D:\gb7logs\r80\report-latest.txt 共 640778 字节, 在字节偏移 546380 处有 12 个
//   字节不是合法 UTF-8, 解码得到 12 个 U+FFFD; 同一轮的 .json 是干净的。
//   那 12 个字节是 ED B6 AC ED B1 A9 ED BB 8E ED B2 BD —— 4 个完整的 3 字节 CESU-8 序列,
//   每个解出一个 UTF-16 孤立低位代理(U+DDAC / U+DC69 / U+DECE / U+DCBD, 全在 DC00-DFFF);
//   同一处的文本还少了结尾的引号与花括号(见 GPU-SNL 原始 JSON 里 "text":"…�,"probe"),
//   也就是 native 侧交出来的那段字符串本身就比模板短 —— 这是"内存/指针层面的残缺",
//   不是任何一种编码路径能造成的(编码器只可能把字符编错, 不会把后面的字符吃掉)。
//
// 根因(实测证据, 见 verify_report_utf8.py):
//   * 同一处的 .json 里是 4 个 \udXXX 转义(JSON.stringify 对孤立代理项就是这么写的),
//     说明源字符串本身就带着孤立代理项 —— 不是 .txt 写盘时才坏的;
//   * 12 个非法字节 = 4 个完整的 3 字节 CESU-8 序列。如果真是"分块写入把一个多字节
//     字符切在块边界上", 最多只会切断一个字符 = 最多 3 个非法字节, 而且不会连续 12 个。
//     ⇒ 不是分块写入的问题;
//   * 这些孤立代理项来自 native 侧某段字符串(GPU-SNL 的 environment.stability.state/text,
//     真机上是内存已损坏的文本; 见报告里"原始 JSON 解析=失败"那一节)。
//
// 本模块给出的保证(硬断言, 不是"尽量"):
//   报告 .txt 必须是合法 UTF-8
//   ArkTS 的字符串是 UTF-16: 只要里面没有孤立代理项, 交给任何标准 UTF-8 编码器
//   (fs.writeSync / JSON.stringify) 写出去都一定是合法 UTF-8。所以本模块只做一件事:
//   在报告文本拼装完毕、写盘之前把孤立代理项换成 U+FFFD(EF BF BD), 并把替换的
//   个数与位置写进报告正文(不静默修改), 同时在写盘处再兜一道(防御性断言)。
//
// 口径: 合法代理对(高位 D800-DBFF 紧跟低位 DC00-DFFF)是正常的 astral 字符(emoji 等),
//       一个都不许动; 只动没配成对的那些。
// ---------------------------------------------------------------------------

/** 一段文本的 UTF-8 可编码性检查结果。 */
export interface Utf8Check {
  ok: boolean;             // true = 没有孤立代理项 -> 编码成 UTF-8 一定合法
  loneSurrogates: number;  // 孤立代理项个数(= 换成 CESU-8 后会变成几个 3 字节非法序列)
  firstIndex: number;      // 第一个孤立代理项在字符串里的下标(UTF-16 码元位), -1 = 没有
  badBytesIfWritten: number; // 若原样按 CESU-8 写出去, 会产生多少个非法字节(= 个数 x 3)
}

function isHigh(c: number): boolean {
  return c >= 0xD800 && c <= 0xDBFF;
}

function isLow(c: number): boolean {
  return c >= 0xDC00 && c <= 0xDFFF;
}

/** 扫描一遍: 有没有孤立代理项, 有几个, 第一处在哪(不改任何内容)。 */
export function checkReportText(s: string): Utf8Check {
  let lone: number = 0;
  let first: number = -1;
  let i: number = 0;
  while (i < s.length) {
    const c: number = s.charCodeAt(i);
    if (isHigh(c)) {
      const n: number = (i + 1 < s.length) ? s.charCodeAt(i + 1) : 0;
      if (isLow(n)) {
        i += 2;          // 合法代理对(astral 字符): 原样保留
        continue;
      }
      lone++;
      if (first < 0) {
        first = i;
      }
      i++;
      continue;
    }
    if (isLow(c)) {
      lone++;
      if (first < 0) {
        first = i;
      }
      i++;
      continue;
    }
    i++;
  }
  return { ok: lone === 0, loneSurrogates: lone, firstIndex: first, badBytesIfWritten: lone * 3 };
}

/**
 * 把孤立代理项换成 U+FFFD(与"按错误替换解码非法字节"的常规做法一致), 合法代理对原样保留。
 * 没有孤立代理项时原样返回同一个字符串(不做任何拷贝与改动)。
 */
export function sanitizeReportText(s: string): string {
  const chk: Utf8Check = checkReportText(s);
  if (chk.ok) {
    return s;
  }
  let out: string = '';
  let i: number = 0;
  while (i < s.length) {
    const c: number = s.charCodeAt(i);
    if (isHigh(c)) {
      const n: number = (i + 1 < s.length) ? s.charCodeAt(i + 1) : 0;
      if (isLow(n)) {
        out += s.substring(i, i + 2);
        i += 2;
        continue;
      }
      out += '\uFFFD';
      i++;
      continue;
    }
    if (isLow(c)) {
      out += '\uFFFD';
      i++;
      continue;
    }
    out += s.charAt(i);
    i++;
  }
  return out;
}

/**
 * 写盘前的硬断言: 返回 '' 表示这段文本写出去一定是合法 UTF-8;
 * 否则返回一句人话(带个数、第一处位置、以及"原样写会产生几个非法字节")。
 * 调用方不许把这个返回值吞掉: 它要么让文本先过 sanitizeReportText, 要么记进报告。
 */
export function assertTxtUtf8Safe(s: string): string {
  const chk: Utf8Check = checkReportText(s);
  if (chk.ok) {
    return '';
  }
  return '文本里有 ' + chk.loneSurrogates.toString() + ' 个孤立代理项(第一个在第 ' +
    chk.firstIndex.toString() + ' 个 UTF-16 码元), 原样写盘会产生 ' +
    chk.badBytesIfWritten.toString() + ' 个非法 UTF-8 字节 —— 报告 .txt 必须是合法 UTF-8, ' +
    '已按 U+FFFD 替换';
}
