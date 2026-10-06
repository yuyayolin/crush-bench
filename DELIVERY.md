# Crush分 交付说明

## 当前交付物（2026-10-06）

交付物：`release/Crush-9.0-unsigned.hap`（23,727,692 字节，未签名，用你自己的华为开发者账号签名后侧载）

版本：**9.0**（versionCode 9000000，bundleName **com.aurora.bench**，桌面名「Crush分」）

SHA-256：`328F84F423B40F3FA40DEC2BF80B256A957B5551AB3A20EA9E79F6BCC5DA597D`

回退基线（上一版，仍然可用，包名相同所以覆盖安装不会丢历史成绩）：
`release/AuroraBench-8.5-unsigned.hap`（23,131,865 字节，SHA-256 `7F5E01A7F7DF6CABA7A9F9FEFD0A6B75BFEDDF27AEBF8ED62AE4E1C415F87989`）

9.0 相对于 8.5 只动了四件事，**分数公式 / k / conv / 单位 / 负载规模一个字都没动**：

| 类别 | 内容 |
|---|---|
| **改名** | 显示名 极光跑分 → **Crush分**。包名不变 —— 改了包名就是另一个应用，手机上已有的历史成绩与 `下载/com.aurora.bench` 会全部找不到 |
| **图标** | 霓虹心。`app_icon.png` 512×512 不透明（桌面，交给系统做圆角遮罩）；`startIcon.png` 512×512 透明底并做边缘淡出（启动页，背景是 `#070B14`，不淡出会露出一块方形紫斑） |
| **打包 SDK** | OpenHarmony SDK 18 → **HarmonyOS SDK 26.0.0.105**（`compileSdkType=HarmonyOS`，`targetAPIVersion=260000026`，`minAPIVersion=50100018`）。同时关掉原生库 strip：6 个 `.so` 的 `.symtab` / `.debug_info` 都还在，崩溃报告里的 pc/lr 仍能还原函数名 |
| **多核口径** | 负载线程数从「逻辑核 8」改成「物理核 6」—— 这台机器只有 6 个可用的物理核，8 个线程会有一对线程抢同一个物理核的 SMT 兄弟核。**多核分与总分因此变化**；单核分、各项吞吐、CS1 单核的跨设备可比性都不受影响 |

自检（本次构建）：hvigor BUILD SUCCESSFUL · ArkTS ERROR 0 · WARN 53（HarmonyOS SDK 的基线，同源码用
OpenHarmony SDK 是 31）· `tools/verify_hap.py` 41/41 · `cpp/verify_*.py` 26/26 · `ref/*.test.mjs` 全过。

**未在真机上装过**：本机没有任何签名材料（无 `%USERPROFILE%\.ohos`，全盘无 `*.p12`/`*.cer`），
unsigned hap 装不上。图标在桌面上到底好不好看、多核 6 线程有没有把 6 个物理核喂饱，都还只有离线证据。

---

## 以下为历史版本记录（5.x 时代，文件名与桌面名都是旧的）

交付物：release/AuroraBench-unsigned.hap（12.58 MB，未签名，用你自己的华为开发者账号签名后侧载）

版本：**5.1**（versionCode 5100000，bundleName com.aurora.bench，桌面名「极光跑分」）

SHA-256：529A05DB7B173D5C113864AF0E596324072F2822B481B1EEF89DA71B27FB2F49

### 5.0/5.1 内容（大改；5.1 仅追加「绑核后被系统挪走」的警告行与措辞修正）

