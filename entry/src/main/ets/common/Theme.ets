// ---------------------------------------------------------------------------
// 设计令牌(单一来源) —— 颜色 / 间距 / 圆角 / 字号 / 阴影 / 渐变
//
//  规矩:
//   1) 界面里不再写十六进制色值(历史遗留的那些逐步收敛到这里), 新写的必须用令牌;
//   2) 这里的每一项都是常量或纯函数, 没有任何副作用、不持有状态、不监听任何东西 ——
//      它不会在跑分期间产生任何 CPU / GPU 开销;
//   3) 界面本身(跑分期间也一直在树上的那一层)只用零成本手段: 纯色分层、1vp 边框高光、
//      静态线性渐变(只画一次, 不重绘) —— 不用阴影、不用模糊、不用持续动画。
//      ★ 2026-10 起多了一层"美化层"(沉浸光感 / 粒子消散), 它确实会用阴影与径向渐变,
//        所以它有两条硬约束, 写在 pages/Index.ets 的 fxOn() 上:
//          ① 唯一判据是 fxOn(), 跑分进行中恒为 false;
//          ② 关掉时不是"不播放", 而是**元件根本不存在 / 属性退化成没有效果**
//             —— 见 fxAura()(关掉时返回本文件下面的 SHADOW_NONE)、fxSheen()、fxHalo()。
// ---------------------------------------------------------------------------

// ---- 颜色 ----
export class C {
  // 背景层次(由深到浅: 页面底 -> 顶部栏 -> 卡片 -> 高亮卡)
  static readonly bg: string = '#070B14';
  static readonly bgTop: string = '#0A1120';
  static readonly tabBar: string = '#0B1220';
  static readonly surface: string = '#0E1524';
  static readonly inset: string = '#101A2C';       // 嵌在卡片内的文本块
  static readonly card: string = '#131C30';
  static readonly cardHi: string = '#1A2540';
  //  卡片 1vp 边框高光(替代阴影): 2026-10 起向品牌色偏一点点(极细的一线暖光),
  //  仍然只是一条静态描边, 不是阴影、不产生离屏层。
  static readonly cardEdge: string = '#252A52';
  static readonly cardEdgeHi: string = '#332F70';
  static readonly chip: string = '#16233A';        // 标签 / 次按钮底色
  static readonly chipViolet: string = '#1B1B3A';
  static readonly chipWarn: string = '#3A2A18';
  static readonly chipWarnAlt: string = '#2A2015';
  static readonly chipDanger: string = '#3A1A24';
  static readonly chipDangerAlt: string = '#2A1520';
  static readonly chipGood: string = '#12301F';
  static readonly heroInner: string = '#101E36';   // 开始按钮内圈
  static readonly heroOuter: string = '#0C1526';   // 开始按钮外圈

  // 主色 / 次色(渐变两端)
  static readonly accent: string = '#4EA8FF';
  static readonly accent2: string = '#7C5CFF';
  static readonly accentSoft: string = '#1E3A66';

  // 语义色
  static readonly good: string = '#2EE59D';
  static readonly warn: string = '#FFB020';
  static readonly danger: string = '#FF5470';
  static readonly track: string = '#16233A';       // 进度条 / 分数条底槽

  // 文字三级 + 辅助
  static readonly text: string = '#EAF2FF';
  static readonly subText: string = '#8AA0C0';
  static readonly dim: string = '#5A6E8C';
  static readonly faint: string = '#4A5B76';
  static readonly onAccent: string = '#06101F';    // 主色填充上的文字
  static readonly onWarn: string = '#0A1020';
  static readonly divider: string = '#1E2B45';

  static readonly transparent: string = '#00000000'; // 系统栏透明(沉浸式: 状态栏/导航栏露出页面底色)

  // ---- 品牌氛围色(2026-10: App 改用自己的深色画布, 不再跟随系统浅色主题) ----
  //  与 App 图标(红玻璃心 + 9)呼应: 深底上叠一层**很淡**的暗红 -> 深紫。
  //  三个停靠点都落在"已经很暗的底 + 一点色相"的区间里, 不是饱和红 ——
  //  深底上一点点色相就看得出来; 饱和红会在 OLED 上糊成一块, 还会吃掉卡片与文字的对比度。
  //  ★ 2026-10-10 加强: 上一版(#1A0A18 / #0E0A1E)在真机上被用户判为"几乎看不出来" ——
  //    那三个色本来就只比底色亮一点点, OLED 上等于没有。这一版把左上角的红提到"一眼能看出
  //    这不是一块纯色底"的量级, 但仍然限制在暗色区间(不饱和、不刺眼)。
  static readonly brandRed: string = '#4A1224';      // 左上: 暗红(明显可见, 但仍是暗色)
  static readonly brandViolet: string = '#2A0F2E';   // 中段: 红紫过渡
  static readonly brandDeep: string = '#141033';     // 靠右下: 深紫回落(终点直接用 bg, 与窗口底色接得上)
}

