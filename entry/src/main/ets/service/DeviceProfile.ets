import { deviceInfo } from '@kit.BasicServicesKit';
import { display } from '@kit.ArkUI';
import { DeviceSpec, MODEL_ALIAS, SAMSUNG_POSTFIX_MIN_LEN, SAMSUNG_PREFIX, TABLE_ENTRY_COUNT,
  isPrefixConflicted, lookupModelWithPostfix, lookupSpec } from './DeviceModelTable';
// 形态判定与布局计划: 纯函数(自身零 import, 无任何 ohos 运行时依赖), 可在 Node 里跑离线用例
// (见 ref/layout_cases.test.mjs)。本文件只负责把"实时窗口 + 折叠状态 + 设备类型"喂进去。
import { LayoutPlan, ShapeInput, FS_EXPANDED, FS_FOLDED, FS_HALF, FS_UNKNOWN,
  resolvePlan, planLines, aspectOf } from '../common/LayoutPlan';

// ---------------------------------------------------------------------------
// 设备画像(单例 + 缓存)
//
//  1. 传播名: 逐级回退, 并且记下"实际命中的是哪一级", 关于页要显示这一条;
//  2. 形态: phone / tablet / 2in1 ... + 折叠屏(展开 / 折叠 / 半折叠, 随折叠状态实时变);
//  3. 由画像推导布局参数(列数 / 内容最大宽度 / 开始按钮系数 / 按钮高度), 断点只作兜底。
//
//  内置对照表(110 条, ./DeviceModelTable.ets, 由 ref/gen_device_table.py 生成)只做两件事:
//    a) 系统给不出传播名时兜底"型号码 → 传播名"(规则见下);
//    b) 系统折叠屏接口不可用时, 用它标记的 formFactor 兜底"这可能是折叠屏 / 平板"。
//  系统 API 可用时一律以系统为准。
//
//  只读设备信息, 不参与任何测试项的计算 / 顺序 / 计分(它只影响观感)。
//  折叠屏接口(display.isFoldable / getFoldStatus / foldStatusChange)全部 try/catch:
//  取不到就降级(内置表兜底形态, 再兜底"断点布局"), 并在关于页写明"折叠屏接口不可用"。
// ---------------------------------------------------------------------------

// ---- 传播名的来源(逐级回退, 关于页显示"实际命中的那一级"; 本机型号 HOP-AL00 走 C1) ----
export const NAME_SRC_ALIAS: string = 'A 系统别名 productModelAlias';
export const NAME_SRC_MARKET: string = 'B 系统市场名 marketName';
export const NAME_SRC_TABLE: string = 'C1 内置表 · 完整型号码精确命中';
export const NAME_SRC_POSTFIX: string = 'C1b 内置表 · 型号码去后缀后精确命中（三星）';
export const NAME_SRC_PREFIX: string = 'C2 内置表 · 前缀唯一命中';
export const NAME_SRC_MODEL: string = 'D 型号码回退（没识别到传播名）';
export const NAME_SRC_PREFIX_CONFLICT: string = '前缀冲突, 未映射（直接显示型号码原文）';

// ---- 内置表形态的兜底说明(系统 API 可用时一律以系统为准) ----
export const FF_FALLBACK_TAG: string = '（来自内置表, 非系统 API）';
export const TABLE_NO_SCREEN: string = '（表内无屏幕参数）';

// ---- 折叠状态文案 ----
export const FOLD_NONE: string = '非折叠屏';
export const FOLD_EXPANDED: string = '展开';
export const FOLD_FOLDED: string = '折叠';
export const FOLD_HALF: string = '半折叠';
export const FOLD_UNKNOWN: string = '未知';

// ---------------------------------------------------------------------------
// 内置对照表(110 条)在 ./DeviceModelTable.ets —— 由 ref/device_models.json 生成,
// 请勿手改; 数据来源与存疑说明见 ref/DEVICE_MODELS.md。本文件只放匹配规则。
//
// 匹配规则(严格照报告第 6 节):
//   A  系统别名 productModelAlias       —— 系统给的, 最准(但等于型号码时视为没给)
//   B  系统市场名 marketName            —— 同上判据
//   C1 内置表 · 完整型号码精确匹配      —— 主路径(本机 const.product.model 就是 HOP-AL00);
//                                          主型号码 + variants 备用型号码都在精确键里
//   C1b 三星去后缀精确匹配              —— 三星 Build.MODEL 常带颜色/容量后缀(如 SM-S948BZVCXME),
//                                          去掉尾部后缀再精确匹配; 只对长度>=8 且 SM- 开头的值启用
//   C2 内置表 · 前缀唯一匹配            —— 取 ^([A-Z]{2,4})- 前缀, **该前缀在表内只对应一个
//                                          传播名**才允许; 有多个(19 组冲突)一律不用
//   D  型号码回退                       —— 显示型号码原文
//   前缀冲突: 归到 D 级, 但解析来源写明"前缀冲突, 未映射"。
//
// 品牌判定只用 deviceInfo.brand —— 不用型号码前缀或形态推断品牌:
//   荣耀 HONOR(BVL-N49 / FCP-N49 / MAG-AN00 / MBH-N49)与华为同源命名;
//   小米/POCO 是 8 位数字码(23127PN0CC)、Redmi 2312DRA50C; vivo/iQOO 形如 V2408A;
//   OPPO 国行 P??110、海外 CPH####; 三星 SM-*。这些形态都只能靠 deviceInfo.brand 判品牌。
//   PD2309 这类是 vivo 的 Build.DEVICE(不是型号码, 且 X100 与 X100s 共用), 表里没有、也不加,
//   读出来只当未知值显示原文。
// ---------------------------------------------------------------------------

// 取型号码前缀(第一个 '-' 之前的部分, 形如 ^([A-Z]{2,4})-)
function modelPrefixOf(model: string): string {
  const dash: number = model.indexOf('-');
  if (dash < 2 || dash > 4) {
    return '';
  }
  const p: string = model.substring(0, dash);
  for (let i = 0; i < p.length; i++) {
    const c: string = p.charAt(i);
    if (c < 'A' || c > 'Z') {
      return '';
    }
  }
  return p;
}

// 唯一前缀匹配: 表内所有以该前缀开头的键(主型号码 + 备用型号码)必须指向同一个传播名。
// 冲突的前缀(ALT- / SGT- / ALN- / BLK- / GRL- ...)在 isPrefixConflicted 里被直接拦掉。
function lookupByPrefix(model: string): string {
  const prefix: string = modelPrefixOf(model);
  if (prefix.length === 0 || isPrefixConflicted(prefix)) {
    return '';
  }
  const keys: string[] = Object.keys(MODEL_ALIAS);
  let hit: string = '';
  for (const k of keys) {
    if (k.length > prefix.length && k.startsWith(prefix + '-')) {
      const n: string = MODEL_ALIAS[k];
      if (hit.length === 0) {
        hit = n;
      } else if (hit !== n) {
        // 表内该前缀对应多个传播名 -> 不许猜
        return '';
      }
    }
  }
  return hit;
}

// 这条型号码在表内是不是"前缀冲突"
function isConflictedModel(model: string): boolean {
  return isPrefixConflicted(modelPrefixOf(model));
}

// 来源 URL 只显示域名(完整 URL 在可复制的设备画像全文里)
function hostOf(url: string): string {
  if (url.length === 0) {
    return '';
  }
  let s: string = url;
  const scheme: number = s.indexOf('://');
  if (scheme >= 0) {
    s = s.substring(scheme + 3);
  }
  const slash: number = s.indexOf('/');
  if (slash >= 0) {
    s = s.substring(0, slash);
  }
  return s;
}

// 表内该条目的形态说明(关于页"匹配方式"那一行的后半段)
function formFactorText(spec: DeviceSpec | null): string {
  if (spec === null) {
    return '';
  }
  if (spec.formFactor === 'foldable') {
    return '折叠屏';
  }
  if (spec.formFactor === 'tablet') {
    return '平板';
  }
  return '直板';
}

// 表内屏幕参数(英寸 + 分辨率 + 来源域名); 表里没有就明说, 不瞎填
function screenSpecText(spec: DeviceSpec | null): string {
  if (spec === null) {
    return '';
  }
  if (spec.innerScreenInch < 0 && spec.outerScreenInch < 0 && spec.resolution.length === 0) {
    return TABLE_NO_SCREEN;
  }
  let s: string = '';
  if (spec.formFactor === 'foldable') {
    s += '内屏 ' + (spec.innerScreenInch > 0 ? (spec.innerScreenInch.toString() + ' 英寸') : '（表内无）');
    s += ' / 外屏 ' + (spec.outerScreenInch > 0 ? (spec.outerScreenInch.toString() + ' 英寸') : '（表内无）');
  } else if (spec.innerScreenInch > 0) {
    s += spec.innerScreenInch.toString() + ' 英寸';
  } else {
    s += '屏幕尺寸（表内无）';
  }
  if (spec.resolution.length > 0) {
    s += ' · ' + spec.resolution;
  }
  const host: string = hostOf(spec.source);
  if (host.length > 0) {
    s += ' · 来源 ' + host;
  } else {
    s += ' · 来源（表内无）';
  }
  return s;
}

