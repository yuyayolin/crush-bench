import fs from '@ohos.file.fs';
import { common } from '@kit.AbilityKit';
import { deviceInfo } from '@kit.BasicServicesKit';
import { DeviceProfile, DeviceProfileData } from './DeviceProfile';
import { LogUpload, UploadResult, UPLOAD_RANGE_HISTORY_N,
  UPLOAD_RANGE_LAST_RUN_CRASH } from './LogUpload';

// ---------------------------------------------------------------------------
// 运行日志 runlog.jsonl —— 让每一次跑分都留下"可拉取"的结构化痕迹
//
// 设计要点:
//  1. 一行一条 JSON(JSONL), 追加写, 便于用 hdc 拉下来后逐行解析;
//  2. 每完成"一项"负载写一行(kind='item'); 每跑完一"轮"再写一行汇总(kind='run_summary');
//     启动时若发现上次异常结束, 写一行 kind='crash';
//     上传开启时, 每次上传结果写一行 kind='upload'。
//  3. 单文件上限 512 KB, 超了就轮转: runlog.jsonl -> runlog.1.jsonl -> ... -> runlog.3.jsonl,
//     最旧的直接删掉(最多保留 3 个历史文件)。
//  4. 全部写入都是 try/catch 包住的短 append: 日志失败只置一个内存标志, 不影响跑分。
//
// 本模块只做记录, 不参与任何测试项的计算 / 顺序 / 计分。
// ---------------------------------------------------------------------------

export const RUNLOG_FILE: string = 'runlog.jsonl';
// 单个文件上限, 超过就在下一次写入前轮转
export const RUNLOG_MAX_BYTES: number = 512 * 1024;
// 历史文件保留个数: runlog.1.jsonl ~ runlog.3.jsonl
export const RUNLOG_KEEP: number = 3;
// 界面上预览 / 复制时取日志末尾多少字符
export const RUNLOG_TAIL: number = 1500;

// ---- 永久账本 runhistory.jsonl -------------------------------------------
// 和上面的 runlog.jsonl 定位完全不同:
//   runlog.jsonl     = 诊断日志(逐项明细), 512 KB 就轮转, 只留 3 份 —— 供排障用;
//   runhistory.jsonl = 永久账本(每完成"一轮"才写一行, 字段精简), 5 MB 才轮转 —— 
//                     目的是"几年后还能查到我什么时候跑过多少分"。
// 写的都是同一批数据, 但账本一行也不参与任何测试项的计算 / 顺序 / 计分。
export const HISTORY_FILE: string = 'runhistory.jsonl';
export const HISTORY_MAX_BYTES: number = 5 * 1024 * 1024;
// 轮转后最多保留 runhistory.1 ~ runhistory.3(加上当前文件共 4 份)
export const HISTORY_KEEP: number = 3;


// 阶段名(与题面口径一致, 只用于日志归档, 不参与计算)
export const PH_GB7_SINGLE: string = 'CS1单核';
export const PH_GB7_MULTI: string = 'CS1多核';
export const PH_GB7_GPU: string = 'CS1GPU';
export const PH_OWN: string = '自研套件';
export const PH_STRESS: string = '压测';

