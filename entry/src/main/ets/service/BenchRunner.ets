import { cpuCoreCount, testCount, testName, runTest, version, runCoreMark, gb7Count, gb7Name,
  runGb7, gb7TestIsMulti, gb7TestIsScored, setLogDir, nativeCrashGuardReady, nativeCrashLogPath,
  takeNativeCrashReport, clearNativeCrashReport, sampleMemory, lastMemorySample, setCurrentItem,
  crashSelfTest, smtCapabilities, smtSetEnabled, smtEnabled, smtThreadsFor, smtSwitchHint,
  cpuIdentityProbe, cpuAffinityProbe, nativeHangLogPath, itemMemoryReport, itemMemoryLogPath,
  gb7CompositeSingle, gb7CompositeMulti, ceilingText,
  npuProbe, npuBench, storageSetDir, storageInfo, storageRun, storageCleanup } from 'libaurorabench.so';
import { sceneCount, sceneName, ready, prepare, start, poll } from 'libauroragpu.so';
import { count as gpu7Count, name as gpu7Name, prepare as gpu7Prepare, run as gpu7Run,
  fullness as gpu7Fullness } from 'libauroragpu7.so';
// Aurora Nomad Light(GPU-SNL)小节: 独立 napi 模块 libaurorasn.so(与 GB7 的 GPU 11 项无关)。
// ready / prepare 与上面 libauroragpu.so 的同名导出重名, 必须 as 重命名; run 是同步调用, 返回 JSON 字符串。
import { ready as snReadyFlag, prepare as snPrep, run as snRunJson, sectionName as snSectionName,
  workloadName as snWorkloadName, benchVersion as snBenchVersionNapi,
  versionText as snVersionText, lastScore as snLastScore, lastError as snLastError } from 'libaurorasn.so';
import { deviceInfo } from '@kit.BasicServicesKit';
import { TestResult, BenchRecord, GpuSceneResult, Gb7Result } from '../model/BenchModel';
import { CrashGuard } from './CrashGuard';
import { DeviceProfile, DeviceProfileData } from './DeviceProfile';
import { MemoryProbe, MemStatus } from './MemoryProbe';
import { RunLog, PH_GB7_SINGLE, PH_GB7_MULTI, PH_GB7_GPU, PH_OWN, PH_STRESS } from './RunLog';
import { ChipFactsInput, ChipJudgment, renderInferLine } from './KirinSocTable';

export interface ProgressInfo {
  phase: string;
  index: number;
  total: number;
  testName: string;
  lastMs: number;
  lastScore: number;
}

export interface GpuProgress {
  sceneIndex: number;
  sceneName: string;
  fps: number;
  framesDone: number;
  framesTarget: number;
  frameMs: number;
}

const GPU_FRAMES = 240;

// ---------------------------------------------------------------------------
// "native 返回的 JSON 解析不了"这件事的显式标记键(2026-10 真机报告缺陷 3 修)
//   为什么需要: parseObj() 解析失败时只能回一个替身对象, 而调用方只读 ok/error/lastError ——
//   "JSON 无法解析"这个真实原因会被丢掉, 报告里就会印出「native 返回失败：（native 未给文本）」
//   这种没有原因的结论, 甚至与同报告里原样附着的原始 JSON 自相矛盾(GPU-SNL 小节真机现场)。
//   键名带双下划线前缀: native 的所有键都是驼峰/下划线单词, 不会与它们相撞。
// ---------------------------------------------------------------------------
export const PARSE_FAILED_KEY: string = '__auroraParseFailed';
export const PARSE_ERROR_KEY: string = '__auroraParseError';

// ---------------------------------------------------------------------------
// GB7 单项"预估峰值内存"表(单位 MB) —— 静态估计, 不是实测
//
// 这些数字是静态估计: 按各负载自己申请的数据规模推算出来的峰值, 依据写在每行注释里,
// 不依赖任何运行时采样。用途只有一个: 开跑前跟可用内存比一比, 不够就跳过该项,
// 降低被系统按 OOM 直接杀掉进程的概率(那种崩溃 ArkTS 侧完全捕获不到)。
//
// 峰值估计口径 = 负载内部同时存活的最大块之和(输入语料 + 中间结果 + 输出),
// 不含 codec / 图形栈 / 运行时的固定开销, 那部分由 MemoryProbe.GATE_HEADROOM_MB 的系统余量兜住。
// ---------------------------------------------------------------------------
export const MEM_EST_DEFAULT_MB = 20;
export const MEM_EST_TEXT_PROCESSING_MB = 200;   // 192 MiB 文本语料 + 分页中间量
export const MEM_EST_ASSET_COMPRESSION_MB = 70;  // 64 MiB 纹理输入 + 压缩输出缓冲
export const MEM_EST_PHOTO_LIBRARY_MB = 40;      // 多张全尺寸位图 + 重采样输出
export const MEM_EST_PDF_VIEWER_MB = 40;         // 页面位图 + 字形缓存
export const MEM_EST_SFM_MB = 60;                // 特征/匹配/轨迹 + 稀疏 BA 稠密块
export const MEM_EST_HDR_MB = 60;                // 多曝光浮点帧 + 色调映射输出
export const MEM_EST_HTML5_BROWSER_MB = 40;      // 并行布局树 + 样式表
export const MEM_EST_RAY_TRACER_MB = 20;         // 低分辨率帧缓冲 + 场景数据
// 其余项(File Compression / Navigation / Video * / Audio Encoder / Clang 等)峰值 < 20 MB

class MemEstimate {
  name: string;
  mb: number;
  constructor(name: string, mb: number) {
    this.name = name;
    this.mb = mb;
  }
}

// 单项预估峰值表(按项名匹配, 与 gb7Count() 的遍历顺序无关)
const MEM_EST: MemEstimate[] = [
  new MemEstimate('Text Processing', MEM_EST_TEXT_PROCESSING_MB),
  new MemEstimate('Asset Compression', MEM_EST_ASSET_COMPRESSION_MB),
  new MemEstimate('Photo Library', MEM_EST_PHOTO_LIBRARY_MB),
  new MemEstimate('PDF Viewer', MEM_EST_PDF_VIEWER_MB),
  new MemEstimate('Structure from Motion', MEM_EST_SFM_MB),
  new MemEstimate('HDR', MEM_EST_HDR_MB),
  new MemEstimate('HTML5 Browser', MEM_EST_HTML5_BROWSER_MB),
  new MemEstimate('Ray Tracer', MEM_EST_RAY_TRACER_MB)
];

export interface MemGateInfo {
  supported: boolean;          // 内存接口是否可用(不可用 = 本次未做闸门)
  availableMb: number;
  needMb: number;              // 该项的预估峰值 MB(静态估计)
  skipped: boolean;
  reason: string;              // skipped = true 时的原因, 否则空串
}

// 查表: 没列出的项一律按 < 20 MB 计
export function itemMemEstimateMb(name: string): number {
  for (const e of MEM_EST) {
    if (e.name === name) {
      return e.mb;
    }
  }
  return MEM_EST_DEFAULT_MB;
}

// 单项执行结果(内部使用): 闸门跳过时 skipped = true, 此时 result 为 null
export interface Gb7ItemOutcome {
  index: number;
  name: string;
  skipped: boolean;
  skipReason: string;
  gate: MemGateInfo;
  result: Gb7Result | null;
}

// GB7 单项"耗时异常"判定线: 超过它就在该行标红(提示可能卡死/降频), 不改动任何数值与计分
export const SLOW_ITEM_MS = 8000;

// 子项划分与权重(本工程自定): 整数与系统类 65% / 浮点与媒体类 35%
const INTEGER_TESTS: string[] = ['数据压缩', '路径查找', '文本处理', 'AI 推理'];
const FLOAT_TESTS: string[] = ['图像处理', '物理模拟', '内存带宽', '音频编解码'];
const W_INTEGER = 0.65;
const W_FLOAT = 0.35;

function subsectionScore(tests: TestResult[], names: string[]): number {
  const vals: number[] = [];
  for (const t of tests) {
    for (const n of names) {
      if (t.name === n && t.score > 0) {
        vals.push(t.score);
      }
    }
  }
  if (vals.length === 0) {
    return 0;
  }
  return geomean(vals);
}

// 子项加权算术平均(两个子项按上面的权重合成) → 一个复合分
function compositeFromSubsections(intScore: number, floatScore: number): number {
  if (intScore > 0 && floatScore > 0) {
    return Math.round(W_INTEGER * intScore + W_FLOAT * floatScore);
  }
  if (intScore > 0) {
    return Math.round(intScore);
  }
  return Math.round(floatScore);
}

function geomean(values: number[]): number {
  let sum = 0;
  for (const v of values) {
    sum += Math.log(v);
  }
  return Math.exp(sum / values.length);
}

function sleep(ms: number): Promise<void> {
  return new Promise<void>((resolve: () => void) => setTimeout(resolve, ms));
}

// ---------------------------------------------------------------------------
// "跑在哪个核 / 哪个簇"(native runGb7 上报的诊断键): 旁路诊断, 不参与任何计算与计分。
// 判据(2026-10 起): "绑核是否生效"看 cpuInFastCluster —— native 把线程绑到大核簇
// (最高频档的全部核, >= 2 个), 而不是钉死在唯一一个最快核, 所以簇内迁移是无害的,
// 不再用 "cpu != cpuAtStart" / "cpuRank > 0" 报警。
// ---------------------------------------------------------------------------

// 这些诊断键的中间结果(便于精确定义类型, 不用 any/unknown)
interface Gb7Partial {
  cpu: number;
  cpuMaxKhz: number;
  cpuRank: number;
  cpuBound: boolean;
  cpuAtStart: number;
  cpuInFastCluster: boolean;
  cpuAtStartInFastCluster: boolean;
  cpuFastClusterCores: number;
  cpuFastClusterMaxKhz: number;
  cpuFastClusterMask: string;
  cpuWorkers: number;
  cpuWorkersOverflow: number;
  cpuInfo: string;
  // 内核允许的核集合 + 两个分开的判据(2026-10-05)
  cpuAllowedOk: boolean;
  cpuAllowedCount: number;
  cpuAllowedMask: string;
  cpuAppliedMask: string;
  cpuFastClusterFallback: boolean;
  cpuFastClusterTierIndex: number;
  cpuInFastClusterJudged: boolean;
  cpuInMachineTopTier: boolean;
  cpuMachineTopTierCores: number;
  cpuMachineTopTierKhz: number;
  cpuMachineTopTierMask: string;
  cpuFastClusterSource: string;
  cpuAllowedText: string;
  diag: string;
  runFreq: string;
  // ---- QoS 运行条件 / 只读分层诊断(cgroup·cpuset·core_ctl)(2026-10 新增) ----
  // 与 runFreq / diag 完全同类: native 的结构化旁路字段, 不参与 metric / unit / k /
  // conv / 计分公式, 也不改变任何负载的算法与尺寸。
  //   qos    <- runGb7 JSON 的 "qos"    键(= qos_priority.h 记下的全部事实: 是否启用 /
  //             负载与旁路各自哪一档 / 每个调用的返回值与 errno / libqos.so 与 canIUse 的
  //             探测结果 / "设-不设"对照的份额比);
  //   cpuset <- runGb7 JSON 的 "cpuset" 键(= cpuset_probe.h 的只读分层诊断全文: /proc/self/cgroup ·
  //             /dev/cpuset 逐组 cpus 与 cpus_perms · cpu/online 与 possible · cpuN/core_ctl 六个文件)
  // 为什么必须补齐(2026-10 真机报告缺陷 2): native 一直算着这两个字段、napi 也一直在输出,
  //   但 ArkTS 的 Gb7Raw/Gb7Partial/Gb7Result 没有这两个键 -> 解析器读不到 -> 报告 JSON 的
  //   items[] 里既没有 qos 也没有 cpuset, 对比工具(verify_report_compare.py / 对比页)与历史
  //   记录因此看不到"这台机器的限制在哪一层""QoS 到底生效没有"的证据。现在逐层接到底。
  qos: string;
  cpuset: string;
  cpuBoundReason: string;
  cpuLogical: number;
  cpuPhysical: number;
  cpuSmtPossible: boolean;
  cpuSmtEnabled: boolean;
  cpuTopoKnown: boolean;
  cpuThreadsRequested: number;
  cpuThreadsEffective: number;
  cpuEffMask: string;
  cpuTopoSource: string;
}

// 新 Gb7Result 的默认值: 既不写 undefined, 也不改变任何已有字段的算法。
// GPU 路径 / GPU 初始化失败项 / 老 native 都走这里(默认即"未知/未绑定/无说明")。
function cpuNeutralResult(name: string, score: number, metric: number, unit: string, ms: number): Gb7Result {
  return {
    name: name,
    score: score,
    metric: metric,
    unit: unit,
    ms: ms,
    cpu: -1,
    cpuMaxKhz: 0,
    cpuRank: -1,
    cpuBound: false,
    cpuAtStart: -1,
    // 簇字段的默认值 = "没上报": bound=false 时调用方必须忽略它们(不报警)
    cpuInFastCluster: false,
    cpuAtStartInFastCluster: false,
    cpuFastClusterCores: 0,
    cpuFastClusterMaxKhz: 0,
    cpuFastClusterMask: '',
    cpuWorkers: 0,
    cpuWorkersOverflow: 0,
    cpuInfo: '',
    // 允许核集合 / 判据字段的默认值一律是"没上报"(空串 / 0 / false),
    // 界面据此区分"没上报"与"上报了但为空" —— 不允许用假值冒充读数。
    cpuAllowedOk: false,
    cpuAllowedCount: 0,
    cpuAllowedMask: '',
    cpuAppliedMask: '',
    cpuFastClusterFallback: false,
    cpuFastClusterTierIndex: 0,
    cpuInFastClusterJudged: false,
    cpuInMachineTopTier: false,
    cpuMachineTopTierCores: 0,
    cpuMachineTopTierKhz: 0,
    cpuMachineTopTierMask: '',
    cpuFastClusterSource: '',
    cpuAllowedText: '',
    diag: '',
    runFreq: '',
    // QoS / 只读分层诊断默认 = "没上报"(空串): GPU 路径 / 自研套件 / 失败项 / 老 native 都走这里,
    // 界面与报告据此区分"没上报"与"上报了但为空", 不用假值冒充读数。
    qos: '',
    cpuset: '',
    cpuBoundReason: '',
    // SMT 字段的默认值 = "没上报": 界面与 runlog 一律不据此报警(不假装有 SMT, 也不假装没有)
    cpuLogical: 0,
    cpuPhysical: 0,
    cpuSmtPossible: false,
    cpuSmtEnabled: true,
    cpuTopoKnown: false,
    cpuThreadsRequested: 1,
    cpuThreadsEffective: 0,
    cpuEffMask: '',
    cpuTopoSource: '',
    // 可重复性 / 离散度: 默认 = "没上报"(0 / 空串)。GPU 路径、失败项、老 native 走这里,
    // 界面看到空 verdict 必须显示"未上报", 不能显示成"稳定"。
    rounds: 0,
    roundsOk: 0,
    scoreMedian: 0,
    scoreSpreadPct: 0,
    repeatVerdict: '',
    repeatText: '',
    // 默认 = 中性(按政策不计分): GPU / 自研套件 / 旧 native 都不会被误标成"运行失败"。
    // 只有 CS1 单核 / 多核路径会用 native 注册表的 scored 覆盖它(见 gb7RunOne / runGb7Multi)。
    scoredByPolicy: false,
    // 负载 id / 跨芯片可比性自检: 默认 = "未上报 / 没做这项自检"。
    // CS1 单核 / 多核路径会填 loadId(见 gb7RunOne / runGb7Multi), 自检结论由
    // service/ScalingAudit.ets 在每个阶段跑完之后原地写回(它是纯粹的旁路结论,
    // 不参与任何 metric / unit / k / conv / 计分公式, 也不改变任何负载)。
    loadId: -1,
    scalingVerdict: '',
    scalingEvidence: '',
    // GPU 跑满判据: 默认 "未上报"(只有 CS1 GPU 路径会填; 见 runGb7Gpu)
    gpuFullness: ''
  };
}

// 从 runGb7 的 JSON 里读出这些诊断键(缺字段一律回落默认值, 不写 undefined)
function cpuFieldsFrom(o: Record<string, Object>): Gb7Partial {
  const out: Gb7Partial = {
    cpu: (o.cpu as number) ?? -1,
    cpuMaxKhz: (o.cpuMaxKhz as number) ?? 0,
    cpuRank: (o.cpuRank as number) ?? -1,
    cpuBound: (o.cpuBound as boolean) ?? false,
    cpuAtStart: (o.cpuAtStart as number) ?? -1,
    cpuInFastCluster: (o.cpuInFastCluster as boolean) ?? false,
    cpuAtStartInFastCluster: (o.cpuAtStartInFastCluster as boolean) ?? false,
    cpuFastClusterCores: (o.cpuFastClusterCores as number) ?? 0,
    cpuFastClusterMaxKhz: (o.cpuFastClusterMaxKhz as number) ?? 0,
    cpuFastClusterMask: (o.cpuFastClusterMask as string) ?? '',
    cpuWorkers: (o.cpuWorkers as number) ?? 0,
    cpuWorkersOverflow: (o.cpuWorkersOverflow as number) ?? 0,
    cpuInfo: (o.cpuInfo as string) ?? '',
    cpuAllowedOk: (o.cpuAllowedOk as boolean) ?? false,
    cpuAllowedCount: (o.cpuAllowedCount as number) ?? 0,
    cpuAllowedMask: (o.cpuAllowedMask as string) ?? '',
    cpuAppliedMask: (o.cpuAppliedMask as string) ?? '',
    cpuFastClusterFallback: (o.cpuFastClusterFallback as boolean) ?? false,
    cpuFastClusterTierIndex: (o.cpuFastClusterTierIndex as number) ?? 0,
    cpuInFastClusterJudged: (o.cpuInFastClusterJudged as boolean) ?? false,
    cpuInMachineTopTier: (o.cpuInMachineTopTier as boolean) ?? false,
    cpuMachineTopTierCores: (o.cpuMachineTopTierCores as number) ?? 0,
    cpuMachineTopTierKhz: (o.cpuMachineTopTierKhz as number) ?? 0,
    cpuMachineTopTierMask: (o.cpuMachineTopTierMask as string) ?? '',
    cpuFastClusterSource: (o.cpuFastClusterSource as string) ?? '',
    cpuAllowedText: (o.cpuAllowedText as string) ?? '',
    diag: (o.diag as string) ?? '',
    runFreq: (o.runFreq as string) ?? '',
    // napi 一直在输出这两个键(runGb7 JSON 的 "qos" / "cpuset"), 这里以前没有读 -> 整条
    //   证据链在 ArkTS 侧断掉(报告 JSON 的 items[] 里看不到)。缺字段/老 native 一律回落到
    //   空串, 不写 undefined, 也不编造。
    qos: (o.qos as string) ?? '',
    cpuset: (o.cpuset as string) ?? '',
    cpuBoundReason: (o.cpuBoundReason as string) ?? '',
    cpuLogical: (o.cpuLogical as number) ?? 0,
    cpuPhysical: (o.cpuPhysical as number) ?? 0,
    cpuSmtPossible: (o.cpuSmtPossible as boolean) ?? false,
    cpuSmtEnabled: (o.cpuSmtEnabled as boolean) ?? true,
    cpuTopoKnown: (o.cpuTopoKnown as boolean) ?? false,
    cpuThreadsRequested: (o.cpuThreadsRequested as number) ?? 1,
    cpuThreadsEffective: (o.cpuThreadsEffective as number) ?? 0,
    cpuEffMask: (o.cpuEffMask as string) ?? '',
    cpuTopoSource: (o.cpuTopoSource as string) ?? ''
  };
  return out;
}

