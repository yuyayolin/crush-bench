import { compare as refNativeCompare, version as refNativeVersion,
  compareReports as refNativeCompareReports } from 'libauroraref.so';
import { Gb7Result } from '../model/BenchModel';

// ===========================================================================
// 参考对照 / 线性-可比性审计的 ArkTS 接线(libauroraref.so, 只读)
//
//  为什么要这个文件
//  native 对照表里有两块是"最高优先级验收标准"点名的: linearityAudit(分数比 == 性能比 的自检)
//  与 comparabilityAudit(条件化可比性: 可用核集合 / 频率档 / 每核与同频归一化)。其中
//     allowedCores / runtimeKhzMedian / nominalTopKhz / threadsUsed / workerCpus
//  这几个量 native 自己拿不到 —— 它们只有"这一次多核阶段到底用了几个线程、绑到了哪些核、
//  跑到什么频率"的调用方(ArkTS)才知道。不传, 报告 JSON 里这几个字段就恒为 null:
//  看起来像"这台机器没数据", 实际上是"根本没人接上"。
//
//  本文件只做接线, 不做任何计算
//   · 每个量都从 BenchRunner / native 已经上报的旁路字段里原样取:
//       threadsUsed      <- Gb7Result.cpuThreadsEffective(多核逐项实测, 取最大)
//       workerCpus       <- Gb7Result.cpuWorkers(真正绑定成功的池线程数)
//       workerCpusList   <- Gb7Result.cpuEffMask(实际使用集合位图)
//       allowedCores     <- Gb7Result.cpuAllowedCount(可用核集合, sched_getaffinity)
//       logicalCores/physicalCores <- Gb7Result.cpuLogical / cpuPhysical
//       runtimeKhzMedian <- runFreq 文本里的"中位 xxxMHz"(native 采样线程实测)
//       nominalTopKhz    <- Gb7Result.cpuMachineTopTierKhz(全机最快频率档)
//   · 拿不到就不传(native 会写 null), 不编一个数;
//   · 每个量的来源 / 为什么没拿到, 都写进 note 与 inputsJson, 报告里可逐条复核。
//   · 这些量只进"对照与自检"块, 不参与任何测试项的 metric / unit / k / conv / 计分。
// ===========================================================================

// ---- 线性/可比性审计要的旁路量(全部可空: null = 拿不到 -> 不传) ----
export interface RefAuditQuantities {
  threadsUsed: number | null;
  workerCpus: number | null;
  workerCpusList: string;
  allowedCores: number | null;
  logicalCores: number | null;
  physicalCores: number | null;
  runtimeKhzMedian: number | null;   // kHz
  nominalTopKhz: number | null;      // kHz
  note: string;                      // 逐项写清"值从哪来 / 为什么是 null"(可整段进报告)
}

export interface RefAuditInput {
  single: Gb7Result[];
  multi: Gb7Result[];
  gpu7: Gb7Result[];
  snRaw: string;                     // GPU-SNL 的原始 JSON('' = 本节没跑)
  singleComposite: number;
  multiComposite: number;
  requestedThreads: number;          // 请求线程数(BenchRunner.benchMultiThreads 的结果, 仅作兜底)
}

export interface RefAuditResult {
  moduleAvailable: boolean;          // 模块能不能调(native 侧 version() 是否成功)
  moduleVersion: string;
  ok: boolean;                       // compare() 是否返回了 ok:true 的对照表
  json: string;                      // native 原样返回(一个字都不改)
  optionsJson: string;               // 我们真正传进去的 options(原样, 便于复核"到底传了什么")
  note: string;                      // 量的来源说明
  error: string;                     // 失败原因(native 原文 / 异常文本)
}

// ---------------------------------------------------------------------------
// 小工具: 从 native 文本里取第一个整数(只认数字, 取不到返回 0)
// ---------------------------------------------------------------------------
function firstIntAfter(text: string, key: string): number {
  const at: number = text.indexOf(key);
  if (at < 0) {
    return 0;
  }
  let i: number = at + key.length;
  let v: number = 0;
  let any: boolean = false;
  while (i < text.length) {
    const c: string = text.charAt(i);
    if (c >= '0' && c <= '9') {
      v = v * 10 + (c.charCodeAt(0) - 48);
      any = true;
      i++;
    } else {
      break;
    }
  }
  return any ? v : 0;
}

// native runFreq 文本形如:
//   "运行时频率 中位 1220MHz / 最小 1150MHz / 最大 2270MHz(采样核标称上限 2270MHz) 采样 12 次(= 8 核 x 3 轮)"
// 这里只取"中位"(kHz); 取不到返回 0(调用方据此传 null)。
function medianKhzFromRunFreq(text: string): number {
  const mhz: number = firstIntAfter(text, '中位 ');
  return mhz > 0 ? mhz * 1000 : 0;
}