// ---- 间距刻度(4 的倍数; 形态计划里的 gutter / pad 也取自这个刻度) ----
export class Sp {
  static readonly x1: number = 4;
  static readonly x2: number = 8;
  static readonly x3: number = 12;
  static readonly x4: number = 16;
  static readonly x5: number = 24;
}

// ---- 圆角 ----
export class Rd {
  static readonly sm: number = 8;
  static readonly md: number = 12;
  static readonly lg: number = 16;
  static readonly pill: number = 999;
}

// ---- 字号刻度(实际字号 = 令牌 x 形态计划的 fontScale, 见 scaledFont()) ----
export class Ft {
  static readonly cap: number = 10;    // 脚注 / 原始读数
  static readonly small: number = 11;  // 说明
  static readonly body: number = 12;   // 正文
  static readonly sub: number = 13;    // 小标题 / 按钮
  static readonly title: number = 15;  // 卡片标题
  static readonly head: number = 20;   // 页面标题
  static readonly hero: number = 40;   // 上次成绩大字
  static readonly giant: number = 64;  // 总分大字(首页主数字的历史取值)
  // 结果页「第一层」的三个大数字(单核 / 多核 / GPU)的历史取值。
  //   2026-10 起结果页三处界面改用实测窗口算出来的字号(common/LayoutPlan.ets 的
  //    ResultDeco.giant / huge / keyNum / keyName / keyUnit), 因为"一屏一眼可见"要求
  //    字号随窗口宽高变(大屏更大、小屏更紧), 固定令牌做不到。这个令牌保留给
  //    "不随窗口变"的场合使用, 结果页不再引用它。
  static readonly huge: number = 56;
}

// ---- 结果页(分数界面)的间距 / 内边距 / 行高 ----
//   与其它间距令牌不同, 结果页这一套不是固定值: "总分 + 单核/多核/GPU + 各小节关键分
//    必须一屏一眼可见"这件事只能由实测窗口宽高决定(大屏左右并排 + 更大字号 + 更多列,
//    小屏紧凑 + 结论放前面)。因此它的单一来源是 common/LayoutPlan.ets 的 resultDecoFor()
//    -> ResultDeco, 三个结果界面(一键跑分结果页 / 单次跑分结果浮层 / 首页分数卡)共用同一份。
//    这里只留一条说明, 免得后来的人又在这里加一个固定的"结果页间距"令牌 —— 那会变成
//    第二个来源, 与自检块显示的值对不上。颜色仍然全部取自上面的 C 令牌。

// 字号缩放(形态不同, 观看距离不同: 平板 / 电脑上略放大; 极窄分屏上略缩小)。
// 纯函数, 无状态。
export function scaledFont(base: number, scale: number): number {
  const v: number = Math.round(base * scale);
  return v < 9 ? 9 : v;
}

// ---- 阴影规格 ----
//  界面本体不使用阴影: ArkUI 的 shadow 会为元素生成离屏图层并做一次模糊,
//  在跑分期间会占用 GPU/CPU。界面本体的层次用 1vp 边框高光替代:
//    .border({ width: { top: ThemeHairline }, color: C.cardEdge })
//  ★ 唯一的例外是"美化层"(见文件顶部第 3 条): 那里的阴影必须经 pages/Index.ets 的
//    fxAura() 出手, 关掉美化时它返回的就是下面的 SHADOW_NONE(半径 0 + 全透明),
//    与"没有这条属性"在渲染上等价。
//  若将来还要在别处用阴影, 规格如下(半径 <= 8, 只允许给静态卡片, 不给列表项):
export interface ShadowSpec {
  radius: number;
  offsetX: number;
  offsetY: number;
  color: string;
}
export const SHADOW_NONE: ShadowSpec = { radius: 0, offsetX: 0, offsetY: 0, color: '#00000000' };
export const HAIRLINE: number = 1;   // 边框高光宽度(vp)
// 主行动卡(「一键全部跑分」)的描边宽度(vp): 比普通卡片的 1vp 高光更醒目一点。
// 它仍然只是一条静态描边 —— 不是阴影、不产生离屏图层、不模糊。
export const BORDER_THICK: number = 2;