// 该核(或大核簇)最高频率的显示文本(kHz -> MHz; 0 = 未知)
function cpuMhzText(khz: number): string {
  if (khz <= 0) {
    return '';
  }
  return Math.round(khz / 1000).toString() + 'MHz';
}

// 单项 note 的"跑在哪个核 / 哪个簇"后缀(接在 itemNote 后面, 不覆盖原有诊断文本)。
// 判据只有一条: cpuBound === true && cpuInFastCluster === false 才算异常。
//   cpuBound = false(读不到频率表) -> 忽略所有簇字段(此时 cpuInFastCluster 恒为 false, 不报警)
//   簇内迁移(如 cpu4 → cpu7)        -> 不算异常(两端本来就在同一个频率档里, 属无害迁移)
//   cpuRank                         -> 只是附加信息, 不再参与判据
function cpuNote(r: Gb7Result): string {
  const smt: string = smtPhaseText(r);
  // 允许核集合那一小段(只有 native 上报了才加; 没上报就完全不加, 免得出现"allowed=0")
  let allow: string = '';
  if (r.cpuAllowedText.length > 0) {
    allow = ' · ' + r.cpuAllowedText;
  } else if (r.cpuAllowedOk && r.cpuAllowedCount > 0) {
    allow = ' · 内核允许核集合 ' + r.cpuAllowedCount.toString() + ' 核(' + r.cpuAllowedMask + ')';
  }
  // "没跑在全机最快档"是独立于绑核判据的一条事实: 只提示, 不报警, 也不改任何数值。
  let tier: string = '';
  if (r.cpuInMachineTopTier) {
    tier = ' · 在全机最快档内';
  } else if (r.cpuMachineTopTierCores > 0) {
    const tMhz: string = cpuMhzText(r.cpuMachineTopTierKhz);
    tier = ' · 不在全机最快档内(' + r.cpuMachineTopTierCores.toString() + ' 核' +
      (tMhz.length > 0 ? '/' + tMhz : '') + ' 才是全机最快档)';
  }
  if (r.cpuBound && r.cpu >= 0) {
    if (!r.cpuInFastCluster) {
      // 异常: 掩码应用成功, 但负载跑完时落在了生效快簇之外
      let bad: string = ' · 注意: 跑完落在核外(cpu=' + r.cpu.toString();
      const wMhz: string = cpuMhzText(r.cpuMaxKhz);
      if (wMhz.length > 0) {
        bad += ', ' + wMhz;
      }
      return bad + ', 不在快簇内)' + smt + tier + allow;
    }
    // 正常: 跑完时仍在生效快簇内(native 保证簇内 >= 2 个核, 簇内迁移是允许的)
    let ok: string = ' · 快簇已生效(cpu=' + r.cpu.toString();
    if (r.cpuFastClusterCores > 0) {
      ok += ', ' + r.cpuFastClusterCores.toString() + ' 核簇';
    }
    const fMhz: string = cpuMhzText(r.cpuFastClusterMaxKhz);
    if (fMhz.length > 0) {
      ok += '/最高' + fMhz;
    }
    if (r.cpuFastClusterTierIndex > 0) {
      ok += ', 但低于全机最快档 ' + r.cpuFastClusterTierIndex.toString() + ' 档';
    }
    ok += ')';
    if (r.cpuWorkersOverflow > 0) {
      ok += ' · 池线程 ' + r.cpuWorkersOverflow.toString() + ' 个因位次冲突改到其它位次';
    }
    return ok + smt + tier + allow;
  }
  if (r.cpuBound) {
    // 掩码应用成功, 但连"当前在哪个核"都取不到: 不能写成"未绑定", 否则会误导判读
    return ' · 已绑定快簇(cpu 编号取不到)' + smt + allow;
  }
  // 未绑定: 三种原因必须分开写, 否则一个"未绑定"会把三种完全不同的情况糊成一句
  //   ① 绑核目标不在可用核集合内(交集为空) —— 系统策略决定的, 已经绑到可用核集合里最好的位置;
  //   ② 读不到 cpuinfo_max_freq —— 读数失败, 已整个降级;
  //   ③ 其它(没有可用绑定目标)。
  let s2: string;
  if (r.cpuBoundReason.length > 0) {
    s2 = ' · ' + r.cpuBoundReason;
  } else {
    s2 = ' · 未绑定(读不到 cpufreq, 已降级)';
  }
  if (r.cpu >= 0) {
    s2 += ' · 负载线程 cpu=' + r.cpu.toString();
  }
  const mhz: string = cpuMhzText(r.cpuMaxKhz);
  if (mhz.length > 0) {
    s2 += '(最高' + mhz + ')';
  }
  if (r.cpuAppliedMask.length > 0) {
    s2 += ' · 实际应用掩码 ' + r.cpuAppliedMask;
  }
  if (r.cpuWorkers > 0) {
    s2 += ' · 池线程 ' + r.cpuWorkers.toString() + ' 个已绑定';
  }
  return s2 + smt + tier + allow;
}

// ---------------------------------------------------------------------------
// 仅用于 runlog.jsonl 的 note 字段: 把"这一项为什么没分/为什么跳过"写成一行诊断串。
// 旁路文本, 不参与任何计算、不改变任何项的执行。
// ---------------------------------------------------------------------------
function itemNote(skipped: boolean, skipReason: string, unit: string, score: number, ms: number): string {
  if (skipped) {
    return skipReason;
  }
  let s: string = '';
  if (score <= 0) {
    s = '未计分/失败: ' + (unit.length > 0 ? unit : 'native 未返回原因');
  } else if (ms <= 0) {
    s = '耗时未上报: ' + (unit.length > 0 ? unit : 'native 未返回耗时');
  }
  if (ms > SLOW_ITEM_MS) {
    s = (s.length > 0 ? (s + ' | ') : '') + '单项耗时 > ' + SLOW_ITEM_MS.toString() +
      ' ms（可能卡死或系统降频，仅提示，数值与计分不变）';
  }
  return s;
}

// ---------------------------------------------------------------------------
// 超线程(SMT)快照: 拓扑(逻辑核 / 物理核 / 来源) + 开关状态 + 多核阶段线程数
// 全部来自 native 的 smtCapabilities() 一行 JSON; 解析失败 -> null(按"默认开"处理, 不猜)。
// 这是旁路信息 + 线程数口径, 不参与任何负载的算法 / 工作量 / 计分。
// ---------------------------------------------------------------------------
export interface SmtCapabilities {
  ok: boolean;
  known: boolean;              // 拓扑是否已知(未知 = 已按 1:1 处理, 开关无效)
  logical: number;             // 逻辑核数
  physical: number;            // 物理核数(未知时 = logical)
  smt: boolean;                // 物理核数 < 逻辑核数
  smtPossible: boolean;        // 同上(显式命名)
  smtEnabled: boolean;         // 开关状态(默认 true)
  threadsMulti: number;        // 多核阶段应当使用的线程数
  requestedAsIs: boolean;      // 用户请求的线程数是否可以原样传下去
  effectiveMask: string;       // 实际使用集合位图
  physicalOfCpu: number[];     // 逻辑核 -> 物理核编号
  source: string;              // 拓扑来源
  probes: string;              // 逐路径实测结果(读到了什么 + errno)
  text: string;                // 一行中文说明
}


// ---------------------------------------------------------------------------
// 内核允许的核集合快照(native cpuAffinityProbe() 一行 JSON 的解析结果)
//
// 与 SmtCapabilities / ChipIdentityProbe 的分工:
//   SmtCapabilities  = "用哪些核"的口径(逻辑核 / 物理核 / SMT 开关 / 线程数);
//   ChipIdentity     = "这台机器是什么芯片"(逐核原始频率 + MIDR);
//   CpuAffinitySnapshot = "内核允不允许我们用最快的那几个核" —— 用户质疑"代际差异没体现"
//     时, 这一份就是第一手答案: 全机最快档在哪些核上、可用核集合是哪些核、两者的交集是什么。
// 全部只读、旁路: 不参与任何测试项的计算、顺序与计分。
// ---------------------------------------------------------------------------
export interface CpuAffinitySnapshot {
  ok: boolean;
  // ---- 内核允许的核集合 ----
  allowedOk: boolean;          // false = 读不到(此时绑核退回全机频率排名, 不做过滤)
  allowedCount: number;        // 可用核集合里的核数(0 = 读不到)
  allowedMask: string;         // 可用核集合位图, 形如 "0x3fff"
  allowedList: string;         // 可用核集合的 cpulist 文本, 形如 "0-9"
  inThreadMask: boolean;       // true = 这份掩码来自 sched_getaffinity(本线程)
  mismatched: boolean;         // true = Cpus_allowed 与 sched_getaffinity 不一致(硬事实)
  statusMaskOk: boolean;       // /proc/self/status 的 Cpus_allowed 是否可读
  statusErrno: number;         // 打开 /proc/self/status 失败时的 errno(0 = 成功)
  statusListFound: boolean;    // 是否找到 Cpus_allowed_list 行
  syscallOk: boolean;          // sched_getaffinity 是否成功
  syscallErrno: number;        // 它的 errno
  memsFound: boolean;          // 是否读到 Mems_allowed_list
  memsList: string;            // Mems_allowed_list 原文
  cgroupPath: string;          // /proc/self/cgroup 原文(或读出它的 errno)
  cpusetDir: string;           // 推出的 cpuset 目录(空 = 推不出来)
  cpusetCpus: string;          // cpuset.cpus 原文
  cpusetCpusErrno: number;     // 读它的 errno
  cpusetEffective: string;     // cpuset.cpus.effective 原文
  cpusetEffErrno: number;      // 读它的 errno
  source: string;              // 可用核集合的来源一行(中文)
  text: string;                // 可用核集合 + cpuset + Mems 的一行完整文本
  // ---- 生效快簇 / 全机最快档(两个概念分开) ----
  fastFallback: boolean;       // true = 交集为空, 快簇退化成"可用核集合里最快的核"
  fastTierIndex: number;       // 生效快簇落在全机的第几个频率档(0 = 全机最快档)
  fastCores: number;           // 生效快簇的核数
  fastMaxKhz: number;          // 生效快簇的最高频率(kHz)
  fastMask: string;            // 生效快簇位图
  fastSource: string;          // 快簇是怎么算出来的(中文一行)
  machineTopTierCores: number; // 全机最快频率档的核数
  machineTopTierKhz: number;   // 全机最快频率档的频率(kHz)
  machineTopTierMask: string;  // 全机最快档位图
  // ---- 线程口径 ----
  threadCap: number;           // 多核阶段实际线程数上限(<= 0 = 不限制)
  threadBasisFull: number;     // 不看可用核集合时(全机口径)应有的线程数
  line: string;                // 面向「设备画像」的一行结论(可直接对比两台设备)
}

// native JSON -> 强类型快照(缺字段一律回落默认值, 不写 undefined)
function affinityFrom(o: Record<string, Object>): CpuAffinitySnapshot {
  const al = (o.allowed as Record<string, Object>) ?? ({} as Record<string, Object>);
  const fast = (o.fast as Record<string, Object>) ?? ({} as Record<string, Object>);
  const th = (o.threads as Record<string, Object>) ?? ({} as Record<string, Object>);
  return {
    ok: (o.ok as boolean) ?? false,
    allowedOk: (al.ok as boolean) ?? false,
    allowedCount: (al.count as number) ?? 0,
    allowedMask: (al.mask as string) ?? '',
    allowedList: (o.list as string) ?? '',
    inThreadMask: (al.inThreadMask as boolean) ?? false,
    mismatched: (al.mismatched as boolean) ?? false,
    statusMaskOk: (al.statusMaskOk as boolean) ?? false,
    statusErrno: (al.statusErrno as number) ?? 0,
    statusListFound: (al.statusListFound as boolean) ?? false,
    syscallOk: (al.syscallOk as boolean) ?? false,
    syscallErrno: (al.syscallErrno as number) ?? 0,
    memsFound: (al.memsFound as boolean) ?? false,
    memsList: (o.memsList as string) ?? '',
    cgroupPath: (o.cgroupPath as string) ?? '',
    cpusetDir: (o.cpusetDir as string) ?? '',
    cpusetCpus: (o.cpusetCpus as string) ?? '',
    cpusetCpusErrno: (al.cpusetCpusErrno as number) ?? 0,
    cpusetEffective: (o.cpusetEffective as string) ?? '',
    cpusetEffErrno: (al.cpusetEffErrno as number) ?? 0,
    source: (o.source as string) ?? '',
    text: (o.text as string) ?? '',
    fastFallback: (fast.fallback as boolean) ?? false,
    fastTierIndex: (fast.tierIndex as number) ?? 0,
    fastCores: (fast.cores as number) ?? 0,
    fastMaxKhz: (fast.maxKhz as number) ?? 0,
    fastMask: (fast.mask as string) ?? '',
    fastSource: (o.fastSource as string) ?? '',
    machineTopTierCores: (fast.machineTopTierCores as number) ?? 0,
    machineTopTierKhz: (fast.machineTopTierKhz as number) ?? 0,
    machineTopTierMask: (fast.machineTopTierMask as string) ?? '',
    threadCap: (th.cap as number) ?? 0,
    threadBasisFull: (th.basisFull as number) ?? 0,
    line: (o.line as string) ?? ''
  };
}
// ---------------------------------------------------------------------------
// 设备画像「内核允许的核集合」一段的数据载体(只有文本与标志位, 没有任何布局语义)
//
// 给页面用: label = 左侧字段名, text = 右侧内容(已是 native 原话, 页面不要再包装),
//          warn = true 时建议用警告色(交集为空 / 生效快簇低于全机最快档 / 线程数被夹)。
// 旁路: 不参与任何测试项的计算、顺序与计分。
// ---------------------------------------------------------------------------
export interface CpuAffinityPanelRow {
  label: string;
  text: string;
  warn: boolean;
}

export interface CpuAffinityPanelData {
  available: boolean;        // false = native 未上报(旧 .so / 解析失败), 此时 rows 里只有一条说明
  restricted: boolean;       // true = 内核不允许我们用最快档(交集为空)或生效快簇低于全机最快档
  warn: boolean;             // 建议给整段用警告色(与 restricted 同义, 单独留一个字段便于日后分化)
  rows: CpuAffinityPanelRow[];   // 建议按序渲染: 允许核集合 / 快簇/来源 / 内核限制 / 线程上限
  copyText: string;          // 复制用的一行(结论 + 快簇来源)
}


// ---------------------------------------------------------------------------
// 芯片判读探测的快照(native cpuIdentityProbe() 一行 JSON 的解析结果)
//
// 与 SmtCapabilities 的分工:
//   SmtCapabilities  = "用哪些核"的口径(开关 / 线程数 / 掩码);
//   ChipIdentity     = "这台机器到底是什么芯片"的取证: 逐核原始频率 + /proc/cpuinfo 的 MIDR。
// 前者影响行为(线程数), 后者旁路: 不参与任何测试项的计算、顺序与计分。
// ---------------------------------------------------------------------------
export interface ChipFreqCoreSample {
  cpu: number;
  raw: string;
  khz: number;
  err: number;
}

export interface ChipFreqTier {
  khz: number;
  count: number;
}

export interface ChipMidrEntry {
  implementer: string;
  architecture: string;
  variant: string;
  part: string;
  partDecimal: number;
  revision: string;
  count: number;
}

export interface ChipIdentityProbe {
  ok: boolean;
  topoLogical: number;
  topoPhysical: number;
  topoKnown: boolean;
  rangeSource: number;
  rangeErrno: number;
  scanCount: number;
  okCount: number;
  firstFailCpu: number;
  firstFailErr: number;
  tableCores: number;
  tableMaxKhz: number;
  tableMaxCpu: number;
  probeMaxKhz: number;
  probeMaxCpu: number;
  truncated: boolean;
  missedFaster: boolean;
  physCount: number;
  range: string;
  perCore: string;
  verdict: string;
  cores: ChipFreqCoreSample[];
  tiers: ChipFreqTier[];
  physTiers: ChipFreqTier[];
  cpuinfoOk: boolean;
  cpuinfoErrno: number;
  processorLines: number;
  cpuinfoText: string;
  midr: ChipMidrEntry[];
}

// 一次判读的最终产物(两行文本 + 逐核原始读数)
export interface ChipJudgmentSnapshot {
  ok: boolean;            // false = native 不可用 / JSON 解析失败(此时 note 说明原因)
  note: string;
  measured: string;       // 第一行: 本机实测(设备自报)
  infer: string;          // 第二行: 第三方资料的型号推断(疑似/推断措辞)
  perCore: string;        // 第三行: 逐核 cpuinfo_max_freq 原始读数
  dataSufficient: boolean;
  topName: string;
  probe: ChipIdentityProbe | null;
}

// native JSON -> 强类型快照(缺字段一律回落默认值, 不写 undefined)
function chipProbeFrom(o: Record<string, Object>): ChipIdentityProbe {
  const freq = (o.freq as Record<string, Object>) ?? ({} as Record<string, Object>);
  const topo = (o.topo as Record<string, Object>) ?? ({} as Record<string, Object>);
  const ci = (o.cpuinfo as Record<string, Object>) ?? ({} as Record<string, Object>);
  const coresRaw = (freq.cores as ChipFreqCoreSample[]) ?? [];
  const tiersRaw = (freq.tiers as ChipFreqTier[]) ?? [];
  const physRaw = (freq.physTiers as ChipFreqTier[]) ?? [];
  const midrRaw = (ci.midr as ChipMidrEntry[]) ?? [];
  return {
    ok: (o.ok as boolean) ?? false,
    topoLogical: (topo.logical as number) ?? 0,
    topoPhysical: (topo.physical as number) ?? 0,
    topoKnown: (topo.known as boolean) ?? false,
    rangeSource: (freq.rangeSource as number) ?? -1,
    rangeErrno: (freq.rangeErrno as number) ?? 0,
    scanCount: (freq.scanCount as number) ?? 0,
    okCount: (freq.okCount as number) ?? 0,
    firstFailCpu: (freq.firstFailCpu as number) ?? -1,
    firstFailErr: (freq.firstFailErr as number) ?? 0,
    tableCores: (freq.tableCores as number) ?? 0,
    tableMaxKhz: (freq.tableMaxKhz as number) ?? 0,
    tableMaxCpu: (freq.tableMaxCpu as number) ?? -1,
    probeMaxKhz: (freq.probeMaxKhz as number) ?? 0,
    probeMaxCpu: (freq.probeMaxCpu as number) ?? -1,
    truncated: (freq.truncated as boolean) ?? false,
    missedFaster: (freq.missedFaster as boolean) ?? false,
    physCount: (freq.physCount as number) ?? 0,
    range: (freq.range as string) ?? '',
    perCore: (freq.perCore as string) ?? '',
    verdict: (freq.verdict as string) ?? '',
    cores: coresRaw,
    tiers: tiersRaw,
    physTiers: physRaw,
    cpuinfoOk: (ci.ok as boolean) ?? false,
    cpuinfoErrno: (ci.errno as number) ?? 0,
    processorLines: (ci.processorLines as number) ?? 0,
    cpuinfoText: (ci.text as string) ?? '',
    midr: midrRaw
  };
}

// 多核阶段 note / 汇总里要带上的 SMT 口径文本(纯展示)。
//   r.cpuThreadsEffective <= 0  -> 空串(没上报: 老 native / GPU 路径都不加这一句)
//   拓扑未知                     -> " · N 线程（逻辑核, 拓扑未知, SMT 开关无效）"
//   检测到 SMT 且开              -> " · N 线程（逻辑核, SMT 开）"
//   检测到 SMT 且关              -> " · N 线程（物理核, SMT 关）"
//   没有 SMT                     -> " · N 线程（无 SMT）"
export function smtPhaseText(r: Gb7Result): string {
  if (r.cpuThreadsEffective <= 0) {
    return '';
  }
  const n: number = r.cpuThreadsEffective;
  if (!r.cpuTopoKnown) {
    return ' · ' + n.toString() + ' 线程（逻辑核, 拓扑未知, SMT 开关无效）';
  }
  if (r.cpuSmtPossible) {
    return ' · ' + n.toString() + ' 线程（' + (r.cpuSmtEnabled ? '逻辑核, SMT 开' : '物理核, SMT 关') + '）';
  }
  return ' · ' + n.toString() + ' 线程（无 SMT）';
}

