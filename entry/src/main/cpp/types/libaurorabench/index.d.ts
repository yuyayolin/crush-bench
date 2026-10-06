export const cpuCoreCount: () => number;
export const testCount: () => number;
export const testName: (id: number) => string;
export const runTest: (id: number, threads: number) => Promise<string>;
export const version: () => string;
export const runCoreMark: () => Promise<string>;
export const gb7Count: () => number;
export const gb7Name: (id: number) => string;
/**
 * 跑第 id 项 GB7 负载。返回 JSON:
 * {"name":..,"section":..,"ms":..,"score":..,"metric":..,"unit":..,"parallelism":..,
 *  "gb7Unit":"官方单位","k":系数,"conv":单位换算,"isMulti":bool,"scored":bool,"basis":"说明",
 *  "rounds":轮数,"repeatability":{...}}
 * score = 0 表示该项未计分(单位语义不可比), 原因在 basis。多核阶段(threads>1)用多核表拟合的 k。
 *
 * options(第三个参数, 可选, 扁平 JSON 字符串) —— 只用来改重复轮数:
 *   {"rounds":N} 或 {"gb7Rounds":N}(1..9); 省略 = 默认 2 轮。
 *    轮数只决定"同一项测量做几次":**单轮的负载算法 / 尺寸 / metric / unit / k / conv /
 *     计分公式 / 线程数 / 绑核策略一个都没有变。代表值一律取中位**(不是最好一轮)。
 *    返回里的 repeatability 块:
 *       rounds / roundsRequested / roundsSource("option"|"default") / roundsOk /
 *       roundsDetail[{round,score,metric,ms,cpu,runFreq}]   (逐轮原始值, 一个都不少)
 *       score{median,min,max,dispersion} / metric{...} / ms{median,min,max}
 *       credibility{verdict, thresholdsAreOursNotOfficial:true, thresholds, text}
 *      verdict: RELIABLE(<=2%) / FAIR(2%~5%) / UNRELIABLE(>5%) /
 *               SINGLE_ROUND_NO_DATA(只跑 1 轮 -> 没有离散度数据, 不能据此判断可信度) /
 *               NO_VALID_ROUNDS / RUN_FAILED
 *      disperson(相对离散度) = (最大 - 最小) / 中位 x 100%; 阈值是我们自己定的, 不是官方的。
 */
export const runGb7: (id: number, threads: number, options?: string) => Promise<string>;
/** 该负载是否属于官方多核 8 项(其余 8 项官方多核页里根本不出现, 多核阶段应过滤掉) */
export const gb7TestIsMulti: (id: number) => boolean;
/**
 * 该负载在注册表里是不是计分项(ENTRIES[id].scored)。
 * true  = 计分项: 得 0 分只可能是"这次没跑出来"(运行失败), 界面必须按失败显示;
 * false = 按政策不计分(例如 Structure from Motion, k = 0, 口径不可复核): 得 0 是设计如此。
 * 与 gb7TestIsMulti 一样只是元数据查询: 不执行任何负载、不计分。
 */
export const gb7TestIsScored: (id: number) => boolean;
/** 该项的单位换算与系数来源说明(可直接显示在 UI 上) */
export const gb7TestBasis: (id: number) => string;
/**
 * 单核复合分 = 已计分项(16 项里 Structure from Motion 未计分被剔除)单项分的几何平均。
 * scores 可省略: 省略时用最近一次 runGb7 的单项分(按负载 id 索引)。
 * 返回 JSON: {"ok":true,"mode":"single","composite":..,"count":..,"items":[..],"basis":".."}
 */
export const gb7CompositeSingle: (scores?: number[]) => string;
/** 多核复合分 = 官方多核 8 项几何平均。参数同 gb7CompositeSingle。 */
export const gb7CompositeMulti: (scores?: number[]) => string;

// ---------------------------------------------------------------------------
// 超线程(SMT): CPU 拓扑读取 + 开关(默认开)
//
// 口径(必须保持):
//   * 开关只决定多核阶段使用哪些逻辑核 —— 线程数 / 大核簇掩码 / 池线程落点分配;
//     不改任何负载的算法、工作量、metric 口径与计分公式;
//   * 单核阶段完全不受影响(仍然只用一个线程跑在一个核上);
//   * 关掉 SMT 时不会把两个线程绑到同一个物理核;
//   * 读不到拓扑时不绑错、不假装: known = false, 线程数维持全部逻辑核, 界面注明
//     "拓扑未知(已按 1:1 处理, SMT 开关无效)"。
//   默认值 = 开, 且 native 侧初值就是开: 不调用 setSmtEnabled 时行为与历史完全一致。
// ---------------------------------------------------------------------------

