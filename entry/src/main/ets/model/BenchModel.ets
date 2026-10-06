export interface TestResult {
  name: string;
  detail: string;
  ms: number;
  score: number;
  metric: number;
  unit: string;
  parallelism: number;
  // ---- 落核 / 该核标称上限 / 运行时频率的取证(2026-10 补; 旁路, 不计分) ----
  //  自研套件每一项此前在报告里都只有 cpu=-1 · maxKhz=0 —— 于是"这一项跑在哪颗核上"
  //  这个最基本的问题答不了, 而"两台同芯片设备自研单核差 37.7%、CS1 单核只差 0.24%"
  //  的根因正藏在这里。现在照 CS1 那套补齐: 落核 / 该核标称上限 / 频率降序位次 /
  //  生效快簇判据 / 全机最快档判据 / 可用核集合核数 / 运行时频率一行。
  //  全部是旁路诊断: 不参与 metric / unit / 计分公式, 也不进任何复合分。
  //  默认值一律是"没上报"(-1 / 0 / 空串), 老 native 与失败项都不会出现 undefined。
  cpu: number;                      // 跑完时所在的核(-1 = 未知)
  cpuAtStart: number;               // 开跑时的核(-1 = 未知)
  cpuMaxKhz: number;                // 该核的 cpuinfo_max_freq(kHz; 0 = 未知)
  cpuRank: number;                  // 该核在全机频率降序表里的位次(-1 = 未知)
  cpuBound: boolean;                // 单核阶段是否已绑到生效快簇
  cpuInFastCluster: boolean;        // 跑完时所在核是否属于生效快簇
  cpuInMachineTopTier: boolean;     // 跑完时所在核是否属于全机最快频率档
  cpuFastClusterCores: number;      // 生效快簇核数(0 = 频率表读不到)
  cpuFastClusterMaxKhz: number;     // 生效快簇最高频率(kHz; 0 = 未知)
  cpuMachineTopTierCores: number;   // 全机最快档核数
  cpuMachineTopTierKhz: number;     // 全机最快档频率(kHz)
  cpuAllowedCount: number;          // 内核实测允许的核数(0 = 读不到)
  cpuSingleTargetCores: number;     // 单核阶段绑核目标核数(= 生效快簇核数)
  cpuInfo: string;                  // native 给的一行中文说明(可直接显示; '' = 未上报)
  runFreq: string;                  // 运行时频率一行(与 CS1 路径同源; '' = 未采到样本)
}

export interface GpuSceneResult {
  scene: string;
  fps: number;
  frameMs: number;
  metric: number;
  unit: string;
  score: number;
}

