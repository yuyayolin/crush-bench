import { AbilityConstant, ConfigurationConstant, UIAbility, Want, bundleManager } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { BenchRunner } from '../service/BenchRunner';
import { CrashGuard } from '../service/CrashGuard';
import { RunLog } from '../service/RunLog';
import { FullReportStore } from '../service/FullRun';
import { ScoreHistory } from '../service/ScoreHistory';
import { C } from '../common/Theme';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    console.info('[AB] onCreate');
    // =========================================================================
    //  ★ App 用自己的深色画布: 不再跟随系统浅色主题 —— 就是这一句决定
    //
    //  用户真机反馈(9.4 截图): 系统处于浅色时, 本 App 被系统按"浅色应用"对待 ——
    //  凡是**没有被显式上色**的东西(组件默认前景/背景、弹出菜单、分隔线、滚动边缘、
    //  按钮的默认态…)全部翻成浅色, 于是这套本来就为深底设计的设计被稀释成一块近白底,
    //  连带把上一轮加的点光源 / 光晕也一起废掉(光感需要暗底才成立)。
    //
    //  这一句把 App 钉死在 COLOR_MODE_DARK 上: 从此系统是深是浅都与本 App 无关,
    //  "深底 -> 浅字"这条对比关系由我们自己保证(页面底色见 common/Theme.ets 的
    //  brandGradient() 与 pages/Index.ets 的 appBackdropGrad())。
    //  放在 onCreate 最前面: 早于 loadContent 的第一帧, 不会出现"先浅后深"的闪一下。
    //  拿不到 context / 调用失败都只写一行日志(退回随系统, 页面仍有自己的底色), 不影响任何功能。
    // =========================================================================
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
      console.info('[AB] color mode pinned to DARK (自带深色画布, 不跟随系统浅色主题)');
    } catch (e) {
      console.warn('[AB] setColorMode(DARK) failed: ' + (e as Error).message);
    }
    // ① native 崩溃取证装置 —— 这里能拿到 context, 并且是整条启动链路里最早的一步:
    //    setLogDir(context.filesDir) 会让 native 侧立刻把 <filesDir>/native_crash.txt
    //    open 好并常驻 fd(崩溃处理器里只 write, 不做 open/malloc), 所以越早越好。
    //    返回值与信号处理器状态全部打到 hilog(console.info → hilog), 便于 hdc 排查。
    const guardOk: boolean = BenchRunner.initNativeGuard(this.context.filesDir);
    const guardReady: boolean = BenchRunner.nativeGuardReady();
    console.info('[AB] native crash guard: setLogDir=' + guardOk.toString() +
      ' nativeCrashGuardReady=' + guardReady.toString() +
      ' path=' + BenchRunner.nativeGuardPath());
    if (!guardOk || !guardReady) {
      // 不静默: 首页崩溃横幅会显示一行「native 取证装置未就绪」(状态已记进 CrashGuard)
      console.warn('[AB] native crash guard NOT ready: setLogDir=' + guardOk.toString() +
        ' ready=' + guardReady.toString() + ' filesDir=' + this.context.filesDir);
    }
    // 崩溃面包屑使用与 BenchStore 同一套 preferences(aurora_bench),
    // 在首帧之前就把 store 拉起来, 保证第一次跑分时面包屑一定能落盘。
    CrashGuard.get().init(this.context).then(() => {
      console.info('[AB] crash guard ready');
    }).catch((e: Error) => {
      console.warn('[AB] crash guard init failed ' + e.message);
    });
    // 运行日志(runlog.jsonl)与可选上传设置也在首帧之前拉起来:
    // 这样第一项负载跑完时就能直接 append, 不需要在 UI 线程上等 I/O 就绪。
    RunLog.get().init(this.context).then(() => {
      console.info('[AB] runlog ready');
    }).catch((e: Error) => {
      console.warn('[AB] runlog init failed ' + e.message);
    });
    // 「一键全部跑分」的报告落盘 store 也在这里就位(filesDir 只取决于 ability 的 context):
    // 页面侧不再依赖任何已废弃的 getContext 取法, 报告一律写到 <filesDir>/report-*。
    // 这一条是"报告明明写成功却显示未落盘"那类问题的地基: 落盘目标目录必须早于界面就确定。
    FullReportStore.get().init(this.context);
    console.info('[AB] full report dir: ' + FullReportStore.get().filesDir());
    // 「历史成绩.txt」的沙箱侧状态(去重状态 + 兜底副本): 同样在首帧之前就位,
    // 免得整轮跑完时还要在 UI 线程上等一次 I/O。
    ScoreHistory.get().init(this.context);
  }

  onDestroy(): void {
    // 正常销毁(用户从最近任务划掉 / 系统正常回收)不算异常结束: 清掉面包屑, 避免下次启动误报。
    // native SIGSEGV / OOM 时进程直接消失, 不会走到这里, 面包屑因此得以保留。
    CrashGuard.get().clear().catch((e: Error) => {
      console.warn('[AB] crash guard clear on destroy failed ' + e.message);
    });
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        console.error('[AB] loadContent failed: ' + JSON.stringify(err));
        return;
      }
      console.info('[AB] content loaded');
    });
    // ---- 沉浸式(边到边): 窗口铺满整屏, 内容区延伸到状态栏 / 导航栏之下 ----
    //  ① setWindowLayoutFullScreen(true): 窗口不再被系统栏挤掉, 页面根节点拿到整屏高度;
    //  ② 系统栏配色与深色主题匹配: 状态栏透明(露出页面底色) + 图标用浅色;
    //  ③ 安全区由页面自己用 insets 做 padding 保证(见 pages/Index.ets 的
    //     safeInsetTop()/safeInsetBottom() 与 build() 的 .expandSafeArea + .padding):
    //     窗口铺满是"背景铺满", 可点内容一律让开系统栏 —— 宁可留白, 不遮住控件。
    //  全部失败也只是"不沉浸"(页面会退回系统自动避让的安全区), 不影响功能。
    try {
      const win: window.Window = windowStage.getMainWindowSync();
      win.setWindowLayoutFullScreen(true).then(() => {
        console.info('[AB] window layout full screen: on');
        this.applySystemBar(win);
      }).catch((e: Error) => {
        console.warn('[AB] setWindowLayoutFullScreen failed ' + e.message);
      });
      // 窗口底色与页面底色一致: 旋转 / 圆角 / 系统栏区域都不会露白
      win.setWindowBackgroundColor(C.bg);
      this.reportRotationPolicy(win);
    } catch (e) {
      console.warn('[AB] immersive setup failed ' + (e as Error).message);
    }
  }

  // ---- 旋转策略: 回读 + 一道"只在对方确实错了时才出手"的保险 ----
  //
  //  背景(用户报的 bug): "系统开了旋转锁定, 为什么在这个 App 里还能转?"
  //  根因: module.json5 里写的是 "auto_rotation", 而本机 SDK(API 18)
  //    D:\\ohos-tools\\sdk\\18\\ets\\api\\@ohos.window.d.ts 的 window.Orientation 注释逐字写着:
  //      AUTO_ROTATION = 5             "Follow the rotation of the sensor, ignore auto rotation lock."
  //      AUTO_ROTATION_RESTRICTED = 8  "Follow the rotation of the sensor, controlled by auto rotation lock."
  //  修法: 声明值改成 "auto_rotation_restricted"(module.json5 的合法取值见
  //    D:\\ohos-tools\\sdk\\18\\toolchains\\modulecheck\\module.json 的 abilities[].orientation.enum)。
  //
  //  这里做的三件事(全部只读 + 一次日志, 不改任何与跑分有关的行为):
  //    ① 把声明值从 bundleManager 回读出来打日志 —— 装机后用 hdc shell hilog 就能确认
  //       真正生效的是哪个取值, 不用靠猜;
  //    ② 回读窗口的 getPreferredOrientation(): 只有当它落在"无视旋转锁定"的那三个取值上时,
  //       才调一次 setPreferredOrientation(AUTO_ROTATION_RESTRICTED) 把它掰回来 —— 这是保险,
  //       正常情况下(没被 setPreferredOrientation 动过)根本不会触发, 也不会与声明值打架;
  //    ③ 全程 try/catch: 读不到/改不了都只写日志, 不影响启动。
  private reportRotationPolicy(win: window.Window): void {
    let declared: string = '(未回读到)';
    try {
      const flags: number = bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_HAP_MODULE |
        bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_ABILITY;
      const info: bundleManager.BundleInfo = bundleManager.getBundleInfoForSelfSync(flags);
      for (const m of info.hapModulesInfo) {
        for (const ab of m.abilitiesInfo) {
          if (ab.name === 'EntryAbility') {
            declared = (ab.orientation as number).toString();
          }
        }
      }
    } catch (e) {
      declared = '(bundleManager 回读失败: ' + (e as Error).message + ')';
    }
    // bundleManager.DisplayOrientation 的取值顺序(见 @ohos.bundle.bundleManager.d.ts):
    //   0 UNSPECIFIED / 1 LANDSCAPE / 2 PORTRAIT / 3 FOLLOW_RECENT / 4 LANDSCAPE_INVERTED /
    //   5 PORTRAIT_INVERTED / 6 AUTO_ROTATION / 7 AUTO_ROTATION_LANDSCAPE /
    //   8 AUTO_ROTATION_PORTRAIT / 9 AUTO_ROTATION_RESTRICTED /
    //   10 AUTO_ROTATION_LANDSCAPE_RESTRICTED / 11 AUTO_ROTATION_PORTRAIT_RESTRICTED /
    //   12 LOCKED / 13 AUTO_ROTATION_UNSPECIFIED / 14 FOLLOW_DESKTOP
    console.info('[AB] rotation policy: declared orientation(DisplayOrientation) = ' + declared +
      ' (期望 9 = AUTO_ROTATION_RESTRICTED, 即"跟随传感器但受系统旋转锁定控制")');
    try {
      const cur: number = win.getPreferredOrientation() as number;
      console.info('[AB] rotation policy: window.getPreferredOrientation() = ' + cur.toString());
      // 只有当前首选方向明确落在"无视旋转锁定"的那一族时, 才出手改一次:
      //   window.Orientation: AUTO_ROTATION = 5 / AUTO_ROTATION_PORTRAIT = 6 / AUTO_ROTATION_LANDSCAPE = 7
      if (cur === 5 || cur === 6 || cur === 7) {
        win.setPreferredOrientation(window.Orientation.AUTO_ROTATION_RESTRICTED).then(() => {
          console.info('[AB] rotation policy: 发现运行时首选方向会无视系统旋转锁定, ' +
            '已改为 AUTO_ROTATION_RESTRICTED');
        }).catch((e: Error) => {
          console.warn('[AB] rotation policy: setPreferredOrientation 失败 ' + e.message +
            '（声明值仍然是 auto_rotation_restricted, 行为不变）');
        });
      }
    } catch (e) {
      console.warn('[AB] rotation policy: getPreferredOrientation 不可用 ' + (e as Error).message);
    }
  }

  // 系统栏配色(与页面深色主题一致): 状态栏透明 + 导航栏取页面底色 + 两处图标都用浅色。
  // 颜色全部来自 common/Theme.ets 的令牌, 这里不出现任何十六进制字面量。
  private applySystemBar(win: window.Window): void {
    const props: window.SystemBarProperties = {
      statusBarColor: C.transparent,
      isStatusBarLightIcon: true,
      statusBarContentColor: C.text,
      navigationBarColor: C.bg,
      isNavigationBarLightIcon: true,
      navigationBarContentColor: C.text
    };
    win.setWindowSystemBarProperties(props).then(() => {
      console.info('[AB] system bar properties applied');
    }).catch((e: Error) => {
      console.warn('[AB] setWindowSystemBarProperties failed ' + e.message);
    });
  }
}
