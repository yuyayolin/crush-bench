# Crush分

一个鸿蒙（HarmonyOS NEXT）原生的性能基准测试 App。CPU 用 C++ 原生负载，GPU 用自带的渲染管线，
另外带 NPU 推理与存储 I/O 两个小节。跑完一轮会落一份完整报告到「下载」目录，App 里也有历史页。

> 桌面名：**Crush分**  ·  包名：`com.aurora.bench`  ·  构建：HarmonyOS SDK（`compileSdkVersion 26.0.0`）

---

## 与第三方跑分的关系（请先读这一段）

**本项目与 Geekbench、3DMark、安兔兔、Primate Labs 等任何第三方跑分软件及其厂商均无任何关联，
也未获得它们的授权、认可或赞助。**

- 本 App 的测试套件、单位与刻度**全部由本工程自定**。分数是绝对刻度：
  `单项分 = 绝对吞吐量 / 公示常量 × 1000`，每 1000 分对应一个固定吞吐量，与"某台设备的耗时"无关。
- 本 App 的分数**不能与任何第三方跑分软件的成绩比较**。即使某一项看起来在测同一件事，
  负载的实现、规模、参与项集合都不一样，数字之间没有换算关系。
- 本项目里出现的任何第三方产品名称，仅用于说明"本工程测的是什么"，不构成任何形式的关联或背书。

## 这份分数能怎么比

只有一件事是**跨设备可比**的：**同一版本下、单线程跑出来的那一组分数**。

- 多核那一组在早期版本里换过一次参与项集合，**不要跨版本比多核**；
- GPU、NPU、存储那几组只适合同一台机器的前后对比（驱动策略、温控、文件系统状态都会影响它们）；
- 「自研套件」那一组是更早的一套负载，**只适合同机对比**，不要拿它去比较不同芯片。

App 的「历史」页与落盘的 `历史成绩.txt` 里都写着同一句口径。

## 构建

需要 DevEco Studio 自带的 HarmonyOS SDK（本仓库在 `26.0.0.105` 上验证过）。

```
$env:DEVECO_SDK_HOME = "D:\DevEco Studio\sdk"
$env:OHOS_BASE_SDK_HOME = "D:\DevEco Studio\sdk"
$env:JAVA_HOME = "D:\DevEco Studio\jbr"

& "D:\DevEco Studio\tools\node\node.exe" `
  "D:\DevEco Studio\tools\hvigor\bin\hvigorw.js" `
  --mode module -p module=entry@default -p product=default assembleHap --no-daemon
```

产物在 `entry/build/default/outputs/default/entry-default-unsigned.hap`，**未签名**。
用自己的华为开发者账号签名后才能侧载。

> 注意：源码目录**不要放在带中文的路径下**，hvigor 与部分工具链在中文路径上会失败。

## 自检

本仓库的验证脚本都放在源码里，改完必须全过：

```
python tools/verify_hap.py <hap 路径> .          # 拆包核对: 图标/版本号/ABI/.so 有没有被 strip
node ref/layout_cases.test.mjs                    # 布局: 14 万个窗口采样 + 源码静态检查
node ref/numberfmt_cases.test.mjs                 # 数字排版口径
python entry/src/main/cpp/verify_*.py             # 26 个原生侧自检
```

## 目录

```
AppScope/                     应用级配置与图标
entry/src/main/ets/           ArkTS 界面与业务(页面 / 服务 / 公共零件)
entry/src/main/cpp/           原生负载、绑核、取证、自检脚本
  third_party/                chibicc / zstd / lz4 / miniz / stb
  coremark/                   EEMBC CoreMark(仅作功能自检, 不计分)
  verify_*.py                 26 个原生侧自检
ref/                          离线用例(布局 / 数字排版)
tools/                        打包产物自检
release/                      构建出的 .hap(不进 git, 见 Releases)
METHODOLOGY.md                计分方法与跨平台可比性说明
STABILITY.md                  稳定性设计与崩溃归因
DELIVERY.md                   交付说明与已知问题
THIRD_PARTY_NOTICES.md        第三方组件与许可
```

## 许可

自有代码按 **MIT** 发布，见 `LICENSE`。
仓库内含第三方组件（chibicc / zstd / lz4 / miniz / stb / CoreMark / libc++），
各自按自己的许可发布，见 `THIRD_PARTY_NOTICES.md`。