/** smtCapabilities() 返回的 JSON 解析结果 */
export interface SmtCapabilitiesResult {
  ok: boolean;
  /** 拓扑是否已知(false = 三级降级都没读到: 逻辑核数/物理核数按 1:1 处理, 开关无效) */
  known: boolean;
  /** 逻辑核数(= std::thread::hardware_concurrency 口径的核数) */
  logical: number;
  /** 物理核数(已知时是真实物理核数; 未知时等于 logical, 即按 1:1 处理) */
  physical: number;
  /** 物理核数 < 逻辑核数 => true(检测到 SMT) */
  smt: boolean;
  /** 与 smt 同义(显式命名, 供界面区分"开关状态"与"硬件能力") */
  smtPossible: boolean;
  /** 开关当前状态(默认 true) */
  smtEnabled: boolean;
  /** 多核阶段应当使用的线程数: 开 -> logical; 关 -> physical; 未知 -> logical */
  threadsMulti: number;
  /** 把用户请求的线程数原样传给 native 是否等于 native 的选择(关掉 SMT 时为 false) */
  requestedAsIs: boolean;
  /** 实际使用集合的位图, 形如 "0x400f" */
  effectiveMask: string;
  /**
   * 实际使用集合的大小(2026-10-07 新增): 开关开 = 全部逻辑核; 开关关 = 物理核数。
   * 与 threadsMulti 的差别: threadsMulti 已经过了"可用核集合"的夹子, 这一个没有 ——
   * 两个数并排看就知道"被内核夹掉了几个核"。
   */
  effectiveCount: number;
  /**
   * 超线程(SMT)开关的真实约束说明(2026-10-07 新增, 中文一行, 不计分):
   * 关掉开关后多核阶段按物理核跑会变成几个线程、比开启时少几个, 全部按本机读到的
   * 逻辑核 / 物理核 / 可用核集合算出。界面直接显示这一行即可 —— 措辞只有 native 一处,
   * 与 CPU 拓扑那一行(physical / effectiveSet.count / cap)不会各说各话。
   */
  switchHint: string;
  /** 每个逻辑核的物理核编号(索引 = 逻辑核号; 长度 = logical, 最多 64) */
  physicalOfCpu: number[];
  /** 拓扑来源(哪个文件 / "未知(…已按 1:1 处理, SMT 开关无效)") */
  source: string;
  /**
   * 逐路径实测结果(App 自己的 SELinux 域下每个候选文件到底读到了什么 + errno):
   * "thread_siblings_list=14/14 核读到(errno=0) · core_id=14/14(errno=0) · …
   *  cpu/possible=可读(errno=0) · cpu/present=可读(errno=0) · /proc/cpuinfo=可读(errno=0)"
   * 值含义: 0 = 读到了; 13 = EACCES(权限不足); 2 = ENOENT(路径不存在); -1/-2 = 读到了但内容空/异常。
   * 用户用 hdc shell 读这些路径会被拒, 而 App 域可能读得到 —— 这条字符串就是"到底读到了什么"的事实。
   */
  probes: string;
  /** 一行中文说明(可直接显示在「设备画像」里) */
  text: string;
}

/**
 * 读 CPU 拓扑 + 当前的 SMT 口径。返回上面 SmtCapabilitiesResult 的 JSON 字符串
 * (解析失败按"未上报"处理, 详见 BenchRunner.benchCapabilities)。
 * 读不到 thread_siblings_list 时自动降级到 core_id + physical_package_id, 再读不到就
 * 上报 known = false(不假装有/没有 SMT)。
 */
export const smtCapabilities: () => string;
/**
 * 写入开关, 返回写入后的状态。默认开: 不调用本函数时行为与历史完全一致(全部逻辑核)。
 * 关掉后: 多核阶段线程数 = 物理核数, 大核簇掩码与池线程落点也只覆盖每个物理核的一个逻辑核。
 */
export const smtSetEnabled: (on: boolean) => boolean;
/** 查询开关状态(供 ArkTS 显示)。 */
export const smtEnabled: () => boolean;
/**
 * 把"用户请求的线程数"过一遍 SMT 口径:
 *   开关开(默认) -> 原样返回 requested; 开关关 -> 返回物理核数; 拓扑未知 -> 原样返回。
 * 单核阶段(requested <= 1)永远原样返回 1。
 */
export const smtThreadsFor: (requested: number) => number;
/**
 * 超线程(SMT)开关的真实约束说明(中文一行, 不计分)。
 * 与 smtCapabilities().switchHint 是同一份文本(同一个函数, 不会各说各话)。
 * 里面同时带着 physical / effectiveSet.count / cap 三个数。
 */
