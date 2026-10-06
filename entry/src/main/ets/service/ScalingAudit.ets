// ============================================================================
//  service/ScalingAudit.ets
//  「本项是否真的随芯片性能变化」的自检(跨芯片可比性的前提)
// ============================================================================
//
//  为什么要有它(用户定的最高验收标准)
//  ---------------------------------------------------------------------------
//  "只要测试的基准相同, 保证不同芯片之间的分数差能真实地表现它们的实力差, 那其他的
//   不怎么重要。" —— 这句话成立的前提是: 每一项的吞吐真的随芯片算力变化。
//  如果某一项测的根本不是算力(负载太轻、时间全花在线程创建/初始化这类固定开销上,
//  工作集小到落在缓存里, 或者被内存延迟/系统策略卡住), 那它在两台芯片上会给出几乎相同
//  的数 -> 这一项把两台设备的分数比往 1 压, 直接毁掉"分数比 == 性能比"。
//
//  本文件只做三件事: ① 用本轮已经采到的数据判每一项; ② 把判据原文写进结论;
//  ③ 把判成"不随芯片变化"的项标注出来(不可用于跨芯片比较), 供读报告的人自行取舍。
//
//  2026-10-05 定(与"复合分参与项集合"的关系, 只此一条)
//  自检结论不改变复合分的参与项集合。复合分的参与项 = 该阶段所有"计分且拿到正分"的项;
//  FAIL 的项照常计入, 只是被标注为不可用于跨芯片比较。
//  为什么(真机 8.1 Pura X Max 的实测): 官方多核套件只有 8 项, 一"排除"就可能只剩 3 项;
//  而"哪几项 FAIL"由 2 轮离散度(阈值 5%)决定, 是噪声敏感的。同一台设备 8.0 -> 8.1:
//    全口径多核几何平均  1447.06 -> 1450.76  (差 +0.26%, 这才是"没变")
//    按自检排除之后      795.81  -> 2071.86  (差 +160%, 变成了噪声抽签)
//  一个会因噪声在两轮之间差 2.6 倍的数, 不能叫"复合分"。所以自检改成提示, 不改口径。
//  这条不变量由 FullRun 的 compositeScopeCheck() 在运行时硬核对(参与项集合 == items[] 有正分的项)。
//
//  **它不跑任何负载、不改任何 metric/unit/k/conv/计分公式/线程数/绑核策略,
//    也不改变任何一项的测量方式。** 全部是纯函数 + 旁路字段。
//
//  判据(两条, 全部只用本轮已有的数据; 阈值都是我们自己定的, 不是官方阈值)
//  ---------------------------------------------------------------------------
//   SC1「并行加速比」: 同一台设备上, 同一项在多核阶段与单核阶段的分数之比
//       S = 多核分 / 单核分。
//       这是"在工作量完全相同的前提下改变可用算力"的直接对照 —— 与用户提的
//       "改变单位数看吞吐变不变"是同一件事的另一面。算力翻倍而吞吐不动, 就说明
//       这一项的吞吐不受算力支配(串行段/固定开销/内存延迟主导)。
//       物理可达区间: 1 <= S <= 实际用到核数 x 1.30(留 30% 给频率档与 SMT 的余量)。
//         S < 1.15                -> FAIL(几乎不随核数增长)
//         S > 核数 x 1.30         -> FAIL(超过物理上限, 读数可疑)
//         其余                    -> PASS
//       只有官方多核 8 项有这项对照(官方多核套件里本来就只有这 8 项)。
//
//   SC2「轮间离散度」: 复用 native 已经在算的相对离散度 (max-min)/中位 x 100%。
//       离散度 > 5% 说明这一项的读数是噪声/抖动主导 -> 该数字不能拿去跨芯片比较。
//       (阈值 5% 与 native 的 UNRELIABLE 判定同源, 也是我们自己定的。)
//
//  判不出来的项一律写 UNPROVEN(未自证), 不假装通过。
//  典型: 官方多核套件里没有的那 8 项 —— 本轮数据里没有任何"改变可用算力"的对照,
//  单核阶段永远是 1 线程, 所以它到底随不随芯片变化, 现有数据判不出来。这一点必须
//  写出来, 而不是标一个绿色的 PASS 让人以为已经证过了。
// ============================================================================

import { Gb7Result } from '../model/BenchModel';