// ---- 渐变 ----
//  只用在"分数条 / 进度条"与"开始按钮描边"这两处静态元素上: 渐变在布局阶段一次性生成,
//  没有动画、没有模糊, 跑分期间不重绘(只有进度数值变化时才重画那一条)。
//  注意返回类型是 LinearGradient(不是 LinearGradientOptions): ArkUI 的
//  Progress.color(value: ResourceColor | LinearGradient) 收的是前者, 两者在 ArkTS 里
//  是两个不同的接口, 不能互相赋值。
export function barGradient(from: string, to: string): LinearGradient {
  const stops: Array<[ResourceColor, number]> = [[from, 0.0], [to, 1.0]];
  const g: LinearGradient = { angle: 90, colors: stops };
  return g;
}

// 分数条 / 进度条的配色(主色渐变): 次色 -> 主色, 方向 90 度(从左到右)
export function accentGradient(): LinearGradient {
  return barGradient(C.accent2, C.accent);
}

// ---- App 自己的画布(品牌氛围底) ----
//  左上 -> 右下(ArkUI 的 angle: 0 = 由下到上, 90 = 由左到右, 135 = 左上 -> 右下):
//    暗红 -> 深紫 -> 回落 -> 页面底色 C.bg。
//  为什么终点是 C.bg: 窗口底色(setWindowBackgroundColor)与页面底必须是同一个色,
//    否则旋转 / 圆角 / 系统栏那几处会露出一条接不上的边。
//  ★ 它**不受 fxOn() 控制**, 这是有意的: 它不是"美化层"的装饰效果, 它就是页面底色本身。
//    代价上也说得通 —— 一次静态填充, 与它替换掉的纯色是同一档(一遍填充, 没有离屏层、
//    没有模糊、不重绘、不注册任何东西)。真正会花 GPU 的那几样(点光/光晕/光带/粒子)
//    仍然全部在 fxOn() 的门后。
//  alpha 参数: 给"磨砂浮层"用的半透明版本 —— 官方材质是"模糊背后 + 自己再盖一层",
//  全不透明的渐变换上去会把磨砂直接盖死; 0.86 那一档既留住了玻璃感, 又让品牌色透出来。
//  (alpha = 1 时逐位等于不透明色, 所以同一个函数两个用途。)
export function brandGradient(alpha: number = 1): LinearGradient {
  //  停靠点: 红只占左上那一角(0 ~ 0.26), 中段迅速转到紫, 到 58% 就已经接近底色 ——
  //  这样"氛围"集中在左上, 右下大面积仍是原来的深蓝黑, 正文区的对比度不受影响。
  const stops: Array<[ResourceColor, number]> = [
    [withAlpha(C.brandRed, alpha), 0.0], [withAlpha(C.brandViolet, alpha), 0.26],
    [withAlpha(C.brandDeep, alpha), 0.58], [withAlpha(C.bg, alpha), 1.0]
  ];
  const g: LinearGradient = { angle: 135, colors: stops };
  return g;
}

// 卡片顶部 1vp 高光边框(替代阴影): .border(edgeTop(C.cardEdge))
export function edgeTop(color: string): BorderOptions {
  const opt: BorderOptions = { width: { top: HAIRLINE }, color: color };
  return opt;
}

// 卡片底部 1vp 分隔线(与 edgeTop 相对): .border(edgeBottom(C.divider))
export function edgeBottom(color: string): BorderOptions {
  const opt: BorderOptions = { width: { bottom: HAIRLINE }, color: color };
  return opt;
}

// 四边 1vp 描边(次按钮的层次感来源): .border(outline(C.chip))
export function outline(color: string): BorderOptions {
  const opt: BorderOptions = { width: HAIRLINE, color: color };
  return opt;
}

// ---- 半透明派生色(材质 / 磨砂用) ----
//  官方材质(backgroundBlurStyle / backgroundEffect)要"半透明底色 + 背景模糊"才看得出效果,
//  而界面里不许写十六进制字面量 —— 所以这里用纯函数从令牌派生出带 alpha 的颜色(#AARRGGBB)。
//  纯函数、无状态、无副作用; 解析失败时原样返回入参(静默降级, 不抛异常)。
export function withAlpha(color: string, alpha: number): string {
  if (color.length !== 7 || color.charAt(0) !== '#') {
    return color;
  }
  const a: number = alpha < 0 ? 0 : (alpha > 1 ? 1 : alpha);
  const v: number = Math.round(a * 255);
  const s: string = v.toString(16);
  return '#' + (s.length === 1 ? ('0' + s) : s) + color.substring(1);
}