export const smtSwitchHint: () => string;
/**
 * 「本机性能天花板」一行结论(中文一句, 不计分; 2026-10 新增)。
 *
 * 为什么有它: "每一个核都跑满、每一个负载都拉满"这件事里, 能做的与做不到的必须被一句话
 * 说清 —— 否则"我们没跑满"和"这颗芯片在第三方 App 里就只能跑到这儿"会被读成同一件事。
 * 这一行同时给出三个事实(全部来自本进程自己读到的 sysfs/procfs 与频率采样):
 *   ① 全机最快档在哪几个核、多少 MHz;
 *   ② 内核实测允许本进程用哪几个核(逐核 sched_setaffinity + 立刻读回的那份集合);
 *   ③ 生效快簇(①∩②)是哪几个核、多少 MHz -> 单核最高只能跑在这一档(第几档),
 *      多核最多几个线程, 以及最近一次采样到的运行时频率中位占标称多少。
 * 首页 / 结果页 / 报告三处显示的是同一句话(native 唯一来源)。读不到的部分写"读不到"。
 */
export const ceilingText: () => string;

// ---------------------------------------------------------------------------
// 芯片判读探测: 逐核原始频率读数 + /proc/cpuinfo 的 MIDR
//
// 与 smtCapabilities 的关系: 那一个是"用哪些核"的口径(开关 / 线程数 / 掩码);
// 这一个纯粹是取证与判读: 把每个核的 cpuinfo_max_freq 逐个读出来(读失败也带 errno),
// 再与"绑核用的那张频率表"对比, 回答"绑核路径有没有漏掉更高频的核"。
// 全部只读: 不改任何全局状态、不绑核、不计分。
// ---------------------------------------------------------------------------

/** 逐核频率探测里的一个核 */
export interface CpuFreqCoreSample {
  /** 核号 */
  cpu: number;
  /** sysfs 里读到的原始字符串(只去了首尾空白/换行); 读不到时为空串 */
  raw: string;
  /** 解析出的 kHz; 0 = 这个核没读到 */
  khz: number;
  /** 0 = 读到并解析成功; 13 = EACCES(权限不足); 2 = ENOENT(路径不存在);
   *  -1 = 打得开但内容为空; -2 = 读到内容但不是正的十进制数 */
  err: number;
}

/** 一个频率档 */
export interface CpuFreqTier {
  khz: number;
  count: number;
}

/**
 * cpuIdentityProbe() 返回的 JSON 解析结果。
 *  这些字段全部是设备自己说的话, 没有任何第三方资料参与。
 */
export interface CpuIdentityProbeResult {
  ok: boolean;
  /** CPU 拓扑(与 smtCapabilities 同一份数据) */
  topo: { logical: number; physical: number; smtPossible: boolean; known: boolean };
  /** 拓扑来源一行文本 */
  topoSource: string;
  freq: {
    /** 0 = 扫描范围取自 cpu/present; 1 = 取自 cpu/possible; 2 = 固定扫 0..31 */
    rangeSource: number;
    /** 范围取自 present/possible 时的 errno(0 = 读到了) */
    rangeErrno: number;
    /** 实际逐核扫了几个核号 */
    scanCount: number;
    /** 读成功(解析出正数)的核数 */
    okCount: number;
    /** 第一个读失败的核号(-1 = 全部成功) */
    firstFailCpu: number;
    /** 它的 errno */
    firstFailErr: number;
    /** 绑核用的频率表(readCoreMaxFreqKhz)实际返回的核数 */
    tableCores: number;
    /** 那张表的最高频率(kHz; 0 = 空表) */
    tableMaxKhz: number;
    /** 最高频率落在哪个位次(-1 = 空表) */
    tableMaxCpu: number;
    /** 逐核探测的最高频率(kHz) */
    probeMaxKhz: number;
    /** 它属于哪个核(-1 = 一个都没读到) */
    probeMaxCpu: number;
    /** true = 绑核表比逐核探测少核(被 readCoreMaxFreqKhz 的 break 截断) */
    truncated: boolean;
    /** true = 逐核探测的最高频 > 绑核表的最高频 ⇒ 绑核路径漏掉了更高频的核 */
    missedFaster: boolean;
    /** 按物理核归并(每个物理核取兄弟线程的最高频)后的物理核数 */
    physCount: number;
  };
  /** 扫描范围是怎么定下来的(一行) */
  range: string;
  /** 逐核原始读数一行(可直接进 runlog / 设备画像) */
  perCore: string;
  /** 漏核 / 截断的判定(一行中文) */
  verdict: string;
  /** 逐核明细 */
  cores: CpuFreqCoreSample[];
  /** 频率档(逻辑核口径, 降序) */
  tiers: CpuFreqTier[];
  /** 频率档(按物理核归并口径, 降序) */
  physTiers: CpuFreqTier[];
  cpuinfo: {
    /** false = /proc/cpuinfo 打不开(此时 errno 有效) */
    ok: boolean;
    /** 打开失败时的 errno(13 = EACCES, 2 = ENOENT) */
    errno: number;
    /** "processor" 行条数(= 逻辑核数) */
    processorLines: number;
    /** 不同的 MIDR 组合数 */
    comboCount: number;
    /** "Hardware" 行(没有则空串) */
    hardware: string;
    /** "model name" 行(没有则空串) */
    modelName: string;
    /** 每个 part 号的十进制换算, 形如 "0xd06=3334" */
    partDecimal: string;
    /** 一行可读文本 */
    text: string;
    /** 每个 MIDR 组合 */
    midr: {
      implementer: string; architecture: string; variant: string;
      part: string; partDecimal: number; revision: string;
      /** 这种 MIDR 对应多少个 processor 块 */
      count: number;
    }[];
  };
}