// ---- 自检结论的三种取值(纯标注; 三种取值都不改变复合分的参与项集合) ----
export const SC_PASS: string = 'PASS';          // 有正向证据
export const SC_FAIL: string = 'FAIL';          // 有反向证据 -> 不可用于跨芯片比较
export const SC_UNPROVEN: string = 'UNPROVEN';  // 现有数据判不出来(未自证)

// ---- 阈值(我们自己定的) ----
const SC_MIN_SPEEDUP: number = 1.15;        // 多核加速比下限
const SC_MAX_SPEEDUP_SLACK: number = 1.30;  // 加速比上限 = 实际核数 x 该系数
const SC_SPREAD_FAIL_PCT: number = 5.0;     // 与 native 的 UNRELIABLE 阈值同源

export interface ScalingRow {
  loadId: number;
  name: string;
  stage: string;          // '单核' / '多核'
  verdict: string;
  evidence: string;
  speedup: number;        // 多核分 / 单核分(只有多核项有)
  hasSpeedup: boolean;
  spreadPct: number;
  rounds: number;
  threads: number;
}

export interface ScalingSummary {
  singleRows: ScalingRow[];
  multiRows: ScalingRow[];
  passN: number;
  failN: number;
  unprovenN: number;
  excludedIds: number[];        // FAIL 项的负载 id(仅作"不可用于跨芯片比较"的标注)
  excludedNames: string[];
  hasAnyProbe: boolean;         // 本轮有没有任何一项拿到了 SC1 对照
  text: string;                 // 一段可直接贴进报告的人话
}

// ---------------------------------------------------------------------------
//  小工具(纯函数)
// ---------------------------------------------------------------------------
function findById(list: Gb7Result[], id: number): Gb7Result | null {
  for (const r of list) {
    if (r.loadId === id) {
      return r;
    }
  }
  return null;
}

function mhzOf(r: Gb7Result): number {
  return r.cpuThreadsEffective > 0 ? r.cpuThreadsEffective : r.cpuThreadsRequested;
}

// 轮间离散度判据。轮数不足 2 -> 没有离散度数据 -> 判不出来(不假装稳定)。
function spreadVerdict(r: Gb7Result): string {
  if (r.roundsOk < 2) {
    return SC_UNPROVEN;
  }
  return r.scoreSpreadPct > SC_SPREAD_FAIL_PCT ? SC_FAIL : SC_PASS;
}

function spreadEvidence(r: Gb7Result): string {
  if (r.roundsOk < 2) {
    return '轮间离散度: 有效轮数 ' + r.roundsOk.toString() + ' < 2, 没有离散度数据, 判不出来';
  }
  return '轮间离散度 ' + r.scoreSpreadPct.toFixed(2) + '%(有效轮数 ' + r.roundsOk.toString() +
    ', 阈值 ' + SC_SPREAD_FAIL_PCT.toFixed(1) + '% 是我们自己定的)' +
    (r.scoreSpreadPct > SC_SPREAD_FAIL_PCT ? ' -> 噪声/抖动主导, 该数字不能拿去跨芯片比较' : ' -> 未超阈值');
}

function rowOf(r: Gb7Result, stage: string, verdict: string, evidence: string,
  speedup: number, hasSpeedup: boolean): ScalingRow {
  return {
    loadId: r.loadId,
    name: r.name,
    stage: stage,
    verdict: verdict,
    evidence: evidence,
    speedup: speedup,
    hasSpeedup: hasSpeedup,
    spreadPct: r.scoreSpreadPct,
    rounds: r.roundsOk,
    threads: mhzOf(r)
  };
}

// ---------------------------------------------------------------------------
//  ① 单核阶段跑完之后: 只能判 SC2(SC1 需要多核阶段的数据, 那时还没有)
//     为什么这样分阶段: 结果页上"单核卡片的综合分"在单核阶段一结束就要显示,
//     它必须与报告里的数同口径。SC1 是"这一项在多核阶段能不能并行"的证据,
//     它的结论只作用于多核口径的复合分 —— 单核口径不受它影响, 这在物理上也对:
//     单核那一遍本来就是串行工况, 与"多核能不能并行"是两件事。
// ---------------------------------------------------------------------------
export function auditSingleStage(single: Gb7Result[]): void {
  for (const r of single) {
    if (r.loadId < 0) {
      continue;   // 非 CS1 注册表项(理论上不会出现在这里)
    }
    const v: string = spreadVerdict(r);
    r.scalingVerdict = v;
    r.scalingEvidence = '单核口径 · ' + spreadEvidence(r) +
      '; 并行对照: 无(单核阶段固定 1 线程, 本阶段没有可比的算力变化)';
  }
}