//  App 版本的唯一来源(2026-10 修)
// 以前这里写死 '5.2', 而 Index.ets 的 APP_LABEL 写死 '极光跑分 7.5' —— 两个数各写各的,
// 结果 runlog.jsonl / runhistory.jsonl 里记的 App 版本是 **5.2**, 而同一台机器生成的报告抬头写 7.5。
// 版本号是这份工具最基本的身份信息(报告、对比、历史记录全靠它区分口径), 它对不上,
// 后面所有"同一版本才能比"的判断都失去依据。
// 规则: 只改这一行 + AppScope/app.json5, 其它地方一律引用 APP_VER; 每交付一版都要 +0.1 或进整数。
//  8.0 为什么是进整数而不是 +0.1 
// 7.5 -> 8.0 这一版改变了已记录的分数: GB7 的单项分是从 metric 字符串解析出来的,
// 7.5 用 %.1f 打印小于 10 的吞吐, HDR/Ray Tracer/Audio Encoder/Video Encoder 四项的分数被量化成
// 20%/50%/50%/7.7% 一格(真机上两台不同芯片给出逐位相同的分数), Game Physics/PDF/File
// Compression/Navigation/Asset Compression/Photo Editor/Video Decoder 七项的网格是 0.8%~11%。
// 8.0 把格式串统一成 %.4g(量化 < 0.1%), 同一台机器在 8.0 上的这十一项分数会与 7.5 不同。
// 分数会变 = 口径变了 = 7.5 与 8.0 的报告不能直接比, 所以按规则跳整数。
// 8.2: 复合分口径修正(参与项集合 == 本阶段有正分的项, 不再因自检失败置零) + GPU-SNL 的
// JSON 漏闭合根因修复 + 频率上限取证容量链扩容。计分公式/k/conv/单位/负载一个字没动。
// 8.4: 只改名字和措辞 —— 套件显示名 GB7 -> GB8(内部键仍用 gb7 前缀), 说明文字改成人话,
// 新增历史成绩单(落下载目录)与 App 内的「历史」页。分数数值一个都没变, 旧数据仍然可比。
// 8.5: 首页主按钮黑块修复(lightUpEffect 取值语义写反, 已删除) + 历史页改大字版 + 支持 2in1(鸿蒙电脑)。
// 9.0: 进整数 —— 三件事一起改, 都是用户看得见的:
//   (a) 多核负载线程数从「逻辑核 8」改成「物理核 6」, 多核分与总分因此变化(参与负载没动, 只是把线程
//       钉到真实物理核上, 不再有两个线程抢同一个物理核的 SMT 兄弟核);
//   (b) 改名: App 显示名与报告抬头 -> Crush分, 图标换成霓虹心; 分数公式 / k / conv / 单位 / 负载一个字没动;
//   (c) 打包 SDK 换成 HarmonyOS SDK(compileSdkVersion 26.0.0, runtimeOS HarmonyOS),
//       原生 .so 保留符号表(崩溃取证仍可用)。
// 9.1: 真机跑完 9.0 之后按实测回退两条**性能口径**(分数因此变化, 所以必须进版本号):
//   (a) 多核线程数: 物理核口径(6) -> 回退成可用逻辑核数(8)。
//       实测 8 线程 gb8Multi = 1481.6 / 1488.4 / 1540.2(parallelism 8.00);
//            6 线程 gb8Multi = 1069.8                 (parallelism 6.00) —— 慢 34%。
//       同一轮里 Clang(单线程项)逐位不变, 所以不是机器状态; 是那两条 SMT 兄弟线程本来就在干活。
//   (b) 自研套件单核: 不再绑到"快簇"(cpu4-7)。实测绑了之后单核分 2710 -> 1816(掉 33%),
//       同一轮 CS1 单核(本来就绑)418.1 -> 418.9 一位没变 —— 即瓶颈是"我们以为自己只能用
//       cpu0-7"这件事本身。详情与待办见交付说明。
//   其余(名字 / 图标 / HarmonyOS SDK 26 / 历史页)一个字没动。
// 9.2: 历史页三个大数字在真机上竖排成三行(用户截图) —— histBigRow 用的是会换行的
//   Flex(wrap) + flexBasis(l1Basis) + minWidth(l1MinW), 而历史页在 665x940 上是分栏的,
//   一张卡只有 ~310vp 宽, l1MinW 比 310/3 大 -> 三个数字各占一行, 卡片高度是估算的 3 倍。
//   改成等分三列的 Row + layoutWeight(1)(放不下时由 minFontSize/maxLines 缩字号, 不可能换行),
//   并给 layout_cases 的 Q 节补了一条"不许再改回会换行的容器"的断言。分数口径一个字没动。
// 9.3: 鸿蒙原生美化层(沉浸光感 + 粒子消散), 并且**跑分期间连节点都不创建**。
//   全部效果走唯一判据 fxOn() = !anyBenchRunning() && FX_CAPABLE; 没有用 hdsEffect/pointLight
//   (实测一个最小用法就 +20 条 ArkTS 告警, 而基线 53 条正好顶格), 改用 ArkUI 自带的
//   径向辉光 / 光晕阴影 / 渐变光带 —— 观感做到了, 告警一条没新增。
//   「开始跑分」按下时先散完粒子(约 646ms)再开跑, 保证装饰动效与负载不重叠一帧。
//   分数 / 负载 / 口径 / 线程数一个字没动。
// 9.4: 三件事一起进 ——
//   (a) 套件显示名 GB8 -> CS1(去掉与第三方跑分软件的关联, 为开源做准备)。**纯文本改名**:
//       内部 JSON 键仍是小写 gb8Single/gb8Multi/gb8Gpu, 手机上已有的历史成绩照常读;
//   (b) 鸿蒙原生沉浸光感(hdsEffect 点光源, 7 处) + 粒子消散, 全部走唯一判据 fxOn(),
//       跑分期间连点光源都不构造(唯一入口 fxLight() 第一句早退);
//   (c) compatibleSdkVersion 抬到 6.0.0(20) 以启用 HDS 点光。
//   ★ 计分口径一个字没动: k / conv / 单位 / 公式 / 负载规模 / 线程数 / 阈值全部逐字未改。
// 9.5: App 自有深色画布(不再跟随系统浅色主题) + 暗红->深紫的品牌渐变, 落在七个整屏容器上;
//   同时把深底上的光晕/点光源力度整体回收 30~40%(深底更容易过曝)。
//   分数 / 负载 / 口径 / 阈值 / fxOn() 判据一个字没动。
// 9.6: 按真机 9.5 的五条反馈修 —— (a) 历史卡 GPU-SNL/NPU/存储 三行永远是 '--'
//   (落盘那一跳 historyAdd() 漏搬三个键, 已补; 老记录仍是 '--', 不补假值);
//   (b) 设备卡那句「物理核口径」错文案回正(cpp 的 auroraSmtSwitchHint 与 ets 的 smtRunNote 两处);
//   (c) 背景渐变加强到"一眼看得出不是纯色底"; (d) **光"很糊、影响阅读"** —— 点光源从
//   BORDER_CONTENT 改成 BORDER(光只描外沿、不照内容), bloom 全部 <= 0.28, 光晕半径砍半,
//   标签项 Text 上的辉光删除; 并把"光不许压到任何一行字上"写成代码里的硬标准 + 4 条断言;
//   (e) 底栏那圈"白框" —— 删掉 tabBarItem 上的点光源, 选中态改成一条静态 2vp 主色底线。
//   分数 / 负载 / 口径 / 线程数 / 阈值一个字没动。
// 9.7: **修 CS1 多核比 8.5 低 17% 的真机缺陷**(分数因此变化, 所以必须进版本号)。
//   真机实测(同一台 HOP-AL00, 同一颗芯片, 都是干净重装):
//     套件      8.5 重装(OpenHarmony SDK)   9.6(HarmonyOS SDK)
//     CS1 单核  418.2                        416
//     CS1 多核  1492.8                       1239     <- 低 17%
//     CS1 GPU   4522.4                       5241
//     自研 单核  2799                         2663
//     自研 多核  6276                         6269     <- 一模一样(满血回来)
//     自研 GPU   1070                         1111
//   ★ 决定性的是自研那一组: 同一个包、同一套 SDK、同一颗芯片, 自研多核只差 0.1% ——
//     所以不是 SDK 的锅, 是 CS1 这条路径自己身上。
//   根因: 多核阶段的**线程数与落点表口径不一致**。2026-10-07/08 把多核落点表改成
//     "每物理核一个代表"的投影(spreadOrder, 真机 6 格); 9.1 按真机 A/B(8 线程 1481.6 /
//     1488.4 / 1540.2 vs 6 线程 1069.8, 慢 34%)把**线程数**回退成可用逻辑核数(8),
//     **但落点表没跟着回退** —— 于是 8 条线程往 6 个格子上钉: index 6/7 越界、拿到 -5 不绑,
//     又被夹回那 6 个核的掩码里与别人抢时间片, 实测并行度回到 ~6。
//     (自研套件没有"失败就夹回 6 核掩码"这一支, 钉不上就保留宽掩码、能落到空闲的 SMT 兄弟上,
//      所以同一版里它满血 —— 这也正是两个套件表现不同的原因。)
//   修法: 把这次没做完的回退做完 —— 落点表与线程数用**同一个口径**(可用逻辑核), 且只有一处定义
//     (cpu_affinity.h 的 multiCoreSlotList()); 自研套件同口径改用它。
//     k / conv / 单位 / 公式 / 负载规模 / 线程数 / 阈值一个字都没动: 变的只是线程钉在哪几颗核上。
//   断言: verify_core_spread.py 的 A8b 把这个不变式钉死(池线程数 == 落点表长度, 同源同长),
//     并在注释里带上 1239 vs 1492.8 这组真机证据。
// 9.8: 界面重做(用户:「关于里面把东西全部放进二级界面, 整体上要非常的简洁」+
//   「历史中的二级界面太难看了, 密密麻麻的」)。**纯界面改动, 分数一个数都不变。**
//   (a) 「关于」页一级只留 7 行入口(关于本应用 / 计分方法 / 跑分条件与温控 / 设备画像 /
//       运行日志 / 永久记录 / 隐私与数据), 一级不再有任何一段说明文字;
//       原来的「界面视效」整张卡连同它的 builder 一起删除(用户:"完全可以删掉");
//       二级页面的文字全部重写短了 —— 一级删掉的长文没有原样搬过去。
//   (b) 历史二级界面(点开一条记录)改成分段: 先给自研三个大数字 + 六行分数(与列表卡片、
//       结果页同一套排版零件), 再一句口径, 最后才是「展开完整报告」——
//       原始报告文本一字不少, 但**默认收起**, 不再一屏糊满几十行。
//   分数 / 负载 / 口径 / 线程数 / 阈值一个字没动。
// 9.9: 回退 9.7 里对**自研套件钉核表**的那次顺手改动(bench_cpu.cpp 的 coresByPerfDesc)。
//   真机实测: 自研多核 9.6 = 6269 -> 9.7 = 6082/6133/5238, 逐项 物理模拟 2.14x / AI 推理 2.60x /
//   文本处理 1.96x / 内存带宽 1.95x 慢; 而同一版自研单核八项全在 ±8% 以内(单核路径没动过)。
//   CS1 那一侧的正确修复保留不动 —— CS1 多核 9.7 = 1458.7/1503.3/1485.4, 已回到 8.5 的 1492.8 水平。
//   两个套件的线程落点策略本来就该不一样, 不要为了"看起来统一"再改回去。
// 10.0: **正式版**。用户:"我们把这个分数的主体突出部位变成 CS1, 然后把这个字眼的组件放到
//   现在 CS1 的位置, 就是用正常的字体和下面的 NPU 储存并列。"
//   (a) **位置对调(纯显示层)**: 结果页第一层 / 首页「最近一次跑分」预览 / 历史卡片上面那一行
//       的大数字, 从自研套件换成 **CS1 单核 / 多核 / GPU**; 下面那一行一项的清单里, 原来
//       前三行的 CS1 让给 **自研 单核 / 多核 / GPU**(正常字体、同一规格, 不放大)。
//       单次完整跑分只跑自研套件、没有 CS1 数据, 所以它的结果浮层仍然显示自研那三个数。
//   (b) **口径句随之改写**(这一条比换位置重要): 上面三个数现在只有单核能跨设备比;
//       下面的自研三项与 GPU-SNL / NPU / 存储只适合同机前后比, 后三者不计分。
//   ★ **分数口径一个字没动**: k / conv / 单位 / 公式 / 负载规模 / 线程数 / 阈值全部逐字未改;
//     账本 JSON 的小写键(gb8Single / ownSingle …)**一个都没改名**, 老历史记录照常读。
//     对调只发生在显示层: 换的是"哪个数摆在哪一行"。
export const APP_VER: string = '10.0';