/**
 * 逐核读 cpufreq/cpuinfo_max_freq + 读 /proc/cpuinfo 的 MIDR, 返回上面结构的 JSON 字符串。
 * 读失败的核也会被列出来(带它自己的 errno) —— 这是本接口存在的理由:
 * 只有把每个核的原始读数摆出来, 才能区分"漏读了最高频的核"与"设备真的只有这几个档"。
 */
export const cpuIdentityProbe: () => string;

// ---------------------------------------------------------------------------
// 内核允许的核集合(只读探测): "内核到底允不允许我们用最快的那几个核"
//
// 为什么需要它(2026-10-05 真机证据驱动):
//   sched_setaffinity() 是请求而不是命令 —— 内核只把它与"进程的可用核集合"
//   (cpuset cgroup 的 cpuset.cpus ∩ 进程已有的亲和掩码)求交集。厂商 ROM 常把第三方应用
//   放进受限 cpuset(把 Prime 核留给前台/系统线程), 此时:
//     * 绑到可用核集合之外的核可能返回成功(掩码被静默夹回), 于是"绑定成功却跑在核外";
//     * 用全机频率排名算出来的"大核簇"可能是空可用集, 等于根本没绑。
//   本接口把这三件事变成可核对的事实: 可用核集合是什么、快簇是怎么算的、两者的交集是什么。
//   全部只读: 不写文件、不改任何全局状态、不计分。
//   errno 口径: 0 = 读到了; 13 = EACCES 权限不足; 2 = ENOENT 路径不存在;
//               -1 = 打得开但内容为空; -2 = 读到了但没有要的那一行。
// ---------------------------------------------------------------------------

/** cpuAffinityProbe() 返回的解析结果(字段名与 native 严格对应) */
export interface CpuAffinityProbeResult {
  ok: boolean;
  allowed: {
    /** false = 可用核集合读不到(此时绑核退回全机频率排名, 不做任何过滤) */
    ok: boolean;
    /** 可用核集合里的核数(0 = 读不到) */
    count: number;
    /** 可用核集合位图, 形如 "0x3fff"(只覆盖前 64 核) */
    mask: string;
    /** true = 这份掩码来自 sched_getaffinity(本线程生效的掩码) */
    inThreadMask: boolean;
    /** true = /proc/self/status 的 Cpus_allowed 与 sched_getaffinity 给出的掩码不同(硬事实) */
    mismatched: boolean;
    /** /proc/self/status 的 Cpus_allowed(十六进制)是否可读且非零 */
    statusMaskOk: boolean;
    /** 打开 /proc/self/status 失败时的 errno */
    statusErrno: number;
    /** 是否找到 Cpus_allowed_list 行 */
    statusListFound: boolean;
    /** sched_getaffinity 是否成功 */
    syscallOk: boolean;
    /** 它的 errno */
    syscallErrno: number;
    /** 是否读到 Mems_allowed_list */
    memsFound: boolean;
    /** 读 cpuset.cpus 的 errno */
    cpusetCpusErrno: number;
    /** 读 cpuset.cpus.effective 的 errno */
    cpusetEffErrno: number;
  };
  /** 可用核集合的 cpulist 文本, 形如 "0-9"(空 = 读不到) */
  list: string;
  /** Mems_allowed_list 原文 */
  memsList: string;
  /** /proc/self/cgroup 原文(或读出它的 errno) */
  cgroupPath: string;
  /** 由 cgroup + mountinfo 推出的 cpuset 目录(空 = 推不出来) */
  cpusetDir: string;
  /** cpuset.cpus 原文 */
  cpusetCpus: string;
  /** cpuset.cpus.effective(或 v1 cpuset.effective_cpus)原文 */
  cpusetEffective: string;
  /** 可用核集合的来源一行(中文) */
  source: string;
  /** 可用核集合 + cpuset + Mems_allowed_list 的一行完整文本 */
  text: string;
  fast: {
    /** true = 全机最快档 ∩ 可用核集合 = 空, 已退化为"可用核集合里最快的核" */
    fallback: boolean;
    /** 生效快簇落在全机的第几个频率档(0 = 全机最快档) */
    tierIndex: number;
    /** 生效快簇的核数 */
    cores: number;
    /** 生效快簇的最高频率(kHz) */
    maxKhz: number;
    /** 生效快簇位图 */
    mask: string;
    /** 全机最快频率档的核数 */
    machineTopTierCores: number;
    /** 全机最快频率档的频率(kHz) */
    machineTopTierKhz: number;
    /** 全机最快档位图 */
    machineTopTierMask: string;
  };
  /** 快簇是怎么算出来的(中文一行) */
  fastSource: string;
  threads: {
    /** 多核阶段实际线程数上限(<= 0 = 不限制; 被可用核集合夹过时 > 0) */
    cap: number;
    /** 不看可用核集合时(全机口径)应有的线程数 */
    basisFull: number;
  };
  /** 面向「设备画像」的一行结论(可直接对比两台设备) */
  line: string;
}