// 「设备画像」那一行: CPU 拓扑。四种形态(第三种是"读不到"时的写法, 不假装):
//   检测到 SMT: "CPU 拓扑：14 逻辑核 / 7 物理核（检测到 SMT, 当前已启用）· 多核阶段 14 线程（逻辑核, SMT 开）· 来源 thread_siblings_list(…)"
//   无 SMT    : "CPU 拓扑：14 逻辑核 / 14 物理核（无 SMT, 开关无差别）· 多核阶段 14 线程 · 来源 thread_siblings_list(…)"
//   未知      : "CPU 拓扑：拓扑未知（已按 1:1 处理, SMT 开关无效）：逻辑核 14 / 物理核 14…"
//   没上报    : "CPU 拓扑：（native 未上报）"
export function smtTopologyLine(c: SmtCapabilities | null): string {
  if (c === null) {
    return 'CPU 拓扑：（native 未上报）';
  }
  if (c.text.length > 0) {
    return 'CPU 拓扑：' + c.text;
  }
  if (!c.known) {
    return 'CPU 拓扑：拓扑未知（已按 1:1 处理, SMT 开关无效）：逻辑核 ' + c.logical.toString() +
      ' / 物理核 ' + c.physical.toString();
  }
  if (c.smtPossible) {
    return 'CPU 拓扑：' + c.logical.toString() + ' 逻辑核 / ' + c.physical.toString() +
      ' 物理核（检测到 SMT, 当前' + (c.smtEnabled ? '已启用' : '已关闭') + '）→ 多核阶段 ' +
      c.threadsMulti.toString() + ' 线程（' + (c.smtEnabled ? '逻辑核, SMT 开' : '物理核, SMT 关') +
      '）· 来源 ' + c.source;
  }
  return 'CPU 拓扑：' + c.logical.toString() + ' 逻辑核 / ' + c.physical.toString() +
    ' 物理核（无 SMT, 开关无差别）→ 多核阶段 ' + c.threadsMulti.toString() + ' 线程 · 来源 ' + c.source;
}

// ===========================================================================
//  NPU(AI 加速器) / 存储 I/O —— 两个独立小节的结果类型
//
//   这两个小节的结果不进入 CS1 单项分 / 复合分 
//    GB7 里没有 NPU 项、也没有存储项, 混进去会直接毁掉与正版 GB7 的可比性。
//    native 返回的 JSON 里带 scored:false / gb7Item:false, 这里原样透出。
//   失败不静默成 0: 每个结果都带 ok / status / error / text, 界面必须显示原因。
//  原生 JSON 的完整字段说明见 entry/src/main/cpp/npu_bench.h 与 storage_bench.h。
// ===========================================================================

/** NPU 设备探测里的一台设备(npuProbe 的 devices[])。*/
export interface NpuDeviceRow {
  id: number;
  name: string;
  typeName: string;   // CPU / GPU / ACCELERATOR / OTHERS
  typeEnum: string;   // OH_NN_XXX(n) 原始枚举
}

/** npuProbe() 的解析结果。status 取值见 npu_bench.h。*/
export interface NpuProbeResult {
  ok: boolean;
  status: string;
  text: string;              // 一行中文结论, 可直接显示
  error: string;
  thirdPartyNpuOpen: boolean; // 关键结论 这台机器是否向第三方应用开放了 NPU
  deviceCount: number;
  devices: NpuDeviceRow[];
  hasCpu: boolean;
  hasGpu: boolean;
  hasAccelerator: boolean;
  nnrtAvailable: boolean;    // NNRt 接口是否拿到(dlopen + dlsym 运行时绑定)
  nnrtBound: number;
  nnrtExpected: number;
  nnrtBinding: string;
  missingSymbols: string;
  runtimeDlerror: string;
  notes: string;
  raw: string;
}

/** npuBench() 的解析结果。单位与口径全部来自 native, 这里不改写任何数字。*/
export interface NpuRunResult {
  ok: boolean;
  status: string;
  text: string;
  error: string;
  notes: string;
  // ---- 实际使用的设备(这一节最重要的元信息) ----
  deviceTypeName: string;    // ACCELERATOR / GPU / CPU —— 回退后的真实设备类型
  deviceLabel: string;
  deviceId: number;
  deviceName: string;
  fallbackText: string;
  attemptCount: number;
  // ---- 模型与算子口径 ----
  scaleLabel: string;
  opSetName: string;
  mainOp: string;
  opSetFallbackUsed: boolean;
  opSetText: string;
  macsPerInference: number;
  flopsPerInference: number;
  macFormula: string;
  precisionShort: string;
  // ---- 计时(稳态) ----
  timingAvailable: boolean;
  meanMs: number;
  minMs: number;
  medianMs: number;
  maxMs: number;
  goPs: number;              // GOPs/s = FLOPs / 平均延迟 / 1e9
  macsPerSec: number;
  measuredRuns: number;
  warmupRuns: number;
  budgetHit: boolean;
  hint: string;
  raw: string;
}

/** GPU-SNL(Aurora Nomad Light)可重复性里的一个轮次。 */
export interface SnRoundRow {
  round: number;
  ok: boolean;
  passesPerFrame: number;
  fps: number;
  perPassMs: number;
  throughputMpxPerSec: number;
  score: number;
  tempMedian: number;
  cpuKhzMedian: number;
  envState: string;
}

/** GPU-SNL 的公开真值对照行。 */
export interface SnRefRow {
  name: string;
  referenceScore: number;
  ourScore: number;
  deltaPercent: number;
  source: string;
}

/**
 * libaurorasn.so run() 的结果。字段名与 native 的 JSON 逐字一致, 本桥不改写任何数字与单位。
 * 关键: section="GPU-SNL" / scored=false / gb7Item=false —— 本小节不并入 CS1 或自研套件复合分。
 * prepError / driverText 是 prepare() 返回的驱动原文(空串 = 初始化成功), 界面必须原样显示。
 */
export interface SnRunResult {
  ok: boolean;
  error: string;
  lastError: string;
  prepError: string;
  driverText: string;
  // ---- "结论从哪来"的两个事实位(2026-10 真机报告缺陷 3 修) ----
  //   rawParseOk      = native 返回的原始 JSON 解析成功没有。false 表示这一节的所有
  //                     解析字段(score/fps/…)都是没取到的默认值, 而不是"读到了 0" ——
  //                     报告必须把这一点写出来, 否则读者会把 0 当成真读数。
  //   rawSelfClaimOk  = 原始 JSON 文本自己是不是以 "ok":true 开头。它只是"原文说了什么"
  //                     的记录, 不是本节的结论: 一份无法解析的 JSON 里写着 ok:true
  //                     不能证明本节成功。两者不一致时报告必须把矛盾写清楚(见 FullRun)。
  rawParseOk: boolean;
  rawSelfClaimOk: boolean;
  section: string;
  workload: string;
  scored: boolean;
  gb7Item: boolean;
  benchVersion: number;
  benchVersionText: string;
  score: number;
  scoreUnit: string;
  scoreFormula: string;
  kScorePerFps: number;
  fps: number;
  meanFrameMs: number;
  medianFrameMs: number;
  bestFrameMs: number;
  worstFrameMs: number;
  frameStability: number;
  measuredFrames: number;
  warmupFrames: number;
  throughputMpx: number;
  throughputUnit: string;
  pixelsPerFrame: number;
  pixelsPerPass: number;
  passesPerFrame: number;
  opsPerPixelPerPass: number;
  opsPerFrame: number;
  scaleSource: string;
  scaleAdaptive: boolean;
  scaleLadder: string;
  scaleTargetMs: number;
  scaleMaxPasses: number;
  scaleLimitReached: boolean;
  verdict: string;
  gpuBoundConclusive: boolean;
  drawCallsPerFrame: number;
  cpuVertexDataBytesPerFrame: number;
  submissionDuty: number;
  gpuBusyPercentOfFrame: number;
  syncPointsPerFrame: number;
  calibrationStatus: string;
  calibrationK: number;
  calibrationDerivation: string;
  ratioValidWithoutCalibration: boolean;
  affectsAbsoluteScoreOnly: boolean;
  cpuLogical: number;
  cpuPhysical: number;
  smtPossible: boolean;
  smtEnabled: boolean;
  threadCap: number;
  simdArch: number;
  simdNeon: boolean;
  simdSve: boolean;
  simdSve2: boolean;
  simdI8mm: boolean;
  repeats: number;
  credibility: string;
  dispersion: number;
  fpsMedian: number;
  fpsMin: number;
  fpsMax: number;
  scoreMedian: number;
  scoreMin: number;
  scoreMax: number;
  envState: string;
  tempMedian: number;
  tempMax: number;
  cpuKhzMedian: number;
  cpuKhzMax: number;
  rounds: SnRoundRow[];
  validityPass: boolean;
  glVersion: string;
  glRenderer: string;
  realUseShort: string;
  refDevice: string;
  refAnchor: string;
  refRows: SnRefRow[];
  warnings: string[];
  risks: string;
  notes: string;
  elapsedMs: number;
  raw: string;
}

/** 存储小节里的单项。哪几个指标存在取决于该项, 缺失的为 -1。*/
export interface StorageItemRow {
  id: string;
  name: string;
  ok: boolean;
  ms: number;
  bytes: number;
  mbps: number;
  iops: number;
  ops: number;
  auxMs: number;
  auxName: string;
  mode: string;
  comparable: string;        // native 给的可比性说明(口径是否与其它设备一致)
  error: string;
  note: string;
}

/** storageRun() 的解析结果。*/
export interface StorageRunResult {
  ok: boolean;
  skipped: boolean;
  skipReason: string;
  fatal: string;
  sectionTitle: string;
  dir: string;
  totalMs: number;
  budgetMs: number;
  stoppedEarly: boolean;
  stopReason: string;
  items: StorageItemRow[];
  warnings: string[];
  // ---- headline(与 native 一致, 不做任何换算) ----
  seqWriteDurableMbps: number;
  seqReadColdMbps: number;
  randRead4kIops: number;
  randWrite4kIops: number;
  filesPerSecCreate: number;
  filesPerSecDelete: number;
  // ---- 环境 ----
  totalBytes: number;
  freeBytes: number;
  availBytes: number;
  blockSize: number;
  oDirectWorks: boolean;
  fadviseDontNeedWorks: boolean;
  bypassMethod: string;
  raw: string;
}

export class BenchRunner {
  static cores(): number {
    return cpuCoreCount();
  }

  // =========================================================================
  // 超线程(SMT)桥 —— 只做三件事: 读拓扑、写开关、按开关算多核阶段的线程数
  //
  // 口径(与 native cpu_affinity.h 的文件头严格一致, 两边不许各写一套):
  //   * 开关只决定多核阶段使用哪些逻辑核(线程数 / 大核簇掩码 / 池线程落点);
  //     不改任何负载的算法、工作量、metric 口径与计分公式; 单核阶段完全不受影响;
  //   * 关掉时不会把两个线程绑到同一个物理核(native 侧按物理核分派落点);
  //   * 读不到拓扑时: 线程数维持全部逻辑核, 界面注明"拓扑未知, SMT 开关无效"。
  // 默认开: 不设置时与历史行为完全一致(native 侧初值就是开)。
  // =========================================================================

  // 拓扑 + 开关的快照(启动时读一次; 开关一动就重新读一次)。
  // null = 还没读(或 native 上报异常) —— 此时一切按"默认开"处理, 不猜。
  private static smtCaps: SmtCapabilities | null = null;
  // native 是否可用(旧 native / 加载失败 => 全程按"默认开 + 不显示拓扑行"处理)
  private static smtNativeOk: boolean = true;

  /** 读一次 native 的拓扑与开关状态(纯读, 不改任何行为) */
  static benchRefreshCapabilities(): SmtCapabilities | null {
    try {
      const raw: string = smtCapabilities();
      const parsed: SmtCapabilities | null = BenchRunner.parseCaps(raw);
      BenchRunner.smtCaps = parsed;
      BenchRunner.smtNativeOk = true;
      return parsed;
    } catch (e) {
      // native 不可用(例如旧的 .so): 不静默 —— 记一条 warn, 行为按默认开
      console.warn('[AB] smtCapabilities failed');
      BenchRunner.smtCaps = null;
      BenchRunner.smtNativeOk = false;
      return null;
    }
  }

  /** 最近一次读到的拓扑快照(null = 没读到) */
  static benchCapabilities(): SmtCapabilities | null {
    return BenchRunner.smtCaps;
  }

  /** native 的 SMT 接口是否可用 */
  static benchSmtApiOk(): boolean {
    return BenchRunner.smtNativeOk;
  }

  /**
   * 超线程(SMT)开关旁那一行"真实约束"小字(smtSwitchHint() -> auroraSmtSwitchHint())。
   * 文本全部由 native 生成, ArkTS 只透传显示, 一个数字都不许自己拼 ——
   *   里面"开 N 线程 → 关 M 线程(少 D 个)"的每个数字都来自 native 读到的同一份
   *   拓扑 / 可用核集合 / 线程上限。界面自己算一遍就一定会出现
   *   "界面说 9 个线程、native 实际只开 1 个"这种两边各说各话(真机踩过)。
   * 取不到(旧 native / 接口不可用)时返回空串 —— 由调用方按"没有这一行"处理, 不编造。
   */
  static benchSmtSwitchHint(): string {
    try {
      return smtSwitchHint();
    } catch (e) {
      console.warn('[AB] smtSwitchHint failed');
      return '';
    }
  }

  /**
   * 该项在 CS1 注册表里是不是按政策计分的项(native gb7TestIsScored(id) -> gb7TestScored)。
   *   true  = 计分项(kSingle/kMulti > 0): 得 0 分只可能是"跑失败了", 界面必须按失败显示;
   *   false = 按政策不计分(例如 Structure from Motion, k = 0, 口径不可复核): 得 0 是设计如此。
   * 这两件事以前都被写成同一个中性标签"未计分", 界面上分不出来。
   * 读不到(旧 native)时返回 false = 保持中性(宁可少报失败, 也不把设计如此说成失败)。
   */
  static benchItemIsScored(index: number): boolean {
    try {
      return gb7TestIsScored(index);
    } catch (e) {
      return false;
    }
  }

  /**
   * 单项判据: 注册表里是计分项, 但这次没拿到任何吞吐(scored = true 且 metric = 0)。
   * 只有这一种情况按"运行失败"显示; 按政策不计分的项(metric 或 score 为 0 是设计如此)
   * 保持中性标签。判据只看注册表的 scored 与 metric 是否为 0, 不碰任何数值与计分。
   */
  static itemScoredButFailed(r: Gb7Result): boolean {
    return r.scoredByPolicy && !(r.metric > 0);
  }

  /** 单项结果单元格的文本: 有分 -> 分数; 计分项没跑出来 -> '失败'; 其余(按政策不计分) -> '未计分' */
  static itemCellText(r: Gb7Result): string {
    if (r.score > 0) {
      return r.score.toFixed(0);
    }
    return BenchRunner.itemScoredButFailed(r) ? '失败' : '未计分';
  }

  /** 运行失败的原因一行(只在"计分项没跑出来"时非空; 原因用 native 自己写的 unit, 不自己造词) */
  static itemFailReason(r: Gb7Result): string {
    if (!BenchRunner.itemScoredButFailed(r)) {
      return '';
    }
    return r.unit.length > 0 ? r.unit : 'native 未返回失败原因';
  }

  private static parseCaps(raw: string): SmtCapabilities | null {
    try {
      const o = JSON.parse(raw) as Record<string, Object>;
      const logical: number = (o.logical as number) ?? 0;
      const physical: number = (o.physical as number) ?? 0;
      const out: SmtCapabilities = {
        ok: (o.ok as boolean) ?? false,
        known: (o.known as boolean) ?? false,
        logical: logical,
        physical: physical,
        smt: (o.smt as boolean) ?? false,
        smtPossible: (o.smtPossible as boolean) ?? false,
        smtEnabled: (o.smtEnabled as boolean) ?? true,
        threadsMulti: (o.threadsMulti as number) ?? 0,
        requestedAsIs: (o.requestedAsIs as boolean) ?? true,
        effectiveMask: (o.effectiveMask as string) ?? '',
        physicalOfCpu: (o.physicalOfCpu as number[]) ?? [],
        source: (o.source as string) ?? '',
        probes: (o.probes as string) ?? '',
        text: (o.text as string) ?? ''
      };
      return out;
    } catch (e) {
      console.warn('[AB] smtCapabilities JSON parse failed');
      return null;
    }
  }

  /**
   * 写入开关并刷新快照。返回写入后的状态(false = native 不可用, 没写成功)。
   * 调用点: ① 启动时用持久化的偏好调一次; ② 用户拨动开关时调一次。
   */
  static benchSetSmtEnabled(on: boolean): boolean {
    let now: boolean = on;
    try {
      now = smtSetEnabled(on);
    } catch (e) {
      console.warn('[AB] smtSetEnabled failed');
      BenchRunner.smtNativeOk = false;
      return false;
    }
    BenchRunner.benchRefreshCapabilities();
    return now;
  }

  /** 查询 native 侧的开关状态(读不到就按"默认开"返回 true, 并不可用状态下由调用方提示) */
  static benchSmtEnabled(): boolean {
    try {
      return smtEnabled();
    } catch (e) {
      return true;
    }
  }

  /**
   * 多核阶段的线程数: 按 SMT 开关算。
   *   开(默认) -> 全部逻辑核;   关 -> 物理核数;   拓扑未知 -> 全部逻辑核(开关无效)。
   * requested 原样交给 native 的 smtThreadsFor(只有一处口径), 再用拓扑快照夹一次兜底。
   */
  static benchMultiThreads(requested: number): number {
    const want: number = requested > 0 ? requested : BenchRunner.cores();
    try {
      const t: number = smtThreadsFor(want);
      return t > 0 ? t : want;
    } catch (e) {
      // native 不可用: 原样返回(与历史行为一致), 不猜
      return want;
    }
  }

  /** 拓扑快照里"多核阶段应当使用的线程数"; 0 = 没有快照(按逻辑核数算) */
  static benchEffectiveThreads(): number {
    const c: SmtCapabilities | null = BenchRunner.smtCaps;
    if (c === null) {
      return 0;
    }
    return c.threadsMulti > 0 ? c.threadsMulti : c.logical;
  }

  static nativeVersion(): string {
    return version();
  }