// ---- 每一行日志的数据结构 ----

// kind = 'item': 一项负载跑完(或跳过)
export interface RunLogItemLine {
  ts: number;
  runId: string;
  kind: string;
  phase: string;
  item: string;
  index: number;
  total: number;
  ms: number;
  metric: number;
  unit: string;
  score: number;
  parallelism: number;
  ok: boolean;
  note: string;
}

// kind = 'run_summary': 一轮跑完的汇总
export interface RunLogSummaryLine {
  ts: number;
  runId: string;
  kind: string;
  phase: string;
  composite: number;
  scored: number;
  skipped: number;
  failed: number;
  durationMs: number;
  device: string;
  nativeVer: string;
  availMemMB: number;
  thermal: string;
  // ---- 多核阶段的线程口径(只有多核阶段有值; 其余阶段为空串 / 0) ----
  // 目的: 日后核对"这一轮的多核分是在 SMT 开还是关、几条线程下跑出来的"
  smt: string;          // '开' / '关' / 空串(= 该阶段与 SMT 无关或没上报)
  threads: number;      // 多核阶段使用的线程数(0 = 不适用)
}

// kind = 'crash': 上次进程异常结束(启动时补记)
export interface RunLogCrashLine {
  ts: number;
  runId: string;
  kind: string;
  phase: string;
  item: string;
  index: number;
  total: number;
  crumbTs: number;
  where: string;
  when: string;
  note: string;
}

// kind = 'upload': 一次联网上传的结果(失败也只是记一行, 不影响跑分)
export interface RunLogUploadLine {
  ts: number;
  runId: string;
  kind: string;
  ok: boolean;
  code: number;
  ms: number;
  target: string;
  note: string;
}

// summary() 的入参(由 UI 侧填, 避免 RunLog 反向依赖 BenchRunner)
export interface RunSummaryInput {
  phase: string;
  composite: number;
  scored: number;
  skipped: number;
  failed: number;
  durationMs: number;
  availMemMB: number;
  thermal: string;
  // 该轮相关的崩溃次数(稳定性压测用会话里的崩溃数; 其它阶段恒为 0 —— 
  // 其它阶段真崩了进程就没了, 也不会走到汇总)
  crashes: number;
  // 多核阶段的 SMT 口径(可选; 单核 / GPU / CoreMark 阶段不用填):
  //   smt = '开' / '关', threads = 该阶段实际使用的线程数。
  // 只进日志, 不参与任何计算 —— 目的是日后能核对"多核分是在 SMT 开还是关下跑的"。
  smt?: string;
  threads?: number;
  // 「一键全跑」那一行额外带上 CS1 三个复合分 / 自研套件三个分 / runId(见 RunHistoryExtra)。
  // 其它阶段不填 —— 没填的键不会出现在账本行里, 老读法完全不受影响。
  extra?: RunHistoryExtra;
}

// ---- 8.4 追加的可选字段(只加不改) ----------------------------------------
// 这一次只**新增**几个可选键, 已有字段的名字与含义一个字都没动 —— 对比工具、历史记录、
// 上传报文都按老键名读, 老行里没有这些键也照样读得下去(缺字段 = undefined, 不补假值)。
// 加它们的目的只有一个: 让「一键全跑」那一行也带上 CS1 的三个复合分与自研套件的三个分,
// 于是「历史成绩.txt」与 runhistory.jsonl 记的是同一批数, 不需要另算一套。
export interface RunHistoryExtra {
  runId?: string;        // 与 runlog 的 runId 同源(同一次跑分同一个 id)
  startedAt?: number;    // 本轮开始时间(报告文件名用的就是它; 用来把这一行对回那一份报告)
  interrupted?: boolean; // true = 中途停止 / 没跑完
  gb8Single?: number;    // CS1 三个复合分(口径与报告 composite.gb8*Composite 完全一致)
  gb8Multi?: number;
  gb8Gpu?: number;
  ownSingle?: number;    // 自研套件的三个分(与首页那三个大数字同源)
  ownMulti?: number;
  ownGpu?: number;
  // ---- 2026-10-06 追加的三个可选键(历史卡片下半部分那三行要用) -----------------
  //  存的都是**结果页关键得分清单里已经显示出来的那个数**, 同一个字段、同一个"没跑成就算没拿到"
  //  的判断 —— 卡片上那个数与结果页/报告上那个数因此不可能对不上。没跑成 / 老 native 一律写 0,
  //  卡片按既有规则写 '--', 绝不补假值。再老的行根本没有这三个键 -> undefined -> 一样是 '--'。
  snlScore?: number;        // GPU-SNL(顺序照结果页: 在 CS1 三行之后) · 显示用 num1
  npuGops?: number;         // NPU 推理 · GOPs/s · 显示用 num2
  storeWriteMbps?: number;  // 存储 I/O · 顺序写 · MiB/s · 显示用 num1
}

// ---- 账本行的**只读视图**(「历史」页用; 与账本是同一份数据, 不另建一份) --------------
// 老行(没有 8.4 那几个键)也解析得出来: 缺的字段给 0 / false, hasScores / hasGb8 / hasOwn = false。
// 2026-10-06: 历史卡片要同时显示 CS1 与自研**两组**三个分, 于是把"这一行记了哪一组"拆成两个
//   布尔(hasGb8 / hasOwn) —— 老行可能只记了其中一组, 卡片上缺的那一组写 --, 另一组照常显示。
//   hasScores 保留原义(只表示"记了 CS1 那一组"), 只加字段不改语义, 老读法一个字都不用动。
export interface RunHistoryRow {
  ts: number;
  startedAt: number;     // 0 = 这一行没记(老行) -> 回落到 ts
  device: string;
  phase: string;
  appVer: string;        // 这一行是哪个版本写的(界面上用它标出口径变更点)
  composite: number;
  runId: string;
  gb8Single: number;
  gb8Multi: number;
  gb8Gpu: number;
  ownSingle: number;
  ownMulti: number;
  ownGpu: number;
  // 下半部分清单的后三行(0 = 这一行没记 / 那一次没拿到 -> 卡片写 --)
  snlScore: number;
  npuGops: number;
  storeWriteMbps: number;
  failed: number;
  skipped: number;
  crashes: number;
  interrupted: boolean;
  hasScores: boolean;    // true = 这一行带 CS1 三个分(即「一键全跑」)
  hasGb8: boolean;       // true = 这一行记了 CS1 那一组(gb8Single / gb8Multi / gb8Gpu)
  hasOwn: boolean;       // true = 这一行记了自研那一组(ownSingle / ownMulti / ownGpu)
}