/**
 * 读"内核允许本进程用哪些核"+ 生效快簇 + 全机最快档, 返回上面结构的 JSON 字符串。
 * 与 runGb7 的 cpuAllowed* 字段同一份数据(同一个 native 单例), 但这是设备级快照。
 */
export const cpuAffinityProbe: () => string;

// ---------------------------------------------------------------------------
// native 崩溃取证(crash_guard.cpp): 只记录, 不改任何负载行为。
// 用途: 进程跑分中途消失时, 区分「收到致命信号(我们的代码越界/UB)」与
//       「被系统 SIGKILL(内存压力/后台管制)」—— 后者不可捕获, 所以盘上没有
//       记录本身就是证据。
// ---------------------------------------------------------------------------
/**
 * 传 context.filesDir, native 侧把 <filesDir>/native_crash.txt 打开并常驻 fd。
 * 崩溃处理里只 write() 这个 fd, 不做 open/malloc。请尽早调用(EntryAbility.onCreate)。
 * @returns true = 打开成功(取证装置已就位)
 */
export const setLogDir: (dir: string) => boolean;
/** 读磁盘上的崩溃记录(空串 = 没有)。文件过大时返回尾部(最近一次崩溃)。 */
export const takeNativeCrashReport: () => string;
/** 清空崩溃记录(横幅展示完之后调用)。 */
export const clearNativeCrashReport: () => void;
/** 证据文件绝对路径(未调用 setLogDir 时为空串)。 */
export const nativeCrashLogPath: () => string;
/** 信号处理器是否已武装(用于自检显示)。 */
export const nativeCrashGuardReady: () => boolean;
/**
 * 手动标记「当前正在跑的项」。native 负载(runGb7/runCoreMark/runTest)
 * 已在 napi 内部自动标记; 这个导出给 ArkTS 标记 native 之外的阶段
 * (例如 CS1 GPU 阶段)用。零分配、零系统调用。
 */
export const setCurrentItem: (phase: string, item: string, index: number, total: number) => void;
/**
 * 手动把「当前项」标记置成空闲(= 上一项已结束, 没有负载在跑)。走 auroraSetIdleMarker(),
 * 看门狗据此不判定卡死。ArkTS 侧的 native 阶段(runGb7/runCoreMark/runTest/存储小节)
 * 已各自在 native 内部收尾; 这个导出是给 **libauroragpu7.so** 用的 —— GPU 阶段的每一项
 * 都在那个模块里跑完, 它通过 napi_load_module("aurorabench") 拿到本模块再调这一句。
 * 不写空闲标记的后果: 看门狗会把已结束的最后一项当成长时间卡死, 每秒写一条现场。
 */
export const setIdleMarker: () => void;
/**
 * 采样一次内存足迹(VmRSS/VmHWM/VmSize/Threads)。建议每项开跑前调一次,
 * 并把 lastMemorySample() 写进自己的面包屑 —— 进程被 SIGKILL 时不会有崩溃记录,
 * 这一行就是唯一的“临死内存足迹”(用于判断是不是内存压力被杀)。
 */
export const sampleMemory: () => void;
/** 最近一次采样结果, 例: "rss=1234kB hwm=5678kB size=90123kB threads=9 sampled=...Z"; 空串 = 未采样。 */
export const lastMemorySample: () => string;
/**
 * 真机自检钩子(排障期用, 发布版可删):
 * 0 = 只往证据文件写一条链路自检记录(不崩, 用来验证 setLogDir / takeNativeCrashReport 通了);
 * 1 = 故意空指针写触发 SIGSEGV(进程会退出, 走完整崩溃取证路径);
 * 2 = raise(SIGABRT)。
 */