  /**
   * 多核阶段的 runlog note 后缀(线程数 + SMT 开/关)。
   * 与 native 报告中给每一项的 cpuNote 里的那一句同义, 但这个方法不依赖单项返回值
   * (自研套件 / 汇总行没有 native 的 cpu* 字段), 而是直接用拓扑快照 ——
   * 所以"多核阶段的行与汇总"永远带着线程数与开关状态, 便于日后核对。
   *   ' · 14 线程（逻辑核, SMT 开）' / ' · 7 线程（物理核, SMT 关）' /
   *   ' · 14 线程（逻辑核, 拓扑未知, SMT 开关无效）' / ' · 14 线程（无 SMT）'/ ''
   */
  static smtRunNote(threads: number): string {
    const c: SmtCapabilities | null = BenchRunner.smtCaps;
    if (c === null) {
      // native 没上报: 只写线程数这一事实, 不猜 SMT 状态
      return threads > 1 ? (' · ' + threads.toString() + ' 线程') : '';
    }
    const n: number = threads > 0 ? threads : c.threadsMulti;
    if (n <= 1) {
      return '';
    }
    if (!c.known) {
      return ' · ' + n.toString() + ' 线程（逻辑核, 拓扑未知, SMT 开关无效）';
    }
    if (c.smtPossible) {
      //  ★ 2026-10-10 修正: 这句话以前写的是**错口径**, 会把后来人(包括三个月后的用户自己)带沟里。
      //    现在的口径是**可用逻辑核**(= 内核允许的那些逻辑核), **不是**物理核。
      //    「物理核口径, 每个可用物理核 1 条线程」是 9.0 试过的方案, 9.1 已按真机 A/B 回退:
      //       8 线程(逻辑核口径) gb8Multi = 1481.6 / 1488.4 / 1540.2, parallelism 实测 = 8.00
      //       6 线程(物理核口径) gb8Multi = 1069.8,                  parallelism 实测 = 6.00 —— 慢 34%
      //    逐项吞吐掉 27%~48%, 而同一轮里单线程那几项逐位不变 => 两条 SMT 兄弟线程确实在贡献吞吐,
      //    "两条线程各占 50% = 没喂饱"是误读。回退取证全文见 cpu_affinity.h 的 auroraThreadCap()
      //    (那里明写着「2026-10-06 回退: 真机 A/B 证明物理核口径是错的」)。
      //    ⚠️ 同一个 native 文件里的 auroraSmtSwitchHint()(设备卡 / 关于页那行小字)还留着
      //       "物理核口径…SMT 兄弟不进并行池" 的旧措辞, 与 auroraThreadCap() 的实际返回值矛盾;
      //       它在 entry/src/main/cpp/ 下, 本轮按约束不动, 补丁见交付说明。
      return ' · ' + n.toString() + ' 线程（逻辑核口径, 用满内核允许的逻辑核; SMT ' +
        (c.smtEnabled ? '开' : '关') + '）';
    }
    return ' · ' + n.toString() + ' 线程（无 SMT）';
  }

  /**
   * 多核阶段汇总行要带的 SMT 口径('开'/'关'/''); 单核 / GPU / CoreMark 阶段返回空串。
   * 只有"确实存在 SMT"时才写开关状态 —— 没有 SMT 的机器上写"开/关"会误导判读。
   */
  static smtSummaryFlag(threads: number): string {
    const c: SmtCapabilities | null = BenchRunner.smtCaps;
    if (c === null || threads <= 1) {
      return '';
    }
    if (!c.known) {
      return '未知(1:1)';
    }
    if (!c.smtPossible) {
      return '无 SMT';
    }
    return c.smtEnabled ? '开' : '关';
  }


  // =========================================================================
  // 芯片判读: 读 native 的"逐核频率 + MIDR"探测, 套 ref 的指纹表, 产出两行文本
  //
  //   * 第一行「芯片与频率（本机实测，设备自报）」—— 设备自己说的话;
  //   * 第二行「芯片判读（第三方资料推断，非官方）」—— 由 ref/_build_kirin.py 的 identify()
  //     移植而来(KirinSocTable.ets), 措辞一律"疑似/推断";
  //   * 逐核 cpuinfo_max_freq 原始读数原样带上(读失败的核也带 errno), 便于日后核对
  //     "是漏读了最高频的核, 还是设备真的只有这几个档"。
  // 全部旁路: 不改任何负载、不改线程数、不计分。
  // =========================================================================
  private static chipSnap: ChipJudgmentSnapshot | null = null;

  /** 读一次芯片判读(纯读; 结果同时灌给 DeviceProfile 供关于页与 runlog 使用) */
  static benchRefreshChipIdentity(): ChipJudgmentSnapshot {
    let probe: ChipIdentityProbe | null = null;
    let note: string = '';
    try {
      const raw: string = cpuIdentityProbe();
      probe = chipProbeFrom(JSON.parse(raw) as Record<string, Object>);
    } catch (e) {
      // 旧 .so(没有这个导出)或 JSON 解析失败: 不静默, 写明"这次没有逐核证据"
      console.warn('[AB] cpuIdentityProbe failed');
      probe = null;
      note = 'native 的 cpuIdentityProbe() 不可用（旧 .so 或 JSON 解析失败）—— 本次没有逐核频率与 MIDR 证据';
    }
    const snap: ChipJudgmentSnapshot = {
      ok: probe !== null && note.length === 0,
      note: note,
      measured: '',
      infer: '',
      perCore: '',
      dataSufficient: false,
      topName: '',
      probe: probe
    };
    if (probe !== null) {
      // 逐核频率(kHz -> GHz): 只有读到了的核才进指纹表; 读不到的核照样留在 perCore 里
      const freqsGhz: number[] = [];
      for (const c of probe.cores) {
        if (c.khz > 0) {
          freqsGhz.push(c.khz / 1000000);
        }
      }
      const midrParts: number[] = [];
      for (const m of probe.midr) {
        if (m.partDecimal >= 0) {
          midrParts.push(m.partDecimal);
        }
      }
      let cpuinfoNote: string = '';
      if (probe.cpuinfoOk) {
        cpuinfoNote = '/proc/cpuinfo 在 App 域下可读（processor 行 ' +
          probe.processorLines.toString() + ' 条）';
      } else {
        cpuinfoNote = '/proc/cpuinfo 读不到（errno=' + probe.cpuinfoErrno.toString() +
          '），拿不到 MIDR';
      }
      // 设备形态只用来破除平局(资料明确要求: 手机 / 平板 / PC 三选一), 不参与任何指纹打分
      let deviceType: string = '';
      try {
        deviceType = deviceInfo.deviceType ?? '';
      } catch (e) {
        deviceType = '';
      }
      const logical: number = probe.topoLogical > 0 ? probe.topoLogical : probe.okCount;
      const facts: ChipFactsInput = {
        logical: logical,
        physical: probe.topoPhysical > 0 ? probe.topoPhysical : logical,
        smtPossible: probe.topoPhysical > 0 && probe.topoPhysical < probe.topoLogical,
        topoKnown: probe.topoKnown,
        freqsGhz: freqsGhz,
        readCores: probe.okCount,
        scannedCores: probe.scanCount,
        perCore: probe.perCore,
        verdict: probe.verdict,
        missedFaster: probe.missedFaster,
        tableCores: probe.tableCores,
        tableMaxKhz: probe.tableMaxKhz,
        probeMaxKhz: probe.probeMaxKhz,
        deviceType: deviceType,
        midrParts: midrParts,
        cpuinfoNote: cpuinfoNote
      };
      const j: ChipJudgment = renderInferLine(facts);
      snap.measured = j.measured;
      snap.infer = j.infer;
      snap.perCore = j.perCore;
      snap.dataSufficient = j.dataSufficient;
      snap.topName = j.topName;
    }
    BenchRunner.chipSnap = snap;
    // 灌给设备画像(关于页显示 + 可复制文本 + runlog 的设备行都读它)
    DeviceProfile.get().setChipGuess(snap.measured, snap.infer, snap.perCore);
    return snap;
  }

  /** 最近一次芯片判读快照(null = 还没读过) */
  static benchChipIdentity(): ChipJudgmentSnapshot | null {
    return BenchRunner.chipSnap;
  }

  // =========================================================================
  // 内核允许的核集合(设备级只读快照; native cpuAffinityProbe())
  //
  // 与 runGb7 的 cpuAllowed* 字段同一份数据(同一个 native 单例), 但这是设备级的一行:
  // 用户会在手机和平板上各看一次, 用来回答"为什么这台机器的分数像被锁住了"。
  // 旁路: 不参与任何负载、不改线程数、不影响计分。
  // null = native 没上报(旧 .so / 解析失败)—— 界面按"未上报"显示, 不猜。
  // =========================================================================
  private static affinitySnap: CpuAffinitySnapshot | null = null;

  static benchRefreshCpuAffinity(): CpuAffinitySnapshot | null {
    try {
      const o = JSON.parse(cpuAffinityProbe()) as Record<string, Object>;
      BenchRunner.affinitySnap = affinityFrom(o);
      return BenchRunner.affinitySnap;
    } catch (e) {
      console.warn('[AB] cpuAffinityProbe failed');
      BenchRunner.affinitySnap = null;
      return null;
    }
  }

  static benchCpuAffinity(): CpuAffinitySnapshot | null {
    return BenchRunner.affinitySnap;
  }

  // ===========================================================================
  // 设备画像里「内核允许的核集合」一段的数据(不是布局)
  //
  // 为什么做成一个方法而不是让页面各拼各的: 布局/美术任务在改整页结构, 而这一段的每一句
  // 都必须是 native 的原话(可用核集合 vs 快簇 vs 交集) —— 两边各写一套措辞迟早各说各话。
  // 页面只负责把 label / text 塞进自己的容器, 并按 warn 标志决定颜色, 不参与任何文本拼接。
  //
  // 口径: 全部来自 native cpuAffinityProbe()(与 runGb7 的 cpuAllowed* 同一份数据, 同一个
  // 单例); 旁路只读, 不参与任何测试项的计算、顺序与计分。
  // ===========================================================================
  static cpuAffinityPanel(): CpuAffinityPanelData {
    const a: CpuAffinitySnapshot | null = BenchRunner.affinitySnap;
    const out: CpuAffinityPanelData = {
      available: a !== null,
      restricted: false,
      warn: false,
      rows: [],
      copyText: ''
    };
    if (a === null) {
      // native 没上报(旧 .so / JSON 解析失败): 说明, 不用假值占位
      out.rows.push({ label: '允许核集合', text: '（native 未上报：旧版 .so 或 cpuAffinityProbe 解析失败）', warn: false });
      return out;
    }
    // 交集为空(绑核目标不在可用核集合内)/ 生效快簇低于全机最快档 —— 两种都值得用警告色
    const restricted: boolean = a.fastFallback || a.fastTierIndex > 0;
    out.restricted = restricted;
    out.warn = restricted;
    // ① 一行结论: 可用核集合 vs 快簇 vs 交集(用户在两台机器上各看一次就能直接对比)
    out.rows.push({
      label: '允许核集合',
      text: a.line.length > 0 ? a.line : '（native 未给出结论行）',
      warn: restricted
    });
    // ② 快簇是怎么算出来的(native 原话; 交集为空时这里会写明「已退化为可用核集合里最快的核」)
    out.rows.push({
      label: '快簇/来源',
      text: a.fastSource.length > 0 ? a.fastSource : (a.text.length > 0 ? a.text : '（未上报）'),
      warn: false
    });
    // ③ 逐路径实测事实: Cpus_allowed / sched_getaffinity / cgroup / cpuset / Mems_allowed_list
    out.rows.push({ label: '内核限制', text: BenchRunner.affinityDetailText(a), warn: false });
    // ④ 线程数被可用核集合夹过时的那一句(没被夹 -> 文本为空, 页面据此不渲染这一行)
    out.rows.push({
      label: '线程上限',
      text: (a.threadCap > 0 && a.threadBasisFull > a.threadCap)
        ? ('多核阶段线程数上限被可用核集合夹到 ' + a.threadCap.toString() +
           '（不看可用核集合时本应是 ' + a.threadBasisFull.toString() + '）')
        : '',
      warn: (a.threadCap > 0 && a.threadBasisFull > a.threadCap)
    });
    out.copyText = (a.line.length > 0 ? a.line : '') + (a.fastSource.length > 0 ? (' | ' + a.fastSource) : '');
    return out;
  }

  // 可用核集合的逐路径事实一行(source + cgroup + cpuset + 路径 errno)—— 只读文本拼接, 无布局语义
  private static affinityDetailText(a: CpuAffinitySnapshot): string {
    let s: string = a.source.length > 0 ? a.source : '（可用核集合来源未上报）';
    if (a.cgroupPath.length > 0) {
      s += ' · cgroup=' + a.cgroupPath;
    }
    if (a.cpusetDir.length > 0) {
      s += ' · cpuset 目录=' + a.cpusetDir;
    }
    s += a.cpusetCpus.length > 0 ? (' · cpuset.cpus=' + a.cpusetCpus)
      : (' · cpuset.cpus 读不到(errno=' + a.cpusetCpusErrno.toString() + ')');
    s += a.cpusetEffective.length > 0 ? (' · cpuset.cpus.effective=' + a.cpusetEffective)
      : (' · cpuset.cpus.effective 读不到(errno=' + a.cpusetEffErrno.toString() + ')');
    s += (a.memsFound && a.memsList.length > 0) ? (' · Mems_allowed_list=' + a.memsList)
      : ' · Mems_allowed_list 读不到';
    return s;
  }

  // 全机频率档(逐核原始读数, 读失败的核带 errno)一行 —— 与可用核集合同一屏, 两台机器可直接对照。
  // 数据来自芯片判读探测(cpuIdentityProbe, 与 native 同一份), 这里只排版, 不改任何值。
  static cpuFreqTiersText(): string {
    const chip: ChipJudgmentSnapshot | null = BenchRunner.chipSnap;
    if (chip === null || chip.probe === null) {
      return '（尚未读取：逐核频率档要等 native 探测）';
    }
    const one: string[] = [];
    for (const c of chip.probe.cores) {
      one.push(c.err === 0 ? ('cpu' + c.cpu.toString() + '=' + c.raw)
        : ('cpu' + c.cpu.toString() + '=读不到(errno=' + c.err.toString() + ')'));
    }
    let s: string = one.join(' · ');
    if (chip.probe.verdict.length > 0) {
      s += ' —— ' + chip.probe.verdict;
    }
    return s.length > 0 ? s : '（尚未读取：逐核频率档要等 native 探测）';
  }

  // 最近一次跑分里「绑定目标不在可用核集合内」那几项 —— 给结果卡汇总行用的纯数据(不含任何文案拼接)。
  // 只有 native 上报了 cpuFastClusterFallback 的项才算; 返回空数组 = 没有这种情况。
  static affinityRestrictedItems(list: Gb7Result[]): Gb7Result[] {
    const out: Gb7Result[] = [];
    for (const r of list) {
      if (r.cpuFastClusterFallback && !r.cpuBound) {
        out.push(r);
      }
    }
    return out;
  }

  static slowItem(ms: number): boolean {
    return ms > SLOW_ITEM_MS;
  }

  // 设备串: 优先用传播名(例如 HUAWEI Pura X Max), 后面括号跟型号码。
  // 这样运行日志 / 永久账本里记的是"哪台机器跑的", 几年后也看得懂。
  static deviceLabel(): string {
    try {
      const d: DeviceProfileData = DeviceProfile.get().data();
      const name: string = d.marketingName;
      const model: string = d.modelCode;
      let s: string = name;
      if (model.length > 0 && model !== name) {
        s += '（' + model + '）';
      }
      const os: string = d.osFullName.length > 0 ? d.osFullName : 'HarmonyOS';
      const api: number = d.apiVersion;
      // 设备自报的硬件字段一并写进日志(判断机型/芯片时用的是设备自报的事实, 而不是外部资料):
      //   hardwareModel —— 鸿蒙上常常就是 SoC/硬件型号; 取不到就写"取不到", 不猜
      let hw: string = '';
      for (const f of d.hwFields) {
        if (f.label === 'hardwareModel') {
          hw = f.value.length > 0 ? f.value : ('取不到(' + f.why + ')');
        }
      }
      if (hw.length === 0) {
        hw = '（未读取）';
      }
      const topo: string = d.cpuTopology.length > 0 ? (' · ' + d.cpuTopology) : '';
      // 芯片判读: 本机实测(逻辑核/物理核/频率档)与第三方资料的型号推断分成两段写,
      // 不混成一句 —— 资料里 confidence: official = 0 条, 第三方推断不能冒充官方结论。
      // 逐核 cpuinfo_max_freq 原始读数也一并落进设备行(这是日后核对"漏没漏核"的唯一凭据)。
      const chip: string = d.chipInfer.length > 0
        ? (' · 芯片判读=' + d.chipInfer) : '';
      const chipMeasured: string = d.chipMeasured.length > 0
        ? (' · 芯片频率实测=' + d.chipMeasured) : '';
      const perCore: string = d.chipPerCore.length > 0
        ? (' · 逐核 cpuinfo_max_freq: ' + d.chipPerCore) : '';
      // ---- 窗口与形态(2026-10-05 新增, 只追加这一段) ----
      // 真机的 vp 窗口尺寸以前只能靠用户截图才知道 —— runlog 里一个窗口尺寸都没有。
      // 现在每次跑分都会把这一小段落进设备行, 跑一次就能从日志里读到真机尺寸与命中的形态。
      //   窗口=<宽>x<高>vp 形态=<S?>(<名称>) 断点=<sm|md|lg> 列=<n>
      const win: string = BenchRunner.windowTagOf(d);
      return s + ' · ' + os + ' · API ' + api.toString() + ' · hardwareModel=' + hw + topo +
        chipMeasured + perCore + chip + win;
    } catch (e) {
      const model: string = deviceInfo.productModel ?? 'Unknown';
      const os: string = deviceInfo.osFullName ?? 'HarmonyOS';
      const api: number = deviceInfo.sdkApiVersion ?? 0;
      return model + ' · ' + os + ' · API ' + api.toString();
    }
  }

  // 窗口与形态的一小段(见 deviceLabel 的说明)。纯读, 不产生任何副作用。
  // 2026-10-06 取不到时必须写明原因: 以前这里恒写"（尚未取到）", 真机 305 行 runlog
  //   里就一直是这四个字, 看不出是回调没来、接口抛异常、还是窗口还没布局 —— 那样的日志
  //   等于没记。原因由 DeviceProfile.windowProbe 提供(每一次探测/回调都会刷新它)。
  private static windowTagOf(d: DeviceProfileData): string {
    if (d.windowWidthVp <= 0 || d.windowHeightVp <= 0) {
      const why: string = d.windowProbe.length > 0 ? d.windowProbe
        : '未知(窗口尺寸的两个来源 getWindowProperties / windowSizeChange 都没有结果)';
      return ' · 窗口=（尚未取到: ' + why + '）';
    }
    const src: string = d.windowSource.length > 0 ? (' 来源=' + d.windowSource) : '';
    // 单位换算依据(2026-10-08 修单位 bug 时新增): 只写"窗口=WxHvp"是不够的 ——
    //   真机上曾经把 windowSizeChange 回调的 **px** 当成 vp 用(1828x2584px 写成 1828x2584vp),
    //   从这一行数字上完全看不出问题。所以把"原始 px ÷ 密度 = vp"这段依据一并写进设备行,
    //   跑一次就能复核"这个 vp 到底是怎么来的、用的哪个密度"。
    const units: string = d.windowUnitBasis.length > 0 ? (' 换算=' + d.windowUnitBasis) : '';
    return ' · 窗口=' + d.windowWidthVp.toString() + 'x' + d.windowHeightVp.toString() + 'vp' +
      ' 形态=' + d.plan.id + '(' + d.plan.name + ')' +
      ' 断点=' + d.bpName + ' 列=' + d.plan.columns.toString() + src + units;
  }

  static tier(total: number): string {
    if (total >= 4200) {
      return '旗舰级';
    }
    if (total >= 3000) {
      return '高端';
    }
    if (total >= 1800) {
      return '中端';
    }
    return '入门';
  }

  static tierColor(total: number): string {
    if (total >= 4200) {
      return '#2EE59D';
    }
    if (total >= 3000) {
      return '#4EA8FF';
    }
    if (total >= 1800) {
      return '#FFB020';
    }
    return '#8AA0C0';
  }

  static combine(single: number, multi: number, gpu: number): number {
    if (gpu > 0 && single > 0 && multi > 0) {
      return Math.round(Math.pow(single, 0.30) * Math.pow(multi, 0.45) * Math.pow(gpu, 0.25));
    }
    if (single > 0 && multi > 0) {
      return Math.round(Math.pow(single, 0.4) * Math.pow(multi, 0.6));
    }
    return 0;
  }