// ---- 永久账本: 一行 = 跑完的一轮 ----
export interface RunHistoryLine extends RunHistoryExtra {
  ts: number;
  kind: string;        // 恒为 'run'
  phase: string;
  composite: number;
  scored: number;
  failed: number;
  skipped: number;
  crashes: number;
  durationMs: number;
  device: string;
  appVer: string;
  nativeVer: string;
}

// 写入永久账本的入参
export interface RunHistoryInput extends RunHistoryExtra {
  phase: string;
  composite: number;
  scored: number;
  failed: number;
  skipped: number;
  crashes: number;
  durationMs: number;
}

// 一次读盘同时拿到"统计 + 最近若干条"(避免「关于」页刷新时把 5 MB 的账本读两遍)
export interface RunHistorySnapshot {
  stats: RunHistoryStats;
  recent: string[];
}

// 永久账本现状(给「关于」页显示)
export interface RunHistoryStats {
  ok: boolean;
  note: string;
  lines: number;
  sizeBytes: number;
  sizeText: string;
  files: number;         // 当前文件 + 轮转文件
  rotatedFiles: number;
  firstTs: number;
  lastTs: number;
  rangeText: string;
}

// 日志文件现状(给"关于"页显示)
export interface RunLogStats {
  ok: boolean;          // 写入是否一直正常
  note: string;         // ok = false 时的原因
  sizeBytes: number;
  sizeText: string;
  lines: number;
  files: number;        // 含历史文件在内的文件个数
  rotatedFiles: number; // 历史文件个数
  firstTs: number;
  lastTs: number;
  rangeText: string;
}

interface RunLogHead {
  ts: number;
  runId: string;
  kind: string;
}

// 上传报文(唯一的形态: 通用 JSON)
interface UploadPayload {
  schema: string;
  uploadedAt: number;
  device: string;
  appVersion: string;
  nativeVersion: string;
  range: string;
  fileBytes: number;
  fileLines: number;
  timeRange: string;
  lastRunId: string;
  lastRunCount: number;
  lastRun: string[];
  lastCrash: string;
  historyCount: number;
  history: string[];
  note: string;
}

function two(n: number): string {
  return n < 10 ? '0' + n.toString() : n.toString();
}

export function stamp(ts: number): string {
  const d = new Date(ts);
  return d.getFullYear().toString() + '-' + two(d.getMonth() + 1) + '-' + two(d.getDate()) + ' ' +
    two(d.getHours()) + ':' + two(d.getMinutes()) + ':' + two(d.getSeconds());
}

function fmtBytes(n: number): string {
  if (n < 1024) {
    return n.toString() + ' B';
  }
  if (n < 1024 * 1024) {
    return (n / 1024).toFixed(1) + ' KB';
  }
  return (n / (1024 * 1024)).toFixed(2) + ' MB';
}

export class RunLog {
  private static inst: RunLog | null = null;
  private ready: Promise<void> | null = null;
  private dir: string = '';
  private sizeBytes: number = 0;
  // 永久账本 runhistory.jsonl 的当前字节数(与 runlog 分开统计, 各自轮转)
  private historyBytes: number = 0;
  private runId: string = '';
  private writeOk: boolean = true;
  private note: string = '';
  private device: string = '';
  private nativeVerValue: string = '';
  private lastCrashJson: string = '';
  private lastCrashTs: number = 0;
  private lastCrashWhere: string = '';
  // 当前项"开跑前"由 native 取证装置采下的内存足迹(CrashGuard.mark 每项前灌进来)。
  // 逐项行的 note 会带上它 —— 这样即使进程随后被 SIGKILL, 盘上也有临死内存足迹可查。
  private memNote: string = '';

  static get(): RunLog {
    if (!RunLog.inst) {
      RunLog.inst = new RunLog();
    }
    return RunLog.inst;
  }

  // 与 BenchStore / CrashGuard 共用同一个 ability Context(filesDir 在沙箱内, hdc 可直接拉)
  init(ctx: common.Context): Promise<void> {
    if (!this.ready) {
      this.ready = this.setup(ctx);
    }
    return this.ready;
  }

  private async setup(ctx: common.Context): Promise<void> {
    try {
      this.dir = ctx.filesDir;
      this.device = this.fallbackDevice();
      try {
        this.sizeBytes = fs.statSync(this.mainPath()).size;
      } catch (e) {
        // 文件还不存在: 首次写入时由 APPEND|CREATE 建出来
        this.sizeBytes = 0;
      }
      try {
        this.historyBytes = fs.statSync(this.historyPath()).size;
      } catch (e) {
        this.historyBytes = 0;
      }
      await LogUpload.get().init(ctx);
    } catch (e) {
      this.writeOk = false;
      this.note = '日志初始化失败';
      console.warn('[AB] runlog init failed');
    }
  }

  // 设备 / native 版本由 UI 侧(已经 import 过 native 模块的地方)一次性灌进来,
  // 这样 RunLog 不需要再 import 一次 libaurorabench.so(那会多出一条 ArkTS 警告)
  setContext(device: string, nativeVer: string): void {
    if (device.length > 0) {
      this.device = device;
    }
    if (nativeVer.length > 0) {
      this.nativeVerValue = nativeVer;
    }
  }

  isReady(): boolean {
    return this.dir.length > 0;
  }

  mainPath(): string {
    return this.dir + '/' + RUNLOG_FILE;
  }

  rotatePath(i: number): string {
    return this.dir + '/runlog.' + i.toString() + '.jsonl';
  }

  // ---- runId: 与崩溃面包屑(CrashGuard)共用同一个会话 id ----
  // CrashGuard 每开始一项之前生成 id 并调到这里; 面包屑后续被 clear 也不影响本值,
  // 这样"整轮结束后的汇总行"能和该轮的逐项行落在同一个 runId 下。
  setRunId(id: string): void {
    if (id.length > 0) {
      this.runId = id;
    }
  }

  currentRunId(): string {
    if (this.runId.length === 0) {
      this.runId = Date.now().toString() + '-' + Math.floor(Math.random() * 100000).toString();
    }
    return this.runId;
  }

  // CrashGuard 每开始一项之前把 native 采样串灌进来; 空串 = 清掉(不显示)。
  setMemNote(mem: string): void {
    this.memNote = mem;
  }

  // 逐项行的 note = 调用方给的说明 + 开跑前的内存足迹
  private withMemNote(note: string): string {
    if (this.memNote.length === 0) {
      return note;
    }
    const mem: string = '开跑前内存 ' + this.memNote;
    return note.length > 0 ? (note + ' · ' + mem) : mem;
  }

  // ---- 写入 ----

  // 一行一次小 append(不重写整个文件); 任何异常都被吃掉, 只在内存里留标记
  private appendLine(line: string): boolean {
    if (this.dir.length === 0) {
      this.writeOk = false;
      this.note = '日志目录未就绪';
      return false;
    }
    try {
      const p: string = this.mainPath();
      const f = fs.openSync(p, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.APPEND);
      try {
        const n: number = fs.writeSync(f.fd, line);
        this.sizeBytes += (n > 0 ? n : line.length);
      } finally {
        fs.closeSync(f);
      }
      if (this.sizeBytes > RUNLOG_MAX_BYTES) {
        this.rotate();
      }
      return true;
    } catch (e) {
      this.writeOk = false;
      this.note = '日志写入失败(跑分不受影响)';
      console.warn('[AB] runlog append failed');
      return false;
    }
  }