export const crashSelfTest: (mode: number) => void;

// ---------------------------------------------------------------------------
// 卡死取证: 挂起看门狗 + 栈采样(和崩溃取证共用当前项标记与输出目录)
// 看门狗线程每 5 秒纯内存检查一次「当前项 + 已运行时长」, 某项超过 30 秒判定疑似卡死,
// 就向跑该项的线程 tgkill(SIGUSR1) 抓 64 层栈, 采 3 次(间隔 1 秒), 不杀进程。
// 证据文件: <filesDir>/native_hang.txt
// ---------------------------------------------------------------------------
/** 读卡死记录(空串 = 还没有卡死记录)。文件过大时返回尾部(最近一次)。 */
export const takeNativeHangReport: () => string;
/** 清空卡死记录(横幅展示完之后调用)。 */
export const clearNativeHangReport: () => void;
/** 卡死证据文件绝对路径。 */
export const nativeHangLogPath: () => string;
/**
 * 一行状态, 例:
 * "watchdog=on threshold_s=30 samples_done=0 unwinder=RTLD_DEFAULT marker_tid=12348 running_s=3 item=\"..\" phase=\"..\" hang_log=/data/.../native_hang.txt"
 */
export const hangStatus: () => string;
/**
 * 自检: 起一个空转线程(默认 45 秒, 范围 1~600)并把它标成当前项,
 * 30 秒后看门狗应当自动抓它的栈。返回 true = 已启动。
 */
export const hangSelfTest: (seconds?: number) => boolean;
// ---------------------------------------------------------------------------
// 存储 I/O 小节(独立小节, 不进 CS1 分数)
//
// 口径要点(实现细节见 storage_bench.cpp 文件头):
//   * 顺序写两个口径: 不 fsync(上界) / write+fsync(更接近真实落盘), 分别报出;
//   * 顺序读两个口径: 页缓存命中(被放大, 仅对照) / 绕过页缓存;
//   * 绕过页缓存的优先级: O_DIRECT > posix_fadvise(POSIX_FADV_DONTNEED) > 无(标 comparable=no);
//     二者在 OHOS musl sysroot 里逐个核实过头文件: O_DIRECT(bits/fcntl.h, 无门控)、
//     posix_fadvise + POSIX_FADV_DONTNEED(fcntl.h, 无门控)都可用; 但运行期是否真的生效
//     由开跑前的一次实测决定, 结果写在 env.oDirectWorks / env.fadviseDontNeedWorks;
//   * 4K 随机项用固定种子 xorshift64*, 偏移可复现, 在整个文件区间内均匀采样,
//     实际覆盖率按 item.distinct4kBlocks / config.seqBytes 解读;
//   * 只写 ArkTS 传入目录下的 storage_bench/ 子目录, 结束(含失败/异常)必删;
//   * 开跑前 statvfs 查空间, 不足直接跳过, 一个文件都不写。
//
// storageRun() 返回的 JSON 结构(可直接 JSON.parse):
// {
//   ok: boolean, section: "storage", jsonVersion: 1,
//   skipped: boolean, skipReason?: string,          // 空间不足/未设目录时 true
//   dir: string, baseDir: string,
//   totalMs: number, budgetMs: number, stoppedEarly: boolean, stopReason?: string,
//   clampedOptions?: string, fatal?: string,
//   env: { statvfsOk, statvfsErrno, blockSize, totalBytes, freeBytes, availBytes,
//          memTotalBytes, fileFitsInPageCache,
//          oDirectMacro: string, oDirectWorks: boolean, oDirectErrno: number,
//          fadviseMacro: string, fadviseRet: number, fadviseErrno: number,
//          fadviseDontNeedWorks: boolean, bypassMethod: string },
//   config: { seqMiB, seqBytes, randOps, blockBytes, smallFiles, smallFileBytes,
//             seed, prng: string, threads: 1, queueDepth: 1, mbpsUnit: string,
//             needBytes, marginMiB? },
//   headline: { seqWriteDurableMbps, seqReadColdMbps, randRead4kIops,
//               randWrite4kIops, filesPerSecCreate, filesPerSecDelete },   // -1 = 该项没跑
//   cleanup: { ok: boolean, removedFiles, removedDirs, errors, leftoverEntries },
//   items: Array<{ id: "seqWrite"|"seqWriteDurable"|"seqReadWarm"|"seqReadCold"|
//                       "randRead4k"|"randWrite4kBuffered"|"randWrite4k"|
//                       "fileCreate"|"fileDelete",
//                  name: string, ok: boolean, error?: string,
//                  ms: number, bytes?: number, mbps?: number, iops?: number, ops?: number,
//                  auxMs?: number, auxName?: string,       // fsyncMs / dropCacheMs / dirFsyncMs
//                  distinct4kBlocks?: number, files?: number,
//                  mode: string, comparable: "yes"|"no"|"partial", note: string }>,
//   warnings: string[], notes: string[]
// }
// 注意: mbps 的单位是 **MiB/s**(1 MiB = 1048576 B); 换算成 MB/s(10^6) 请 ×1.048576。
// ---------------------------------------------------------------------------