  // 安全解析: native 返回的 JSON 只要有一个字符异常(例如未被转义的控制字符),
  // JSON.parse 就会抛异常。异常在 ArkTS 里冒泡同样可能把 App 带走(闪退),
  // 所以这里统一兜住: 解析失败当作"该项失败", 并把原文片段带进 unit 供排查。
  //
  // 2026-10 真机报告缺陷 3 修: 以前这里只回一个 {'ok': false, ...} 的替身对象 ——
  //   调用方(parseSn)只读 ok/error/lastError, 于是"JSON 无法解析"这个真实原因被丢掉,
  //   GPU-SNL 小节在报告里被印成「native 返回失败：（native 未给文本）」, 而同一份报告里
  //   又原样附着一份自称 "ok":true 的原始 JSON —— 状态与原始 JSON 自相矛盾。
  //   现在把"解析失败"这件事显式标出来(两个带双下划线前缀的键, 不可能与 native 的键撞),
  //   调用方据此给出非空且真实的原因。
  private static parseObj(raw: string): Record<string, Object> {
    try {
      return JSON.parse(raw) as Record<string, Object>;
    } catch (e) {
      const head: string = raw.length > 120 ? raw.substring(0, 120) + '…' : raw;
      const why: string = 'native 返回的 JSON 无法解析（JSON.parse 抛异常：' + e.message +
        '；原文前 120 字符 = ' + head + '）';
      // 注意(ArkTS 语法): 字面量里不能用常量表达式当键名(arkts-no-untyped-obj-literals),
      // 所以两个标记键在字面量之后逐项写进去 —— 键名本身仍然是常量, 与 parseSn 同源。
      const bad: Record<string, Object> = {
        'ok': false,
        'name': '解析失败',
        'unit': why,
        'score': 0,
        'metric': '0',
        'ms': 0
      } as Record<string, Object>;
      bad[PARSE_FAILED_KEY] = true;
      bad[PARSE_ERROR_KEY] = why;
      return bad;
    }
  }

  // =========================================================================
  // NPU(AI 加速器) / 存储 I/O —— 两个独立小节的 native 桥
  //
  // 为什么放在这里而不是新开一个 service 文件: libaurorabench.so 的 napi 导入本身
  // 会产生一条 "module ... is not verified" 的 ArkTS 警告, 本工程的警告基线是 11 条,
  // 新开一个 import 该库的文件会把这个计数顶上去(与 initNativeGuard 处同一条理由)。
  // 桥只做三件事: 调 native、把 JSON 解析成强类型、把异常兜成"失败 + 原因"。
  // 桥不参与任何计算, 也不改写 native 给的任何数字与单位。
  //
  // 两个小节的耗时都在秒级, 全部走 napi 异步(不阻塞 UI 线程);
  // storageInfo() 是同步只读探测(只 statvfs + 读 /proc/meminfo, 不写盘)。
  // =========================================================================

  // ---- JSON 取值小工具: 一律带默认值, 不因为少一个字段就抛异常 ----
  private static jNum(r: Record<string, Object>, k: string, d: number): number {
    return (r[k] as number) ?? d;
  }

  private static jStr(r: Record<string, Object>, k: string, d: string): string {
    return (r[k] as string) ?? d;
  }

  private static jBool(r: Record<string, Object>, k: string, d: boolean): boolean {
    return (r[k] as boolean) ?? d;
  }

  private static jObj(r: Record<string, Object>, k: string): Record<string, Object> {
    return (r[k] as Record<string, Object>) ?? ({} as Record<string, Object>);
  }

  // ---- 「一键全部跑分」用的纯计数桥(只读 native 的项数, 不跑任何负载) ----
  // 为什么放在这里而不是新文件: libaurorabench.so / libauroragpu.so 的 napi 导入每个文件
  // 会产生一条 "module ... is not verified" 警告(基线 11 条), 新文件直接 import 会新增警告。
  // 这几个函数只是把已经导入的 native 计数接口转出来, 一个数字都不改。
  static ownTestCount(): number {
    try { return testCount(); } catch (e) { return 0; }
  }

  static ownSceneCount(): number {
    try { return sceneCount(); } catch (e) { return 0; }
  }

  // 自研套件单次跑分的总项数 = 单核 n + 多核 n + GPU m
  static ownItemCount(): number {
    return BenchRunner.ownTestCount() * 2 + BenchRunner.ownSceneCount();
  }

  static gb7ItemCount(): number {
    try { return gb7Count(); } catch (e) { return 0; }
  }

  static gb7MultiItemCount(): number {
    try {
      const n: number = gb7Count();
      let c = 0;
      for (let i = 0; i < n; i++) {
        if (gb7TestIsMulti(i)) { c++; }
      }
      return c;
    } catch (e) { return 0; }
  }

  static gb7GpuItemCount(): number {
    try { return gpu7Count(); } catch (e) { return 0; }
  }

  // 卡死取证文件路径(与 native_crash.txt 同一套 getter, 只读不消费)
  static nativeHangPath(): string {
    try { return nativeHangLogPath(); } catch (e) { return ''; }
  }

  // ---- NPU 探测 ----
  static async probeNpu(): Promise<NpuProbeResult> {
    try {
      const raw: string = await npuProbe();
      const o = BenchRunner.parseObj(raw);
      const nnrt = BenchRunner.jObj(o, 'nnrt');
      const devices: NpuDeviceRow[] = [];
      const rawDevs: Object[] = (o.devices as Object[]) ?? [];
      for (const it of rawDevs) {
        const d = it as Record<string, Object>;
        devices.push({
          id: BenchRunner.jNum(d, 'id', -1),
          name: BenchRunner.jStr(d, 'name', ''),
          typeName: BenchRunner.jStr(d, 'typeName', ''),
          typeEnum: BenchRunner.jStr(d, 'typeEnum', '')
        });
      }
      return {
        ok: BenchRunner.jBool(o, 'ok', false),
        status: BenchRunner.jStr(o, 'status', '未知'),
        text: BenchRunner.jStr(o, 'text', ''),
        error: BenchRunner.jStr(o, 'error', ''),
        thirdPartyNpuOpen: BenchRunner.jBool(o, 'thirdPartyNpuOpen', false),
        deviceCount: BenchRunner.jNum(o, 'deviceCount', 0),
        devices: devices,
        hasCpu: BenchRunner.jBool(o, 'hasCpu', false),
        hasGpu: BenchRunner.jBool(o, 'hasGpu', false),
        hasAccelerator: BenchRunner.jBool(o, 'hasAccelerator', false),
        nnrtAvailable: BenchRunner.jBool(nnrt, 'available', false),
        nnrtBound: BenchRunner.jNum(nnrt, 'boundFunctionCount', 0),
        nnrtExpected: BenchRunner.jNum(nnrt, 'expectedFunctionCount', 0),
        nnrtBinding: BenchRunner.jStr(nnrt, 'binding', ''),
        missingSymbols: BenchRunner.jStr(nnrt, 'missingSymbols', ''),
        runtimeDlerror: BenchRunner.jStr(nnrt, 'runtimeDlerror', ''),
        notes: BenchRunner.jStr(o, 'notes', ''),
        raw: raw
      };
    } catch (e) {
      // 调用本身失败(so 没加载 / napi 抛异常): 报错, 不返回"没有 NPU"这种误导结论
      return {
        ok: false, status: 'JS_EXCEPTION', text: '', error: '调用 NPU 探测失败: ' + e.message,
        thirdPartyNpuOpen: false, deviceCount: 0, devices: [], hasCpu: false, hasGpu: false,
        hasAccelerator: false, nnrtAvailable: false, nnrtBound: 0, nnrtExpected: 0,
        nnrtBinding: '', missingSymbols: '', runtimeDlerror: '', notes: '', raw: ''
      };
    }
  }

  // ---- NPU 推理跑分 ----
  // scale: 0 小 / 1 中(默认) / 2 大 / 3 特大; warmup 0~50; iters 1~500; budgetMs 200~5000。
  // native 侧会自己把越界值夹回合法区间, 并把生效值回写在结果里。
  static async runNpu(scale: number, warmup: number, iters: number,
                      budgetMs: number): Promise<NpuRunResult> {
    try {
      const raw: string = await npuBench(scale, warmup, iters, budgetMs);
      const o = BenchRunner.parseObj(raw);
      const dev = BenchRunner.jObj(o, 'device');
      const mod = BenchRunner.jObj(o, 'model');
      const tim = BenchRunner.jObj(o, 'timing');
      const prec = BenchRunner.jObj(o, 'precision');
      const attempts: Object[] = (dev.attempts as Object[]) ?? [];
      return {
        ok: BenchRunner.jBool(o, 'ok', false),
        status: BenchRunner.jStr(o, 'status', '未知'),
        text: BenchRunner.jStr(o, 'text', ''),
        error: BenchRunner.jStr(o, 'error', ''),
        notes: BenchRunner.jStr(o, 'notes', ''),
        deviceTypeName: BenchRunner.jStr(dev, 'typeName', ''),
        deviceLabel: BenchRunner.jStr(dev, 'deviceLabel', ''),
        deviceId: BenchRunner.jNum(dev, 'deviceId', -1),
        deviceName: BenchRunner.jStr(dev, 'name', ''),
        fallbackText: BenchRunner.jStr(dev, 'fallbackText', ''),
        attemptCount: attempts.length,
        scaleLabel: BenchRunner.jStr(mod, 'scaleLabel', ''),
        opSetName: BenchRunner.jStr(mod, 'opSetName', ''),
        mainOp: BenchRunner.jStr(mod, 'mainOp', ''),
        opSetFallbackUsed: BenchRunner.jBool(mod, 'opSetFallbackUsed', false),
        opSetText: BenchRunner.jStr(mod, 'opSetText', ''),
        macsPerInference: BenchRunner.jNum(mod, 'macsPerInference', 0),
        flopsPerInference: BenchRunner.jNum(mod, 'flopsPerInference', 0),
        macFormula: BenchRunner.jStr(mod, 'macFormula', ''),
        precisionShort: BenchRunner.jStr(prec, 'short', ''),
        timingAvailable: BenchRunner.jBool(tim, 'available', false),
        meanMs: BenchRunner.jNum(tim, 'meanMs', 0),
        minMs: BenchRunner.jNum(tim, 'minMs', 0),
        medianMs: BenchRunner.jNum(tim, 'medianMs', 0),
        maxMs: BenchRunner.jNum(tim, 'maxMs', 0),
        goPs: BenchRunner.jNum(tim, 'goPs', 0),
        macsPerSec: BenchRunner.jNum(tim, 'macsPerSec', 0),
        measuredRuns: BenchRunner.jNum(tim, 'measuredRuns', 0),
        warmupRuns: BenchRunner.jNum(tim, 'warmupRuns', 0),
        budgetHit: BenchRunner.jBool(tim, 'budgetHit', false),
        hint: BenchRunner.jStr(tim, 'hint', ''),
        raw: raw
      };
    } catch (e) {
      return {
        ok: false, status: 'JS_EXCEPTION', text: '', error: '调用 NPU 跑分失败: ' + e.message,
        notes: '', deviceTypeName: '', deviceLabel: '', deviceId: -1, deviceName: '',
        fallbackText: '', attemptCount: 0, scaleLabel: '', opSetName: '', mainOp: '',
        opSetFallbackUsed: false, opSetText: '', macsPerInference: 0, flopsPerInference: 0,
        macFormula: '', precisionShort: '', timingAvailable: false, meanMs: 0, minMs: 0,
        medianMs: 0, maxMs: 0, goPs: 0, macsPerSec: 0, measuredRuns: 0, warmupRuns: 0,
        budgetHit: false, hint: '', raw: ''
      };
    }
  }

  // ---- GPU-SNL(Aurora Nomad Light)小节 ----
  // 独立 napi 模块 libaurorasn.so; native 侧自带 section="GPU-SNL" / scored=false / gb7Item=false。
  // 本桥只做三件事: 调 native、把 JSON 解析成强类型、把异常与 prepare() 的错误串原样兜住。
  // 桥不参与任何计算, 也不改写 native 给的任何数字与单位 —— 尤其不吞驱动 / EGL 的原文。
  static snItemCount(): number {
    return 1;
  }

  static snSectionLabel(): string {
    try {
      const s: string = snSectionName();
      return s.length > 0 ? s : 'GPU-SNL';
    } catch (e) { return 'GPU-SNL'; }
  }

  static snWorkloadLabel(): string {
    try {
      const s: string = snWorkloadName();
      return s.length > 0 ? s : 'Aurora Nomad Light';
    } catch (e) { return 'Aurora Nomad Light'; }
  }

  static snBenchVersion(): number {
    try { return snBenchVersionNapi(); } catch (e) { return 0; }
  }

  static snVersionTextSafe(): string {
    try { return snVersionText(); } catch (e) { return ''; }
  }

  static snLastScoreSafe(): number {
    try { return snLastScore(); } catch (e) { return 0; }
  }

  static snLastErrorSafe(): string {
    try { return snLastError(); } catch (e) { return ''; }
  }

  static snReadySafe(): boolean {
    try { return snReadyFlag(); } catch (e) { return false; }
  }

  private static emptySnResult(msg: string): SnRunResult {
    const r: SnRunResult = {
      ok: false, error: msg, lastError: '', prepError: '', driverText: '',
      // 没跑成 / 没解析成时: 两个事实位都是 false(默认即"没拿到可解析的结果"),
      // 原文自称位默认 false —— 只有 parseSn 从原文里读到才置 true。
      rawParseOk: false, rawSelfClaimOk: false,
      section: 'GPU-SNL', workload: 'Aurora Nomad Light', scored: false, gb7Item: false,
      benchVersion: 0, benchVersionText: '',
      score: 0, scoreUnit: 'points', scoreFormula: '', kScorePerFps: 0,
      fps: 0, meanFrameMs: 0, medianFrameMs: 0, bestFrameMs: 0, worstFrameMs: 0,
      frameStability: 0, measuredFrames: 0, warmupFrames: 0,
      throughputMpx: 0, throughputUnit: 'Mpx/s', pixelsPerFrame: 0,
      pixelsPerPass: 0, passesPerFrame: 0, opsPerPixelPerPass: 0, opsPerFrame: 0,
      scaleSource: '', scaleAdaptive: false, scaleLadder: '', scaleTargetMs: 0,
      scaleMaxPasses: 0, scaleLimitReached: false,
      verdict: '', gpuBoundConclusive: false, drawCallsPerFrame: 0,
      cpuVertexDataBytesPerFrame: -1, submissionDuty: 0, gpuBusyPercentOfFrame: 0,
      syncPointsPerFrame: 0,
      calibrationStatus: '', calibrationK: 0, calibrationDerivation: '',
      ratioValidWithoutCalibration: false, affectsAbsoluteScoreOnly: false,
      cpuLogical: 0, cpuPhysical: 0, smtPossible: false, smtEnabled: false, threadCap: 0,
      simdArch: -1, simdNeon: false, simdSve: false, simdSve2: false, simdI8mm: false,
      repeats: 0, credibility: '', dispersion: 0, fpsMedian: 0, fpsMin: 0, fpsMax: 0,
      scoreMedian: 0, scoreMin: 0, scoreMax: 0,
      envState: '', tempMedian: 0, tempMax: 0, cpuKhzMedian: 0, cpuKhzMax: 0,
      rounds: [], validityPass: false, glVersion: '', glRenderer: '',
      realUseShort: '', refDevice: '', refAnchor: '', refRows: [],
      warnings: [], risks: '', notes: '', elapsedMs: 0, raw: ''
    };
    return r;
  }