// ---------------------------------------------------------------------------
//  ② 多核阶段跑完之后: 判 SC1 + SC2, 结论只写进多核阶段的那些结果对象
// ---------------------------------------------------------------------------
export function auditMultiStage(single: Gb7Result[], multi: Gb7Result[]): void {
  for (const r of multi) {
    if (r.loadId < 0) {
      continue;
    }
    const s: Gb7Result | null = findById(single, r.loadId);
    const threads: number = mhzOf(r);
    let v1: string = SC_UNPROVEN;
    let e1: string = '';
    let speedup: number = 0;
    let hasSpeedup: boolean = false;
    if (s === null) {
      e1 = '并行对照: 单核阶段没有同一项(id=' + r.loadId.toString() + '), 算不出加速比';
    } else if (!(s.score > 0) || !(r.score > 0)) {
      e1 = '并行对照: 有一侧没有正分数(单核 ' + s.score.toFixed(1) + ' / 多核 ' +
        r.score.toFixed(1) + '), 算不出加速比';
    } else if (threads < 2) {
      e1 = '并行对照: 本项多核阶段实际只用 ' + threads.toString() + ' 个核, 没有算力变化可言';
    } else {
      speedup = r.score / s.score;
      hasSpeedup = true;
      const upper: number = threads * SC_MAX_SPEEDUP_SLACK;
      if (speedup < SC_MIN_SPEEDUP) {
        v1 = SC_FAIL;
        e1 = '并行加速比 = 多核 ' + r.score.toFixed(1) + ' / 单核 ' + s.score.toFixed(1) + ' = ' +
          speedup.toFixed(3) + 'x < ' + SC_MIN_SPEEDUP.toFixed(2) +
          '(实际用到 ' + threads.toString() + ' 核) -> 算力变多了吞吐却几乎不动, 说明本项吞吐不由算力支配';
      } else if (speedup > upper) {
        v1 = SC_FAIL;
        e1 = '并行加速比 = ' + speedup.toFixed(3) + 'x > 实际核数 ' + threads.toString() +
          ' x ' + SC_MAX_SPEEDUP_SLACK.toFixed(2) + ' = ' + upper.toFixed(2) +
          ' -> 超过物理上限, 本项读数可疑';
      } else {
        v1 = SC_PASS;
        e1 = '并行加速比 = 多核 ' + r.score.toFixed(1) + ' / 单核 ' + s.score.toFixed(1) + ' = ' +
          speedup.toFixed(3) + 'x, 落在 [ ' + SC_MIN_SPEEDUP.toFixed(2) + ', ' +
          upper.toFixed(2) + ' ](实际用到 ' + threads.toString() +
          ' 核) -> 吞吐确实随可用算力变化';
      }
    }
    const v2: string = spreadVerdict(r);
    const e2: string = spreadEvidence(r);
    let verdict: string = SC_UNPROVEN;
    if (v1 === SC_FAIL || v2 === SC_FAIL) {
      verdict = SC_FAIL;
    } else if (v1 === SC_PASS && v2 === SC_PASS) {
      verdict = SC_PASS;
    } else if (v1 === SC_PASS || v2 === SC_PASS) {
      // 一条通过、另一条没数据: 有正向证据, 但必须写明缺哪一条
      verdict = SC_PASS;
    }
    r.scalingVerdict = verdict;
    r.scalingEvidence = '多核口径 · ' + e1 + '; ' + e2;
  }
}