  // 轮转: 最旧的删掉, 其余的依次后移, 当前文件变成 runlog.1.jsonl
  private rotate(): void {
    try {
      const last: string = this.rotatePath(RUNLOG_KEEP);
      if (fs.accessSync(last)) {
        fs.unlinkSync(last);
      }
      for (let i = RUNLOG_KEEP - 1; i >= 1; i--) {
        const from: string = this.rotatePath(i);
        if (fs.accessSync(from)) {
          fs.renameSync(from, this.rotatePath(i + 1));
        }
      }
      const main: string = this.mainPath();
      if (fs.accessSync(main)) {
        fs.renameSync(main, this.rotatePath(1));
      }
    } catch (e) {
      // 轮转失败时的兜底: 直接清空当前文件, 保证文件不会无限涨
      try {
        fs.truncateSync(this.mainPath(), 0);
      } catch (e2) {
        this.writeOk = false;
        this.note = '日志轮转失败';
      }
    }
    this.sizeBytes = 0;
  }

  // ---- 记录点 ----

  // 每完成"一项"负载后调用(同步: 一次小写入, 一行的量级)
  item(phase: string, item: string, index: number, total: number, ms: number,
    metric: number, unit: string, score: number, parallelism: number,
    ok: boolean, note: string): void {
    const line: RunLogItemLine = {
      ts: Date.now(),
      runId: this.currentRunId(),
      kind: 'item',
      phase: phase,
      item: item,
      index: index,
      total: total,
      ms: ms,
      metric: metric,
      unit: unit,
      score: score,
      parallelism: parallelism,
      ok: ok,
      note: this.withMemNote(note)
    };
    this.appendLine(JSON.stringify(line) + '\n');
  }

  // 每跑完"一轮"后调用; 顺带在开关打开时把日志 POST 出去(fire-and-forget)
  summary(s: RunSummaryInput): void {
    const line: RunLogSummaryLine = {
      ts: Date.now(),
      runId: this.currentRunId(),
      kind: 'run_summary',
      phase: s.phase,
      composite: s.composite,
      scored: s.scored,
      skipped: s.skipped,
      failed: s.failed,
      durationMs: s.durationMs,
      device: this.device.length > 0 ? this.device : this.fallbackDevice(),
      nativeVer: this.nativeVerValue.length > 0 ? this.nativeVerValue : 'unknown',
      availMemMB: s.availMemMB,
      thermal: s.thermal,
      smt: s.smt ?? '',
      threads: s.threads ?? 0
    };
    this.appendLine(JSON.stringify(line) + '\n');
    // 永久账本(旁路): 每完成"一轮"追加一行, 不参与日常轮转(5 MB 才轮转)
    const h: RunHistoryInput = {
      phase: s.phase,
      composite: s.composite,
      scored: s.scored,
      failed: s.failed,
      skipped: s.skipped,
      crashes: s.crashes,
      durationMs: s.durationMs
    };
    // 可选扩展字段: 只有调用方给了才写(没给就整键不出现, 老行的形状保持不变)
    const ex: RunHistoryExtra | undefined = s.extra;
    if (ex !== undefined) {
      h.runId = ex.runId;
      h.gb8Single = ex.gb8Single;
      h.gb8Multi = ex.gb8Multi;
      h.gb8Gpu = ex.gb8Gpu;
      h.ownSingle = ex.ownSingle;
      h.ownMulti = ex.ownMulti;
      h.ownGpu = ex.ownGpu;
      h.snlScore = ex.snlScore;
      h.npuGops = ex.npuGops;
      h.storeWriteMbps = ex.storeWriteMbps;
    }
    this.historyAdd(h);
    this.uploadNow().then((res: UploadResult) => {
      if (!res.ok) {
        console.info('[AB] runlog upload skipped/failed: ' + res.note);
      }
    });
  }

  // 启动时发现上次异常结束: 补一条崩溃记录(含最后启动的项 / 时间 / runId)
  crash(runId: string, phase: string, item: string, index: number, total: number,
    crumbTs: number, note: string): void {
    // 同一次崩溃(同一个面包屑时间戳)在同一次进程里只记一遍
    if (this.lastCrashJson.length > 0 && this.lastCrashTs === crumbTs) {
      return;
    }
    const where: string = phase + ' · ' + item;
    const line: RunLogCrashLine = {
      ts: Date.now(),
      runId: runId,
      kind: 'crash',
      phase: phase,
      item: item,
      index: index,
      total: total,
      crumbTs: crumbTs,
      where: where,
      when: stamp(crumbTs),
      note: note
    };
    const json: string = JSON.stringify(line);
    this.lastCrashJson = json;
    this.lastCrashTs = crumbTs;
    this.lastCrashWhere = where;
    this.appendLine(json + '\n');
  }

  lastCrashWhereText(): string {
    return this.lastCrashWhere;
  }

  lastCrashAt(): number {
    return this.lastCrashTs;
  }

  // ---- 联网上传(可选; 未开启时直接返回, 不产生任何网络行为) ----
  // manual = true: 用户在「关于」页点「立即发送一次」(只要求地址非空);
  // manual = false: 每轮跑分结束后的自动发送(要求"开关打开 且 地址非空")。
  uploadNow(manual: boolean = false): Promise<UploadResult> {
    if (!manual && !LogUpload.get().active()) {
      const skip: UploadResult = { ok: false, code: 0, ms: 0, note: '未开启日志自动发送(地址为空或开关关闭)' };
      return Promise.resolve(skip);
    }
    if (manual && LogUpload.get().url().length === 0) {
      const noUrl: UploadResult = { ok: false, code: 0, ms: 0, note: '未填写上传地址：本应用不发送任何数据' };
      return Promise.resolve(noUrl);
    }
    const runId: string = this.currentRunId();
    // 只有一种形态: 把通用 JSON 报文 POST 出去(报文按"发送范围"组包)
    const range: string = LogUpload.get().range();
    return LogUpload.get().send(this.buildPayload(range), LogUpload.get().rangeLabel()).then((res: UploadResult) => {
      this.logUploadResult(runId, res);
      return res;
    }).catch((e: Error) => {
      const res: UploadResult = { ok: false, code: 0, ms: 0, note: '上传异常: ' + e.message };
      this.logUploadResult(runId, res);
      return res;
    });
  }

  private logUploadResult(runId: string, res: UploadResult): void {
    const line: RunLogUploadLine = {
      ts: Date.now(),
      runId: runId,
      kind: 'upload',
      ok: res.ok,
      code: res.code,
      ms: res.ms,
      target: LogUpload.get().targetLabel(),
      note: res.note
    };
    this.appendLine(JSON.stringify(line) + '\n');
  }

  // ---- 上传素材(两种形态: 通用 JSON 报文 / 人读摘要) ----

  // 最近一轮 = runId 相同的全部原始行
  lastRunLines(): string[] {
    const lines: string[] = this.readLines();
    let id: string = '';
    for (let i = lines.length - 1; i >= 0; i--) {
      const h: RunLogHead | null = this.parseHead(lines[i]);
      if (h !== null && h.kind === 'run_summary') {
        id = h.runId;
        break;
      }
    }
    const out: string[] = [];
    if (id.length === 0) {
      return out;
    }
    for (const l of lines) {
      const h: RunLogHead | null = this.parseHead(l);
      if (h !== null && h.runId === id) {
        out.push(l);
      }
    }
    return out;
  }