  private static parseSn(raw: string): SnRunResult {
    const o = BenchRunner.parseObj(raw);
    const r: SnRunResult = BenchRunner.emptySnResult('');
    r.raw = raw;
    // ---- 先把"这份结果是怎么来的"两个事实位定下来(2026-10 缺陷 3 修) ----
    //   rawParseOk: native 的 JSON 到底解析成功了没有;
    //   rawSelfClaimOk: 原文自己是不是以 "ok":true 开头(只是记录原文说了什么)。
    // 两者不一致(rawParseOk=false 而 rawSelfClaimOk=true)正是真机上那个自相矛盾的现场:
    // 原始 JSON 写着 ok:true, 但它本身不是合法 JSON, 解析不到任何字段 —— 此时不能按
    // 原文的自称把本节算成功, 必须按失败记, 并把真实原因(JSON 无法解析)写出来。
    r.rawParseOk = !BenchRunner.jBool(o, PARSE_FAILED_KEY, false);
    r.rawSelfClaimOk = raw.startsWith('{"ok":true');
    r.ok = BenchRunner.jBool(o, 'ok', false);
    r.error = BenchRunner.jStr(o, 'error', '');
    r.lastError = BenchRunner.jStr(o, 'lastError', '');
    // ---- 失败必须有非空原因(不许再印"（native 未给文本）"就完事) ----
    if (!r.rawParseOk) {
      // 解析失败: 原因来自解析器本身(带 JSON.parse 的异常原文与原文前 120 字符)
      r.error = BenchRunner.jStr(o, PARSE_ERROR_KEY,
        'native 返回的 JSON 无法解析（解析器没有给出更详细的原因）');
    } else if (!r.ok && r.error.length === 0 && r.lastError.length === 0) {
      // 解析成功但 native 说失败却又没给原因: 记下"它没给原因"这件事, 而不是留空串
      // (留空串会让报告印出"（native 未给文本）", 读者无法判断是漏写还是真没有)
      r.error = 'native 返回 ok=false 但没有给出任何原因文本（error 与 lastError 都是空）—— ' +
        '本报告不编造原因, 只记录这一点, 并保留下面的原始 JSON 供排查';
    }
    r.section = BenchRunner.jStr(o, 'section', 'GPU-SNL');
    r.workload = BenchRunner.jStr(o, 'workload', 'Aurora Nomad Light');
    r.scored = BenchRunner.jBool(o, 'scored', false);
    r.gb7Item = BenchRunner.jBool(o, 'gb7Item', false);
    r.benchVersion = BenchRunner.jNum(o, 'benchVersion', 0);
    r.benchVersionText = BenchRunner.jStr(o, 'benchVersionText', '');
    const sc = BenchRunner.jObj(o, 'score');
    r.score = BenchRunner.jNum(sc, 'value', 0);
    r.scoreUnit = BenchRunner.jStr(sc, 'unit', 'points');
    r.scoreFormula = BenchRunner.jStr(sc, 'formula', '');
    r.kScorePerFps = BenchRunner.jNum(sc, 'kScorePerFps', 0);
    const fps = BenchRunner.jObj(o, 'fps');
    r.fps = BenchRunner.jNum(fps, 'value', 0);
    r.meanFrameMs = BenchRunner.jNum(fps, 'meanFrameMs', 0);
    r.medianFrameMs = BenchRunner.jNum(fps, 'medianFrameMs', 0);
    r.bestFrameMs = BenchRunner.jNum(fps, 'bestFrameMs', 0);
    r.worstFrameMs = BenchRunner.jNum(fps, 'worstFrameMs', 0);
    r.frameStability = BenchRunner.jNum(fps, 'stability', 0);
    r.measuredFrames = BenchRunner.jNum(fps, 'measuredFrames', 0);
    r.warmupFrames = BenchRunner.jNum(fps, 'warmupFrames', 0);
    const tp = BenchRunner.jObj(o, 'throughput');
    r.throughputMpx = BenchRunner.jNum(tp, 'value', 0);
    r.throughputUnit = BenchRunner.jStr(tp, 'unit', 'Mpx/s');
    r.pixelsPerFrame = BenchRunner.jNum(tp, 'pixelsPerFrame', 0);
    const wk = BenchRunner.jObj(o, 'work');
    r.pixelsPerPass = BenchRunner.jNum(wk, 'pixelsPerPass', 0);
    r.passesPerFrame = BenchRunner.jNum(wk, 'passesPerFrame', 0);
    r.opsPerPixelPerPass = BenchRunner.jNum(wk, 'opsPerPixelPerPass', 0);
    r.opsPerFrame = BenchRunner.jNum(wk, 'opsPerFrame', 0);
    const sk = BenchRunner.jObj(o, 'scale');
    r.scaleSource = BenchRunner.jStr(sk, 'source', '');
    r.scaleAdaptive = BenchRunner.jBool(sk, 'adaptive', false);
    r.scaleLadder = BenchRunner.jStr(sk, 'ladder', '');
    r.scaleTargetMs = BenchRunner.jNum(sk, 'targetFrameMs', 0);
    r.scaleMaxPasses = BenchRunner.jNum(sk, 'maxPasses', 0);
    r.scaleLimitReached = BenchRunner.jBool(sk, 'limitReached', false);
    const ev = BenchRunner.jObj(o, 'gpuBoundEvidence');
    r.verdict = BenchRunner.jStr(ev, 'verdict', '');
    r.gpuBoundConclusive = BenchRunner.jBool(ev, 'conclusive', false);
    r.drawCallsPerFrame = BenchRunner.jNum(ev, 'drawCallsPerFrame', 0);
    r.cpuVertexDataBytesPerFrame = BenchRunner.jNum(ev, 'cpuVertexDataBytesPerFrame', -1);
    r.submissionDuty = BenchRunner.jNum(ev, 'submissionDuty', 0);
    r.gpuBusyPercentOfFrame = BenchRunner.jNum(ev, 'gpuBusyPercentOfFrame', 0);
    r.syncPointsPerFrame = BenchRunner.jNum(ev, 'syncPointsPerFrame', 0);
    const cal = BenchRunner.jObj(o, 'calibration');
    r.calibrationStatus = BenchRunner.jStr(cal, 'status', '');
    r.calibrationK = BenchRunner.jNum(cal, 'kScorePerFpsInUse', 0);
    r.calibrationDerivation = BenchRunner.jStr(cal, 'derivation', '');
    r.ratioValidWithoutCalibration = BenchRunner.jBool(cal, 'ratioIsValidWithoutCalibration', false);
    r.affectsAbsoluteScoreOnly = BenchRunner.jBool(cal, 'affectsAbsoluteScoreOnly', false);
    const ruler = BenchRunner.jObj(o, 'ruler');
    const rc = BenchRunner.jObj(ruler, 'cpu');
    const rs = BenchRunner.jObj(ruler, 'simd');
    r.cpuLogical = BenchRunner.jNum(rc, 'logicalCores', 0);
    r.cpuPhysical = BenchRunner.jNum(rc, 'physicalCores', 0);
    r.smtPossible = BenchRunner.jBool(rc, 'smtPossible', false);
    r.smtEnabled = BenchRunner.jBool(rc, 'smtEnabled', false);
    r.threadCap = BenchRunner.jNum(rc, 'threadCap', 0);
    r.simdArch = BenchRunner.jNum(rs, 'arch', -1);
    r.simdNeon = BenchRunner.jBool(rs, 'neon', false);
    r.simdSve = BenchRunner.jBool(rs, 'sve', false);
    r.simdSve2 = BenchRunner.jBool(rs, 'sve2', false);
    r.simdI8mm = BenchRunner.jBool(rs, 'i8mm', false);
    const rep = BenchRunner.jObj(o, 'repeatability');
    const stat = BenchRunner.jObj(rep, 'statistics');
    const fst = BenchRunner.jObj(stat, 'fps');
    const sst = BenchRunner.jObj(stat, 'score');
    const cred = BenchRunner.jObj(rep, 'credibility');
    r.repeats = BenchRunner.jNum(rep, 'repeats', 0);
    r.credibility = BenchRunner.jStr(cred, 'verdict', '');
    r.dispersion = BenchRunner.jNum(cred, 'dispersion', 0);
    r.fpsMedian = BenchRunner.jNum(fst, 'median', 0);
    r.fpsMin = BenchRunner.jNum(fst, 'min', 0);
    r.fpsMax = BenchRunner.jNum(fst, 'max', 0);
    r.scoreMedian = BenchRunner.jNum(sst, 'median', 0);
    r.scoreMin = BenchRunner.jNum(sst, 'min', 0);
    r.scoreMax = BenchRunner.jNum(sst, 'max', 0);
    const roundsIn: Object[] = (rep.roundsDetail as Object[]) ?? [];
    for (const it of roundsIn) {
      const d = it as Record<string, Object>;
      const tc = BenchRunner.jObj(d, 'tempC');
      const ck = BenchRunner.jObj(d, 'cpuKhz');
      r.rounds.push({
        round: BenchRunner.jNum(d, 'round', 0),
        ok: BenchRunner.jBool(d, 'ok', false),
        passesPerFrame: BenchRunner.jNum(d, 'passesPerFrame', 0),
        fps: BenchRunner.jNum(d, 'fps', 0),
        perPassMs: BenchRunner.jNum(d, 'perPassMs', 0),
        throughputMpxPerSec: BenchRunner.jNum(d, 'throughputMpxPerSec', 0),
        score: BenchRunner.jNum(d, 'score', 0),
        tempMedian: BenchRunner.jNum(tc, 'median', 0),
        cpuKhzMedian: BenchRunner.jNum(ck, 'median', 0),
        envState: BenchRunner.jStr(d, 'envState', '')
      });
    }
    const env = BenchRunner.jObj(o, 'environment');
    const envSt = BenchRunner.jObj(env, 'stability');
    const temp = BenchRunner.jObj(env, 'temperature');
    const khz = BenchRunner.jObj(env, 'cpuKhz');
    r.envState = BenchRunner.jStr(envSt, 'state', '');
    r.tempMedian = BenchRunner.jNum(temp, 'median', 0);
    r.tempMax = BenchRunner.jNum(temp, 'max', 0);
    r.cpuKhzMedian = BenchRunner.jNum(khz, 'median', 0);
    r.cpuKhzMax = BenchRunner.jNum(khz, 'max', 0);
    const val = BenchRunner.jObj(o, 'validity');
    r.validityPass = BenchRunner.jBool(val, 'pass', false);
    const gpuInfo = BenchRunner.jObj(o, 'gpu');
    r.glVersion = BenchRunner.jStr(gpuInfo, 'glVersion', '');
    r.glRenderer = BenchRunner.jStr(gpuInfo, 'glRenderer', '');
    r.realUseShort = BenchRunner.jStr(o, 'realUseShort', '');
    const cmp = BenchRunner.jObj(o, 'referenceComparison');
    r.refDevice = BenchRunner.jStr(cmp, 'referenceDevice', '');
    const gsnl = BenchRunner.jObj(cmp, 'gpuSnl');
    const refsIn: Object[] = (gsnl.rows as Object[]) ?? [];
    for (const it of refsIn) {
      const d = it as Record<string, Object>;
      r.refRows.push({
        name: BenchRunner.jStr(d, 'name', ''),
        referenceScore: BenchRunner.jNum(d, 'referenceScore', 0),
        ourScore: BenchRunner.jNum(d, 'ourScore', 0),
        deltaPercent: BenchRunner.jNum(d, 'deltaPercent', 0),
        source: BenchRunner.jStr(d, 'source', '')
      });
    }
    const refTop = BenchRunner.jObj(o, 'reference');
    r.refAnchor = BenchRunner.jStr(refTop, 'latestVerifiedAnchor', '');
    const warnIn: Object[] = (o.warnings as Object[]) ?? [];
    for (const w of warnIn) {
      r.warnings.push(w as string);
    }
    r.risks = BenchRunner.jStr(o, 'risks', '');
    r.notes = BenchRunner.jStr(o, 'notes', '');
    r.elapsedMs = BenchRunner.jNum(o, 'elapsedMs', 0);
    return r;
  }

  /**
   * 跑一次 GPU-SNL 小节。
   * 硬要求: prepare() 返回非空错误串时原样带出驱动原文(prepError / driverText),
   * 并把本节标记为失败 —— 但不抛异常、不中断整轮(调用方照常继续下一节)。
   */
  static async runSn(): Promise<SnRunResult> {
    let prep: string = '';
    try {
      prep = snPrep();
    } catch (e) {
      prep = 'prepare() 抛出异常：' + e.message;
    }
    if (prep.length > 0) {
      const bad: SnRunResult = BenchRunner.emptySnResult(prep);
      bad.prepError = prep;
      bad.driverText = prep;
      bad.benchVersion = BenchRunner.snBenchVersion();
      bad.benchVersionText = BenchRunner.snVersionTextSafe();
      return bad;
    }
    try {
      const raw: string = snRunJson();
      const r: SnRunResult = BenchRunner.parseSn(raw);
      r.raw = raw;
      return r;
    } catch (e) {
      const bad: SnRunResult = BenchRunner.emptySnResult('run() 抛出异常：' + e.message);
      bad.benchVersion = BenchRunner.snBenchVersion();
      return bad;
    }
  }

  // ---- 存储 I/O 小节 ----
  // 沙箱根目录一律由 ArkTS 传入(通常是 context.filesDir), native 不自己猜路径。
  static storagePrepare(dir: string): boolean {
    try {
      return storageSetDir(dir);
    } catch (e) {
      console.warn('[AB] storageSetDir failed');
      return false;
    }
  }

  // 只读探测(不写任何文件): 空间够不够、O_DIRECT / fadvise 能不能用。返回原始 JSON 文本。
  static storageInfoText(): string {
    try {
      return storageInfo();
    } catch (e) {
      return '{"ok":false,"error":"调用 storageInfo 失败: ' + e.message + '"}';
    }
  }

  static async runStorage(optionsJson: string): Promise<StorageRunResult> {
    try {
      const raw: string = await storageRun(optionsJson);
      const o = BenchRunner.parseObj(raw);
      const env = BenchRunner.jObj(o, 'env');
      const head = BenchRunner.jObj(o, 'headline');
      const items: StorageItemRow[] = [];
      const rawItems: Object[] = (o.items as Object[]) ?? [];
      for (const it of rawItems) {
        const r = it as Record<string, Object>;
        items.push({
          id: BenchRunner.jStr(r, 'id', ''),
          name: BenchRunner.jStr(r, 'name', ''),
          ok: BenchRunner.jBool(r, 'ok', false),
          ms: BenchRunner.jNum(r, 'ms', 0),
          bytes: BenchRunner.jNum(r, 'bytes', -1),
          mbps: BenchRunner.jNum(r, 'mbps', -1),
          iops: BenchRunner.jNum(r, 'iops', -1),
          ops: BenchRunner.jNum(r, 'ops', -1),
          auxMs: BenchRunner.jNum(r, 'auxMs', -1),
          auxName: BenchRunner.jStr(r, 'auxName', ''),
          mode: BenchRunner.jStr(r, 'mode', ''),
          comparable: BenchRunner.jStr(r, 'comparable', ''),
          error: BenchRunner.jStr(r, 'error', ''),
          note: BenchRunner.jStr(r, 'note', '')
        });
      }
      const warnings: string[] = [];
      const rawWarn: Object[] = (o.warnings as Object[]) ?? [];
      for (const w of rawWarn) {
        warnings.push(w as string);
      }
      return {
        ok: BenchRunner.jBool(o, 'ok', false),
        skipped: BenchRunner.jBool(o, 'skipped', false),
        skipReason: BenchRunner.jStr(o, 'skipReason', ''),
        fatal: BenchRunner.jStr(o, 'fatal', ''),
        sectionTitle: BenchRunner.jStr(o, 'sectionTitle', ''),
        dir: BenchRunner.jStr(o, 'dir', ''),
        totalMs: BenchRunner.jNum(o, 'totalMs', 0),
        budgetMs: BenchRunner.jNum(o, 'budgetMs', 0),
        stoppedEarly: BenchRunner.jBool(o, 'stoppedEarly', false),
        stopReason: BenchRunner.jStr(o, 'stopReason', ''),
        items: items,
        warnings: warnings,
        seqWriteDurableMbps: BenchRunner.jNum(head, 'seqWriteDurableMbps', 0),
        seqReadColdMbps: BenchRunner.jNum(head, 'seqReadColdMbps', 0),
        randRead4kIops: BenchRunner.jNum(head, 'randRead4kIops', 0),
        randWrite4kIops: BenchRunner.jNum(head, 'randWrite4kIops', 0),
        filesPerSecCreate: BenchRunner.jNum(head, 'filesPerSecCreate', 0),
        filesPerSecDelete: BenchRunner.jNum(head, 'filesPerSecDelete', 0),
        totalBytes: BenchRunner.jNum(env, 'totalBytes', 0),
        freeBytes: BenchRunner.jNum(env, 'freeBytes', 0),
        availBytes: BenchRunner.jNum(env, 'availBytes', 0),
        blockSize: BenchRunner.jNum(env, 'blockSize', 0),
        oDirectWorks: BenchRunner.jBool(env, 'oDirectWorks', false),
        fadviseDontNeedWorks: BenchRunner.jBool(env, 'fadviseDontNeedWorks', false),
        bypassMethod: BenchRunner.jStr(env, 'bypassMethod', ''),
        raw: raw
      };
    } catch (e) {
      return {
        ok: false, skipped: false, skipReason: '', fatal: '调用存储跑分失败: ' + e.message,
        sectionTitle: '', dir: '', totalMs: 0, budgetMs: 0, stoppedEarly: false, stopReason: '',
        items: [], warnings: [], seqWriteDurableMbps: 0, seqReadColdMbps: 0, randRead4kIops: 0,
        randWrite4kIops: 0, filesPerSecCreate: 0, filesPerSecDelete: 0, totalBytes: 0,
        freeBytes: 0, availBytes: 0, blockSize: 0, oDirectWorks: false,
        fadviseDontNeedWorks: false, bypassMethod: '', raw: ''
      };
    }
  }

  // 兜底清理: 无论成功/失败/提前中止, 都把 <filesDir>/storage_bench 下的临时文件删干净。
  static storageCleanupNow(): boolean {
    try {
      return storageCleanup();
    } catch (e) {
      console.warn('[AB] storageCleanup failed');
      return false;
    }
  }

  private static async one(id: number, threads: number): Promise<TestResult> {
    const raw: string = await runTest(id, threads);
    const o = BenchRunner.parseObj(raw);
    return {
      name: (o.name as string) ?? 'test',
      detail: (o.detail as string) ?? '',
      ms: (o.ms as number) ?? 0,
      score: (o.score as number) ?? 0,
      metric: (o.metric as number) ?? 0,
      unit: (o.unit as string) ?? '',
      parallelism: (o.parallelism as number) ?? 1,
      // ---- 落核 / 该核标称上限 / 运行时频率的取证(2026-10 补) ----
      //  以前这三样在自研套件路径上全是"没上报"(cpu=-1 · maxKhz=0), 于是报告里看不出
      //  这一项跑在哪颗核上。缺字段一律回落"未上报"的默认值, 不写 undefined。
      cpu: (o.cpu as number) ?? -1,
      cpuAtStart: (o.cpuAtStart as number) ?? -1,
      cpuMaxKhz: (o.cpuMaxKhz as number) ?? 0,
      cpuRank: (o.cpuRank as number) ?? -1,
      cpuBound: (o.cpuBound as boolean) ?? false,
      cpuInFastCluster: (o.cpuInFastCluster as boolean) ?? false,
      cpuInMachineTopTier: (o.cpuInMachineTopTier as boolean) ?? false,
      cpuFastClusterCores: (o.cpuFastClusterCores as number) ?? 0,
      cpuFastClusterMaxKhz: (o.cpuFastClusterMaxKhz as number) ?? 0,
      cpuMachineTopTierCores: (o.cpuMachineTopTierCores as number) ?? 0,
      cpuMachineTopTierKhz: (o.cpuMachineTopTierKhz as number) ?? 0,
      cpuAllowedCount: (o.cpuAllowedCount as number) ?? 0,
      cpuSingleTargetCores: (o.cpuSingleTargetCores as number) ?? 0,
      cpuInfo: (o.cpuInfo as string) ?? '',
      runFreq: (o.runFreq as string) ?? ''
    };
  }

  /**
   * 「本机性能天花板」一行(native 唯一来源).
   *   首页 / 结果页 / 报告三处都读这一个函数 —— 三处显示的是同一句话, 不可能各说各话.
   *   失败(老 .so 没有这个导出 / native 抛异常)时返回空串: 调用方按"未上报"显示,
   *   不用任何本地拼出来的近似句冒充.
   */
  static ceiling(): string {
    try {
      const s: string = ceilingText();
      return s.length > 0 ? s : '';
    } catch (e) {
      return '';
    }
  }

  // =========================================================================
  // native 崩溃取证桥(crash_guard.cpp 的 8 个导出)
  //
  // 为什么要在这里转一手: libaurorabench.so 的 napi 导入会产生一条
  // "module ... is not verified" 的 ArkTS 警告(基线 11 条), 所以只有本来就已经
  // 导入它的 BenchRunner / Index 直接调用 native; EntryAbility 通过这里的
  // initNativeGuard() 完成"尽早 setLogDir", 不额外多出一条警告。
  // 桥只做记录与自检: 不改任何负载的计算、顺序与计分。
  // =========================================================================

  // EntryAbility.onCreate 里最早调用: 把 filesDir 交给取证装置(越早越好,
  // native 侧会立刻把 <filesDir>/native_crash.txt open 好并常驻 fd, 处理器里只 write)。
  // 返回值与状态同时记进 CrashGuard, 供 UI 显示(失败不静默)。
  static initNativeGuard(dir: string): boolean {
    let ok: boolean = false;
    let ready: boolean = false;
    let path: string = '';
    try {
      ok = setLogDir(dir);
      ready = nativeCrashGuardReady();
      path = nativeCrashLogPath();
    } catch (e) {
      console.warn('[AB] native crash guard init failed');
    }
    CrashGuard.get().setGuardState(dir, ok, ready, path);
    return ok;
  }

  static nativeGuardReady(): boolean {
    try {
      return nativeCrashGuardReady();
    } catch (e) {
      return false;
    }
  }

  static nativeGuardPath(): string {
    try {
      return nativeCrashLogPath();
    } catch (e) {
      return '';
    }
  }

  // 逐项地址空间归因(旁路: 不进任何分数、不影响任何负载)。
  // 回答的是一个真机问题: 「这一轮的虚拟地址空间是哪一项撑起来的」——
  // 实测 VmSize 到过 14 GB 而 VmRSS 只有 200 MB。native 在每个 item 标记点采一次
  // /proc/self/status, 这里把整表取回来; 空串 = 本次会话还没跑过任何一项。
  // 同一个渲染函数也用在崩溃记录里, 两处同源、不会自相矛盾。
  static itemMemoryReport(): string {
    try {
      return itemMemoryReport();
    } catch (e) {
      return '';
    }
  }

  // 逐项账目落盘文件(逐行追加, 96 KB 上限)。真机上进程最可能是被 SIGKILL —— 连崩溃记录
  // 都不会有, 那份文件就是盘上唯一的地址空间归因。空串 = 还没调 setLogDir。
  static itemMemoryLogPath(): string {
    try {
      return itemMemoryLogPath();
    } catch (e) {
      return '';
    }
  }

  // 读回上次留下的 native 现场(空串 = 磁盘上没有记录)。读完由调用方 clear。
  static takeNativeReport(): string {
    try {
      return takeNativeCrashReport();
    } catch (e) {
      return '';
    }
  }

  static clearNativeReport(): void {
    try {
      clearNativeCrashReport();
    } catch (e) {
      console.warn('[AB] native crash report clear failed');
    }
  }

  // 自检钩子: 0 = 只写一条链路自检记录(不崩); 1 = 故意 SIGSEGV; 2 = raise(SIGABRT)。
  // 只给「关于」页的取证实测卡用, 正常跑分流程里永远不会调到。
  static nativeSelfTest(mode: number): void {
    crashSelfTest(mode);
  }