// 布局计划 LayoutPlan 已移到 ../common/LayoutPlan.ets —— 那里是纯函数(自身零 import),
// 判定规则与参数表都在那一份文件里, 可以原样抽出来在 Node 里跑离线用例核对。

// ---------------------------------------------------------------------------
//  @ohos.deviceInfo 逐字段读取结果(关于页「设备画像」要展示的一张表)
//
//  为什么要逐字段列出来: 用户问过"你读不到我的信息吗" —— 之前用 hdc shell 读不到 SoC,
//  但 App 自己(自己的 SELinux 域)可能读得到 deviceInfo.hardwareModel, 那才是**设备自报的
//  权威答案**, 比任何第三方资料可信。读不到的字段也要列出(写清取不到), 不许留空假装。
// ---------------------------------------------------------------------------
export interface HwField {
  label: string;   // 界面上显示的字段名(与 deviceInfo 的属性名一一对应)
  value: string;   // 读到的值; 空串 = 取不到, 此时 why 说明原因
  why: string;     // 取不到的原因(读到值时为空串)
}

// 读一个 deviceInfo 字段: 返回 [值, 取不到的原因]。
//   读到了但为空 -> '（空值）'; 抛异常 -> '取不到：' + 异常; 类型不对 -> '取不到：类型不是字符串'
function readDevField(label: string, getter: () => string): HwField {
  try {
    const v: string = getter();
    if (v === undefined || v === null) {
      return { label: label, value: '', why: '取不到：接口返回 null/undefined' };
    }
    if (typeof v !== 'string') {
      return { label: label, value: '', why: '取不到：类型不是字符串' };
    }
    if (v.length === 0) {
      return { label: label, value: '', why: '取不到：（空字符串）' };
    }
    return { label: label, value: v, why: '' };
  } catch (e) {
    const msg: string = (e as Error).message ?? '未知异常';
    return { label: label, value: '', why: '取不到：' + msg };
  }
}

// 读一个数值型 deviceInfo 字段(鸿蒙的 majorVersion / seniorVersion / firstApiVersion 是 number)
function readDevNumField(label: string, getter: () => number): HwField {
  try {
    const v: number = getter();
    if (v === undefined || v === null) {
      return { label: label, value: '', why: '取不到：接口返回 null/undefined' };
    }
    if (typeof v !== 'number') {
      return { label: label, value: '', why: '取不到：类型不是数字' };
    }
    return { label: label, value: v.toString(), why: '' };
  } catch (e) {
    const msg: string = (e as Error).message ?? '未知异常';
    return { label: label, value: '', why: '取不到：' + msg };
  }
}

// deviceInfo 里与"这台机器是什么"有关的字段, 逐项读一遍(顺序固定, 便于对照)。
//   hardwareModel  —— 重点: 鸿蒙上常常就是 SoC/硬件型号; 有值就是这台机器的权威答案
//   marketName / productModel / productModelAlias / brand / deviceType —— 机型身份
//   osFullName / displayVersion / sdkApiVersion —— 系统与版本
//   udid —— 设备唯一标识(部分权限下取不到, 取不到就写"取不到")
export function collectDeviceInfoFields(): HwField[] {
  const out: HwField[] = [];
  out.push(readDevField('hardwareModel', () => deviceInfo.hardwareModel as string));
  out.push(readDevField('marketName', () => deviceInfo.marketName as string));
  out.push(readDevField('productModel', () => deviceInfo.productModel as string));
  out.push(readDevField('productModelAlias', () => deviceInfo.productModelAlias as string));
  out.push(readDevField('brand', () => deviceInfo.brand as string));
  out.push(readDevField('deviceType', () => deviceInfo.deviceType as string));
  out.push(readDevField('osFullName', () => deviceInfo.osFullName as string));
  out.push(readDevField('displayVersion', () => deviceInfo.displayVersion as string));
  out.push(readDevField('softwareModel', () => deviceInfo.softwareModel as string));
  out.push(readDevField('manufacture', () => deviceInfo.manufacture as string));
  out.push(readDevField('productSeries', () => deviceInfo.productSeries as string));
  out.push(readDevField('bootloaderVersion', () => deviceInfo.bootloaderVersion as string));
  out.push(readDevField('abiList', () => deviceInfo.abiList as string));
  out.push(readDevField('buildRootHash', () => deviceInfo.buildRootHash as string));
  out.push(readDevField('distributionOSName', () => deviceInfo.distributionOSName as string));
  out.push(readDevField('distributionOSVersion', () => deviceInfo.distributionOSVersion as string));
  out.push(readDevField('osReleaseType', () => deviceInfo.osReleaseType as string));
  out.push(readDevField('securityPatchTag', () => deviceInfo.securityPatchTag as string));
  out.push(readDevNumField('majorVersion', () => deviceInfo.majorVersion as number));
  out.push(readDevNumField('seniorVersion', () => deviceInfo.seniorVersion as number));
  out.push(readDevField('incrementalVersion', () => deviceInfo.incrementalVersion as string));
  out.push(readDevField('buildType', () => deviceInfo.buildType as string));
  out.push(readDevNumField('firstApiVersion', () => deviceInfo.firstApiVersion as number));
  out.push(readDevField('versionId', () => deviceInfo.versionId as string));
  // ---- 以下三个字段本 SDK 标了权限注解, 读它们会让编译期多出 1 条 ArkTS 告警 ----
  // udid / serial / diskSN 需要 ohos.permission.sec.ACCESS_UDID; 告警预算只有 11 条(基线),
  // 所以这里不加它们(而不是假装读过)。要显示这三项时, 直接把下面两行注释打开即可
  // (代价: ArkTS 告警从 11 条变成 13 条), 并在 module.json5 里按需申请该权限。
  // out.push(readDevField('udid', () => deviceInfo.udid as string));
  // out.push(readDevField('serial', () => deviceInfo.serial as string));
  // out.push(readDevField('diskSN', () => deviceInfo.diskSN as string));
  // sdkApiVersion 也是 number, 同上
  out.push(readDevNumField('sdkApiVersion', () => deviceInfo.sdkApiVersion as number));
  return out;
}