  lastRunIdValue(): string {
    const lines: string[] = this.readLines();
    for (let i = lines.length - 1; i >= 0; i--) {
      const h: RunLogHead | null = this.parseHead(lines[i]);
      if (h !== null && h.kind === 'run_summary') {
        return h.runId;
      }
    }
    return '';
  }

  // 最近一次崩溃行(本次启动识别到的那条优先)
  lastCrashLine(): string {
    if (this.lastCrashJson.length > 0) {
      return this.lastCrashJson;
    }
    const lines: string[] = this.readLines();
    for (let i = lines.length - 1; i >= 0; i--) {
      const h: RunLogHead | null = this.parseHead(lines[i]);
      if (h !== null && h.kind === 'crash') {
        return lines[i];
      }
    }
    return '';
  }

  // 最近一次"发送"的结果(从盘上的 upload 行读回 —— 重启 App 后「关于」页仍然看得到)
  lastUploadLineText(): string {
    const lines: string[] = this.readLines();
    for (let i = lines.length - 1; i >= 0; i--) {
      const h: RunLogHead | null = this.parseHead(lines[i]);
      if (h !== null && h.kind === 'upload') {
        try {
          const u: RunLogUploadLine = JSON.parse(lines[i]) as RunLogUploadLine;
          return stamp(u.ts) + ' · ' + (u.ok ? '成功' : '失败') + ' · HTTP ' + u.code.toString() +
            ' · ' + u.ms.toString() + ' ms · 目标 ' + u.target + ' · ' + u.note;
        } catch (e) {
          return lines[i];
        }
      }
    }
    return '';
  }

  // 上传报文: 唯一形态 = 通用 JSON(手动「立即发送一次」与每轮自动发送都走它)
  buildPayload(range: string): string {
    const st: RunLogStats = this.stats();
    const runLines: string[] = this.lastRunLines();
    const history: string[] = range === UPLOAD_RANGE_HISTORY_N
      ? this.historyRecentLines(LogUpload.get().historyCount()) : [];
    const p: UploadPayload = {
      schema: 'aurora-runlog/1',
      uploadedAt: Date.now(),
      device: this.device.length > 0 ? this.device : this.fallbackDevice(),
      appVersion: 'Crush分 ' + APP_VER,
      nativeVersion: this.nativeVerValue.length > 0 ? this.nativeVerValue : 'unknown',
      range: LogUpload.get().rangeLabel(),
      fileBytes: st.sizeBytes,
      fileLines: st.lines,
      timeRange: st.rangeText,
      lastRunId: this.lastRunIdValue(),
      lastRunCount: runLines.length,
      lastRun: runLines,
      lastCrash: this.lastCrashLine(),
      historyCount: history.length,
      history: history,
      note: '本请求由用户在「关于」页主动填写地址并开启开关后触发; 未开启时本应用不产生任何上传。'
    };
    return JSON.stringify(p);
  }

  // =========================================================================
  // 永久账本 runhistory.jsonl —— 每完成"一轮"写一行, 5 MB 才轮转
  // (runlog.jsonl 是诊断日志: 逐项明细, 512 KB 就轮转; 两者互不影响)
  // =========================================================================

  historyPath(): string {
    return this.dir + '/' + HISTORY_FILE;
  }

  historyRotatePath(i: number): string {
    return this.dir + '/runhistory.' + i.toString() + '.jsonl';
  }

  // 追加一行账本; 只有超过 5 MB 才轮转(日常完全不轮转, 这就是它与 runlog 的区别)
  private appendHistoryLine(line: string): boolean {
    if (this.dir.length === 0) {
      this.writeOk = false;
      this.note = '日志目录未就绪';
      return false;
    }
    try {
      const p: string = this.historyPath();
      const f = fs.openSync(p, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.APPEND);
      try {
        const n: number = fs.writeSync(f.fd, line);
        this.historyBytes += (n > 0 ? n : line.length);
      } finally {
        fs.closeSync(f);
      }
      if (this.historyBytes > HISTORY_MAX_BYTES) {
        this.rotateHistory();
      }
      return true;
    } catch (e) {
      this.writeOk = false;
      this.note = '永久记录写入失败';
      console.warn('[AB] runhistory append failed');
      return false;
    }
  }

  // 轮转: runhistory.jsonl -> .1 -> .2 -> .3, 最旧的删除(最多保留 3 份)
  private rotateHistory(): void {
    try {
      const last: string = this.historyRotatePath(HISTORY_KEEP);
      if (fs.accessSync(last)) {
        fs.unlinkSync(last);
      }
      for (let i = HISTORY_KEEP - 1; i >= 1; i--) {
        const from: string = this.historyRotatePath(i);
        if (fs.accessSync(from)) {
          fs.renameSync(from, this.historyRotatePath(i + 1));
        }
      }
      const main: string = this.historyPath();
      if (fs.accessSync(main)) {
        fs.renameSync(main, this.historyRotatePath(1));
      }
    } catch (e) {
      console.warn('[AB] runhistory rotate failed');
    }
    this.historyBytes = 0;
  }

  // 每完成"一轮"追加一行。字段精简, 只回答"什么时候跑了多少分"。
  historyAdd(h: RunHistoryInput): void {
    const line: RunHistoryLine = {
      ts: Date.now(),
      kind: 'run',
      phase: h.phase,
      composite: Math.round(h.composite),
      scored: h.scored,
      failed: h.failed,
      skipped: h.skipped,
      crashes: h.crashes,
      durationMs: h.durationMs,
      device: this.device.length > 0 ? this.device : this.fallbackDevice(),
      appVer: 'Crush分 ' + APP_VER,
      nativeVer: this.nativeVerValue.length > 0 ? this.nativeVerValue : 'unknown'
    };
    // 可选扩展键: undefined 的键 JSON.stringify 会直接省略 —— 老读法看到的形状与以前一致
    line.startedAt = h.startedAt;
    line.interrupted = h.interrupted;
    line.runId = h.runId;
    line.gb8Single = h.gb8Single;
    line.gb8Multi = h.gb8Multi;
    line.gb8Gpu = h.gb8Gpu;
    line.ownSingle = h.ownSingle;
    line.ownMulti = h.ownMulti;
    line.ownGpu = h.ownGpu;
    //  ★ 2026-10-10 修: 这三个键以前**没有在这里赋值**, 于是虽然 summary() 把它们放进了
    //    RunHistoryInput、这里却一个字都没搬到真正落盘的 line 上 —— 账本行里根本没有这三个键,
    //    历史卡片下半部分那三行因此永远是 '--'(用户真机截图报的缺陷)。
    //    写账本这一步是"接口声明 / 搬进 line / 解析行"三处里的第二处, 少一处前面全白做;
    //    这条以后由 layout_cases 的 Q3f 断言钉住(它现在会检查 historyAdd 里有没有这三句)。
    line.snlScore = h.snlScore;
    line.npuGops = h.npuGops;
    line.storeWriteMbps = h.storeWriteMbps;
    this.appendHistoryLine(JSON.stringify(line) + '\n');
  }

  private readFileLines(path: string): string[] {
    const out: string[] = [];
    try {
      if (!fs.accessSync(path)) {
        return out;
      }
      if (fs.statSync(path).size === 0) {
        return out;
      }
      const text: string = fs.readTextSync(path);
      const parts: string[] = text.split('\n');
      for (const s of parts) {
        if (s.length > 0) {
          out.push(s);
        }
      }
    } catch (e) {
      this.writeOk = false;
      this.note = '永久记录读取失败';
    }
    return out;
  }

  historyLinesAll(): string[] {
    return this.readFileLines(this.historyPath());
  }