// ---------------------------------------------------------------------------
// 从多核逐项结果里收集那几个量。只读字段, 不做任何换算(runFreq 的 MHz→kHz 是单位说明,
// 而 native 侧的 runtimeKhzMedian 本来就要 kHz)。
// ---------------------------------------------------------------------------
export function quantitiesFromMulti(list: Gb7Result[], requestedThreads: number): RefAuditQuantities {
  const notes: string[] = [];
  let best: Gb7Result | null = null;
  let workerCpus: number = 0;
  let allowedCores: number = 0;
  let logical: number = 0;
  let physical: number = 0;
  let topTierKhz: number = 0;
  let medianZero: number = 0;          // 代表项自己的中位(与 threadsUsed 同源, 首选)
  let medianAny: number = 0;           // 退路: 本轮有读数的项里最大的中位
  let medianLo: number = 0;            // 本轮各多核项的中位范围(只为把话写全)
  let medianHi: number = 0;
  let workerList: string = '';
  let allowedSeen: boolean = false;
  for (const r of list) {
    if (best === null || r.cpuThreadsEffective > best.cpuThreadsEffective) {
      best = r;
    }
    if (r.cpuWorkers > workerCpus) {
      workerCpus = r.cpuWorkers;
      if (r.cpuEffMask.length > 0) {
        workerList = r.cpuEffMask;
      }
    }
    if (r.cpuAllowedOk && r.cpuAllowedCount > allowedCores) {
      allowedCores = r.cpuAllowedCount;
      allowedSeen = true;
    }
    if (r.cpuLogical > logical) {
      logical = r.cpuLogical;
    }
    if (r.cpuPhysical > physical) {
      physical = r.cpuPhysical;
    }
    if (r.cpuMachineTopTierKhz > topTierKhz) {
      topTierKhz = r.cpuMachineTopTierKhz;
    }
    const m: number = medianKhzFromRunFreq(r.runFreq);
    if (m > 0) {
      if (medianLo === 0 || m < medianLo) {
        medianLo = m;
      }
      if (m > medianHi) {
        medianHi = m;
      }
      if (m > medianAny) {
        medianAny = m;
      }
    }
  }
  // 代表项(线程数最多的那一项)的中位: 与 threadsUsed 同源 —— 一个"用来做同频归一化"的数
  // 必须和"用了几个线程"说的是同一项, 否则这个归一化量的含义就糊了。
  if (best !== null) {
    medianZero = medianKhzFromRunFreq(best.runFreq);
  }
  // threadsUsed: 只认实测的 cpuThreadsEffective; 没有就退回"请求线程数"并在 note 里写明这不是实测值
  let threadsUsed: number | null = null;
  if (best !== null && best.cpuThreadsEffective > 0) {
    threadsUsed = best.cpuThreadsEffective;
    notes.push('threadsUsed=' + threadsUsed.toString() + '（实测：多核逐项 cpuThreadsEffective 的最大值，代表项=' +
      best.name + '）');
  } else if (requestedThreads > 0) {
    threadsUsed = requestedThreads;
    notes.push('threadsUsed=' + requestedThreads.toString() +
      '（不是实测值：native 没上报 cpuThreadsEffective，退回请求线程数 benchMultiThreads()）');
  } else {
    notes.push('threadsUsed=未传（既没有 cpuThreadsEffective，也拿不到请求线程数）');
  }
  let workerOut: number | null = null;
  if (workerCpus > 0) {
    workerOut = workerCpus;
    notes.push('workerCpus=' + workerCpus.toString() +
      '（实测：真正绑定成功的池线程数 cpuWorkers，取各多核项的最大值）');
  } else {
    notes.push('workerCpus=未传（native 没上报 cpuWorkers）');
  }
  let allowedOut: number | null = null;
  if (allowedSeen && allowedCores > 0) {
    allowedOut = allowedCores;
    notes.push('allowedCores=' + allowedCores.toString() +
      '（实测：可用核集合 sched_getaffinity 的核数 cpuAllowedCount；全机口径, 各多核项一致）');
  } else {
    notes.push('allowedCores=未传（native 没读到可用核集合）');
  }
  let medianOut: number | null = null;
  const rangeText: string = (medianLo > 0 && medianHi > medianLo)
    ? ('；本轮各多核项的中位范围 ' + medianLo.toString() + '~' + medianHi.toString() + ' kHz')
    : '';
  if (medianZero > 0) {
    medianOut = medianZero;
    notes.push('runtimeKhzMedian=' + medianZero.toString() + ' kHz（实测：代表项的运行频率中位, ' +
      '与 threadsUsed 同源；取自 native runFreq 文本里的"中位 xxxMHz"' + rangeText + '）');
  } else if (medianAny > 0) {
    medianOut = medianAny;
    notes.push('runtimeKhzMedian=' + medianAny.toString() + ' kHz（代表项没有频率读数, 退回到本轮有读数的' +
      '多核项里最大的那个中位' + rangeText + '）');
  } else {
    notes.push('runtimeKhzMedian=未传（runFreq 里没有"中位"读数）');
  }
  let topOut: number | null = null;
  if (topTierKhz > 0) {
    topOut = topTierKhz;
    notes.push('nominalTopKhz=' + topTierKhz.toString() + ' kHz（标称：全机最快频率档 cpuMachineTopTierKhz）');
  } else {
    notes.push('nominalTopKhz=未传（native 没上报全机最快频率档）');
  }
  const q: RefAuditQuantities = {
    threadsUsed: threadsUsed,
    workerCpus: workerOut,
    workerCpusList: workerList,
    allowedCores: allowedOut,
    logicalCores: logical > 0 ? logical : null,
    physicalCores: physical > 0 ? physical : null,
    runtimeKhzMedian: medianOut,
    nominalTopKhz: topOut,
    note: notes.join('；')
  };
  return q;
}