export interface DeviceProfileData {
  // ---- 身份 ----
  brand: string;
  modelCode: string;
  marketingName: string;
  nameSource: string;
  nameSourceDetail: string;
  matchMethod: string;        // 关于页那一行"匹配方式"(实际命中的那一级)
  // ---- 内置表(DeviceModelTable.ets)查到的 ----
  tableHit: boolean;          // 型号码是否命中内置表(精确 / 唯一前缀)
  tableName: string;          // 表内传播名(未命中为空)
  tableFormFactor: string;    // 表内形态 bar | foldable | tablet(未命中为空)
  tableScreenText: string;    // 表内屏幕参数(内屏/外屏英寸 · 分辨率 · 来源域名); 表内没有则说明"表内无屏幕参数"
  tableSpecSource: string;    // 完整来源 URL(给可复制文本)
  tableSpecHost: string;      // 来源域名(界面只显示这个)
  tableNote: string;          // 表内备注(存疑说明)
  // ---- 最近一次跑分的绑核状态(native runGb7 的 cpuInfo; 没跑过就是空) ----
  // 全文原样透传 native 的措辞(native 已按"大核簇"更新): 这里不自己拼判据
  lastCpuInfo: string;        // 最近一次跑分的"跑在哪个核 / 哪个簇"一行文本
  lastCpuAt: number;          // 写入时间(0 = 还没有)
  // ---- CPU 拓扑(逻辑核 / 物理核 / 是否检测到 SMT / 来源) ----
  // 与 lastCpuInfo 一样是"跑分侧灌进来的旁路文本": 本类不 import native 模块
  // (免得再多一条 ArkTS 模块未验证的警告), 只负责显示与复制。
  cpuTopology: string;        // 形如 "14 逻辑核 / 14 物理核（无 SMT, 开关无差别）→ 多核阶段 14 线程；来源 thread_siblings_list(…)"
  // ---- 内核允许的核集合(2026-10-05): 回答"为什么这台机器的分数像被锁住了" ----
  // 同样由跑分侧灌进来(本类不 import native)。两行分开, 因为它们是两个不同的问题:
  //   cpuAffinityLine = 可用核集合 vs 快簇 vs 交集(一行结论, 例:
  //     "cpuset: allowed=0-9 (10 核) · 全机最快档=10-13(4 核, 最高 2750 MHz) · 交集=空 => ...")
  //   cpuAffinityFull = 逐路径实测事实(Cpus_allowed / sched_getaffinity / cgroup / cpuset / Mems_allowed_list)
  cpuAffinityLine: string;
  cpuAffinityFull: string;
  // 全机频率档(逐核口径, 含读失败的核): 与可用核集合放在一起, 两台机器一眼可比
  cpuFreqTiers: string;
  // ---- 芯片判读(同样由跑分侧灌进来; 本类不 import native) ----
  //  刻意拆成三段, 且顺序固定: 先"本机实测", 再"逐核原始读数", 最后才是"第三方资料的型号推断"。
  //   资料里 26 条麒麟 SoC 的 confidence: official = 0 条 —— 华为从未公布过任何麒麟 SoC 的
  //   CPU 核心布局。把设备自报的事实与第三方推断混成一句, 等于替华为背书一个它没说过的事实。
  chipMeasured: string;       // "芯片与频率（本机实测，设备自报）：14 逻辑核 / 9 物理核（检测到 SMT）· 逐核 … 档位 …"
  chipPerCore: string;        // 逐核 cpuinfo_max_freq 原始读数(读失败的核带 errno)
  chipInfer: string;          // "芯片判读（第三方资料推断，非官方）：疑似 …/ 数据不足 …/ 无法区分 …"
  // ---- @ohos.deviceInfo 逐字段读取结果(读到什么 / 取不到什么, 一项都不省略) ----
  hwFields: HwField[];
  deviceType: string;
  osFullName: string;
  apiVersion: number;
  // ---- 形态 ----
  foldable: boolean;
  foldApiOk: boolean;
  foldApiNote: string;
  foldStatusText: string;
  expanded: boolean;
  // ---- 屏幕 / 窗口 ----
  screenWidthPx: number;
  screenHeightPx: number;
  densityDpi: number;
  windowWidthVp: number;
  windowHeightVp: number;
  windowAspect: number;       // 窗口宽高比(0 = 未取到)
  windowSource: string;       // 窗口尺寸的来源: 'windowSizeChange(px→vp)' / 'getWindowProperties(px→vp)' / 空
  // 单位换算依据(2026-10-08 修单位 bug 时新增): 窗口尺寸到底是 px 还是 vp、原始值多少、
  //   用哪个密度换算的、与 mediaquery 断点档是否一致 —— 一句话写全, 直接进 runlog 的设备行。
  //   真机把 windowSizeChange 的 **px** 当成 vp 用(1828x2584 写成 1828x2584vp), 形态判定跟着
  //   错成"竖直内屏", 就是缺少这一段造成的: 报告里只说 vp, 没说这个 vp 是怎么来的。
  windowUnitBasis: string;    // 形如 '1828x2584px ÷ 密度 2.750(densityPixels) = 664x940vp · 与 mediaquery md 档一致'
  densityPixels: number;      // 屏幕密度(px → vp 的除数): vp = px / densityPixels; 0 = 未取到
  windowProbe: string;        // 窗口尺寸是怎么取到的 / 取不到时的原因(不许只留"尚未取到")
  bpWide: boolean;
  bpName: string;             // 'sm' | 'md' | 'lg' —— mediaquery 三档, 只作旁证, 不参与形态判定
  // ---- 形态判定(纯函数 LayoutPlan.ets 的输出; 关于页「布局自检」逐项显示, 可复制) ----
  shapeId: string;            // S0 ~ S12(纯几何编号; 只用于显示与日志, 不参与布局决策)
  shapeIndex: number;         // 需求表里的形态编号 1..9(0 = 窗口尺寸未取到)
  shapeName: string;
  shapeBasis: string;         // 判定依据(窗口尺寸 / 折叠状态 / 设备类型)
  shapeNote: string;
  shapeDegrade: string;       // 降级原因(空串 = 没有任何降级, 所有判定接口都可用)
  foldKey: string;            // FS_NONE / FS_EXPANDED / FS_FOLDED / FS_HALF / FS_UNKNOWN
  shapeSig: string;           // 上次重算的输入签名(输入没变就一个字段都不动 -> 幂等, 不产生 UI 刷新)
  layoutSelfCheck: string;    // 「布局自检」可复制全文
  // ---- 推导出的布局 ----
  plan: LayoutPlan;
}

// 窗口尺寸探测的结果(见 DeviceProfile.applyWindowPx)。
//   ok = false 时 why 里必须写清为什么取不到(例如"getWindowProperties 返回 0x0"),
//   由 BenchRunner.deviceLabel 原样写进 runlog 的设备行 —— 真机日志里"窗口=（尚未取到）"
//   出现过几百次却没有任何原因, 那种日志等于什么都没说。
export interface WindowSizeVp {
  ok: boolean;
  widthVp: number;
  heightVp: number;
  why: string;
  // ---- 换算依据(换算成功时填; 失败时全 0/空) ----
  widthPx: number;            // 原始 px 读数(调用方给什么写什么, 不改写)
  heightPx: number;
  scale: number;              // 用的密度(px / vp)
  scaleSource: string;        // 密度是从哪读到的: 'densityPixels' / 'densityDPI/160' / ''(没读到)
  basis: string;              // 一句话换算依据(可直接进设备行 / 布局自检)
}

// 窗口尺寸还没被任何来源填上时的初值(会原样出现在 runlog 的设备行里)
export const WINDOW_PROBE_INIT: string =
  '两个来源都还没有结果(getWindowProperties 与 windowSizeChange 都未回调)';

// 兜底计划: 窗口尺寸还没取到时用(最保守的单列窄屏参数)。
// 它不是"用一个默认值假装成功": shapeDegrade / layoutSelfCheck 会写明"窗口尺寸未取到",
// windowSizeChange 一到达就立刻重算。
function planFallback(): LayoutPlan {
  const inp: ShapeInput = {
    widthVp: 0, heightVp: 0, foldState: FS_UNKNOWN, foldApiOk: false,
    deviceType: '', tableFormFactor: ''
  };
  return resolvePlan(inp, '');
}

// 密度的小数写法: 2 -> "2", 2.75 -> "2.750"(设备行里要能一眼看出用的是哪个密度,
// 所以固定三位小数; 整数密度不补零, 免得把 2 写成 "2.000" 让人以为是另一个数)。
export function scaleText(scale: number): string {
  if (!(scale > 0)) {
    return '（未取到）';
  }
  if (Math.abs(scale - Math.round(scale)) < 0.0005) {
    return Math.round(scale).toString();
  }
  return scale.toFixed(3);
}

export class DeviceProfile {
  private static inst: DeviceProfile | null = null;
  private inited: boolean = false;
  private dataValue: DeviceProfileData | null = null;
  private foldListener: (status: display.FoldStatus) => void = (status: display.FoldStatus) => {
    this.applyFoldStatus(status as number);
  };
  private listening: boolean = false;
  private onChange: (() => void) | null = null;

  static get(): DeviceProfile {
    if (!DeviceProfile.inst) {
      DeviceProfile.inst = new DeviceProfile();
    }
    return DeviceProfile.inst;
  }

  // 折叠状态变化时通知 UI 重排(幂等: 只注册一次); 传 null 可以在页面销毁时清掉
  setOnChange(cb: (() => void) | null): void {
    this.onChange = cb;
  }

  // 读一次设备身份(带缓存), 并挂上折叠状态监听
  init(): void {
    if (!this.inited) {
      this.inited = true;
      this.buildIdentity();
      this.watchFold();
    }
    this.refreshFoldState();
  }