  // -------------------------------------------------------------------------
  //  账本行的只读视图(「历史」页用)
  // -------------------------------------------------------------------------
  //  数据源就是同一个 runhistory.jsonl —— **不另建一份数据**: 这一页与
  //  「历史成绩.txt」、与「关于」页的永久记录读的是同一批行, 只是这里解析成结构化字段。
  //  老行(8.4 之前的)没有 gb8*/own* 那几个键: 那几列给 0 并且 hasGb8 / hasOwn = false,
  //  由界面决定"显示 -- 还是干脆不显示这一条", 绝不补假值。
  //  只记了其中一组的老行(例如只记了自研那一组): 那一组为 true, 另一组为 false ——
  //  卡片上缺的那一组照样按"没拿到分"画 --(buildBigThree 的口径), 不编数。
  static parseHistoryRow(line: string): RunHistoryRow | null {
    if (line.length === 0) {
      return null;
    }
    try {
      const h: RunHistoryLine = JSON.parse(line) as RunHistoryLine;
      if (h.kind !== 'run') {
        return null;
      }
      const row: RunHistoryRow = {
        ts: h.ts,
        startedAt: (h.startedAt !== undefined && h.startedAt > 0) ? h.startedAt : h.ts,
        device: h.device !== undefined ? h.device : '',
        phase: h.phase !== undefined ? h.phase : '',
        appVer: h.appVer !== undefined ? h.appVer : '',
        composite: h.composite !== undefined ? h.composite : 0,
        runId: h.runId !== undefined ? h.runId : '',
        gb8Single: h.gb8Single !== undefined ? h.gb8Single : 0,
        gb8Multi: h.gb8Multi !== undefined ? h.gb8Multi : 0,
        gb8Gpu: h.gb8Gpu !== undefined ? h.gb8Gpu : 0,
        ownSingle: h.ownSingle !== undefined ? h.ownSingle : 0,
        ownMulti: h.ownMulti !== undefined ? h.ownMulti : 0,
        ownGpu: h.ownGpu !== undefined ? h.ownGpu : 0,
        snlScore: h.snlScore !== undefined ? h.snlScore : 0,
        npuGops: h.npuGops !== undefined ? h.npuGops : 0,
        storeWriteMbps: h.storeWriteMbps !== undefined ? h.storeWriteMbps : 0,
        failed: h.failed !== undefined ? h.failed : 0,
        skipped: h.skipped !== undefined ? h.skipped : 0,
        crashes: h.crashes !== undefined ? h.crashes : 0,
        interrupted: h.interrupted === true,
        hasScores: h.gb8Single !== undefined,
        hasGb8: h.gb8Single !== undefined,
        hasOwn: h.ownSingle !== undefined
      };
      return row;
    } catch (e) {
      return null;
    }
  }

  // 最近 limit 条(时间倒序: 最近一次在最前面); limit <= 0 = 全部。只读一次文件。
  historyRowsNewestFirst(limit: number): RunHistoryRow[] {
    const lines: string[] = this.historyLinesAll();
    const out: RunHistoryRow[] = [];
    for (let i = lines.length - 1; i >= 0; i--) {
      const r: RunHistoryRow | null = RunLog.parseHistoryRow(lines[i]);
      if (r !== null) {
        out.push(r);
        if (limit > 0 && out.length >= limit) {
          break;
        }
      }
    }
    return out;
  }

  // 最近 n 条(保持时间顺序); n <= 0 或不足 n 条时返回全部
  historyRecentLines(n: number): string[] {
    const all: string[] = this.historyLinesAll();
    if (n <= 0 || all.length <= n) {
      return all;
    }
    const out: string[] = [];
    for (let i = all.length - n; i < all.length; i++) {
      out.push(all[i]);
    }
    return out;
  }

  private historyTs(line: string): number {
    try {
      const h: RunHistoryLine = JSON.parse(line) as RunHistoryLine;
      return h.ts;
    } catch (e) {
      return 0;
    }
  }

  // 一行账本的人读形式(给摘要/界面用)
  historyLineText(line: string): string {
    try {
      const h: RunHistoryLine = JSON.parse(line) as RunHistoryLine;
      return stamp(h.ts) + ' · ' + h.phase + ' · 综合 ' + h.composite.toFixed(0) +
        ' · 计分 ' + h.scored.toString() + ' · 失败 ' + h.failed.toString() +
        ' · 跳过 ' + h.skipped.toString() + ' · 崩溃 ' + h.crashes.toString() +
        ' · 用时 ' + (h.durationMs / 1000).toFixed(1) + ' s';
    } catch (e) {
      return line;
    }
  }

  historyStats(): RunHistoryStats {
    return this.historyStatsOf(this.historyLinesAll());
  }

  // 统计 + 最近 recentN 条(只读一次文件)
  historySnapshot(recentN: number): RunHistorySnapshot {
    const all: string[] = this.historyLinesAll();
    const recent: string[] = (recentN <= 0 || all.length <= recentN)
      ? all : all.slice(all.length - recentN);
    const snap: RunHistorySnapshot = { stats: this.historyStatsOf(all), recent: recent };
    return snap;
  }

  private historyStatsOf(all: string[]): RunHistoryStats {
    let lines: number = 0;
    let firstTs: number = 0;
    let lastTs: number = 0;
    let rot: number = 0;
    let rotBytes: number = 0;
    let ok: boolean = this.writeOk;
    let note: string = this.note;
    try {
      lines = all.length;
      if (lines > 0) {
        firstTs = this.historyTs(all[0]);
        lastTs = this.historyTs(all[lines - 1]);
      }
      for (let i = 1; i <= HISTORY_KEEP; i++) {
        const rp: string = this.historyRotatePath(i);
        if (fs.accessSync(rp)) {
          rot++;
          rotBytes += fs.statSync(rp).size;
        }
      }
    } catch (e) {
      ok = false;
      note = '永久记录统计失败';
    }
    const size: number = this.historyBytes + rotBytes;
    const st: RunHistoryStats = {
      ok: ok,
      note: note,
      lines: lines,
      sizeBytes: size,
      sizeText: fmtBytes(size),
      files: rot + (this.historyBytes > 0 ? 1 : 0),
      rotatedFiles: rot,
      firstTs: firstTs,
      lastTs: lastTs,
      rangeText: firstTs > 0 ? (stamp(firstTs) + ' ~ ' + stamp(lastTs)) : '（暂无记录）'
    };
    return st;
  }

  // 导出用全文: 先历史文件(旧)再当前文件(新); 开头几行 '#' 是自解释的注释, 其余每行一个 JSON
  historyExportText(): string {
    const st: RunHistoryStats = this.historyStats();
    let s: string = '# Crush分 · 永久记录 ' + HISTORY_FILE + '\n';
    s += '# 设备：' + (this.device.length > 0 ? this.device : this.fallbackDevice()) + '\n';
    s += '# App：Crush分 ' + APP_VER + ' · Native ' +
      (this.nativeVerValue.length > 0 ? this.nativeVerValue : 'unknown') + '\n';
    s += '# 导出时间：' + stamp(Date.now()) + '\n';
    s += '# 共 ' + st.lines.toString() + ' 条（当前文件），另有历史文件 ' +
      st.rotatedFiles.toString() + ' 个；时间跨度 ' + st.rangeText + '\n';
    s += '# 每行一个 JSON：{ ts, kind, phase, composite, scored, failed, skipped, crashes, durationMs, device, appVer, nativeVer }\n';
    for (let i = HISTORY_KEEP; i >= 1; i--) {
      for (const l of this.readFileLines(this.historyRotatePath(i))) {
        s += l + '\n';
      }
    }
    for (const l of this.historyLinesAll()) {
      s += l + '\n';
    }
    return s;
  }