  // 官方 CoreMark 标准分(每秒迭代数), 可与各平台公开数据库直接对照
  static async runCoreMark(): Promise<string> {
    // 面包屑 + 临死内存足迹(旁路, 不参与任何计算): 开跑前先采一口 native 内存再写盘
    sampleMemory();
    await CrashGuard.get().mark('CoreMark', 'CoreMark 官方负载', 1, 1, lastMemorySample());
    try {
      const raw: string = await runCoreMark();
      const o = BenchRunner.parseObj(raw);
      const ok = (o.ok as boolean) ?? false;
      if (!ok) {
        // 运行日志(旁路): 失败也留一行, note 带上 native 返回的原因
        RunLog.get().item(PH_OWN, 'CoreMark', 1, 1, (o.ms as number) ?? 0, 0, '', 0, 1, false,
          'CoreMark 运行失败: ' + ((o.unit as string) ?? 'native 未返回原因'));
        return 'CoreMark 运行失败';
      }
      const score = (o.score as number) ?? 0;
      const crc = ((o.crcOk as boolean) ?? false) ? '校验通过' : '校验失败';
      RunLog.get().item(PH_OWN, 'CoreMark', 1, 1, (o.ms as number) ?? 0, score, 'iterations/sec',
        score, 1, true, '官方 CoreMark 负载 · ' + crc);
      return score.toFixed(2) + ' iterations/sec（' + crc + '）';
    } finally {
      await CrashGuard.get().clear();
    }
  }

  // 跑"一项"GB7 负载并解析成 Gb7Result。
  // 负载顺序、参数(threads 原样透传)、分数解析口径都与原来完全一致 —— 一行计算逻辑都没动。
  private static async gb7RunOne(i: number, threads: number): Promise<Gb7Result> {
    const raw: string = await runGb7(i, threads);
    const o = BenchRunner.parseObj(raw);
    const metricRaw: string = (o.metric as string) ?? '0';
    const base: Gb7Result = cpuNeutralResult(
      (o.name as string) ?? 'test',
      (o.score as number) ?? 0,
      parseFloat(metricRaw),
      (o.unit as string) ?? '',
      (o.ms as number) ?? 0);
    // 注册表的 scored: 这一项是不是计分项(与"这一次有没有拿到分"是两件事)。
    // 界面据此把"计分项跑失败"与"按政策不计分"分开显示(判据见 itemScoredButFailed)。
    base.scoredByPolicy = BenchRunner.benchItemIsScored(i);
    // 负载 id: 复合分要按 id 传数组给 native(见 Gb7Result.loadId 的说明)。
    base.loadId = i;
    return BenchRunner.withCpuFields(base, o);
  }

  // 把 native 给的诊断键填进 Gb7Result(缺字段用默认值; 不碰其它任何字段)
  private static withCpuFields(r: Gb7Result, o: Record<string, Object>): Gb7Result {
    const c: Gb7Partial = cpuFieldsFrom(o);
    r.cpu = c.cpu;
    r.cpuMaxKhz = c.cpuMaxKhz;
    r.cpuRank = c.cpuRank;
    r.cpuBound = c.cpuBound;
    r.cpuAtStart = c.cpuAtStart;
    r.cpuInFastCluster = c.cpuInFastCluster;
    r.cpuAtStartInFastCluster = c.cpuAtStartInFastCluster;
    r.cpuFastClusterCores = c.cpuFastClusterCores;
    r.cpuFastClusterMaxKhz = c.cpuFastClusterMaxKhz;
    r.cpuFastClusterMask = c.cpuFastClusterMask;
    r.cpuWorkers = c.cpuWorkers;
    r.cpuWorkersOverflow = c.cpuWorkersOverflow;
    r.cpuInfo = c.cpuInfo;
    r.cpuAllowedOk = c.cpuAllowedOk;
    r.cpuAllowedCount = c.cpuAllowedCount;
    r.cpuAllowedMask = c.cpuAllowedMask;
    r.cpuAppliedMask = c.cpuAppliedMask;
    r.cpuFastClusterFallback = c.cpuFastClusterFallback;
    r.cpuFastClusterTierIndex = c.cpuFastClusterTierIndex;
    r.cpuInFastClusterJudged = c.cpuInFastClusterJudged;
    r.cpuInMachineTopTier = c.cpuInMachineTopTier;
    r.cpuMachineTopTierCores = c.cpuMachineTopTierCores;
    r.cpuMachineTopTierKhz = c.cpuMachineTopTierKhz;
    r.cpuMachineTopTierMask = c.cpuMachineTopTierMask;
    r.cpuFastClusterSource = c.cpuFastClusterSource;
    r.cpuAllowedText = c.cpuAllowedText;
    r.cpuBoundReason = c.cpuBoundReason;
    r.diag = c.diag;
    r.runFreq = c.runFreq;
    // QoS 运行条件 / 只读分层诊断(cgroup·cpuset·core_ctl): 原样搬 native 的文本, 一个字不加工。
    r.qos = c.qos;
    r.cpuset = c.cpuset;
    r.cpuLogical = c.cpuLogical;
    r.cpuPhysical = c.cpuPhysical;
    r.cpuSmtPossible = c.cpuSmtPossible;
    r.cpuSmtEnabled = c.cpuSmtEnabled;
    r.cpuTopoKnown = c.cpuTopoKnown;
    r.cpuThreadsRequested = c.cpuThreadsRequested;
    r.cpuThreadsEffective = c.cpuThreadsEffective;
    r.cpuEffMask = c.cpuEffMask;
    r.cpuTopoSource = c.cpuTopoSource;
    // 可重复性 / 离散度(native repeatability 块; 缺块 = 老 native -> 保持"未上报"默认值)
    const rep: Record<string, Object> = BenchRunner.jObj(o, 'repeatability');
    const repScore: Record<string, Object> = BenchRunner.jObj(rep, 'score');
    const repCred: Record<string, Object> = BenchRunner.jObj(rep, 'credibility');
    r.rounds = BenchRunner.jNum(rep, 'rounds', 0);
    r.roundsOk = BenchRunner.jNum(rep, 'roundsOk', 0);
    r.scoreMedian = BenchRunner.jNum(repScore, 'median', 0);
    r.scoreSpreadPct = BenchRunner.jNum(repScore, 'dispersion', 0);
    r.repeatVerdict = BenchRunner.jStr(repCred, 'verdict', '');
    r.repeatText = BenchRunner.jStr(repCred, 'text', '');
    return r;
  }

  // =========================================================================
  // 可重复性 / 离散度的显示口径(纯读, 不改任何数值)
  //
  //  两条硬规矩(与 native 侧一致, 界面上必须照做)
  //   1) 离散度 > 5%(verdict = UNRELIABLE)时, 界面上必须显眼地写
  //      「本项重复性差, 该数字仅供参考」—— 不许把不稳的数字显示得跟稳的一样;
  //   2) 只跑了 1 轮(或只有 1 轮有有效读数)时, 必须写明
  //      「本次只跑 1 轮, 没有离散度数据, 不能据此判断可信度」—— 不许假装稳定。
  //   阈值(2% / 5%)是我们自己定的, 照 native 的原文显示, 不写成官方阈值。
  // =========================================================================

  /** 该项到底跑了几轮(0 = native 没上报) */
  static repeatRounds(r: Gb7Result): number {
    return r.rounds > 0 ? r.rounds : 0;
  }

  /** 有没有"离散度数据"(至少要 2 轮有有效读数) */
  static repeatHasData(r: Gb7Result): boolean {
    return r.roundsOk >= 2;
  }

  /** 重复性差(离散度超阈值) —— 界面上必须用报警色 +  文案 */
  static repeatBad(r: Gb7Result): boolean {
    return r.repeatVerdict === 'UNRELIABLE';
  }

  /** 运行失败 / 全程没有有效读数 */
  static repeatNoValid(r: Gb7Result): boolean {
    return r.repeatVerdict === 'NO_VALID_ROUNDS' || r.repeatVerdict === 'RUN_FAILED';
  }

  /** 一行显示文本(轮数 · 离散度 · 结论)。没有数据时写"没有离散度数据", 不写"稳定"。 */
  static repeatLine(r: Gb7Result): string {
    if (r.repeatVerdict.length === 0) {
      return '';
    }
    if (r.repeatVerdict === 'RUN_FAILED') {
      return '重复性：本项运行失败，没有任何轮次数据 —— 不编离散度';
    }
    if (r.repeatVerdict === 'NO_VALID_ROUNDS') {
      return '重复性：跑了 ' + r.rounds.toString() + ' 轮但没有任何一轮给出有效读数，没有代表值';
    }
    const rounds: number = r.rounds > 0 ? r.rounds : 1;
    const okRounds: number = r.roundsOk > 0 ? r.roundsOk : 0;
    if (okRounds < 2) {
      // 不许把"只有一轮"说成"稳定"
      if (rounds <= 1) {
        return '本次只跑 1 轮, 没有离散度数据, 不能据此判断可信度';
      }
      return '本次跑了 ' + rounds.toString() + ' 轮, 但只有 ' + okRounds.toString() +
        ' 轮给出有效读数, 没有离散度数据, 不能据此判断可信度';
    }
    const sp: string = r.scoreSpreadPct.toFixed(2);
    if (r.repeatVerdict === 'RELIABLE') {
      return rounds.toString() + ' 轮 · 相对离散度 ' + sp + '% (<= 2%) · 重复性好，代表值取中位';
    }
    if (r.repeatVerdict === 'FAIR') {
      return rounds.toString() + ' 轮 · 相对离散度 ' + sp + '% (2% ~ 5%) · 重复性一般，代表值仍取中位';
    }
    return '本项重复性差, 该数字仅供参考 ' + rounds.toString() + ' 轮 · 相对离散度 ' + sp +
      '% (> 5%)';
  }

  /** 阈值来源那一句(必须与数字一起显示; native 原文优先) */
  static repeatThresholdNote(): string {
    return '阈值(2% / 5%)与判定都是我们自己定的, 不是官方阈值。';
  }

  /**
   * 复合分的可重复性(libaurorabench.so 的 gb7CompositeSingle / gb7CompositeMulti)。
   *
   * 取值口径(2026-10-08 修)
   * 旧写法: 不传 scores -> native 读它自己那份 g_lastScore 缓存。那份缓存当时**没有阶段
   *   维度**, 而多核阶段只跑 8 项却会把这 8 项覆盖成多核分, 于是 native 算出来的"单核复合分"
   *   其实是 {8 项多核分} ∪ {7 项单核分} 的几何平均 —— 真机 561.1, 而卡片口径(15 项单核分)
   *   是 328.2, 差 1.7 倍。报告里那行「核对不一致」就是它。这是 bug, 不是口径差。
   * 新写法: 总是把本阶段的逐项分按负载 id 传下去(native 一直有这个可选参数), 于是
   *   native 算的 composite 与报告卡片上的数由构造保证同源; 剩余差别只有 native 回传
   *   单项分时 %.1f 的舍入(实测 923.8 vs 923.9 = 0.01%)。
   * 返回 native 原样 JSON(失败返回 '', 调用方写"没拿到", 不编一个离散度)。
   */
  static compositeRepeatJson(multi: boolean, scores: number[]): string {
    try {
      return multi ? gb7CompositeMulti(scores) : gb7CompositeSingle(scores);
    } catch (e) {
      return '';
    }
  }

  /**
   * 把一列 Gb7Result 摊成"按负载 id 索引"的分数数组 —— 就是 native scoreOf() 的取值口径。
   *
   * 为什么不能按下标: 单核阶段的列表有 16 项、多核阶段只有 8 项, 而 native 的数组是
   * "下标 = 负载 id"; 用列表下标会整体错位(例如多核列表的第 1 项其实是 id=0 之后的某一项)。
   * 所以这里只认 r.loadId, 不认位置。
   * 拿不到 loadId(-1, GPU / 自研 / 老 native)的项一律不进数组。
   *
   * 2026-10-05 修(口径): 这里不再有 skipIds / 强制置 0
   * 旧写法允许调用方传一串"要置 0 的负载 id", 用来把"未通过随芯片变化自检"的项从复合分里
   * 悄悄抹掉。后果是同一份报告自相矛盾: items[] 里那一项明明是正分(真机 8.1:
   * File Compression 176.9 / Photo Library 5446.1 / HDR 39.2 / Ray Tracer 3957.8 / Clang 14761.3),
   * native 却把它的 0 解释成"本次没有拿到正的分数"并写进「排除项」, 而小节结论那一行同时印着
   * "未拿到分 0 项"; 更要命的是置 0 用的是跨阶段并集(多核判 FAIL 的项被一起置 0),
   * 单核口径因此也丢项 —— 真机报告里那行「核对不一致 native 409.4 / 本报告 416.2」就是它。
   * 现在: 数组逐位等于本阶段各项的真实分数, 于是
   *   复合分参与项集合 == 报告 items[] 里该阶段有正分的项集合  (逐项相等, 不再是"看起来像")
   * "未通过随芯片变化自检"的项照常参与复合分, 只在报告与界面上标注"不可用于跨芯片比较"
   * (它是提示, 不再是静默的口径改写)。
   */
  static gb7ScoreArray(list: Gb7Result[]): number[] {
    const n: number = gb7Count();
    const arr: number[] = [];
    for (let i = 0; i < n; i++) {
      arr.push(0);
    }
    for (const r of list) {
      if (r.loadId >= 0 && r.loadId < n) {
        arr[r.loadId] = r.score;
      }
    }
    return arr;
  }

  /**
   * native 复合分 JSON 里 items[] 的负载 id 集合(= native 实际的参与项集合)。
   * 用途: 运行时把"native 的参与项集合"与"报告 items[] 里该阶段有正分的项集合"逐项比对,
   * 不一致就 FAIL —— 从源头上禁止"复合分少算了几项却没人发现"。
   */
  static compositeItemIds(json: string): number[] {
    const out: number[] = [];
    if (json.length === 0) {
      return out;
    }
    try {
      const o = JSON.parse(json) as Record<string, Object>;
      const arr: Object[] = (o.items as Object[]) ?? [];
      for (const it of arr) {
        const m = it as Record<string, Object>;
        const id: number = (m.id as number) ?? -1;
        if (id >= 0 && out.indexOf(id) < 0) {
          out.push(id);
        }
      }
    } catch (e) {
      return out;
    }
    return out;
  }

  /** 从复合分 JSON 里取一个数(null / 缺字段 -> 0; 0 在这里表示"没有这个数") */
  static compositeNum(json: string, key: string): number {
    if (json.length === 0) {
      return 0;
    }
    try {
      const o = JSON.parse(json) as Record<string, Object>;
      const rep: Record<string, Object> = (o.repeatability as Record<string, Object>) ?? ({} as Record<string, Object>);
      const v: number = (rep[key] as number) ?? 0;
      return v > 0 ? v : 0;
    } catch (e) {
      return 0;
    }
  }

  /** 复合分的"由中位分算出的复合分"(native composite 字段; 0 = 没拿到) */
  static compositeValue(json: string): number {
    if (json.length === 0) {
      return 0;
    }
    try {
      const o = JSON.parse(json) as Record<string, Object>;
      return (o.composite as number) ?? 0;
    } catch (e) {
      return 0;
    }
  }

  /** 复合分的可信度判定(native repeatability.credibility.verdict; '' = 没拿到) */
  static compositeVerdict(json: string): string {
    if (json.length === 0) {
      return '';
    }
    try {
      const o = JSON.parse(json) as Record<string, Object>;
      const rep: Record<string, Object> = (o.repeatability as Record<string, Object>) ?? ({} as Record<string, Object>);
      const cred: Record<string, Object> = (rep.credibility as Record<string, Object>) ?? ({} as Record<string, Object>);
      return (cred.verdict as string) ?? '';
    } catch (e) {
      return '';
    }
  }

  /** 复合分的可信度原文(native repeatability.credibility.text) */
  static compositeVerdictText(json: string): string {
    if (json.length === 0) {
      return '';
    }
    try {
      const o = JSON.parse(json) as Record<string, Object>;
      const rep: Record<string, Object> = (o.repeatability as Record<string, Object>) ?? ({} as Record<string, Object>);
      const cred: Record<string, Object> = (rep.credibility as Record<string, Object>) ?? ({} as Record<string, Object>);
      return (cred.text as string) ?? '';
    } catch (e) {
      return '';
    }
  }

  // ---- 复合分 JSON 里的"口径证据"读数(纯搬运, 一个数都不改) ----

  /** native 回传的"这个复合分是由哪一份单项分算出来的"原文('' = 没拿到) */
  static compositeSourceText(json: string): string {
    if (json.length === 0) {
      return '';
    }
    try {
      const o = JSON.parse(json) as Record<string, Object>;
      const src: string = (o.scoreSource as string) ?? '';
      const txt: string = (o.scoreSourceText as string) ?? '';
      const stage: string = (o.stageName as string) ?? '';
      if (src.length === 0) {
        return '';
      }
      return src + (stage.length > 0 ? (' · ' + stage) : '') + (txt.length > 0 ? ('：' + txt) : '');
    } catch (e) {
      return '';
    }
  }

  /** 复合分实际参与项数(native count; 0 = 没拿到) */
  static compositeCount(json: string): number {
    if (json.length === 0) {
      return 0;
    }
    try {
      const o = JSON.parse(json) as Record<string, Object>;
      return (o.count as number) ?? 0;
    } catch (e) {
      return 0;
    }
  }

  /** 复合分参与项的逐项对照文本(native items[]; '' = 没拿到) */
  static compositeItemsText(json: string): string {
    return BenchRunner.compositeListText(json, 'items', false);
  }

  /** 复合分排除项的逐项对照文本(native excluded[]; '' = 没拿到) */
  static compositeExcludedText(json: string): string {
    return BenchRunner.compositeListText(json, 'excluded', true);
  }

  private static compositeListText(json: string, key: string, withWhy: boolean): string {
    if (json.length === 0) {
      return '';
    }
    try {
      const o = JSON.parse(json) as Record<string, Object>;
      const arr: Object[] = (o[key] as Object[]) ?? [];
      const parts: string[] = [];
      for (const it of arr) {
        const m = it as Record<string, Object>;
        const name: string = (m.name as string) ?? '?';
        const id: number = (m.id as number) ?? -1;
        if (withWhy) {
          parts.push(name + '(id=' + id.toString() + ': ' + ((m.why as string) ?? '') + ')');
        } else {
          parts.push(name + ' ' + (((m.score as number) ?? 0).toFixed(1)) + '(id=' + id.toString() + ')');
        }
      }
      return parts.join(' · ');
    } catch (e) {
      return '';
    }
  }

  /** 复合分的"逐轮复合分"个数(0 = 没有这一列) */
  static compositeRounds(json: string): number {
    if (json.length === 0) {
      return 0;
    }
    try {
      const o = JSON.parse(json) as Record<string, Object>;
      const rep: Record<string, Object> = (o.repeatability as Record<string, Object>) ?? ({} as Record<string, Object>);
      const arr: Object[] = (rep.compositePerRound as Object[]) ?? [];
      return arr.length;
    } catch (e) {
      return 0;
    }
  }