  private buildIdentity(): void {
    let brand: string = '';
    let model: string = '';
    let dtype: string = 'phone';
    let osFull: string = '';
    let api: number = 0;
    try {
      brand = deviceInfo.brand ?? '';
      model = deviceInfo.productModel ?? '';
      dtype = deviceInfo.deviceType ?? 'phone';
      osFull = deviceInfo.osFullName ?? '';
      api = deviceInfo.sdkApiVersion ?? 0;
    } catch (e) {
      console.warn('[AB] deviceInfo read failed');
    }
    // 硬件字段逐项读取: hardwareModel 在鸿蒙上常常就是 SoC/硬件型号, 但不保证有值 ——
    // 所以这里一项一项读、一项一项记, 读到什么写什么, 取不到就写清"取不到(原因)"。
    // 目的: 以后判断机型/芯片时用的是设备自报的事实, 而不是任何外部资料。
    const hwFieldsValue: HwField[] = collectDeviceInfoFields();
    // productModelAlias 是 API 14 才有的字段: 老设备上可能取不到, 单独兜一层
    let alias: string = '';
    try {
      alias = deviceInfo.productModelAlias ?? '';
    } catch (e) {
      alias = '';
    }
    let market: string = '';
    try {
      market = deviceInfo.marketName ?? '';
    } catch (e) {
      market = '';
    }

    // ---- 传播名: A 系统别名 -> B 系统市场名 -> C1 完整型号码精确 -> C2 唯一前缀 -> D 型号码 ----
    // 注意: 有的 ROM 会把"传播名"直接填成型号码 —— 那种值等于没给, 只会挡住后面的内置表,
    // 所以这里额外要求"与型号码不同"才算命中(大小写不敏感)。
    const modelUpper: string = model.toUpperCase();
    const aliasUsable: boolean = alias.length > 0 && alias.toUpperCase() !== modelUpper;
    const marketUsable: boolean = market.length > 0 && market.toUpperCase() !== modelUpper;
    const skippedAlias: boolean = alias.length > 0 && !aliasUsable;
    const skippedMarket: boolean = market.length > 0 && !marketUsable;

    // 内置表: C1 完整型号码精确匹配(含 variants 备用型号码)
    //      -> C1b 三星去后缀精确匹配(只对长度>=8 且 SM- 开头的值)
    //      -> C2 前缀唯一匹配
    //      -> 前缀冲突(D 级 + 注明)
    const exact: string | undefined = MODEL_ALIAS[model];
    let tableName: string = exact !== undefined ? exact : '';
    let tableKey: string = tableName.length > 0 ? model : '';
    let tableLevel: string = tableName.length > 0 ? NAME_SRC_TABLE : '';
    let matchedByPrefix: boolean = false;
    let matchedByPostfix: boolean = false;
    let conflicted: boolean = false;
    if (tableName.length === 0) {
      // 三星: SM-S948BZVCXME -> SM-S948B(去尾部颜色/容量后缀)
      const base: string = lookupModelWithPostfix(model);
      if (base.length > 0) {
        tableKey = base;
        tableName = MODEL_ALIAS[base];
        tableLevel = NAME_SRC_POSTFIX;
        matchedByPostfix = true;
      } else {
        const byPrefix: string = lookupByPrefix(model);
        if (byPrefix.length > 0) {
          tableName = byPrefix;
          tableLevel = NAME_SRC_PREFIX;
          matchedByPrefix = true;
        } else if (isConflictedModel(model)) {
          conflicted = true;
        }
      }
    }
    // 规格用"实际命中的那个键"去查(去后缀命中时用去掉后缀的短键)
    const specKey: string = tableKey.length > 0 ? tableKey : model;
    const tableSpec: DeviceSpec | null = tableName.length > 0 ? lookupSpec(specKey) : null;

    let name: string = '';
    let src: string = NAME_SRC_MODEL;
    let detail: string = '';
    if (aliasUsable) {
      name = alias;
      src = NAME_SRC_ALIAS;
      detail = 'deviceInfo.productModelAlias 直接命中（系统给的最准, 不查内置表）';
    } else if (marketUsable) {
      name = market;
      src = NAME_SRC_MARKET;
      detail = 'productModelAlias ' + (skippedAlias ? '返回的是型号码, ' : '为空, ') + '回退到 marketName';
    } else if (tableName.length > 0) {
      name = tableName;
      src = tableLevel;
      if (matchedByPostfix) {
        detail = '型号码 ' + model + ' 是三星带后缀形态（去掉尾部颜色/容量后缀 → ' + tableKey +
          '）, 仅对长度 >= ' + SAMSUNG_POSTFIX_MIN_LEN.toString() + ' 且以 ' + SAMSUNG_PREFIX +
          ' 开头的值启用（表内 ' + TABLE_ENTRY_COUNT.toString() + ' 条）';
      } else if (matchedByPrefix) {
        detail = '型号码 ' + model + ' 未精确命中, 但前缀 ' + modelPrefixOf(model) +
          '- 在内置表内唯一（' + TABLE_ENTRY_COUNT.toString() + ' 条）→ 归到该传播名';
      } else {
        detail = '内置表完整型号码精确命中（表内 ' + TABLE_ENTRY_COUNT.toString() +
          ' 条, 精确键含 variants 备用型号码）';
      }
    } else {
      name = model.length > 0 ? model : '未知设备';
      src = NAME_SRC_MODEL;
      if (conflicted) {
        detail = NAME_SRC_PREFIX_CONFLICT + '：前缀 ' + modelPrefixOf(model) +
          '- 在内置表里对应多个传播名（本表共 19 组冲突前缀, 含 SM-）→ 不猜名字, 直接显示型号码';
      } else if (skippedAlias || skippedMarket) {
        detail = '系统接口给的值等于型号码(等于没给), 内置表既没有该型号码精确条目、也没有唯一前缀 → 直接显示型号码';
      } else {
        detail = '系统接口与内置表都没有该型号码（精确与前缀均未命中）→ 直接显示型号码';
      }
    }

    // 关于页"匹配方式"那一行: 实际命中的那一级
    let method: string = src;
    if (tableSpec !== null) {
      method += ' · 表内形态 ' + formFactorText(tableSpec);
    }
    if (conflicted) {
      method += ' · ' + NAME_SRC_PREFIX_CONFLICT;
    }
    const specScreen: string = screenSpecText(tableSpec);
    const specHost: string = tableSpec !== null ? hostOf(tableSpec.source) : '';

    let sw: number = 0;
    let sh: number = 0;
    let dpi: number = 0;
    let dp: number = 0;
    let screenNote: string = '';
    try {
      const d = display.getDefaultDisplaySync();
      sw = d.width;
      sh = d.height;
      dpi = d.densityDPI;
      // densityPixels 就是 px→vp 的除数(vp = px / densityPixels)。缺了它就只能退回
      // densityDPI/160 估算 —— 两者不一致时按密度字段记录差异, 不"取其中一个凑数"。
      dp = d.densityPixels;
    } catch (e) {
      screenNote = '屏幕参数读取失败';
    }

    this.dataValue = {
      brand: brand,
      modelCode: model,
      marketingName: name,
      nameSource: src,
      nameSourceDetail: detail,
      matchMethod: method,
      tableHit: tableName.length > 0,
      tableName: tableName,
      tableFormFactor: tableSpec !== null ? tableSpec.formFactor : '',
      tableScreenText: specScreen,
      tableSpecSource: tableSpec !== null ? tableSpec.source : '',
      tableSpecHost: specHost,
      tableNote: tableSpec !== null ? tableSpec.note : '',
      lastCpuInfo: this.dataValue !== null ? this.dataValue.lastCpuInfo : '',
      cpuTopology: this.dataValue !== null ? this.dataValue.cpuTopology : '',
      // 内核允许的核集合三行同样沿用上一次的值(真正的重读由 Index 的 refreshCpuAffinity 负责)
      cpuAffinityLine: this.dataValue !== null ? this.dataValue.cpuAffinityLine : '',
      cpuAffinityFull: this.dataValue !== null ? this.dataValue.cpuAffinityFull : '',
      cpuFreqTiers: this.dataValue !== null ? this.dataValue.cpuFreqTiers : '',
      // 芯片判读三段同样"重新识别时保留上一次的值"(真正的重读由 Index 的 refreshChipIdentity 负责)
      chipMeasured: this.dataValue !== null ? this.dataValue.chipMeasured : '',
      chipPerCore: this.dataValue !== null ? this.dataValue.chipPerCore : '',
      chipInfer: this.dataValue !== null ? this.dataValue.chipInfer : '',
      hwFields: hwFieldsValue,
      lastCpuAt: this.dataValue !== null ? this.dataValue.lastCpuAt : 0,
      deviceType: dtype,
      osFullName: osFull,
      apiVersion: api,
      foldable: false,
      foldApiOk: false,
      foldApiNote: screenNote,
      foldStatusText: FOLD_UNKNOWN,
      foldKey: FS_UNKNOWN,
      expanded: false,
      screenWidthPx: sw,
      screenHeightPx: sh,
      densityDpi: dpi,
      densityPixels: dp > 0 ? dp : (this.dataValue !== null ? this.dataValue.densityPixels : 0),
      // 窗口尺寸沿用上一次的值: 「重新识别」不应该把已经取到的窗口尺寸清成 0,
      // 否则界面会先跳回兜底布局、再等下一次 windowSizeChange 才恢复(那就是可见的抖动)。
      windowWidthVp: this.dataValue !== null ? this.dataValue.windowWidthVp : 0,
      windowHeightVp: this.dataValue !== null ? this.dataValue.windowHeightVp : 0,
      windowAspect: this.dataValue !== null ? this.dataValue.windowAspect : 0,
      // 窗口尺寸的来源与"取不到的原因"同样沿用上一次的值(重新识别不该把它清空)
      windowSource: this.dataValue !== null ? this.dataValue.windowSource : '',
      windowUnitBasis: this.dataValue !== null ? this.dataValue.windowUnitBasis : '',
      windowProbe: this.dataValue !== null ? this.dataValue.windowProbe : WINDOW_PROBE_INIT,
      bpWide: this.dataValue !== null ? this.dataValue.bpWide : false,
      bpName: this.dataValue !== null ? this.dataValue.bpName : 'sm',
      shapeId: '', shapeIndex: 0, shapeName: '', shapeBasis: '', shapeNote: '',
      shapeDegrade: '', shapeSig: '', layoutSelfCheck: '',
      plan: planFallback()
    };
  }