/**
 * 设置沙箱根目录(ArkTS 传 context.filesDir 或 context.cacheDir)。
 * native 只在 <dir>/storage_bench/ 下放临时文件, 不写用户可见目录。
 * 调用时会顺手清掉上一次进程被杀/断电残留的临时文件。
 * @returns true = 目录可用
 */
export const storageSetDir: (dir: string) => boolean;

/**
 * 只读探测(同步, 不写任何文件): 可用空间 / 物理内存 / 编译期能力。
 * 用于 UI 在开跑前判断"能不能跑"。返回一行 JSON:
 * {ok, section, dirSet, baseDir, statvfsOk, statvfsErrno, availBytes, totalBytes,
 *  memTotalBytes, needBytes, wouldRun, oDirectMacro, fadviseMacro, note}
 */
export const storageInfo: () => string;

/**
 * 跑完整存储 I/O 小节(异步, 不阻塞 UI 线程), 返回上面的结果 JSON 字符串。
 * optionsJson 是扁平、值全为数字的 JSON, 全部可选:
 *   {"seqMiB":256,"randOps":4096,"smallFiles":512,"smallFileBytes":4096,
 *    "seed":20260101,"budgetMs":10000,"marginMiB":128}
 * 传空串/不传即全默认。实际生效值原样回写在结果的 config 里。
 * 单项跑到一半不会被打断; budgetMs 只在子项之间生效(超了就跳过剩下的并写 stopReason)。
 */
export const storageRun: (optionsJson?: string) => Promise<string>;

/** 删除残留临时文件/目录(兜底用)。返回 true = 已清干净(残留条目为 0)。 */
export const storageCleanup: () => boolean;

// ---------------------------------------------------------------------------
// NPU(AI 加速器)小节 —— 独立小节, 不进 CS1 分数
//
// GB7 的 CPU/GPU 项里没有任何 NPU 对应项, 把这一节的结果混进 CS1 复合分会立刻毁掉与
// CS1 分数自身的可比性。因此 native 侧返回的 JSON 里带 scored:false / gb7Item:false
// 两个标记, 请单独显示。
//
// 两个接口都是异步 Promise<string>, 内容是 JSON 字符串(完整字段见 npu_bench.h 注释)。
// 无论成功失败都会 resolve 出 JSON —— 失败时 ok:false, error / unit 里带 **OH_NN_ReturnCode
// 的原始枚举名与数值**(例如 "NPU 探测失败: OH_NN_UNAVAILABLE_DEVICE(14)"),
// metric 固定为 "0" 且 unit 写着失败原因, 不会静默跳过。
//
// NNRt 是运行期 dlopen/dlsym 绑定的(native 侧自己 dlopen, 没有链接期依赖)——
// 这样"设备上有没有这两个库"只影响本小节, 不会让整个 requireNapi("aurorabench") 加载失败。
// 因此多了一个状态要区分:
//   status === "NNRT_UNAVAILABLE" -> 本 App 拿不到 NNRt 接口(库不在/缺符号),
//                                    原因是 nnrt.error, 缺失符号在 nnrt.missingSymbols;
//   status === "NOT_OPEN"         -> 接口拿到了, 但这台机器没向第三方应用开放 NPU。
// 两者都不是"NPU 跑分为 0", 不要混为一谈。
// ---------------------------------------------------------------------------