// ---------------------------------------------------------------------------
// 逐项 -> native 要的 JSON 数组字符串(name/score/metric/unit; metric 在 native 侧按字符串读)
// ---------------------------------------------------------------------------
export function gb7ItemsJson(list: Gb7Result[]): string {
  const rows: string[] = [];
  for (const r of list) {
    rows.push('{"name":' + JSON.stringify(r.name) +
      ',"score":' + (r.score > 0 ? r.score.toString() : '0') +
      ',"metric":' + JSON.stringify(r.metric.toString()) +
      ',"unit":' + JSON.stringify(r.unit) + '}');
  }
  return '[' + rows.join(',') + ']';
}

// ---------------------------------------------------------------------------
// options 组装: 拿不到的键一个都不写(native 侧读不到就写 null, 不会编 0)
// ---------------------------------------------------------------------------
export function refAuditOptionsJson(input: RefAuditInput, q: RefAuditQuantities): string {
  const parts: string[] = [];
  const single: string = gb7ItemsJson(input.single);
  const multi: string = gb7ItemsJson(input.multi);
  const gpu7: string = gb7ItemsJson(input.gpu7);
  if (input.single.length > 0) {
    parts.push('"singleCore":' + JSON.stringify(single));
  }
  if (input.multi.length > 0) {
    parts.push('"multiCore":' + JSON.stringify(multi));
  }
  if (input.gpu7.length > 0) {
    parts.push('"gpu7Items":' + JSON.stringify(gpu7));
  }
  if (input.snRaw.length > 0) {
    parts.push('"gpuSnl":' + JSON.stringify(input.snRaw));
  }
  if (input.singleComposite > 0) {
    parts.push('"singleComposite":' + input.singleComposite.toString());
  }
  if (input.multiComposite > 0) {
    parts.push('"multiComposite":' + input.multiComposite.toString());
  }
  if (q.threadsUsed !== null) {
    parts.push('"threadsUsed":' + q.threadsUsed.toString());
  }
  if (q.workerCpus !== null) {
    parts.push('"workerCpus":' + q.workerCpus.toString());
  }
  if (q.workerCpusList.length > 0) {
    parts.push('"workerCpusList":' + JSON.stringify(q.workerCpusList));
  }
  if (q.allowedCores !== null) {
    parts.push('"allowedCores":' + q.allowedCores.toString());
  }
  if (q.logicalCores !== null) {
    parts.push('"logicalCores":' + q.logicalCores.toString());
  }
  if (q.physicalCores !== null) {
    parts.push('"physicalCores":' + q.physicalCores.toString());
  }
  if (q.runtimeKhzMedian !== null) {
    parts.push('"runtimeKhzMedian":' + q.runtimeKhzMedian.toString());
  }
  if (q.nominalTopKhz !== null) {
    parts.push('"nominalTopKhz":' + q.nominalTopKhz.toString());
  }
  return '{' + parts.join(',') + '}';
}