  // 折叠屏: 接口不可用就降级(不抛)
  private watchFold(): void {
    if (this.listening) {
      return;
    }
    try {
      display.on('foldStatusChange', this.foldListener);
      this.listening = true;
    } catch (e) {
      this.listening = false;
      console.warn('[AB] foldStatusChange listener unavailable');
    }
  }

  unwatchFold(): void {
    if (!this.listening) {
      return;
    }
    try {
      display.off('foldStatusChange', this.foldListener);
    } catch (e) {
      console.warn('[AB] foldStatusChange off failed');
    }
    this.listening = false;
  }

  private applyFoldStatus(status: number): void {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null) {
      return;
    }
    d.foldStatusText = this.foldTextOf(d, status);
    d.foldKey = this.foldKeyOf(d, status);
    d.expanded = status === display.FoldStatus.FOLD_STATUS_EXPANDED;
    this.recomputePlan();
    const cb: (() => void) | null = this.onChange;
    if (cb !== null) {
      cb();
    }
  }

  private foldTextOf(d: DeviceProfileData, status: number): string {
    if (!d.foldApiOk) {
      return FOLD_UNKNOWN;
    }
    if (!d.foldable) {
      return FOLD_NONE;
    }
    if (status === display.FoldStatus.FOLD_STATUS_EXPANDED) {
      return FOLD_EXPANDED;
    }
    if (status === display.FoldStatus.FOLD_STATUS_FOLDED) {
      return FOLD_FOLDED;
    }
    if (status === display.FoldStatus.FOLD_STATUS_HALF_FOLDED) {
      return FOLD_HALF;
    }
    return FOLD_UNKNOWN;
  }

  // 与上面同源的稳定英文键(给纯函数 LayoutPlan.ets 用; 界面显示仍用中文文案)。
  // 折叠接口不可用时一律 FS_UNKNOWN —— 不猜。
  private foldKeyOf(d: DeviceProfileData, status: number): string {
    if (!d.foldApiOk || !d.foldable) {
      return FS_UNKNOWN;
    }
    if (status === display.FoldStatus.FOLD_STATUS_EXPANDED) {
      return FS_EXPANDED;
    }
    if (status === display.FoldStatus.FOLD_STATUS_FOLDED) {
      return FS_FOLDED;
    }
    if (status === display.FoldStatus.FOLD_STATUS_HALF_FOLDED) {
      return FS_HALF;
    }
    return FS_UNKNOWN;
  }

  // 读一次当前折叠状态(启动 / 切页时调用)
  refreshFoldState(): void {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null) {
      return;
    }
    let ok: boolean = false;
    let foldable: boolean = false;
    let status: number = -1;
    let note: string = d.foldApiNote;
    try {
      foldable = display.isFoldable();
      status = display.getFoldStatus() as number;
      ok = true;
    } catch (e) {
      ok = false;
      note = (note.length > 0 ? (note + '；') : '') +
        '折叠屏接口不可用（display.isFoldable / getFoldStatus 抛异常）→ 先按内置表形态兜底, 再回退断点布局';
      console.warn('[AB] foldable APIs unavailable, fallback to breakpoints');
    }
    d.foldApiOk = ok;
    d.foldable = foldable;
    d.foldApiNote = note;
    d.foldStatusText = ok ? this.foldTextOf(d, status) : FOLD_UNKNOWN;
    d.foldKey = ok ? this.foldKeyOf(d, status) : FS_UNKNOWN;
    d.expanded = ok && foldable && status === display.FoldStatus.FOLD_STATUS_EXPANDED;
    this.recomputePlan();
  }

  // 重新识别(关于页「重新识别」按钮): 重读设备信息 + 折叠状态 + 屏幕参数
  refreshAll(): void {
    this.buildIdentity();
    this.refreshFoldState();
  }

  // 窗口尺寸 / 断点变化时调用。只接受真实窗口尺寸(不再用断点的假宽度覆盖),
  // 否则"mediaquery 回调"与"windowSizeChange 回调"会互相覆盖导致界面来回跳。
  applyWindow(widthVp: number, heightVp: number, bpName: string): void {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null) {
      return;
    }
    if (widthVp > 0) {
      d.windowWidthVp = Math.round(widthVp);
    }
    if (heightVp > 0) {
      d.windowHeightVp = Math.round(heightVp);
    }
    d.bpName = bpName;
    d.bpWide = bpName === 'md' || bpName === 'lg';
    this.recomputePlan();
  }

  // 把"窗口尺寸是怎么来的 / 为什么取不到"记下来(写明, 供 runlog 的设备行与关于页显示)。
  //   note = 一句中文说明; source = 'windowSizeChange' / '' (空 = 这次没拿到尺寸)
  //   unitBasis = 单位换算依据(非空时写进画像, 设备行会带上; 失败路径传空串 —— 失败原因写在 note 里)
  setWindowProbe(note: string, source: string, unitBasis: string): void {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null || note.length === 0) {
      return;
    }
    d.windowProbe = note;
    if (source.length > 0) {
      d.windowSource = source;
    }
    if (unitBasis.length > 0) {
      d.windowUnitBasis = unitBasis;
    }
    this.recomputePlan();   // 让「布局自检」的降级说明跟着更新(输入没变时它一个字段都不动)
  }

  // px → vp 的换算(两个窗口尺寸来源共用这一个口子)。返回换算结果, 失败时 why 里写明原因。
  // 只换算, 不写画像 —— 调用方(Index 的窗口探测)要先拿这个结果去做单位口径的交叉验证
  // (原始值的单位若不是 px, 除以密度就会小好几倍), 验证通过再调 commitWindowVp 落库。
  //
  // 单位口径(官方文档, 2026-10-08 核实)
  //   · window.Size(windowSizeChange 回调给的)是 **px** —— 官方 API 参考:
  //     "Size 窗口大小, 单位为px"(API 23 起另给了 SizeInVP, 正说明 Size 本身是 px)。
  //     以前 Index.onWinSize 直接把它当 vp 用, 于是真机 1828x2584px 被写成 1828x2584vp,
  //     形态判定(纯看宽高比)跟着从"横向内屏"错成"竖直内屏"。
  //   · WindowProperties.windowRect(Rect)也是 **px**。
  //   · 两个来源都必须先除以屏幕密度(densityPixels; 读不到时退回 densityDPI/160)才是 vp。
  //   · 密度也读不到时: 返回失败, 不把 px 当 vp 继续往下走(调用方会写明原因并跳过形态判定)。
  applyWindowPx(widthPx: number, heightPx: number): WindowSizeVp {
    const d: DeviceProfileData | null = this.dataValue;
    const pxW: number = Math.round(widthPx);
    const pxH: number = Math.round(heightPx);
    if (d === null) {
      return {
        ok: false, widthVp: 0, heightVp: 0, widthPx: pxW, heightPx: pxH,
        scale: 0, scaleSource: '', basis: '',
        why: '设备画像还没建立'
      } as WindowSizeVp;
    }
    if (!(widthPx > 0) || !(heightPx > 0)) {
      return {
        ok: false, widthVp: 0, heightVp: 0, widthPx: pxW, heightPx: pxH,
        scale: 0, scaleSource: '', basis: '',
        why: '拿到的是 ' + pxW.toString() + 'x' + pxH.toString() +
          'px(窗口还没完成布局 / 接口返回 0x0)'
      } as WindowSizeVp;
    }
    let scale: number = 0;
    let scaleSrc: string = '';
    try {
      scale = display.getDefaultDisplaySync().densityPixels;
      if (scale > 0) {
        scaleSrc = 'densityPixels';
      }
    } catch (e) {
      scale = 0;
    }
    if (!(scale > 0)) {
      scale = d.densityDpi > 0 ? (d.densityDpi / 160) : 0;   // 兜底: vp 的基准是 160dpi
      if (scale > 0) {
        scaleSrc = 'densityDPI/160';
      }
    }
    if (!(scale > 0)) {
      return {
        ok: false, widthVp: 0, heightVp: 0, widthPx: pxW, heightPx: pxH,
        scale: 0, scaleSource: '', basis: '',
        why: '拿到了 ' + pxW.toString() + 'x' + pxH.toString() +
          'px 尺寸但读不到屏幕密度(densityPixels 与 densityDPI 都不可用), 无法换算成 vp'
      } as WindowSizeVp;
    }
    const wVp: number = Math.round(widthPx / scale);
    const hVp: number = Math.round(heightPx / scale);
    const basis: string = pxW.toString() + 'x' + pxH.toString() + 'px ÷ 密度 ' +
      scaleText(scale) + '(' + scaleSrc + ') = ' + wVp.toString() + 'x' + hVp.toString() + 'vp';
    if (!(wVp > 0) || !(hVp > 0)) {
      return {
        ok: false, widthVp: 0, heightVp: 0, widthPx: pxW, heightPx: pxH,
        scale: scale, scaleSource: scaleSrc, basis: basis,
        why: 'px 尺寸换算成 vp 后为 ' + wVp.toString() + 'x' + hVp.toString() + '(' + basis + ')'
      } as WindowSizeVp;
    }
    return {
      ok: true, widthVp: wVp, heightVp: hVp, why: '',
      widthPx: pxW, heightPx: pxH, scale: scale, scaleSource: scaleSrc, basis: basis
    } as WindowSizeVp;
  }

  // 把已经验证过的窗口 vp 尺寸写进画像(写窗口宽高 + 来源, 并重算形态/布局计划)。
  // 只有"单位口径交叉验证通过"的尺寸才允许走到这里 —— 否则一个错口径的宽度会直接改掉
  // 形态判定与布局(那比日志里少一个数字严重得多)。
  commitWindowVp(widthVp: number, heightVp: number, source: string, unitBasis: string): void {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null || !(widthVp > 0) || !(heightVp > 0)) {
      return;
    }
    d.windowWidthVp = Math.round(widthVp);
    d.windowHeightVp = Math.round(heightVp);
    if (source.length > 0) {
      d.windowSource = source;
    }
    if (unitBasis.length > 0) {
      d.windowUnitBasis = unitBasis;
    }
    this.recomputePlan();
  }

  // 系统 deviceType(deviceInfo.deviceType)是否表示大屏(平板 / 2in1 / TV)。系统给的就以系统为准。
  private typeBigScreen(): boolean {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null) {
      return false;
    }
    const t: string = d.deviceType.toLowerCase();
    return t === 'tablet' || t === '2in1' || t === 'tv';
  }

  // 内置表形态兜底(只在系统 API / deviceType 给不出答案时用; 返回的文案里注明"非系统 API")
  //   foldable: display.isFoldable 抛异常 / 不可用时
  //   tablet  : deviceInfo.deviceType 取不到或不是大屏取值时
  private tableFoldableFallback(): boolean {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null || d.foldApiOk) {
      return false;
    }
    return d.tableFormFactor === 'foldable' || d.tableFormFactor === 'tablet';
  }

  private tableTabletFallback(): boolean {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null) {
      return false;
    }
    return !this.typeBigScreen() && d.tableFormFactor === 'tablet';
  }

  // ---- 形态判定 + 布局计划: 全部交给纯函数 ../common/LayoutPlan.ets ----
  //
  //  输入 = 实时窗口 vp 宽高 + 宽高比(由宽高算出) + 折叠状态 + 设备类型 + 内置表形态;
  //  输出 = 一整份 LayoutPlan(形态编号 / 判定依据 / 列数 / 限宽 / 按钮尺寸 / 字号缩放 / 降级原因)。
  //
  //  为什么要这样: PC 上窗口可以自由拖大拖小、折叠屏随时展开折叠、分屏随时改宽高比 ——
  //  "这台机器是什么型号"不足以决定布局。机型只作为加权修正(deviceType 决定平板/电脑档),
  //  真正的主判据是实时窗口尺寸与宽高比。
  //
  //  幂等 + 不抖动:
  //   a) 纯函数: 同样的输入必然得到同样的输出;
  //   b) 这里再做一层签名比较 —— 输入(形态 + 宽高 + 折叠键 + 断点)没变就一个字段都不动,
  //      因此重复调用不会产生任何 UI 刷新, 也就不会出现"重排一帧又变回去"的抖动;
  //   c) 形态切换本身带 ±8vp 的边界死区(在纯函数里, 见 resolveShapeId)。
  private recomputePlan(): void {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null) {
      return;
    }
    const inp: ShapeInput = {
      widthVp: d.windowWidthVp,
      heightVp: d.windowHeightVp,
      foldState: d.foldKey,
      foldApiOk: d.foldApiOk,
      deviceType: d.deviceType,
      tableFormFactor: d.tableFormFactor
    };
    // 上一次的形态只在"窗口尺寸已经取到"时才参与抖动抑制(重识别后不沿用旧形态)
    const prevId: string = d.windowWidthVp > 0 ? d.plan.id : '';
    const p: LayoutPlan = resolvePlan(inp, prevId);
    const sig: string = p.id + '|' + inp.widthVp.toString() + 'x' + inp.heightVp.toString() +
      '|' + inp.foldState + '|' + (inp.foldApiOk ? '1' : '0') + '|' + inp.deviceType +
      '|' + inp.tableFormFactor + '|' + d.bpName;
    if (sig === d.shapeSig) {
      return;
    }
    d.shapeSig = sig;
    d.plan = p;
    d.shapeId = p.id;
    d.shapeIndex = p.index;
    d.shapeName = p.name;
    d.shapeBasis = p.basis;
    d.shapeNote = p.note;
    d.shapeDegrade = p.degrade;
    d.windowAspect = aspectOf(inp.widthVp, inp.heightVp);
    d.layoutSelfCheck = this.selfCheckText(inp, p);
  }

  // 「布局自检」全文(关于页显示 + 复制按钮共用同一份文本)。
  // 里面既有判定结果(形态编号 / 名称 / 列数 / 限宽 / 按钮高 / 字号缩放),
  // 也有判定依据(是窗口尺寸还是折叠状态还是设备类型), 还有降级原因。
  private selfCheckText(inp: ShapeInput, p: LayoutPlan): string {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null) {
      return planLines(inp, p, '');
    }
    const bpRange: string = d.bpName === 'sm' ? '< 600vp'
      : (d.bpName === 'md' ? '600 ~ 840vp' : '>= 840vp');
    let extra: string = '';
    extra += '断点（mediaquery 三档）：' + d.bpName + '（' + bpRange +
      '）· 只作旁证，不参与形态判定\n';
    extra += '设备类型：deviceInfo.deviceType = ' +
      (d.deviceType.length > 0 ? d.deviceType : '（空）') + ' · 内置表形态 = ' +
      (d.tableFormFactor.length > 0 ? d.tableFormFactor : '（未命中内置表）') + '\n';
    extra += '折叠屏接口：' + (d.foldApiOk ? '可用' : '不可用') + ' · 折叠状态 = ' +
      d.foldStatusText + '（' + d.foldKey + '）\n';
    if (d.foldApiNote.length > 0) {
      extra += '折叠屏接口备注：' + d.foldApiNote + '\n';
    }
    extra += '屏幕：' + (d.screenWidthPx > 0
      ? (d.screenWidthPx.toString() + ' x ' + d.screenHeightPx.toString() + ' px · ' +
        d.densityDpi.toString() + ' dpi')
      : '（未取到）') +
      ' · px→vp 密度 ' + (d.densityPixels > 0
        ? (scaleText(d.densityPixels) + '（display.densityPixels）')
        : '（未取到：只能退回 dpi/160 或干脆不换算）') + '\n';
    // 窗口尺寸的来源与单位: 这两行是"这个 vp 是怎么来的"的凭据。真机上曾经把
    // windowSizeChange 的 px 直接当 vp 用(1828x2584px 写成 1828x2584vp), 只看结果永远
    // 看不出问题; 把来源、原始 px、密度、换算结果与验证结论全写出来才可复核。
    extra += '窗口尺寸来源：' + (d.windowSource.length > 0 ? d.windowSource : '（还没有任何来源给出尺寸）') + '\n';
    extra += '窗口单位换算依据：' + (d.windowUnitBasis.length > 0
      ? d.windowUnitBasis : '（无 —— 还没成功换算过：要么两个来源都没结果, 要么换算失败, 原因见下一行）') + '\n';
    extra += '窗口探测说明：' + d.windowProbe + '\n';
    extra += '设备：' + d.marketingName + ' · ' + (d.modelCode.length > 0 ? d.modelCode : '型号未知') + '\n';
    extra += '说明：布局只影响观感（列数 / 限宽 / 按钮与字号），不参与任何测试项的计算、顺序与计分。\n';
    return planLines(inp, p, extra);
  }

  data(): DeviceProfileData {
    if (!this.inited) {
      this.init();
    }
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null) {
      this.buildIdentity();
      this.recomputePlan();
    }
    const out: DeviceProfileData | null = this.dataValue;
    if (out === null) {
      // 理论上到不了这里(兜底, 避免任何 undefined 泄漏到 UI)
      const fallback: DeviceProfileData = {
        brand: '', modelCode: '', marketingName: '未知设备', nameSource: NAME_SRC_MODEL,
        nameSourceDetail: '设备信息读取失败', matchMethod: NAME_SRC_MODEL,
        tableHit: false, tableName: '', tableFormFactor: '', tableScreenText: '',
        tableSpecSource: '', tableSpecHost: '', tableNote: '',
        lastCpuInfo: '', lastCpuAt: 0, cpuTopology: '',
        cpuAffinityLine: '', cpuAffinityFull: '', cpuFreqTiers: '',
        chipMeasured: '', chipPerCore: '', chipInfer: '',
        hwFields: [],
        deviceType: 'phone', osFullName: '', apiVersion: 0,
        foldable: false, foldApiOk: false, foldApiNote: '设备信息读取失败',
        foldStatusText: FOLD_UNKNOWN, foldKey: FS_UNKNOWN, expanded: false,
        screenWidthPx: 0, screenHeightPx: 0, densityDpi: 0, densityPixels: 0,
        windowWidthVp: 0, windowHeightVp: 0, windowAspect: 0,
        windowSource: '', windowUnitBasis: '',
        windowProbe: '设备信息读取失败, 窗口尺寸未取到',
        bpWide: false, bpName: 'sm',
        shapeId: '', shapeIndex: 0, shapeName: '', shapeBasis: '', shapeNote: '',
        shapeDegrade: '设备信息读取失败', shapeSig: '', layoutSelfCheck: '',
        plan: planFallback()
      };
      return fallback;
    }
    return out;
  }

  // ---- 给 UI 用的快捷读法(全部有兜底, 不会抛) ----

  marketingName(): string {
    return this.data().marketingName;
  }

  // 是否采用"左操作 / 右结果"左右分栏(直接来自形态计划, 不再是"设备型号说了算")
  forceWide(): boolean {
    return this.data().plan.split;
  }

  forceNarrow(): boolean {
    return !this.data().plan.split;
  }

  columns(): number {
    return this.data().plan.columns;
  }

  contentMaxVp(): number {
    return this.data().plan.contentMaxVp;
  }

  // 开始按钮直径: 已经在纯函数里按"当前窗口宽 x 系数"算好并夹好了上下限
  heroDia(): number {
    return this.data().plan.heroDia;
  }

  buttonHeightVp(): number {
    return this.data().plan.btnH;
  }

  cardBasis(): string {
    return this.data().plan.cardBasis;
  }

  minCardW(): number {
    return this.data().plan.minCardVp;
  }

  plan(): LayoutPlan {
    return this.data().plan;
  }

  // 关于页「布局自检」全文(可复制)
  selfCheck(): string {
    return this.data().layoutSelfCheck;
  }

  // 「布局自检」用的几个即时字段(界面上一行一行显示)
  shapeId(): string {
    return this.data().shapeId;
  }

  shapeIndex(): number {
    return this.data().shapeIndex;
  }

  shapeName(): string {
    return this.data().shapeName;
  }

  shapeBasis(): string {
    return this.data().shapeBasis;
  }

  shapeNote(): string {
    return this.data().shapeNote;
  }

  shapeDegrade(): string {
    return this.data().shapeDegrade;
  }

  bpName(): string {
    return this.data().bpName;
  }

  // 设备卡第二行(型号码 · 系统 · API)
  modelLine(): string {
    const d: DeviceProfileData = this.data();
    let s: string = d.modelCode.length > 0 ? d.modelCode : '型号未知';
    if (d.osFullName.length > 0) {
      s += ' · ' + d.osFullName;
    }
    if (d.apiVersion > 0) {
      s += ' · API ' + d.apiVersion.toString();
    }
    return s;
  }

  // 折叠屏才显示的小标签(展开 / 折叠 / 半折叠); 非折叠屏返回空串
  foldTag(): string {
    const d: DeviceProfileData = this.data();
    if (!d.foldApiOk || !d.foldable) {
      return '';
    }
    return d.foldStatusText;
  }

  // 界面行: 表内屏幕参数(没有就说没有)
  tableScreenLine(): string {
    const d: DeviceProfileData = this.data();
    if (!d.tableHit) {
      return '未命中内置表';
    }
    return d.tableName + ' · ' + (d.tableScreenText.length > 0 ? d.tableScreenText : TABLE_NO_SCREEN);
  }

  // 界面行: 匹配方式(实际命中的那一级)
  matchMethodLine(): string {
    return this.data().matchMethod;
  }

  // 跑分侧每完成一项后写上来的"跑在哪个核"一行文本(旁路, 只影响关于页显示)
  setLastCpuInfo(text: string): void {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null || text.length === 0) {
      return;
    }
    d.lastCpuInfo = text;
    d.lastCpuAt = Date.now();
  }

  // 跑分侧把 CPU 拓扑那一行写上来(旁路, 只影响关于页显示与可复制文本)
  setCpuTopology(text: string): void {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null || text.length === 0) {
      return;
    }
    d.cpuTopology = text;
  }

  // 界面行: CPU 拓扑(native 灌进来的那一行; 还没读到就写"尚未读取")
  cpuTopologyLine(): string {
    const d: DeviceProfileData = this.data();
    return d.cpuTopology.length > 0 ? d.cpuTopology : '（尚未读取：native 上报后显示）';
  }


  // 跑分侧把「内核允许的核集合」三行写上来(旁路, 只影响关于页显示与可复制文本)。
  //   line = 一行结论(可用核集合 vs 快簇 vs 交集) —— 用户在两台机器上各看一次就能直接对比;
  //   full = 逐路径实测事实(Cpus_allowed / sched_getaffinity / cgroup / cpuset / Mems_allowed_list);
  //   tiers = 全机频率档(逐核口径, 读失败的核也带 errno)。
  setCpuAffinity(line: string, full: string, tiers: string): void {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null) {
      return;
    }
    if (line.length > 0) {
      d.cpuAffinityLine = line;
    }
    if (full.length > 0) {
      d.cpuAffinityFull = full;
    }
    if (tiers.length > 0) {
      d.cpuFreqTiers = tiers;
    }
  }

  // 界面行: 可用核集合 / 快簇 / 交集(一行结论)
  cpuAffinityLineText(): string {
    const d: DeviceProfileData = this.data();
    return d.cpuAffinityLine.length > 0 ? d.cpuAffinityLine
      : '（尚未读取：native 的 cpuAffinityProbe 上报后显示）';
  }

  // 界面行: 逐路径实测事实(可用核集合是怎么读到的、cpuset 在哪、读不到的是什么 errno)
  cpuAffinityFullText(): string {
    const d: DeviceProfileData = this.data();
    return d.cpuAffinityFull.length > 0 ? d.cpuAffinityFull
      : '（尚未读取：逐路径读数要等 native 探测）';
  }

  // 界面行: 全机频率档(逐核口径; 读失败的核也在里面, 带 errno)
  cpuFreqTiersText(): string {
    const d: DeviceProfileData = this.data();
    return d.cpuFreqTiers.length > 0 ? d.cpuFreqTiers
      : '（尚未读取：逐核频率档要等 native 探测）';
  }

  // 跑分侧把芯片判读的三段文本写上来(旁路, 只影响关于页显示、可复制文本与 runlog 设备行)
  setChipGuess(measured: string, infer: string, perCore: string): void {
    const d: DeviceProfileData | null = this.dataValue;
    if (d === null) {
      return;
    }
    if (measured.length > 0) {
      d.chipMeasured = measured;
    }
    if (infer.length > 0) {
      d.chipInfer = infer;
    }
    if (perCore.length > 0) {
      d.chipPerCore = perCore;
    }
  }

  // 界面行: 芯片与频率(本机实测; 设备自己说的话)
  chipMeasuredLine(): string {
    const d: DeviceProfileData = this.data();
    return d.chipMeasured.length > 0 ? d.chipMeasured
      : '（尚未读取：native 的 cpuIdentityProbe 上报后显示）';
  }

  // 界面行: 逐核 cpuinfo_max_freq 原始读数(读失败的核也在里面, 带 errno)
  chipPerCoreLine(): string {
    const d: DeviceProfileData = this.data();
    return d.chipPerCore.length > 0 ? d.chipPerCore
      : '（尚未读取：逐核原始读数要等 native 探测）';
  }

  // 界面行: 芯片判读(第三方资料推断; 明确带"非官方"字样)
  chipInferLine(): string {
    const d: DeviceProfileData = this.data();
    return d.chipInfer.length > 0 ? d.chipInfer
      : '（尚未读取：native 探测 + 第三方对照表判读后显示）';
  }

  // 界面行: deviceInfo 逐字段(一行一个; 读到什么写什么, 取不到写原因)
  hwFieldLines(): string {
    const d: DeviceProfileData = this.data();
    const out: string[] = [];
    for (const f of d.hwFields) {
      out.push(f.label + ' = ' + (f.value.length > 0 ? f.value : ('—— ' + f.why)));
    }
    return out.join('\n');
  }

  // 界面行: hardwareModel 单列一行(用户最关心: 它常常就是 SoC/硬件型号)
  hardwareModelLine(): string {
    const d: DeviceProfileData = this.data();
    for (const f of d.hwFields) {
      if (f.label === 'hardwareModel') {
        if (f.value.length > 0) {
          return f.value;
        }
        return '取不到（' + f.why + '）';
      }
    }
    return '取不到（未读取）';
  }

  // 界面行: 当前绑核状态(最近一次跑分的 cpuInfo; 没跑过就是空)
  cpuInfoLine(): string {
    const d: DeviceProfileData = this.data();
    if (d.lastCpuInfo.length === 0) {
      return '（还没跑分：跑完 CS1 单核 / 多核后这里会显示"跑在哪个核 / 哪个簇"）';
    }
    return d.lastCpuInfo;
  }

  // 界面行: 表内来源(只显示域名; 完整 URL 在可复制文本里)
  tableSourceLine(): string {
    const d: DeviceProfileData = this.data();
    if (!d.tableHit) {
      return '（未命中内置表）';
    }
    return d.tableSpecHost.length > 0 ? d.tableSpecHost : '（表内无来源）';
  }

  // ---- 关于页: 可复制的设备画像全文 ----
  describe(): string {
    const d: DeviceProfileData = this.data();
    const p: LayoutPlan = d.plan;
    let s: string = '[Crush分 · 设备画像]\n';
    s += '传播名：' + d.marketingName + '\n';
    s += '解析来源：' + d.nameSource + (d.nameSourceDetail.length > 0 ? ('（' + d.nameSourceDetail + '）') : '') + '\n';
    s += '匹配方式：' + d.matchMethod + '\n';
    s += '型号码：' + (d.modelCode.length > 0 ? d.modelCode : '（未取到）') + '\n';
    s += '品牌：' + (d.brand.length > 0 ? d.brand : '（未取到）') + ' · 只按 deviceInfo.brand 判定（不用型号码前缀推断）\n';
    s += '设备类型：' + d.deviceType + '\n';
    s += '绑核状态（最近一次跑分）：' +
      (d.lastCpuInfo.length > 0 ? d.lastCpuInfo : '（还没跑分）') + '\n';
    // CPU 拓扑(逻辑核 / 物理核 / SMT): 由跑分侧灌进来(见 setCpuTopology), 不在这里读 native ——
    // 本文件刻意不 import native 模块(否则会多出一条 ArkTS 模块未验证的警告)。
    s += 'CPU 拓扑：' + (d.cpuTopology.length > 0 ? d.cpuTopology : '（尚未读取）') + '\n';
    // 内核允许的核集合(2026-10-05): 回答"为什么这台机器快/慢"的第一手事实 
    // 三行分开写, 顺序固定: ①一行结论(可用核集合 vs 快簇 vs 交集) ②逐路径实测事实
    // ③全机频率档(逐核原始读数)。用户会在手机和平板上各导出一次这段全文, 逐行对比。
    s += '允许核集合/快簇：' + (d.cpuAffinityLine.length > 0 ? d.cpuAffinityLine : '（尚未读取）') + '\n';
    s += '允许核集合逐路径：' + (d.cpuAffinityFull.length > 0 ? d.cpuAffinityFull : '（尚未读取）') + '\n';
    s += '全机频率档（逐核）：' + (d.cpuFreqTiers.length > 0 ? d.cpuFreqTiers : '（尚未读取）') + '\n';
    // 芯片判读: 本机实测 -> 逐核原始读数 -> 第三方推断, 顺序固定, 三段分开
    s += (d.chipMeasured.length > 0 ? d.chipMeasured : '芯片与频率（本机实测）：（尚未读取）') + '\n';
    s += '逐核 cpuinfo_max_freq 原始读数：' +
      (d.chipPerCore.length > 0 ? d.chipPerCore : '（尚未读取）') + '\n';
    s += (d.chipInfer.length > 0 ? d.chipInfer : '芯片判读（第三方资料推断，非官方）：（尚未读取）') + '\n';
    if (d.tableHit) {
      s += '内置表：命中 ' + d.tableName + ' · 形态 ' +
        (d.tableFormFactor.length > 0 ? d.tableFormFactor : '（表内无）') + '（表内共 ' +
        TABLE_ENTRY_COUNT.toString() + ' 条）\n';
      s += '内置表屏幕参数：' + (d.tableScreenText.length > 0 ? d.tableScreenText : TABLE_NO_SCREEN) + '\n';
      s += '内置表来源：' + (d.tableSpecSource.length > 0 ? d.tableSpecSource : '（表内无来源）') + '\n';
      if (d.tableNote.length > 0) {
        s += '内置表备注：' + d.tableNote + '\n';
      }
    } else {
      s += '内置表：未命中（表内共 ' + TABLE_ENTRY_COUNT.toString() + ' 条, 无该型号码且无唯一前缀）\n';
      s += '内置表屏幕参数：' + TABLE_NO_SCREEN + '\n';
    }
    const foldTail: string = !d.foldApiOk
      ? ('（' + d.foldApiNote + '）' + (this.tableFoldableFallback() ? (' · 内置表标记为' +
        (d.tableFormFactor === 'tablet' ? '平板' : '折叠屏') + FF_FALLBACK_TAG) : ''))
      : '';
    s += '是否折叠屏：' + (!d.foldApiOk ? ('未知' + foldTail) : (d.foldable ? '是' : '否')) + '\n';
    if (this.tableTabletFallback()) {
      s += '平板识别：系统 deviceType 未按大屏上报, 按内置表兜底为平板' + FF_FALLBACK_TAG + '\n';
    }
    s += '当前形态：' + d.foldStatusText + (!d.foldApiOk ? '（折叠屏接口不可用）' : '') + '\n';
    s += '屏幕：' + (d.screenWidthPx > 0 ? (d.screenWidthPx.toString() + ' x ' + d.screenHeightPx.toString() + ' px') : '（未取到）') +
      ' · 密度 ' + (d.densityDpi > 0 ? (d.densityDpi.toString() + ' dpi') : '（未取到）') +
      ' · px→vp 密度 ' + (d.densityPixels > 0 ? scaleText(d.densityPixels) : '（未取到）') + '\n';
    s += '窗口：' + (d.windowWidthVp > 0 ? (d.windowWidthVp.toString() + ' x ' + d.windowHeightVp.toString() + ' vp') : '（未取到）') +
      ' · 宽高比 ' + (d.windowAspect > 0 ? d.windowAspect.toFixed(3) : '（未取到）') +
      ' · 断点 ' + d.bpName + (d.bpWide ? '（md/lg）' : '（sm）') + '\n';
    // 来源与单位(见 selfCheckText 里的同名字段): 设备行/关于页都要能回答"这个 vp 从哪来、按哪个密度算的"
    s += '窗口来源：' + (d.windowSource.length > 0 ? d.windowSource : '（还没有任何来源给出尺寸）') + '\n';
    s += '窗口单位换算依据：' + (d.windowUnitBasis.length > 0 ? d.windowUnitBasis :
      '（无 —— 未成功换算; 原因见下面一行）') + '\n';
    s += '窗口探测说明：' + d.windowProbe + '\n';
    s += '命中形态：' + d.shapeId + ' · 编号 ' + d.shapeIndex.toString() + ' · ' + d.shapeName + '\n';
    s += '采用布局：' + (p.split
      ? ('左操作 / 右结果 两栏（12 栅格 ' + p.leftSpan.toString() + ':' + p.rightSpan.toString() + '）')
      : '单栏（不分栏）') +
      ' · 结果卡 ' + p.columns.toString() + ' 列 · 内容最大宽度 ' +
      (p.contentMaxVp > 0 ? (p.contentMaxVp.toString() + ' vp') : '不限') +
      ' · 卡片基准 ' + p.cardBasis + '（最小 ' +
      (p.minCardVp > 0 ? (p.minCardVp.toString() + ' vp') : '不设下限') + '）' +
      ' · 开始按钮 ' + p.heroDia.toString() + 'vp · 按钮高 ' + p.btnH.toString() + 'vp' +
      ' · 字号缩放 x' + p.fontScale.toFixed(2) +
      ' · 间距 ' + p.gutter.toString() + 'vp · 内边距 ' + p.pad.toString() +
      'vp · 圆角 ' + p.radius.toString() + 'vp\n';
    s += '布局依据：' + p.basis + '\n';
    s += '形态说明：' + p.note + '\n';
    s += '降级说明：' + (p.degrade.length > 0 ? p.degrade : '无（所有判定接口都可用）') + '\n';
    s += '布局自检（可复制全文）：\n' + d.layoutSelfCheck;
    s += '系统：' + (d.osFullName.length > 0 ? d.osFullName : '未知') + ' · API ' + d.apiVersion.toString() + '\n';
    // deviceInfo 逐字段: 读到什么写什么, 取不到就写清原因(不用第三方资料替设备说话)
    s += 'deviceInfo 逐字段（读到什么写什么；hardwareModel 若是具体型号, 那就是这台机器的权威答案）：\n';
    for (const f of d.hwFields) {
      s += '  ' + f.label + ' = ' + (f.value.length > 0 ? f.value : ('—— ' + f.why)) + '\n';
    }
    s += '说明：设备画像只影响观感（列数 / 宽度 / 按钮尺寸），不参与任何测试项的计算、顺序与计分。';
    return s;
  }
}