// ===========================================================================
//  显示名 = CS1; 内部键仍用 gb7 前缀
// ---------------------------------------------------------------------------
//  套件显示名改过两次: 8.4 把 GB7 改成 GB8, 本次开源改名再把 GB8 改成 CS1。
//  代码里的内部标识符与 JSON 键名一律保留 gb7 前缀
//  (gb7RunTest / gb7SingleComposite / gb7Items / 'gb7s' 等等)。
//  为什么不改: 报告对比工具、历史记录与 runhistory 都按这些键名读,
//  改名会直接断掉跨版本对比。显示名与内部键指的是同一套东西。
//  另: 两次改名都只动显示名, 不改任何负载 / 单位 / k / conv / 计分公式, 所以
//  benchVersion 不递增, 旧 GB7 / GB8 分数与新 CS1 分数在数值上完全可比。
// ===========================================================================
// (上面这段同样适用于本接口: 类型叫 Gb7Result, 显示名是 CS1)
export interface Gb7Result {
  name: string;
  score: number;
  metric: number;
  unit: string;
  ms: number;
  // ---- "跑在哪个核 / 哪个簇"(native runGb7 上报的旁路诊断信息, 不计分与顺序) ----
  // 全部给了默认值, 老 native / GPU 路径 / 失败项都不会出现 undefined。
  cpu: number;                      // 负载线程实际所在的 CPU 编号, -1 = 未知
  cpuMaxKhz: number;                // 该核最高频率(kHz), 0 = 未知
  cpuRank: number;                  // 频率降序位次(0 = 最快), -1 = 未知(只是附加信息, 不再是判据)
  cpuBound: boolean;                // 是否成功应用了"大核簇掩码"(false = 读不到 cpufreq, 已降级)
  cpuAtStart: number;               // 绑核后立刻回读的 CPU 编号(-1 = 未知; 仅供参考, 不用它报警)
  // ---- 大核簇(2026-10 起的新判据): "绑核是否生效"只看 cpuInFastCluster ----
  cpuInFastCluster: boolean;        // 跑完时所在核是否属于大核簇(true 才算绑核生效)
  cpuAtStartInFastCluster: boolean; // 开跑时的采样是否也在簇内(仅参考, 不用它报警)
  cpuFastClusterCores: number;      // 大核簇核数(0 = 读不到频率表, 簇未定义)
  cpuFastClusterMaxKhz: number;     // 大核簇最高频率(kHz), 0 = 未知
  cpuFastClusterMask: string;       // 大核簇位图, 形如 "0x1f"(排障用; '' = 未上报)
  cpuWorkers: number;               // 成功绑定自己的池线程数(0 = 无/未知)
  cpuWorkersOverflow: number;       // 其中因"分配给我的位次已被占用"而改到其它位次的个数
  cpuInfo: string;                  // native 给的一行中文说明, 可直接显示
  // ---- 内核允许的核集合(2026-10-05): "为什么这台机器的分数像被锁住了"的第一手事实 ----
  // 内核(cpuset cgroup / 厂商策略)可能只允许本进程用一部分核, 且不含频率最高的那些核 ——
  // 此时 sched_setaffinity 到最快核会被静默夹回(甚至返回成功), 全机频率排名算出来的快簇等于空集。
  cpuAllowedOk: boolean;            // 可用核集合是否读到(native 侧 ok)
  cpuAllowedCount: number;          // 可用核集合里的核数(0 = 读不到)
  cpuAllowedMask: string;           // 可用核集合位图, 形如 "0x3fff"(只覆盖前 64 核)
  cpuAppliedMask: string;           // 主线程真正应用的那张掩码(排障用)
  cpuFastClusterFallback: boolean;  // true = 全机最快档 ∩ 可用核集合 = 空, 已退化为"可用核集合里最快的核"
  cpuFastClusterTierIndex: number;  // 生效快簇落在全机的第几个频率档(0 = 全机最快档)
  // ---- 两个判据分开报(不混成一个) ----
  cpuInFastClusterJudged: boolean;  // true = 生效快簇已定义, cpuInFastCluster 才有意义
  cpuInMachineTopTier: boolean;     // 跑完时在不在"全机最快频率档"里(我们到底够不够快)
  cpuMachineTopTierCores: number;   // 全机最快频率档有几个核
  cpuMachineTopTierKhz: number;     // 全机最快频率档的频率(kHz)
  cpuMachineTopTierMask: string;    // 全机最快档位图(排障用)
  cpuFastClusterSource: string;     // 快簇是怎么算出来的(中文一行, 可直接显示)
  cpuAllowedText: string;           // 允许核集合的一行完整文本(含 cpuset / Mems_allowed_list)
  diag: string;                     // 旁路自证字段(k=v 一行, 如 Video Decoder 的 passes/decodedFrames/selfCheck)
                                    // —— 只用于判读"这一项到底跑满没有", 不参与 metric/unit/计分
  cpuBoundReason: string;           // cpuBound = false 时的原因(目标不在可用核集合内 / 读不到频率表 …)
  runFreq: string;                  // 运行时实际频率(native runFreq 旁路字段): 负载真正跑的时候采样到的中位/最小/最大
                                    //   MHz + 采样次数 + 采样核 + governor, 形如
                                    //   "运行时频率 中位 1220MHz / 最小 1150MHz / 最大 2270MHz(本核标称上限 2270MHz) 采样 12 次; …"
                                    //   同样不参与 metric / unit / k / conv / 计分, 只用于报告判读。'' = 未上报。
  // ---- QoS 运行条件(native "qos" 旁路字段, 2026-10 补齐) ----
  //   回答"本次跑分到底有没有启用 QoS、启在哪一档、每个调用返回了什么":
  //   是否启用 / 负载与旁路各自用了哪一档 / 每个调用的返回值与 errno / libqos.so 与 canIUse
  //   的探测结果 / "设-不设"对照的份额比。与 runFreq/diag 完全同类: 不计分。
  //   以前这里没有这个字段 -> native 算出来、napi 也输出, 但报告 JSON 的 items[] 里看不到,
  //   对比工具与历史记录因此无法回答"这台机器的限制在哪一层 / QoS 生效没有"。'' = 未上报。
  qos: string;
  // ---- 只读分层诊断(cgroup / cpuset 分组 / core_ctl)(native "cpuset" 旁路字段, 2026-10 补齐) ----
  //   逐项 errno 的完整文本: /proc/self/cgroup · /dev/cpuset 逐组 cpus 与 cpus.effective ·
  //   cpu/online 与 cpu/possible 原文 · cpuN/core_ctl 六个文件逐核(enable/min_cpus/max_cpus/
  //   active_cpus/need_cpus/…), 外加"限制到底在哪一层"的结论。全部只读, 不计分。
  cpuset: string;
  // ---- 超线程(SMT)诊断(同样只是旁路信息, 不计分) ----
  cpuLogical: number;               // 逻辑核数(0 = 未知)
  cpuPhysical: number;              // 物理核数(拓扑未知时 = 逻辑核数, 按 1:1 处理)
  cpuSmtPossible: boolean;          // 物理核数 < 逻辑核数(检测到 SMT)
  cpuSmtEnabled: boolean;           // 跑本项时开关的状态(true = 开)
  cpuTopoKnown: boolean;            // 拓扑是否已知(false = 未知, 已按 1:1 处理, 开关无效)
  cpuThreadsRequested: number;      // 调用方请求的线程数
  cpuThreadsEffective: number;      // 实际使用集合大小(= 该阶段应当使用的线程数)
  cpuEffMask: string;               // 实际使用集合的位图, 形如 0x400f
  cpuTopoSource: string;            // 拓扑来源(哪个文件 / 未知, 已按 1:1 处理)
  // ---- 可重复性 / 离散度(2026-08-31 起 native 默认跑 2 轮) ----
  // 全部来自 native runGb7 返回的 repeatability 块, 是旁路字段: 不参与 metric / unit /
  // k / conv / 计分公式, 也不改变任何负载的算法与尺寸。它只回答一件事:
  // "这同一项测量做了 N 次, 代表值(中位)稳不稳"。
  // 默认值一律是"没上报"(0 / 空串) —— 老 native / GPU 路径 / 失败项都不会出现 undefined,
  // 界面据此区分"没上报"与"上报了稳定", 不把'没数据'显示成'稳定'。
  rounds: number;                   // 本项实际跑了几轮(repeatability.rounds; 0 = 未上报/运行失败)
  roundsOk: number;                 // 其中有有效吞吐读数的轮数(0 = 全部轮次都没读数)
  scoreMedian: number;              // 代表值 = 中位分(不是最好一轮, 也不是平均; 0 = 无读数)
  scoreSpreadPct: number;           // 相对离散度 = (最大 - 最小) / 中位 x 100%
  repeatVerdict: string;            // RELIABLE / FAIR / UNRELIABLE / SINGLE_ROUND_NO_DATA /
                                    // NO_VALID_ROUNDS / RUN_FAILED / ''(未上报)
  repeatText: string;               // native 给的一句话人话结论(含"阈值是我们自己定的")
  // ---- 「未计分」与「运行失败」必须分得开(2026-10-07) ----
  //   true  = 该项在 CS1 注册表里是计分项(scored=true, kSingle/kMulti > 0):
  //           这时 score = 0 只可能是"这一轮没跑出来"(例如 Clang), 必须按失败显示;
  //   false = 按政策不计分(注册表 scored=false, 例如 Structure from Motion, k = 0 ——
  //           口径不可复核, 故意剔除出复合分): 得 0 是设计如此, 保持中性标签。
  //   以前两者都被写成同一个"未计分", 于是"设计如此"和"跑挂了"在界面上长得一模一样。
  //   来源: native gb7TestIsScored(id)(= 注册表的 scored); GPU / 自研 / 老 native 路径
  //   一律默认 false(中性), 不会被误标成失败。
  scoredByPolicy: boolean;
  // ---- 负载 id + 跨芯片可比性自检(2026-10-08) ----
  // loadId = 该项在 native CS1 注册表(ENTRIES[])里的下标。
  //   为什么必须带上它: 复合分要由"本报告里那一项的那个分"算出来, 而 native 的
  //   gb7CompositeSingle(scores) 是按负载 id 索引取值的(见 gb7.cpp 的 scoreOf)。
  //   以前 ArkTS 不传数组, native 就去读它自己那份 g_lastScore 缓存 —— 那份缓存**没有
  //   阶段维度**, 多核阶段只跑 8 项却把这 8 项的缓存覆盖成了多核分, 于是"单核复合分"
  //   被污染: 真机报告 328.2(卡片口径) vs native 561.1(被污染的口径), 差 1.7 倍。
  //   有了 loadId 才能把"本阶段的各项分"原样传下去, 两个数由构造保证逐位一致。
  //   -1 = 未上报(GPU / 自研套件 / 老 native / 失败项)。
  loadId: number;
  // 本项"是否真的随芯片性能变化"的自检结论(见 service/ScalingAudit.ets)。
  //   PASS     = 有正向证据(吞吐真的受算力支配);
  //   FAIL     = 有反向证据 -> 不可用于跨芯片比较(仅标注; 复合分的参与项集合不受它影响, 2026-10-05);
  //   UNPROVEN = 本工程现有数据判不出来(写"未自证", 不假装通过);
  //   ''       = 本路径没有做这项自检(GPU / 自研套件 / 老 native)。
  scalingVerdict: string;
  scalingEvidence: string;          // 判据原文(数字 + 依据), 可直接显示
  // ---- GPU「跑满判据」的一行结论(native gpu7Fullness(id) 的 text 字段, 2026-10 追加) ----
  //  形如 "GPU 跑满判据 -> 已达到(...)" / "... -> 未达到(还差 x.x 个百分点: ...)" /
  //  "... -> 本项不适用(原因: ...)"。判据在**计时区间之外**用同一份负载跑两档得到,
  //  两档读数一律丢弃, 不改任何 GPU 负载的尺寸 / 算法 / 计分。旁路字段, 不计分。
  //  '' = 未上报(老 native / 非 CS1 GPU 路径 / 失败项) —— 界面必须显示"未上报", 不显示成"跑满了"。
  gpuFullness: string;
}

export interface BenchRecord {
  time: number;
  total: number;
  single: number;
  multi: number;
  gpu: number;
  intSingle: number;
  floatSingle: number;
  intMulti: number;
  floatMulti: number;
  device: string;
  tests: TestResult[];
  multiTests: TestResult[];
  gpuScenes: GpuSceneResult[];
}