// ---------------------------------------------------------------------------
// 调用一次(同步, 毫秒级)。任何失败都不抛给上层: 对照表是只读旁路, 拿不到就写原因。
// ---------------------------------------------------------------------------
export function runRefAudit(input: RefAuditInput): RefAuditResult {
  const q: RefAuditQuantities = quantitiesFromMulti(input.multi, input.requestedThreads);
  const optionsJson: string = refAuditOptionsJson(input, q);
  const r: RefAuditResult = {
    moduleAvailable: false, moduleVersion: '', ok: false, json: '',
    optionsJson: optionsJson, note: q.note, error: ''
  };
  try {
    r.moduleVersion = refNativeVersion();
    r.moduleAvailable = true;
  } catch (e) {
    r.moduleAvailable = false;
    r.error = 'libauroraref.so 加载/调用失败（version()）: ' + e.message;
    return r;
  }
  try {
    const out: string = refNativeCompare(optionsJson);
    r.json = out;
    if (out.length === 0) {
      r.error = 'native 返回空串（对照表没生成）';
      return r;
    }
    r.ok = out.indexOf('"ok":true') >= 0;
    if (!r.ok) {
      r.error = out;
    }
  } catch (e) {
    r.ok = false;
    r.error = 'compare() 抛异常: ' + e.message;
  }
  return r;
}

// ---------------------------------------------------------------------------
// 两份报告对比(compareReports)的桥
//
// 为什么不直接在"对比界面"那个新文件里 import 'libauroraref.so':
//   ArkTS 每多一个文件 import 同一个 .so, 就多一条 "module ... is not verified" 警告,
//   本工程的警告基线是 11 条(与 BenchRunner 里 NPU/存储桥、Index 里不再直接 import
//   libaurorabench.so 是同一条理由)。所以这里做一层同义直通桥: 新文件 import 本文件即可。
//
// 本函数只调用, 不做任何计算、不改 native 返回的一个字:
//   · 输入 = 两份 report-latest.json 的全文(native 自己解析, 缺什么写什么);
//   · 任何异常都不抛给上层, 兜成 ok=false + error(对比是只读旁路, 不能把界面带走)。
// ---------------------------------------------------------------------------
export interface RefCompareResult {
  moduleAvailable: boolean;   // native 模块能不能调
  ok: boolean;                // native 是否返回了 ok:true 的对比结果
  json: string;               // native 原样返回(一个字都不改)
  error: string;              // 失败原因(native 原文 / 异常文本)
  optionsJson: string;        // 我们真正传进去的 options(原样, 便于复核)
}

export function compareTwoReports(jsonA: string, jsonB: string, optionsJson: string): RefCompareResult {
  const r: RefCompareResult = {
    moduleAvailable: false, ok: false, json: '', error: '', optionsJson: optionsJson
  };
  if (jsonA.length === 0 || jsonB.length === 0) {
    r.error = '有一份报告是空的(没有读到文件内容) —— 没有输入就没有对比, 不编一个结果出来。';
    return r;
  }
  try {
    refNativeVersion();
    r.moduleAvailable = true;
  } catch (e) {
    r.error = 'libauroraref.so 加载/调用失败（version()）: ' + e.message;
    return r;
  }
  try {
    const out: string = refNativeCompareReports(jsonA, jsonB, optionsJson);
    r.json = out;
    if (out.length === 0) {
      r.error = 'native 返回空串（对比结果没生成）';
      return r;
    }
    r.ok = out.indexOf('"ok":true') >= 0;
    if (!r.ok) {
      r.error = out;
    }
  } catch (e) {
    r.ok = false;
    r.error = 'compareReports() 抛异常: ' + e.message;
  }
  return r;
}

// ---------------------------------------------------------------------------
// 从对照表 JSON 里整体摘出一个子对象(用于文本报告只嵌该嵌的两块)。
// 纯括号配平, 不解析语义; 摘不到返回 ''(调用方说明"摘不出来", 不编内容)。
// ---------------------------------------------------------------------------
export function extractObject(json: string, key: string): string {
  const at: number = json.indexOf('"' + key + '":');
  if (at < 0) {
    return '';
  }
  const start: number = json.indexOf('{', at);
  if (start < 0) {
    return '';
  }
  let depth: number = 0;
  let inStr: boolean = false;
  for (let i = start; i < json.length; i++) {
    const c: string = json.charAt(i);
    if (inStr) {
      if (c === '\\') {
        i++;
      } else if (c === '"') {
        inStr = false;
      }
      continue;
    }
    if (c === '"') {
      inStr = true;
    } else if (c === '{') {
      depth++;
    } else if (c === '}') {
      depth--;
      if (depth === 0) {
        return json.substring(start, i + 1);
      }
    }
  }
  return '';
}

// 对照表是不是一段合法的 JSON 对象(只有合法才允许原样嵌进报告 JSON, 免得把报告写坏)
export function looksLikeJsonObject(json: string): boolean {
  const t: string = json.trim();
  if (t.length < 2 || t.charAt(0) !== '{' || t.charAt(t.length - 1) !== '}') {
    return false;
  }
  try {
    const parsed: Object = JSON.parse(t) as Object;
    return parsed !== null;
  } catch (e) {
    return false;
  }
}