  // 把文本写到用户选定的文件(文档选择器返回的 uri)。
  // 失败降级: 有的 provider 不接受 TRUNC, 退一步用普通读写模式再试一次。
  writeToUri(uri: string, text: string): boolean {
    try {
      const f = fs.openSync(uri, fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
      try {
        fs.writeSync(f.fd, text);
      } finally {
        fs.closeSync(f);
      }
      return true;
    } catch (e) {
      try {
        const f2 = fs.openSync(uri, fs.OpenMode.READ_WRITE);
        try {
          fs.writeSync(f2.fd, text);
        } finally {
          fs.closeSync(f2);
        }
        return true;
      } catch (e2) {
        console.warn('[AB] export write failed');
        return false;
      }
    }
  }

  // ---- 读取 / 展示 ----

  private parseHead(line: string): RunLogHead | null {
    if (line.length === 0) {
      return null;
    }
    try {
      return JSON.parse(line) as RunLogHead;
    } catch (e) {
      return null;
    }
  }

  // 读取当前文件全部内容(上限 512 KB, 只在"关于"页刷新 / 上传时调用)
  private readAll(): string {
    try {
      const p: string = this.mainPath();
      if (!fs.accessSync(p)) {
        return '';
      }
      if (fs.statSync(p).size === 0) {
        return '';
      }
      return fs.readTextSync(p);
    } catch (e) {
      this.writeOk = false;
      this.note = '日志读取失败';
      return '';
    }
  }

  private readLines(): string[] {
    const text: string = this.readAll();
    if (text.length === 0) {
      return [];
    }
    const parts: string[] = text.split('\n');
    const out: string[] = [];
    for (const s of parts) {
      if (s.length > 0) {
        out.push(s);
      }
    }
    return out;
  }

  // 尾部预览: 只取末尾 RUNLOG_TAIL 个字符, 不整块塞进 UI
  tailText(max: number): string {
    const text: string = this.readAll();
    if (text.length === 0) {
      return '（暂无日志）';
    }
    if (text.length <= max) {
      return text;
    }
    return '…（省略前 ' + (text.length - max).toString() + ' 字符）\n' + text.substring(text.length - max);
  }

  stats(): RunLogStats {
    const mainSize: number = this.sizeBytes;
    let size: number = 0;
    let lines: number = 0;
    let rot: number = 0;
    let rotBytes: number = 0;
    let firstTs: number = 0;
    let lastTs: number = 0;
    let ok: boolean = this.writeOk;
    let note: string = this.note;
    try {
      for (let i = 1; i <= RUNLOG_KEEP; i++) {
        const rp: string = this.rotatePath(i);
        if (fs.accessSync(rp)) {
          rot++;
          rotBytes += fs.statSync(rp).size;
        }
      }
      const text: string = this.readAll();
      const parts: string[] = text.split('\n');
      for (const s of parts) {
        if (s.length > 0) {
          lines++;
        }
      }
      // 时间范围: 只解析首尾两行, 不遍历全部
      for (let i = 0; i < parts.length; i++) {
        const h: RunLogHead | null = this.parseHead(parts[i]);
        if (h !== null) {
          firstTs = h.ts;
          break;
        }
      }
      for (let i = parts.length - 1; i >= 0; i--) {
        const h: RunLogHead | null = this.parseHead(parts[i]);
        if (h !== null) {
          lastTs = h.ts;
          break;
        }
      }
      size = mainSize + rotBytes;
    } catch (e) {
      ok = false;
      note = '日志统计失败';
    }
    // readAll() 失败时会把 writeOk/note 改掉, 所以这里再取一次最终状态
    if (!this.writeOk) {
      ok = false;
      if (this.note.length > 0) {
        note = this.note;
      }
    }
    const st: RunLogStats = {
      ok: ok,
      note: note,
      sizeBytes: size,
      sizeText: fmtBytes(size),
      lines: lines,
      files: rot + (mainSize > 0 ? 1 : 0),
      rotatedFiles: rot,
      firstTs: firstTs,
      lastTs: lastTs,
      rangeText: firstTs > 0 ? (stamp(firstTs) + ' ~ ' + stamp(lastTs)) : '（暂无时间范围）'
    };
    return st;
  }

  // 复制出来的文本自带上下文, 用户直接粘回即可用
  copyText(): string {
    const st: RunLogStats = this.stats();
    let s: string = '[Crush分 · 运行日志 runlog.jsonl]\n';
    s += '设备：' + (this.device.length > 0 ? this.device : this.fallbackDevice()) + '\n';
    s += 'App 版本：Crush分 ' + APP_VER + '\n';
    s += 'Native 版本：' + (this.nativeVerValue.length > 0 ? this.nativeVerValue : 'unknown') + '\n';
    s += '日志文件：' + RUNLOG_FILE + ' · ' + st.sizeText + ' · ' + st.lines.toString() + ' 行';
    if (st.rotatedFiles > 0) {
      s += '（另有历史文件 ' + st.rotatedFiles.toString() + ' 个）';
    }
    s += '\n';
    s += '时间范围：' + st.rangeText + '\n';
    s += '导出时间：' + stamp(Date.now()) + '\n';
    s += '写入状态：' + (st.ok ? '正常' : ('异常 —— ' + st.note)) + '\n';
    const crashWhere: string = this.lastCrashWhereText();
    if (crashWhere.length > 0) {
      s += '本次启动记录的崩溃：' + crashWhere + ' · ' + stamp(this.lastCrashTs) + '\n';
    }
    s += '日志字段：kind=item(逐项) / run_summary(每轮汇总) / crash(异常结束) / upload(上传结果)\n';
    s += '----------------------------------------\n';
    s += this.tailText(RUNLOG_TAIL);
    return s;
  }

  // 清空当前 + 全部历史文件(带二次确认由 UI 负责)
  clear(): boolean {
    try {
      for (let i = 1; i <= RUNLOG_KEEP; i++) {
        const rp: string = this.rotatePath(i);
        if (fs.accessSync(rp)) {
          fs.unlinkSync(rp);
        }
      }
      const p: string = this.mainPath();
      if (fs.accessSync(p)) {
        fs.unlinkSync(p);
      }
      this.sizeBytes = 0;
      this.writeOk = true;
      this.note = '';
      return true;
    } catch (e) {
      this.writeOk = false;
      this.note = '清空日志失败';
      return false;
    }
  }

  // 设备串兜底(与 BenchRunner.deviceLabel 同口径: 传播名（型号码）· 系统 · API)
  private fallbackDevice(): string {
    try {
      const d: DeviceProfileData = DeviceProfile.get().data();
      let s: string = d.marketingName;
      if (d.modelCode.length > 0 && d.modelCode !== s) {
        s += '（' + d.modelCode + '）';
      }
      const os: string = d.osFullName.length > 0 ? d.osFullName : 'HarmonyOS';
      return s + ' · ' + os + ' · API ' + d.apiVersion.toString();
    } catch (e) {
      try {
        const model: string = deviceInfo.productModel ?? 'Unknown';
        const os: string = deviceInfo.osFullName ?? 'HarmonyOS';
        const api: number = deviceInfo.sdkApiVersion ?? 0;
        return model + ' · ' + os + ' · API ' + api.toString();
      } catch (e2) {
        return 'Unknown';
      }
    }
  }
}