// ---------------------------------------------------------------------------
//  ③ 汇总(报告 / 界面都用这一份, 保证两处口径一致)
// ---------------------------------------------------------------------------
export function summarize(single: Gb7Result[], multi: Gb7Result[]): ScalingSummary {
  const sRows: ScalingRow[] = [];
  const mRows: ScalingRow[] = [];
  let passN: number = 0;
  let failN: number = 0;
  let unprovenN: number = 0;
  const excludedIds: number[] = [];
  const excludedNames: string[] = [];
  let probes: number = 0;
  for (const r of single) {
    if (r.scalingVerdict.length === 0) {
      continue;
    }
    sRows.push(rowOf(r, '单核', r.scalingVerdict, r.scalingEvidence, 0, false));
    if (r.scalingVerdict === SC_PASS) {
      passN++;
    } else if (r.scalingVerdict === SC_FAIL) {
      failN++;
    } else {
      unprovenN++;
    }
  }
  for (const r of multi) {
    if (r.scalingVerdict.length === 0) {
      continue;
    }
    const hasSp: boolean = r.scalingEvidence.indexOf('并行加速比 = 多核') >= 0;
    if (hasSp) {
      probes++;
    }
    mRows.push(rowOf(r, '多核', r.scalingVerdict, r.scalingEvidence, 0, hasSp));
    if (r.scalingVerdict === SC_PASS) {
      passN++;
    } else if (r.scalingVerdict === SC_FAIL) {
      failN++;
    } else {
      unprovenN++;
    }
    if (r.scalingVerdict === SC_FAIL && excludedIds.indexOf(r.loadId) < 0) {
      excludedIds.push(r.loadId);
      excludedNames.push(r.name);
    }
  }
  const sum: ScalingSummary = {
    singleRows: sRows,
    multiRows: mRows,
    passN: passN,
    failN: failN,
    unprovenN: unprovenN,
    excludedIds: excludedIds,
    excludedNames: excludedNames,
    hasAnyProbe: probes > 0,
    text: ''
  };
  sum.text = summaryText(sum);
  return sum;
}

function summaryText(s: ScalingSummary): string {
  const parts: string[] = [];
  parts.push('本项是否真的随芯片性能变化 —— 用本轮已有的数据判, 判据与阈值都是我们自己定的');
  parts.push('通过 ' + s.passN.toString() + ' 项 · 不通过 ' + s.failN.toString() +
    ' 项 · 未自证 ' + s.unprovenN.toString() + ' 项');
  if (s.failN > 0) {
    parts.push('不通过(不可用于跨芯片比较; 仅提示, 仍按同一口径计入复合分): ' +
      s.excludedNames.join('、'));
  } else {
    parts.push('不可用于跨芯片比较的项: 无(本轮没有任何一项被判为"不随算力变化")');
  }
  if (s.hasAnyProbe) {
    parts.push('有"改变可用算力、工作量不变"对照的项: 官方多核 8 项(单核 1 线程 vs 多核 N 线程)');
  } else {
    parts.push('本轮没有任何一项拿到"改变可用算力"的对照, 因此所有结论都只是未自证');
  }
  parts.push('未自证的项: 现有数据里没有"算力变化而工作量不变"的对照(多核套件只有官方那 8 项),' +
    '不能据此判断它们随不随芯片变化 —— 写"未自证", 不冒充通过');
  return parts.join('；');
}

// ---------------------------------------------------------------------------
//  ④ 「能不能拿去跨芯片比」的判据(唯一入口; 报告、结果页、JSON 全部走它)
//
//  2026-10-05 改语义: 这个函数不再是复合分的过滤器, 只回答"这一项适不适合跨芯片比"。
//     复合分的参与项集合 = 该阶段所有"计分且拿到正分"的项(见 FullReportBuilder.geoOf),
//     与这里的结论无关 —— 旧写法把它接进 geoOf, 于是同一份报告里
//     "参与项 / 排除项 / 小节结论"三处各说各话(真机 8.1 实测)。
//     FAIL 是"有反向证据, 别拿它跨芯片比"; UNPROVEN 是"没有反向证据, 也判不出来" ——
//     两者都不该被静默地从复合分里抹掉。
// ---------------------------------------------------------------------------
export function isComparable(r: Gb7Result): boolean {
  return r.scalingVerdict !== SC_FAIL;
}

export function comparableCount(list: Gb7Result[]): number {
  let n: number = 0;
  for (const r of list) {
    if (r.score > 0 && isComparable(r)) {
      n++;
    }
  }
  return n;
}

export function excludedOf(list: Gb7Result[]): Gb7Result[] {
  const out: Gb7Result[] = [];
  for (const r of list) {
    if (r.score > 0 && !isComparable(r)) {
      out.push(r);
    }
  }
  return out;
}