  // CS1 单核逐项执行(可选内存闸门)。这是唯一的"单核 16 项"实现:
  // runGb7All 与稳定性压测都走它, 因此两边跑的一定是同一批负载、同一顺序、同一参数。
  //
  // gate = null   -> 不做内存闸门, 行为与历史版本完全一致(runGb7All 走这条)
  // gate != null  -> 每项开跑前比一次可用内存, 不够就跳过该项(skipped = true),
  //                  跳过的项不执行、不产生分数, 但不中断整轮
  // phase 只用于 runlog.jsonl 的分档(单次跑分 = 'CS1单核', 压测 = '压测'), 不影响任何行为
  static async runGb7Items(threads: number, onItem: (o: Gb7ItemOutcome) => void,
    gate: MemStatus | null, phase: string = PH_GB7_SINGLE): Promise<Gb7ItemOutcome[]> {
    const n: number = gb7Count();
    const out: Gb7ItemOutcome[] = [];
    try {
    for (let i = 0; i < n; i++) {
      const name: string = gb7Name(i);
      // 面包屑: 每"单项"开跑前写盘(粒度 = 单项, 不是每轮)
      // 先采样(此时是"这项还没吃内存"的基线), 再写面包屑: 盘上留下的就是该项起跑时的内存足迹
      sampleMemory();
      await CrashGuard.get().mark('CS1 单核', name, i + 1, n, lastMemorySample());
      const need: number = itemMemEstimateMb(name);
      let reason: string | null = null;
      if (gate !== null) {
        reason = MemoryProbe.gate(gate, need);
      }
      if (reason !== null) {
        // 内存闸门: 跳过该项, 不计分, 不影响其它项
        const skippedOutcome: Gb7ItemOutcome = {
          index: i,
          name: name,
          skipped: true,
          skipReason: reason,
          gate: {
            supported: gate !== null && gate.supported,
            availableMb: gate !== null ? gate.availableMb : 0,
            needMb: need,
            skipped: true,
            reason: reason
          },
          result: null
        };
        // 运行日志(旁路): 跳过的项也留一行, note = 跳过原因
        RunLog.get().item(phase, name, i + 1, n, 0, 0, '', 0, 1, false, reason);
        out.push(skippedOutcome);
        onItem(skippedOutcome);
        continue;
      }
      const r: Gb7Result = await BenchRunner.gb7RunOne(i, threads);
      const doneOutcome: Gb7ItemOutcome = {
        index: i,
        name: r.name,
        skipped: false,
        skipReason: '',
        gate: {
          supported: gate !== null && gate.supported,
          availableMb: gate !== null ? gate.availableMb : 0,
          needMb: need,
          skipped: false,
          reason: ''
        },
        result: r
      };
      // 运行日志(旁路): 每完成一项写一行(一次小 append)
      RunLog.get().item(phase, r.name, i + 1, n, r.ms, r.metric, r.unit, r.score, 1,
        r.score > 0, itemNote(false, '', r.unit, r.score, r.ms) + cpuNote(r) + (r.diag.length > 0 ? ' · ' + r.diag : ''));
      out.push(doneOutcome);
      onItem(doneOutcome);
    }
    return out;
    } finally {
      // 整轮结束(或 JS 侧抛错)后清面包屑; native 崩溃时进程直接消失, 走不到这里
      await CrashGuard.get().clear();
    }
  }

  // 稳定性压测的一轮 = CS1 单核 16 项(与首页卡片同一份实现, 分数口径也相同)。
  // 与 runGb7All 的唯一区别: 开跑前逐项做内存闸门(memGate = null 时不做),
  // 并且把"跳过/失败"的逐项信息一并交给上层记录。不写历史、不改计分。
  static async runStressRound(run: number, threads: number, memGate: MemStatus | null,
    onItem: (o: Gb7ItemOutcome) => void,
    onStatus: (runNo: number, item: string) => void): Promise<Gb7ItemOutcome[]> {
    const items: Gb7ItemOutcome[] = await BenchRunner.runGb7Items(threads, (o: Gb7ItemOutcome) => {
      onStatus(run, o.name);
      onItem(o);
    }, memGate, PH_STRESS);
    return items;
  }

  // CS1 负载: 分数已按本工程自定的换算常量锚定(常量见文件头的口径说明)
  static async runGb7All(threads: number, onOne: (r: Gb7Result) => void): Promise<Gb7Result[]> {
    const out: Gb7Result[] = [];
    // gate = null: 单次跑分保持历史行为不变(不做闸门); 面包屑的写入与清除都在 runGb7Items 内部
    await BenchRunner.runGb7Items(threads, (o: Gb7ItemOutcome) => {
      if (!o.skipped && o.result !== null) {
        const r: Gb7Result = o.result as Gb7Result;
        out.push(r);
        onOne(r);
      }
    }, null, PH_GB7_SINGLE);
    return out;
  }

  // CS1 GPU 负载(11 项, 全部在 GPU 上以真实 GLSL ES 管线执行)
  static async runGb7Gpu(onOne: (r: Gb7Result) => void): Promise<Gb7Result[]> {
    const prep: string = gpu7Prepare();
    if (prep.length > 0) {
      const err: Gb7Result = cpuNeutralResult('GPU 初始化失败：' + prep, 0, 0, '', 0);
      // 运行日志(旁路): GPU 初始化失败也留一行
      RunLog.get().item(PH_GB7_GPU, 'GPU 初始化', 0, gpu7Count(), 0, 0, '', 0, 1, false, prep);
      onOne(err);
      return [err];
    }
    const n: number = gpu7Count();
    const out: Gb7Result[] = [];
    try {
    for (let i = 0; i < n; i++) {
      // 面包屑: GPU 单项开跑前写盘
      // GPU 阶段: native 的自动标记只覆盖 runGb7/runCoreMark/runTest,
      // 所以这里必须补一次 setCurrentItem, 否则 native 现场里的 item 是空的(无法与面包屑对齐)。
      sampleMemory();
      setCurrentItem('CS1 GPU', gpu7Name(i), i + 1, n);
      await CrashGuard.get().mark('CS1 GPU', gpu7Name(i), i + 1, n, lastMemorySample());
      // ---- GPU 跑满判据(2026-10 追加; 在正式计时之前、计时区间之外跑一次探测) ----
      //  为什么要单独一趟: 判据要的读数(每帧等 GPU 排水的时间 / 两档吞吐比)只能靠
      //  "同一份负载按不同工作量跑两档"拿到, 而这两档的耗时与读数一律丢弃 ——
      //  它们不进 metric / score / 复合分, 也不改任何 GPU 负载的尺寸 / 算法 / 帧数。
      //  探测失败不影响正式那一遍: 失败时判据显示"本项不适用(原因)", 不假装跑满。
      let fullness: string = '';
      try {
        const fRaw: string = gpu7Fullness(i);
        const fo = BenchRunner.parseObj(fRaw);
        if (((fo.ok as boolean) ?? false)) {
          fullness = (fo.text as string) ?? '';
        } else {
          fullness = 'GPU 跑满判据 -> 本项不适用(原因: 探测没有跑起来 —— ' +
            ((fo.error as string) ?? '未知错误') + '; 因此既不记"已达到"也不记"未达到")';
        }
      } catch (e) {
        fullness = 'GPU 跑满判据 -> 本项不适用(原因: 调用探测失败 —— ' + e.message +
          '; 因此既不记"已达到"也不记"未达到")';
      }
      const raw: string = gpu7Run(i);
      const o = BenchRunner.parseObj(raw);
      const ok: boolean = (o.ok as boolean) ?? false;
      let r: Gb7Result;
      if (!ok) {
        r = cpuNeutralResult(gpu7Name(i), 0, 0, (o.error as string) ?? '失败', 0);
      } else {
        r = cpuNeutralResult(
          gpu7Name(i),
          (o.score as number) ?? 0,
          parseFloat((o.metric as string) ?? '0'),
          (o.unit as string) ?? '',
          (o.ms as number) ?? 0);
        if (r.score <= 0) {
          r.unit = ((o.unit as string) ?? '') + ' 未计分';
        }
      }
      r.gpuFullness = fullness;
      // 运行日志(旁路): 每个 GPU 负载一项一行(带跑满判据那一行)
      RunLog.get().item(PH_GB7_GPU, r.name, i + 1, n, r.ms, r.metric, r.unit, r.score, 1,
        ok && r.score > 0, (ok ? itemNote(false, '', r.unit, r.score, r.ms) : ('GPU 负载失败: ' + r.unit)) +
        (fullness.length > 0 ? (' · ' + fullness) : ''));
      out.push(r);
      onOne(r);
    }
    return out;
    } finally {
      await CrashGuard.get().clear();
    }
  }

  // CS1多核阶段。
  // 官方规则: 只有"在真实场景里本来就多线程"的 8 项进入多核套件
  // (File Compression / Photo Library / Clang / Text Processing / Asset Compression / HDR /
  //  Photo Editor / Ray Tracer); 其余 8 项在官方多核结果页里根本不出现, 因此这里也跳过。
  static async runGb7Multi(threads: number, onOne: (r: Gb7Result) => void): Promise<Gb7Result[]> {
    const n: number = gb7Count();
    // 先数出官方多核套件的项数, 只用于面包屑的"第 x/y 项", 不参与任何计算/顺序
    let multiTotal = 0;
    for (let k = 0; k < n; k++) {
      if (gb7TestIsMulti(k)) {
        multiTotal++;
      }
    }
    const out: Gb7Result[] = [];
    try {
    let seq = 0;
    for (let i = 0; i < n; i++) {
      if (!gb7TestIsMulti(i)) {
        continue;
      }
      seq++;
      // 面包屑: 多核阶段每项开跑前写盘
      sampleMemory();
      await CrashGuard.get().mark('CS1 多核', gb7Name(i), seq, multiTotal, lastMemorySample());
      const raw: string = await runGb7(i, threads);
      const o = BenchRunner.parseObj(raw);
      const metricRaw: string = (o.metric as string) ?? '0';
      const baseM: Gb7Result = cpuNeutralResult(
        (o.name as string) ?? 'test',
        (o.score as number) ?? 0,
        parseFloat(metricRaw),
        (o.unit as string) ?? '',
        (o.ms as number) ?? 0);
      // 注册表的 scored(同单核路径): 多核套件里 Clang 是计分项, 跑失败要按失败显示
      baseM.scoredByPolicy = BenchRunner.benchItemIsScored(i);
      // 负载 id(同单核路径): 多核复合分同样按 id 传数组给 native
      baseM.loadId = i;
      const r: Gb7Result = BenchRunner.withCpuFields(baseM, o);
      // 运行日志(旁路, 多核阶段): 并行度记在 parallelism 字段里; note 追加"跑在哪个核"
      RunLog.get().item(PH_GB7_MULTI, r.name, seq, multiTotal, r.ms, r.metric, r.unit, r.score,
        threads, r.score > 0, itemNote(false, '', r.unit, r.score, r.ms) + cpuNote(r) + (r.diag.length > 0 ? ' · ' + r.diag : ''));
      out.push(r);
      onOne(r);
    }
    return out;
    } finally {
      await CrashGuard.get().clear();
    }
  }

  static async runCpu(onProgress: (p: ProgressInfo) => void, scope: string = ''): Promise<TestResult[]> {
    const n: number = testCount();
    const cores: number = cpuCoreCount();
    // 多核阶段的线程数按 SMT 开关算(开关开 = cores, 关 = 物理核数; 拓扑未知 = cores)。
    // 注意下面 parThreshold 仍然用 cores(逻辑核数)—— 那是"实测并行度是否达标"的判据口径,
    // 与线程数无关, 不能让开关顺带改变它, 否则会悄悄改变"哪些负载计入多核分"。
    const multiThreads: number = BenchRunner.benchMultiThreads(cores);
    const singleTests: TestResult[] = [];
    const singleScores: number[] = [];
    for (let i = 0; i < n; i++) {
      // 面包屑: 单项开跑前写盘
      sampleMemory();
      await CrashGuard.get().mark(scope + 'CPU 单核', testName(i), i + 1, n, lastMemorySample());
      const r = await BenchRunner.one(i, 1);
      // 运行日志(旁路, 自研套件单核)
      RunLog.get().item(PH_OWN, '单核·' + r.name, i + 1, n, r.ms, r.metric, r.unit, r.score,
        r.parallelism, r.score > 0, itemNote(false, '', r.unit, r.score, r.ms));
      singleTests.push(r);
      singleScores.push(r.score);
      onProgress({ phase: 'CPU 单核', index: i + 1, total: n * 2, testName: r.name, lastMs: r.ms, lastScore: r.score });
    }
    const multiScores: number[] = [];
    const multiTests: TestResult[] = [];
    for (let i = 0; i < n; i++) {
      sampleMemory();
      await CrashGuard.get().mark(scope + 'CPU 多核', testName(i), i + 1, n, lastMemorySample());
      const r = await BenchRunner.one(i, multiThreads);
      // 运行日志(旁路, 自研套件多核): parallelism = 实测占用核心数
      RunLog.get().item(PH_OWN, '多核·' + r.name, i + 1, n, r.ms, r.metric, r.unit, r.score,
        multiThreads, r.score > 0,
        itemNote(false, '', r.unit, r.score, r.ms) + BenchRunner.smtRunNote(multiThreads));
      multiScores.push(r.score);
      multiTests.push(r);
      onProgress({ phase: 'CPU 多核', index: n + i + 1, total: n * 2, testName: r.name, lastMs: r.ms, lastScore: r.score });
    }
    const intSingle = subsectionScore(singleTests, INTEGER_TESTS);
    const floatSingle = subsectionScore(singleTests, FLOAT_TESTS);
    const single = compositeFromSubsections(intSingle, floatSingle);
    AppStorage.setOrCreate('intSingle', Math.round(intSingle));
    AppStorage.setOrCreate('floatSingle', Math.round(floatSingle));
    // 多核合成的取舍(本工程自定): 只把实测并行度达标(真正多线程)的负载计入多核分,
    // 避免把单线程敏感的负载强行并行化导致分数失真
    const parThreshold = Math.max(2.0, (cores as number) * 0.35);
    const parallelScores: number[] = [];
    const excluded: string[] = [];
    for (const t of multiTests) {
      if (t.parallelism >= parThreshold) {
        parallelScores.push(t.score);
      } else {
        excluded.push(t.name);
      }
    }
    const usedScores = parallelScores.length >= 2 ? parallelScores : multiScores;
    // 多核同样按子项结构合成(仅使用并行度达标的负载)
    const multiForSubs: TestResult[] = [];
    for (const t of multiTests) {
      if (t.parallelism >= parThreshold) {
        multiForSubs.push(t);
      }
    }
    const subSource = multiForSubs.length >= 2 ? multiForSubs : multiTests;
    const intMulti = subsectionScore(subSource, INTEGER_TESTS);
    const floatMulti = subsectionScore(subSource, FLOAT_TESTS);
    const multi = usedScores.length > 0 && intMulti <= 0 ? Math.round(geomean(usedScores))
      : compositeFromSubsections(intMulti, floatMulti);
    AppStorage.setOrCreate('intMulti', Math.round(intMulti));
    AppStorage.setOrCreate('floatMulti', Math.round(floatMulti));
    AppStorage.setOrCreate('multiExcluded', excluded);
    AppStorage.setOrCreate('lastSingle', single);
    AppStorage.setOrCreate('lastMulti', multi);
    AppStorage.setOrCreate('cpuTests', singleTests);
    AppStorage.setOrCreate('multiTests', multiTests);
    return singleTests;
  }

  static async runGpu(onProgress: (p: GpuProgress) => void, scope: string = ''): Promise<GpuSceneResult[]> {
    const out: GpuSceneResult[] = [];
    const prepRaw: string = prepare();
    const prep = BenchRunner.parseObj(prepRaw);
    if (!((prep.ok as boolean) ?? false)) {
      const msg = (prep.error as string) ?? 'GPU 初始化失败';
      console.error('[AB] gpu prepare failed: ' + msg);
      AppStorage.setOrCreate('gpuError', msg);
      // 运行日志(旁路): 自研 GPU 初始化失败
      RunLog.get().item(PH_OWN, 'GPU 初始化', 0, sceneCount(), 0, 0, '', 0, 1, false, msg);
      return out;
    }
    AppStorage.setOrCreate('gpuError', '');
    const n: number = sceneCount();
    for (let i = 0; i < n; i++) {
      const name: string = sceneName(i);
      // 面包屑: GPU 场景开跑前写盘
      // 自研 GPU 场景同样是 native 之外的阶段: 手动补 native 当前项标记 + 采内存足迹。
      // 阶段名与 ArkTS 面包屑保持一致(scope 为空时就是 "GPU 场景"), 便于两边对照。
      sampleMemory();
      setCurrentItem(scope + 'GPU 场景', name, i + 1, n);
      await CrashGuard.get().mark(scope + 'GPU 场景', name, i + 1, n, lastMemorySample());
      const startRaw: string = start(i, GPU_FRAMES);
      const so = BenchRunner.parseObj(startRaw);
      if (!so.ok) {
        const msg = (so.error as string) ?? 'scene start failed';
        console.error('[AB] gpu scene start failed ' + name + ': ' + msg);
        AppStorage.setOrCreate('gpuError', name + ': ' + msg);
        // 运行日志(旁路): 场景启动失败也留一行
        RunLog.get().item(PH_OWN, 'GPU·' + name, i + 1, n, 0, 0, 'FPS', 0, 1, false, msg);
        continue;
      }
      let guard = 0;
      while (guard < 900) {
        guard++;
        await sleep(250);
        const pollRaw: string = poll();
        const p = BenchRunner.parseObj(pollRaw);
        const running = (p.running as boolean) ?? false;
        onProgress({
          sceneIndex: i,
          sceneName: name,
          fps: (p.fps as number) ?? 0,
          framesDone: (p.framesDone as number) ?? 0,
          framesTarget: (p.framesTarget as number) ?? GPU_FRAMES,
          frameMs: (p.frameMs as number) ?? 0
        });
        if (!running) {
          const ok = (p.ok as boolean) ?? false;
          const unsupported = (p.unsupported as boolean) ?? false;
          const fps: number = (p.fps as number) ?? 0;
          const frameMs: number = (p.frameMs as number) ?? 0;
          const sceneScore: number = (p.score as number) ?? 0;
          const sceneUnit: string = (p.unit as string) ?? '';
          // 运行日志(旁路): 每个自研 GPU 场景一项一行(ms = 单帧耗时)
          RunLog.get().item(PH_OWN, 'GPU·' + name, i + 1, n, frameMs, fps, 'FPS', sceneScore, 1,
            ok && !unsupported && sceneScore > 0,
            unsupported ? '场景不支持(unsupported)：本机未跑此场景'
              : (ok ? ('ms 字段为单帧耗时; ' + (sceneUnit.length > 0 ? sceneUnit : 'FPS'))
                : ('场景失败: ' + ((p.error as string) ?? 'native 未返回原因'))));
          if (ok && !unsupported) {
            out.push({
              scene: name,
              fps: fps,
              frameMs: frameMs,
              metric: (p.metric as number) ?? 0,
              unit: sceneUnit,
              score: sceneScore
            });
          }
          break;
        }
      }
    }
    return out;
  }

  static async runAll(
    onCpu: (p: ProgressInfo) => void,
    onGpu: (p: GpuProgress) => void,
    scope: string = ''
  ): Promise<BenchRecord> {
    try {
    const tests = await BenchRunner.runCpu(onCpu, scope);
    const multiTests: TestResult[] = (AppStorage.get<TestResult[]>('multiTests') ?? []);
    const gpuScenes = await BenchRunner.runGpu(onGpu, scope);
    const single: number = (AppStorage.get<number>('lastSingle') ?? 0);
    const multi: number = (AppStorage.get<number>('lastMulti') ?? 0);
    let gpu = 0;
    if (gpuScenes.length > 0) {
      const scores: number[] = [];
      for (const g of gpuScenes) {
        scores.push(g.score);
      }
      gpu = Math.round(geomean(scores));
    }
    const total = BenchRunner.combine(single, multi, gpu);
    return {
      time: Date.now(),
      total: total,
      single: single,
      multi: multi,
      gpu: gpu,
      intSingle: (AppStorage.get<number>('intSingle') ?? 0),
      floatSingle: (AppStorage.get<number>('floatSingle') ?? 0),
      intMulti: (AppStorage.get<number>('intMulti') ?? 0),
      floatMulti: (AppStorage.get<number>('floatMulti') ?? 0),
      device: BenchRunner.deviceLabel(),
      tests: tests,
      multiTests: multiTests,
      gpuScenes: gpuScenes
    };
    } finally {
      // 整轮跑完(或 JS 侧抛错)后清除面包屑; native 崩溃时进程直接消失, 走不到这里
      await CrashGuard.get().clear();
    }
  }
}