/**
 * 第一步: 设备探测(必须先看这个结论)。
 * 用 OH_NNDevice_GetAllDevicesID 枚举设备, 逐个 OH_NNDevice_GetName / OH_NNDevice_GetType。
 * 返回 JSON:
 * {
 *   ok: boolean, kind: "npu-probe",
 *   status: "OPEN"|"NOT_OPEN"|"NO_DEVICE"|"ENUM_FAILED"|"NNRT_UNAVAILABLE",
 *   nnrt: {                       // NNRt 运行期绑定状态(两个版本都有这个对象)
 *     available: boolean, binding: string,
 *     runtimeLibrary: string, runtimeOpened: boolean, runtimeDlerror: string,
 *     coreLibrary: string, coreOpened: boolean, coreDlerror: string,
 *     expectedFunctionCount: number, boundFunctionCount: number,
 *     missingSymbols: string, error: string },
 *   thirdPartyNpuOpen: boolean,   // 这台机器是否向第三方应用开放了 NPU
 *   text: string,                 // 一行中文结论, 可直接显示
 *   error: string,                // status=ENUM_FAILED 时形如 "NPU 探测失败: OH_NN_XXX(n)"
 *   enumRc: number, enumRcName: string, deviceCount: number, nnrtReportedCount: number,
 *   devices: [{ id, name, nameRc, nameRcName, type, typeName, typeEnum, typeRc, typeRcName }],
 *   hasCpu: boolean, hasGpu: boolean, hasAccelerator: boolean,
 *   enumNote: string, notes: string
 * }
 * 若 devices 里一个 OH_NN_ACCELERATOR 都没有, text 会明确写"未向第三方开放 NPU",
 * 不会硬凑成 NPU 分数。
 */
export const npuProbe: () => Promise<string>;

/**
 * 第二步: 推理跑分(纯代码构造 MATMUL 链 + RELU 模型, 不需要任何模型文件)。
 * 编译与执行按 OH_NN_ACCELERATOR -> OH_NN_GPU -> OH_NN_CPU 依次回退, **实际用的是哪个
 * 设备写在 device.typeName / device.typeEnum / device.fallbackUsed / device.fallbackText 里**;
 * 每次尝试的完整返回码串在 device.attempts[].rcTrail 里。
 * 算子组也会回退: 先用 MATMUL+RELU; 该组在所有设备上都没成功, 才换 FULL_CONNECTION+RELU
 * 把整条阶梯再走一遍(两组乘加次数口径相同: MACs = layers * dim^3)。实际用的哪一组写在
 * model.opSet / model.opSetName / model.opSetText 里; model.opSetVerifiedOnHost 恒为 false ——
 * FULL_CONNECTION 的轴/布局语义本机无法验证, 那一组的数字请连同 opSet 一起解读。
 * 返回 JSON 关键字段:
 * {
 *   ok: boolean, kind: "npu-bench", section: "NPU", scored: false, gb7Item: false,
 *   text: string,                                   // 一行中文总结(失败时是失败原因)
 *   metric: string, unit: string,                   // 成功 "GOPs/s"; 失败 unit="运行失败: ..."
 *   device: { chosenDeviceId, typeName, typeEnum, deviceLabel, name, preferredTypeName,
 *             preferredOrder, fallbackUsed, fallbackText, attemptCount, attempts: [...] },
 *   precision: { available, modelDataType, float16Requested, float16Rc, float16RcFallback,
 *                float16Enabled, execInputDataType, execInputFormat, execOutputDataType,
 *                short, known, text },
 *   model: { dim, layers, scale, scaleLabel,
 *            opSet, opSetName, mainOp, opSetFallbackUsed, opSetVerifiedOnHost, opSetText, opSets,
 *            ops, matmulOps, reluOps,
 *            macsPerInference, flopsPerInference, macFormula, weightBytes, inputBytes },
 *   timing: { available, buildMs, firstRunMs, firstInferenceMs, firstInferenceNote,
 *             warmupRuns, warmupShortened, measuredRuns, itersPlanned, budgetMs, budgetHit,
 *             sampleFromFirstRun, meanMs, minMs, medianMs, maxMs, medianNote,
 *             goPs, macsPerSec, goPsDefinition, nonFinite, hint },
 *   deviceCount: number, devices: [...], scalePresets: [...], notes: string,
 *   nnrt: { 同 npuProbe 的那个对象 },
 *   totalRunMs: number
 * }
 * 口径: MACs = layers * dim^3(一次 MATMUL = dim^2 个输出元素 x 每个 dim 次乘加;
 * 一次 FULL_CONNECTION = dim 个输入分量 x dim 个输出通道 —— 两组同为 dim^3),
 * FLOPs = 2 * MACs(一次乘加记 2 次浮点运算), GOPs/s = FLOPs / 稳态平均单次延迟(秒) / 1e9。
 * 首次推理延迟(firstInferenceMs = buildMs + firstRunMs, 含编译/加载)与稳态延迟分开报。
 * @param scale 模型规模档位 0(小)/1(中, 默认, 约 100.7M 乘加/次)/2(大)/3(特大)
 * @param warmup 预热次数(默认 2, 0~50)
 * @param iters 计时次数上限(默认 10, 1~500), 实际次数还受 budgetMs 约束
 * @param budgetMs 整个跑分的时间预算毫秒(默认 1200, 200~5000), 保证单次运行 1~3 秒量级
 */
export const npuBench: (scale?: number, warmup?: number, iters?: number, budgetMs?: number) => Promise<string>;