| 类别 | 内容 |
|---|---|
| **CoreMark 修复** | 真机上从来没成功过一次（每次必崩）。根因：align_mem() 只有定义没有原型 → C99 隐式声明当成 int → 64 位指针被截断成 32 位 → 矩阵指针成野指针 → SIGSEGV/SEGV_MAPERR。宿主可执行文件复现 + aarch64 反汇编（sxtw x9, w0）双重证实；修复后在宿主上跑出**合法 CoreMark**（seedcrc 0xe9f5、三个 CRC 与官方已知值逐条一致、Total errors=0） |
| **CPU 绑核** | GB7 的 16 项此前完全没有绑核（只有旧的 7 项自研套件有），单核数字会随机落到小核（同机同构建实测 1761 ms 与 3212 ms 两种）。现在：绑到「大核簇」（频率最高档的全部核；最高档只有 1 个核时并入次高档）。**注意：早期版本试过「钉死唯一最快核」，结果被内核推翻**（EAS 把限制在单核上的线程判为 misfit 强行迁移，真机实测跑到 cpu7/cpu8）。判定标准也随之改为 cpuInFastCluster（跑完是否落在簇内）—— 「cpu != cpuAtStart」是误报，因为同档内迁移无害。多核阶段池线程按频率降序分配到各均衡组，绑核在计时区间之外，读不到频率静默降级 |
| **跑在哪个核（新诊断）** | 每项记录 cpu 编号 / 该核最高频率 / 频率位次 / 是否绑定成功，进 runlog 与结果卡底部汇总行；异常项（跑在小核 / 未绑定 / 绑后被挪走）用警告色点名 |
| **设备识别** | 显示传播名（你这台是 HUAWEI Pura X Max）；内置 110 条机型对照表（来源：华为官方文档 + Google 官方 Play 兼容设备清单）；精确匹配优先、前缀仅在唯一时使用（19 组冲突前缀一律回退型号码）、三星需去颜色/容量后缀；按机型与折叠状态适配布局（展开态 2 列 + 900vp） |
| **崩溃取证** | 信号处理器记录 信号号/si_code/故障地址/pc/当时的负载名/内存快照；关键字段是 pc 是否落在自己的 .so 里（区分「我们越界」与「系统/驱动问题」） |
| **卡死取证** | 看门狗：某项超过 30 秒即向负载线程投递信号抓 64 层调用栈 + 中断点寄存器 + 原始栈，并统计每线程 CPU 增量（有增量=死循环、无增量=死锁） |
| **运行日志** | runlog.jsonl（每项一行 + 每轮汇总 + 崩溃行，512 KB 轮转）+ runhistory.jsonl（每轮一行，**永久账本**，5 MB 才归档）；关于页可看/复制/导出 |
| **无线发送** | 可选：把你自己的 URL 填入后，每轮结束 POST 一份 JSON（默认关闭，留空则零请求；只用 INTERNET 普通权限） |
| **Clang 诊断** | 失败时把 chibicc 的错误原文、失败阶段、arena 峰值打到 hilog（此前只能看到失败在 codegen，看不到原因） |
| **构建加固** | ① CoreMark 源启用 -Werror=implicit-function-declaration 与 -Werror=int-to-pointer-cast（这个 bug 本来编译期就报了警告）；② 工程显式指定 **C++17** —— 工具链默认是 gnu++14，而 std::string::data() 在 C++14 返回 const char*，曾导致三处编译失败 |

### 已知未解 / 待真机定性
1. **Photo Editor 曾出现的 411 Mpx/s（58.4x）无法复现**，同构建真机重测为 62.6~63.7 Mpx/s（8.4x）。metric × ms/1000 必然等于该负载写死的分子（PE=24 Mpx），据此可离线判定任一行数据是否自洽 —— 待 5.0 的 runlog 拿到后定性；
2. **Navigation 卡死**（跑到第 2/16 项空转约 1.3 核）尚未定位，卡死取证装置上线后下一次即可拿到调用栈；
3. **Clang 仍 0 分**：失败阶段已从 parse 推进到 codegen（arena 悬垂已修），但首轮即失败说明另有原因，诊断已就位；
4. 3 项标「未计分」（CPU 的 SfM、GPU 的 Feature Matching / Horizon Detection）—— 单位语义无法定义换算，按「宁可未计分也不硬凑」保留。

### 版本历史

| 版本 | 内容 |
|---|---|
| 2.0 | GB7 复刻 16 项 CPU + 11 项 GPU 全部实现；真多核 |
| 2.1 | 界面自适应重做（滚动容器 + 断点 + 宽屏双列） |
| 3.0 | 工作量标定（对齐 GB7 量级、耗时收敛到 1.5~3 秒） |
| 4.0 | 计分改用官方公开系数 k；GPU 11 项从全灭到可用；多核只跑官方 8 项；总分改几何平均 |
| 4.1 | 稳定性专项：崩溃根因修复 + 内存闸门 + 面包屑 + 长跑自检 |
| 5.1 | 绑核异常三类（跑在小核 / 未绑定 / 绑后被挪走）统一判定与警告行 |
| 5.0 | CoreMark 修复 + CPU 绑核 + 设备识别（110 条表）+ 崩溃/卡死取证 + 运行日志与永久账本 + 无线发送 + Clang 诊断 + 构建加固 |
### 构建环境必读

- 必须设 JAVA_HOME 并把 %JAVA_HOME%\bin 放进 PATH，否则 hvigor 在 PackageHap 阶段报 spawn java ENOENT（环境问题，不是代码问题）；
- 工程必须在 ASCII 路径下构建（中文路径会让 hvigor 报 Invalid project path），当前用 D:\ab-hos；
- 构建命令：hvigor --mode module -p module=entry@default -p product=default assembleHap --no-daemon；
- 一键拉日志＋逐项自洽性校验：tools\pull_and_analyze.ps1（需要手机连 USB 且 App 已运行过）
