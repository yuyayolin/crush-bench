#include "napi/native_api.h"
#include "bench.h"
#include "gb7.h"
// 「本机性能天花板」一行 + 运行时频率的派生量(接口与口径见 cpu_freq_sample.h 末尾)
#include "cpu_freq_sample.h"
#include "npu_bench.h"   // NPU(AI 加速器)小节(独立, 不进 CS1 分数)
#include "storage_bench.h"   // 存储 I/O 小节(独立, 不进 CS1 分数)
#include <exception>
#include <string>
#include <thread>

extern "C" double coremark_run_official(void);
extern "C" int coremark_crc_ok(void);
extern "C" int coremark_error_count(void);

namespace {

struct AsyncTestWork {
    napi_env env;
    napi_async_work work;
    napi_deferred deferred;
    int id;
    int threads;
    // runGb7 的第三个参数(可选, 扁平 JSON 字符串)。目前只用一件事: 重复轮数
    // ({"rounds":N} / {"gb7Rounds":N}) —— 它只决定"同一项测量做几次", 不改单轮的任何工作量。
    // 没传 = 空串 = 用默认轮数(见 gb7.h 的 GB7_DEFAULT_ROUNDS)。
    std::string options;
    std::string result;
};

std::string escapeJson(const std::string& s)
{
    // 必须转义控制字符: GLSL info log、chibicc 的错误文本、以及任何多行失败说明里都带换行,
    // 而 JSON 字符串里不允许出现裸的 0x00-0x1F。一旦漏了, ArkTS 侧 JSON.parse 会抛异常
    // (真机上表现为跑分中途闪退), 所以这里按 JSON 规范转成 \uXXXX。
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                    out += esc;
                } else {
                    out.push_back((char)c);
                }
                break;
        }
    }
    return out;
}

// 安全读取一个 JS 字符串参数(不抛异常, 拿不到就返回空串)
std::string jsStringArg(napi_env env, napi_value v)
{
    std::string out;
    if (v == nullptr) {
        return out;
    }
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok || len == 0) {
        return out;
    }
    out.resize(len + 1);
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, v, out.data(), len + 1, &copied) != napi_ok) {
        return std::string();
    }
    out.resize(copied);
    return out;
}

// 崩溃取证: 一项跑完(或异常退出前)把标记清成“无负载运行中”。
// 这样证据里的 item 字段能区分“崩在负载里”和“崩在两项之间的胶水代码里”。
void markIdle()
{
    // 必须走 auroraSetIdleMarker: 它除了写同样的空闲面包屑, 还会把"当前没有负载在跑"
    // 这个标志置 1 —— 看门狗据此不判定卡死, 否则两项之间的空档超过 30 秒就会被当成
    // 卡死并写出上百 KB 的假现场(真机 native_hang.txt 112 KB 就是这么来的)。
    auroraSetIdleMarker();
}

void CompleteTest(napi_env env, napi_status status, void* data);

void ExecuteCoreMark(napi_env env, void* data)
{
    (void)env;
    auto* w = static_cast<AsyncTestWork*>(data);
    // 崩溃取证标记(只写静态缓冲, 不影响 CoreMark 的算法/工作量/计分)
    auroraSetCurrentItem("CoreMark", "CoreMark 官方负载", 1, 1);
    double score = coremark_run_official();
    int crcOk = coremark_crc_ok();
    int errs = coremark_error_count();
    markIdle();
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"ok\":%s,\"score\":%.2f,\"crcOk\":%s,\"errors\":%d}",
        score > 0.0 ? "true" : "false", score, crcOk ? "true" : "false", errs);
    w->result.assign(buf);
}

std::string gb7FailJson(int id, const std::string& why);
void ExecuteGb7Body(AsyncTestWork* w);

void ExecuteGb7(napi_env env, void* data)
{
    (void)env;
    auto* w = static_cast<AsyncTestWork*>(data);
    // 崩溃防护: 负载内部若抛出 C++ 异常(典型是内存不足时的 std::bad_alloc),
    // 未捕获会导致 std::terminate -> SIGABRT, 在真机上表现为"闪退"且用户看不到任何原因。
    // 这里把它转成一条可见的失败结果(unit 字段带原因), 让 UI 能显示出来。
    try {
        ExecuteGb7Body(w);
    } catch (const std::exception& e) {
        w->result = gb7FailJson(w->id, std::string("C++ 异常: ") + e.what());
    } catch (...) {
        w->result = gb7FailJson(w->id, "未知 C++ 异常(可能是内存不足)");
    }
}

std::string gb7FailJson(int id, const std::string& why)
{
    char num[96];
    std::string out = "{\"name\":\"" + escapeJson(gb7TestName(id)) + "\",\"section\":\"" +
        escapeJson(gb7TestSection(id)) + "\"";
    snprintf(num, sizeof(num), ",\"ms\":0.0,\"score\":0.0");
    out += num;
    out += ",\"metric\":\"0\",\"unit\":\"运行失败: " + escapeJson(why) + "\"";
    snprintf(num, sizeof(num), ",\"parallelism\":0.0");
    out += num;
    out += gb7TestIsMulti(id) ? ",\"isMulti\":true" : ",\"isMulti\":false";
    // 失败项同样补上 runFreq(空串) —— 与 diag 一样, 形状恒定才不会让 ArkTS 侧读到 undefined
    out += ",\"scored\":false,\"basis\":\"\",\"diag\":\"\",\"runFreq\":\"\",\"qos\":\"\",\"cpuset\":\"\"";
    // 失败项也补上重复性字段(形状恒定, 免得 ArkTS 侧读到 undefined)。
    // 失败时没有轮次数据 —— 写 verdict=RUN_FAILED, 不编一个离散度出来。
    out += ",\"rounds\":0,\"roundsRequested\":0,\"roundsSource\":\"default\"";
    out += ",\"repeatability\":{\"available\":false,\"rounds\":0,\"roundsOk\":0,"
           "\"representative\":\"median(中位) —— 不是最好一轮, 也不是平均\","
           "\"dispersionDefinition\":\"相对离散度 = (最大 - 最小) / 中位 x 100%\","
           "\"credibility\":{\"verdict\":\"RUN_FAILED\",\"thresholdsAreOursNotOfficial\":true,"
           "\"text\":\"本项运行失败(原因见 unit), 没有任何轮次数据 —— 写空, 不编离散度\"}}";
    // 失败结果也把亲和性字段补成与成功结果同一形状, 免得 ArkTS 侧读到 undefined
    // (字段清单与 executeGb7Body 里那一组严格一致, 见下面的注释)
    out += ",\"cpu\":-1,\"cpuMaxKhz\":0,\"cpuRank\":-1,\"cpuBound\":false,\"cpuAtStart\":-1,"
           "\"cpuInFastCluster\":false,\"cpuAtStartInFastCluster\":false,"
           "\"cpuFastClusterCores\":0,\"cpuFastClusterMaxKhz\":0,\"cpuFastClusterMask\":\"0x0\","
           "\"cpuWorkers\":0,\"cpuWorkersOverflow\":0,"
           "\"cpuLogical\":0,\"cpuPhysical\":0,\"cpuSmtPossible\":false,\"cpuSmtEnabled\":true,"
           "\"cpuTopoKnown\":false,\"cpuThreadsRequested\":1,\"cpuThreadsEffective\":0,"
           "\"cpuThreadsBasisFull\":0,"
           "\"cpuEffMask\":\"0x0\",\"cpuTopoSource\":\"\",\"cpuInfo\":\"\","
           // 允许核集合一组(2026-10-05): 失败项也补齐成同一形状, 免得 ArkTS 侧读到 undefined
           "\"cpuAllowedOk\":false,\"cpuAllowedCount\":0,\"cpuAllowedMask\":\"0x0\","
           "\"cpuAppliedMask\":\"0x0\",\"cpuFastClusterFallback\":false,"
           "\"cpuFastClusterTierIndex\":0,"
           "\"cpuInFastClusterJudged\":false,\"cpuInMachineTopTier\":false,"
           "\"cpuMachineTopTierCores\":0,\"cpuMachineTopTierKhz\":0,"
           "\"cpuMachineTopTierMask\":\"0x0\","
           "\"cpuFastClusterSource\":\"\",\"cpuAllowedText\":\"\",\"cpuBoundReason\":\"\"";
    //  收尾的 '}' 必须补(2026-10-05 回归修复): 末尾那组诊断字段是后来追加的,
    //   追加时把原本写在最后一条 snprintf 格式串里的 '}' 弄丢了 —— 少了它, 整个返回串就是
    //   非法 JSON, ArkTS 侧 JSON.parse 直接抛异常, 表现为 GB7 每一项都"解析失败"。
    out += "}";
    return out;
}

void ExecuteGb7Body(AsyncTestWork* w)
{
    // 崩溃取证标记: 必须在 gb7RunTest 之前落好, 崩在负载里时它就是现场。
    const std::string curName = gb7TestName(w->id);
    auroraSetCurrentItem(w->threads > 1 ? "CS1 多核" : "CS1 单核", curName.c_str(), w->id + 1, gb7TestCount());
    // 重复轮数只来自选项(缺省 = GB7_DEFAULT_ROUNDS)。每一轮都是同一次完整测量 ——
    // 单轮的负载算法/尺寸/metric/unit/k/conv/计分公式/线程数/绑核策略全部未改动。
    bool roundsFromOption = false;
    const int rounds = gb7RoundsFromOptions(w->options, &roundsFromOption);
    Gb7Outcome r = gb7RunTestRepeated(w->id, w->threads, rounds, roundsFromOption);
    markIdle();
    // 用 std::string 拼接而不是 snprintf 定长缓冲: basis 是较长的中文说明(数百字节),
    // 定长缓冲会把它截断成非法 JSON。
    //  缓冲区必须按最坏情况实算, 不能拍脑袋(2026-10-05 真机回归修复)
    // 下面那个 "cpu* 诊断字段" 的 snprintf 一条就写完这么多键:
    //   ,"cpuAllowedOk":true,"cpuAllowedCount":14,"cpuAllowedMask":"0x3fff",
    //   "cpuAppliedMask":"0x3fff","cpuFastClusterFallback":false,"cpuFastClusterTierIndex":0,
    //   "cpuInFastClusterJudged":true,"cpuInMachineTopTier":true,"cpuMachineTopTierCores":2,
    //   "cpuMachineTopTierKhz":2750000,"cpuMachineTopTierMask":"0x3000"
    // 光键名 + 典型值就 280 字符以上; 原来这里是 256(注释按"~130 字节"估的, 那是加这组字段之前的
    // 数字), 于是 snprintf 截断, out 追加到半截字符串 -> 整个返回串非法 JSON ->
    // ArkTS 侧 JSON.parse 抛异常 -> 真机表现为 GB7 每一项都"解析失败"(GPU/自研套件走别的函数, 不受影响)。
    // 取 768: 即使将来再加一组同量级的诊断字段也放得下。改这一组字段时请同步复核这个大小。
    char num[768];
    std::string out = "{\"name\":\"" + escapeJson(r.name) + "\",\"section\":\"" +
        escapeJson(r.section) + "\"";
    snprintf(num, sizeof(num), ",\"ms\":%.1f,\"score\":%.1f", r.ms, r.score);
    out += num;
    out += ",\"metric\":\"" + escapeJson(r.metric) + "\",\"unit\":\"" + escapeJson(r.unit) + "\"";
    snprintf(num, sizeof(num), ",\"parallelism\":%.2f", r.parallelism);
    out += num;
    out += ",\"gb7Unit\":\"" + escapeJson(gb7TestGb7Unit(w->id)) + "\"";
    snprintf(num, sizeof(num), ",\"k\":%.10g,\"conv\":%.10g",
             gb7TestK(w->id, w->threads > 1 ? 1 : 0), gb7TestConversion(w->id));
    out += num;
    out += gb7TestIsMulti(w->id) ? ",\"isMulti\":true" : ",\"isMulti\":false";
    out += gb7TestScored(w->id) ? ",\"scored\":true" : ",\"scored\":false";
    // ---- CPU 亲和性诊断(旁路字段, 不塞进 metric/unit, 不计分口径) ----
    // 绑定语义(2026-10 起): 主线程绑到"大核簇"= 频率最高的一档核组成的集合(至少 2 个
    // 核), 不再是"最快的单个核" —— 单核掩码会被内核判 misfit 并强行迁走(真机 16 项里 5 项
    // 出现 "cpuBound=true 但起于 cpu4 结束在 cpu7"), 理由与簇的定义见 cpu_affinity.h 文件头。
    // 字段清单(键名 / 类型 / 含义):
    //   cpu                      int   : 负载跑完时所在的 CPU 编号(-1 = 取不到)
    //   cpuMaxKhz                int   : 该 CPU 的 cpuinfo_max_freq(kHz; 0 = 该核不在表里/读不到)
    //   cpuRank                  int   : 该 CPU 在"频率降序"表里的位次(0 = 最快; -1 = 未知)
    //                                    附加信息, 不再是判据
    //   cpuBound                 bool  : 主线程是否成功应用了大核簇掩码
    //                                    (false = 读不到频率表, 已静默降级 -> 忽略下面所有簇字段)
    //   cpuAtStart               int   : 绑核刚做完时对负载线程的采样(绑完先 sched_yield 再采样)
    //   cpuInFastCluster         bool  : 新判据 跑完时所在的核是否属于大核簇。
    //                                    true 才算"绑核确实生效"; 簇内迁移(cpu4 -> cpu7)是
    //                                    设计允许的, 不再算异常
    //   cpuAtStartInFastCluster  bool  : cpuAtStart 是否在簇内(仅参考: 迁移最终由内核决定)
    //   cpuFastClusterCores      int   : 大核簇包含几个核(0 = 读不到频率表, 簇未定义)
    //   cpuFastClusterMaxKhz     int   : 大核簇的最高频率(kHz; 0 = 未知)
    //   cpuFastClusterMask       string: 大核簇的核心位图(十六进制, 只覆盖前 64 核) —— 排障用
    //   cpuWorkers               int   : 成功绑定自己的池线程个数
    //   cpuInfo                  string: 一行人类可读文本(进 runlog)
    // ArkTS 侧接法: "绑核未生效"的告警条件从 "cpu != cpuAtStart" 改成
    //   (cpuBound == true && cpuInFastCluster == false)。
    snprintf(num, sizeof(num),
             ",\"cpu\":%d,\"cpuMaxKhz\":%d,\"cpuRank\":%d,\"cpuBound\":%s,\"cpuAtStart\":%d,"
             "\"cpuInFastCluster\":%s,\"cpuAtStartInFastCluster\":%s",
             r.cpuInfo.cpu, r.cpuInfo.maxKhz, r.cpuInfo.rank, r.cpuInfo.bound ? "true" : "false",
             r.cpuInfo.cpuAtStart,
             r.cpuInfo.cpuInFastCluster ? "true" : "false",
             r.cpuInfo.cpuAtStartInFastCluster ? "true" : "false");
    out += num;
    snprintf(num, sizeof(num), ",\"cpuFastClusterCores\":%d,\"cpuFastClusterMaxKhz\":%d",
             r.cpuInfo.fastClusterCores, r.cpuInfo.fastClusterMaxKhz);
    out += num;
    snprintf(num, sizeof(num), ",\"cpuFastClusterMask\":\"0x%llx\"", r.cpuInfo.fastClusterMask);
    out += num;
    snprintf(num, sizeof(num), ",\"cpuWorkers\":%d,\"cpuWorkersOverflow\":%d",
             r.cpuInfo.workers, r.cpuInfo.workersOverflow);
    out += num;
    // ---- 超线程(SMT)诊断(同样只是旁路字段: 不塞进 metric/unit, 不参与计分) ----
    // cpuLogical / cpuPhysical  : 逻辑核数 / 物理核数; 拓扑未知时两者按相等(1:1)
    // cpuSmtPossible            : 物理核数 < 逻辑核数(检测到 SMT)
    // cpuSmtEnabled             : 跑本项时开关的状态(1 开 / 0 关)
    // cpuTopoKnown              : 拓扑是否已知(0 = 未知, 已按 1:1 处理, 开关无效)
    // cpuThreadsRequested       : 调用方请求的线程数(未被夹时与 cpuThreadsEffective 相同)
    // cpuThreadsEffective       : 本项真正开了几个线程(已过可用核集合/物理核的夹子)
    //                             (2026-10-06 修正: 以前这里是"实际使用集合大小", 在被内核限核
    //                              的机器上会把单核项也写成"14 线程", 与同一行的 allowed=0-8 打架)
    // cpuThreadsBasisFull       : 不看可用核集合时本应几个线程(= 全机口径; 与上面之差即被夹掉的量)
    // cpuEffMask                : 实际使用集合的位图(十六进制字符串)
    // cpuSmtText                : 一行中文说明(可直接显示)
    // cpuTopoSource             : 拓扑来源(哪个文件 / "未知, 已按 1:1 处理")
    snprintf(num, sizeof(num),
             ",\"cpuLogical\":%d,\"cpuPhysical\":%d,\"cpuSmtPossible\":%s,\"cpuSmtEnabled\":%s,"
             "\"cpuTopoKnown\":%s,\"cpuThreadsRequested\":%d,\"cpuThreadsEffective\":%d,"
             "\"cpuThreadsBasisFull\":%d",
             r.cpuInfo.logical, r.cpuInfo.physical,
             r.cpuInfo.smtPossible ? "true" : "false",
             r.cpuInfo.smtEnabled ? "true" : "false",
             r.cpuInfo.topoKnown ? "true" : "false",
             r.cpuInfo.threadsRequested, r.cpuInfo.threadsEffective, r.cpuInfo.threadsBasisFull);
    out += num;
    snprintf(num, sizeof(num), ",\"cpuEffMask\":\"0x%llx\"", r.cpuInfo.effMask);
    out += num;
    out += ",\"cpuTopoSource\":\"" + escapeJson(std::string(r.cpuInfo.topoSource)) + "\"";
    out += ",\"cpuInfo\":\"" + escapeJson(r.cpuInfo.text) + "\"";
    // ---- 内核允许的核集合 + 两个分开的判据(2026-10-05 追加; 同样是旁路字段) ----
    // cpuAllowedOk / cpuAllowedCount / cpuAllowedMask : 内核允许本进程用哪些核
    //   (/proc/self/status Cpus_allowed_list 与 sched_getaffinity 互相印证)。
    //   这是"为什么这台机器像被锁住了"的第一手事实: 可用核集合可能不含最高频的那几个核。
    // cpuAppliedMask         : 主线程真正应用的那张掩码(sched_setaffinity 的目标)
    // cpuFastClusterFallback : 1 = 全机最快档 ∩ 可用核集合 = 空, 已退化成"可用核集合里最快的核"
    //   (此时 cpuBound 记 false —— 不假装绑定成功, 但掩码确实绑到了可用核集合里最好的位置)
    // cpuFastClusterTierIndex: 生效快簇落在全机的第几个频率档(0 = 全机最快档)
    // 两个判据分开报, 不混成一个
    //   cpuInFastCluster    : 跑完时在不在"生效快簇"里(绑核有没有生效)
    //   cpuInMachineTopTier : 跑完时在不在"全机最快频率档"里(我们到底够不够快)
    //   可用核集合不含 Prime 核时会出现 true/false 组合 —— 那正是"绑核生效却拿不到最快核"。
    // cpuFastClusterSource / cpuAllowedText / cpuBoundReason : 三行可直接显示的中文说明
    snprintf(num, sizeof(num),
             ",\"cpuAllowedOk\":%s,\"cpuAllowedCount\":%d,\"cpuAllowedMask\":\"0x%llx\","
             "\"cpuAppliedMask\":\"0x%llx\",\"cpuFastClusterFallback\":%s,"
             "\"cpuFastClusterTierIndex\":%d,"
             "\"cpuInFastClusterJudged\":%s,\"cpuInMachineTopTier\":%s,"
             "\"cpuMachineTopTierCores\":%d,\"cpuMachineTopTierKhz\":%d,"
             "\"cpuMachineTopTierMask\":\"0x%llx\","
             // ---- 逐核启动状态与 A/B/C/D 分类(2026-10 新增; 旁路字段, 不计分) ----
             //   回答"为什么有核没有启动": 每个核归入且仅归入一类 ——
             //     A 在线且被内核允许(可用) / B 在线但被本进程的许可集合拒绝 /
             //     C 离线(offline, 核没有启动) / D present 里根本没有。
             //   判据全部是读数: 全局 present/possible/online 原文 + 逐核 cpuN/online 原文 +
             //   逐核 sched_setaffinity({c}) 立刻读回 + 逐核频率/拓扑原文; 逐核明细与 errno
             //   在 cpuAllowedText 那一行里(它已被 ArkTS 原样拼进每项 note)。
             //   coreOnlineWrite* = C 类拉起探测的结果(写 '1' 前先备份, 无论成败都按原值恢复):
             //     writeOk>0 = 真的拉起来了; writeFail>0 且 lastOpenErrno 非 0 = 应用域无权拉起。
             "\"coreStartupRan\":%s,\"coreStartupUpper\":%d,"
             "\"coreClassA\":%d,\"coreClassB\":%d,\"coreClassC\":%d,\"coreClassD\":%d,"
             "\"coreClassAMask\":\"0x%llx\",\"coreClassBMask\":\"0x%llx\","
             "\"coreClassCMask\":\"0x%llx\",\"coreClassDMask\":\"0x%llx\","
             "\"coreOnlineWriteAttempted\":%d,\"coreOnlineWriteOk\":%d,"
             "\"coreOnlineWriteFail\":%d,\"coreOnlineLastOpenErrno\":%d,"
             "\"coreOnlineLastWriteErrno\":%d,\"coreOnlineRestoreOk\":%d,"
             "\"coreOnlineRestoreTotal\":%d",
             r.cpuInfo.cpuAllowedOk ? "true" : "false", r.cpuInfo.cpuAllowedCount,
             r.cpuInfo.cpuAllowedMask, r.cpuInfo.cpuAppliedMask,
             r.cpuInfo.cpuFastClusterFallback ? "true" : "false",
             r.cpuInfo.cpuFastClusterTierIndex,
             r.cpuInfo.cpuInFastClusterJudged ? "true" : "false",
             r.cpuInfo.cpuInMachineTopTier ? "true" : "false",
             r.cpuInfo.cpuMachineTopTierCores, r.cpuInfo.cpuMachineTopTierKhz,
             r.cpuInfo.cpuMachineTopTierMask,
             r.cpuInfo.coreStartupRan ? "true" : "false", r.cpuInfo.coreStartupUpper,
             r.cpuInfo.coreClassA, r.cpuInfo.coreClassB, r.cpuInfo.coreClassC, r.cpuInfo.coreClassD,
             r.cpuInfo.coreClassAMask, r.cpuInfo.coreClassBMask,
             r.cpuInfo.coreClassCMask, r.cpuInfo.coreClassDMask,
             r.cpuInfo.coreOnlineWriteAttempted, r.cpuInfo.coreOnlineWriteOk,
             r.cpuInfo.coreOnlineWriteFail, r.cpuInfo.coreOnlineLastOpenErrno,
             r.cpuInfo.coreOnlineLastWriteErrno, r.cpuInfo.coreOnlineRestoreOk,
             r.cpuInfo.coreOnlineRestoreTotal);
    out += num;
    out += ",\"cpuFastClusterSource\":\"" + escapeJson(std::string(r.cpuInfo.cpuFastClusterSource)) + "\"";
    out += ",\"cpuAllowedText\":\"" + escapeJson(std::string(r.cpuInfo.cpuAllowedText)) + "\"";
    out += ",\"cpuBoundReason\":\"" + escapeJson(std::string(r.cpuInfo.cpuBoundReason)) + "\"";
    // ---- 自证字段(2026-10-06 新增): 负载自己上报的"这一次到底跑了多少工作量" ----
    // 与上面那组 cpu* 一样是旁路字段: 不参与 metric / unit / k / conv / 计分公式,
    // 也不改变任何负载的算法与尺寸。它的存在只为一件事: 让 runlog 自己能证明"跑满 / 提前退"。
    // 为什么必须有: 有些负载的计时区间里含不进 metric 分子、也不进 unit 的工作
    // (典型 = Video Decoder 的全流自检), 那段工作一旦提前退出, o.ms 会掉而 metric/unit
    // 一个字都不变 —— 2026-10-06 的 6.2 掉档 31% 就是这样变成悬案的。
    // 内容形如: passes=2 passesDone=2 decodedFrames=10 wantFrames=10 failedFrames=0
    //           selfCheckFrames=5/5 selfCheckMismatches=0 selfCheckMs=... streamBytes=...
    // ArkTS 侧接法(回报里给了完整片段; 本文件不做 ets 改动):
    //   model/BenchModel.ets 的 Gb7Result 增加 diag: string;
    //   service/BenchRunner.ets 的 withCpuFields() 里加 r.diag = (o.diag as string) ?? '';
    //   再把 r.diag 拼进 runlog 的 note 即可。
    out += ",\"diag\":\"" + escapeJson(r.diag) + "\"";
    // ---- 运行时实际频率(2026-10 新增; 与 diag 一样是旁路字段) ----
    // 与上面的 cpu*/diag 一样: 不参与 metric / unit / k / conv / 计分公式, 也不改变任何负载的
    // 算法与尺寸。它回答的是此前完全缺失的那个问题: "标称 2270MHz 的机器, 跑负载时实际
    // 跑在多少 MHz"(此前只报 cpuinfo_max_freq = 标称上限, 于是"被限频的机器"与"跑满的机器"
    // 在日志里长得一模一样)。口径(采哪个核)与开销论证写在 cpu_freq_sample.h 文件头;
    // 同一行文本已经由 gb7RunTest 追加进 cpuInfo.cpuAllowedText, 因此 ArkTS 侧不动也
    // 能在每项 note 里看到它; 这个字段供将来要把"标称 vs 实际"单列一栏时直接取用。
    // 内容形如:
    //   "运行时频率 中位 1220MHz / 最小 1150MHz / 最大 2270MHz(本核标称上限 2270MHz) 采样 12 次;
    //    采样核 = 本线程所在核(cpu8); 口径 = 跑本项负载的那个线程在每次采样时刻所在的核, 不是
    //    全机所有核; 源 scaling_cur_freq 全部读到(errno=0; 本项 12/12 次); governor=schedutil"
    out += ",\"runFreq\":\"" + escapeJson(r.runFreq) + "\"";
    // ---- QoS 运行条件(旁路字段, 2026-10 新增; 不计分) ----
    // 用户要求"必须在报告里标注『本次跑分是否启用了 QoS 及其等级』"。
    // 这里给出一份结构化的同源文本(与 note 里追加的那一段逐字相同),
    // 内容含: 是否启用 / 负载与旁路各自用了哪一档 / 每个调用的返回值与 errno /
    //         libqos.so 与 canIUse 的探测结果 / "设-不设"对照的份额比。
    // 口径与降级策略见 qos_priority.h; 它不改任何 metric/unit/k/conv/计分公式。
    out += ",\"qos\":\"" + escapeJson(r.qos) + "\"";
    // ---- 只读分层诊断(旁路字段, 2026-10 新增; 不计分) ----
    // cgroup / cpuset 分组 / core_ctl 的逐项 errno 与"限制在哪一层"的结论。
    // 全部只读(fopen "r" + opendir/readdir), 一个字节都不写这些节点; 口径见 cpuset_probe.h。
    out += ",\"cpuset\":\"" + escapeJson(r.cpuset) + "\"";
    // ---- 可重复性 / 离散度(2026-08-31 新增; 同样是旁路字段, 不改任何负载与计分) ----
    // 逐轮原始值 + 中位/最小/最大/相对离散度 + 可信度判定(阈值是我们自己定的)。
    // 代表值是中位, 不是最好一轮 —— 本轮 r.score / r.metric 本身就是中位值。
    // ArkTS 侧接线(回报里给了完整片段; 本文件不做 ets 改动):
    //   model/BenchModel.ets 的 Gb7Result 增加 rounds / repeatability(原样存 JSON 文本即可);
    //   service/BenchRunner.ets 的 withCpuFields() 里 r.rounds = (o.rounds as number) ?? 1;
    //   报告 JSON 的 items[] 里加 rounds / repeatScoreSpread 两个数即可。
    out += gb7RepeatabilityJson(r);
    out += ",\"basis\":\"" + escapeJson(gb7TestBasis(w->id)) + "\"}";
    w->result = out;
}

napi_value RunGb7(napi_env env, napi_callback_info info)
{
    // 参数: (id, threads, options?)。
    //  第三个参数是后加的(2026-08-31), 老调用方只传两个参数照样工作  ——
    //   此时 options 为空串, 重复轮数取默认值(GB7_DEFAULT_ROUNDS = 2)。
    //   它唯一的用途是"同一项测量做几次"(可重复性/离散度); 单轮的负载工作量一个字都没动。
    size_t argc = 3;
    napi_value args[3] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t id = 0;
    int32_t threads = 1;
    napi_get_value_int32(env, args[0], &id);
    if (argc > 1) {
        napi_get_value_int32(env, args[1], &threads);
    }
    std::string options;
    if (argc > 2) {
        options = jsStringArg(env, args[2]);   // 非字符串 / undefined -> 空串(走默认值)
    }
    auto* w = new AsyncTestWork();
    w->env = env;
    w->id = id;
    w->threads = threads;
    w->options = options;
    napi_value promise = nullptr;
    napi_create_promise(env, &w->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "auroragb7", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName, ExecuteGb7, CompleteTest, w, &w->work);
    napi_queue_async_work(env, w->work);
    return promise;
}

napi_value Gb7Count(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out = nullptr;
    napi_create_int32(env, gb7TestCount(), &out);
    return out;
}

napi_value Gb7Name(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t id = 0;
    napi_get_value_int32(env, args[0], &id);
    std::string name = gb7TestName(id);
    napi_value out = nullptr;
    napi_create_string_utf8(env, name.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value RunCoreMark(napi_env env, napi_callback_info info)
{
    (void)info;
    auto* w = new AsyncTestWork();
    w->env = env;
    w->id = -1;
    w->threads = 1;
    napi_value promise = nullptr;
    napi_create_promise(env, &w->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "auroracoremark", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName, ExecuteCoreMark, CompleteTest, w, &w->work);
    napi_queue_async_work(env, w->work);
    return promise;
}

void ExecuteTest(napi_env env, void* data)
{
    (void)env;
    auto* w = static_cast<AsyncTestWork*>(data);
    // 同样的崩溃防护: 未捕获的 C++ 异常会 terminate 掉整个进程。
    try {
        const std::string curName = auroraTestName(w->id);
        auroraSetCurrentItem(w->threads > 1 ? "CPU 多核" : "CPU 单核", curName.c_str(), w->id + 1, auroraTestCount());
        BenchOutcome r = auroraRunTest(w->id, w->threads);
        markIdle();
        // 自研套件每一项的落核 / 标称上限 / 运行时频率取证(2026-10 补; 与 CS1 路径同口径)。
        // 为什么必须补: 此前自研套件这一串字段在报告里全是 cpu=-1 · maxKhz=0, 于是
        // "这一项跑在哪颗核上"这个最基本的问题答不了 —— 而"两台同芯片设备自研单核差 37.7%、
        // CS1 单核只差 0.24%"这件事的根因正藏在这里。
        // 全部是旁路诊断: 不参与 metric / unit / k / conv / 计分公式, 也不进任何复合分。
        char cpuNum[768];
        snprintf(cpuNum, sizeof(cpuNum),
                 ",\"cpu\":%d,\"cpuAtStart\":%d,\"cpuMaxKhz\":%d,\"cpuRank\":%d,"
                 "\"cpuBound\":%s,\"cpuInFastClusterJudged\":%s,\"cpuInFastCluster\":%s,"
                 "\"cpuInMachineTopTier\":%s,\"cpuFastClusterCores\":%d,"
                 "\"cpuFastClusterMaxKhz\":%d,\"cpuMachineTopTierCores\":%d,"
                 "\"cpuMachineTopTierKhz\":%d,\"cpuAllowedCount\":%d,"
                 "\"cpuSingleTargetCores\":%d",
                 r.cpu, r.cpuAtStart, r.cpuMaxKhz, r.cpuRank,
                 r.cpuBound ? "true" : "false",
                 r.cpuInFastClusterJudged ? "true" : "false",
                 r.cpuInFastCluster ? "true" : "false",
                 r.cpuInMachineTopTier ? "true" : "false",
                 r.cpuFastClusterCores, r.cpuFastClusterMaxKhz,
                 r.cpuMachineTopTierCores, r.cpuMachineTopTierKhz,
                 r.cpuAllowedCount, r.cpuSingleTargetCores);
        w->result = "{\"name\":\"" + escapeJson(r.name) + "\",\"ms\":" + std::to_string(r.ms) +
            ",\"score\":" + std::to_string(r.score) + ",\"detail\":\"" + escapeJson(r.detail) +
            "\",\"metric\":" + std::to_string(r.metric) + ",\"unit\":\"" + escapeJson(r.unit) +
            "\",\"parallelism\":" + std::to_string(r.parallelism) + std::string(cpuNum) +
            ",\"cpuInfo\":\"" + escapeJson(r.cpuInfo) + "\"" +
            ",\"runFreq\":\"" + escapeJson(r.runFreq) + "\"}";
    } catch (const std::exception& e) {
        w->result = "{\"name\":\"" + escapeJson(auroraTestName(w->id)) + "\",\"ms\":0,\"score\":0," +
            "\"detail\":\"\",\"metric\":0,\"unit\":\"运行失败: " +
            escapeJson(std::string("C++ 异常 ") + e.what()) + "\",\"parallelism\":0}";
    } catch (...) {
        w->result = "{\"name\":\"" + escapeJson(auroraTestName(w->id)) + "\",\"ms\":0,\"score\":0," +
            "\"detail\":\"\",\"metric\":0,\"unit\":\"运行失败: 未知 C++ 异常\",\"parallelism\":0}";
    }
}

void CompleteTest(napi_env env, napi_status status, void* data)
{
    auto* w = static_cast<AsyncTestWork*>(data);
    napi_value value = nullptr;
    if (status == napi_ok && napi_create_string_utf8(env, w->result.c_str(), NAPI_AUTO_LENGTH, &value) == napi_ok) {
        napi_resolve_deferred(env, w->deferred, value);
    } else {
        napi_value message = nullptr;
        napi_value error = nullptr;
        napi_create_string_utf8(env, "benchmark failed", NAPI_AUTO_LENGTH, &message);
        napi_create_error(env, nullptr, message, &error);
        napi_reject_deferred(env, w->deferred, error);
    }
    napi_delete_async_work(env, w->work);
    delete w;
}

napi_value CpuCoreCount(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out = nullptr;
    napi_create_int32(env, auroraCpuCount(), &out);
    return out;
}

napi_value TestCount(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out = nullptr;
    napi_create_int32(env, auroraTestCount(), &out);
    return out;
}

napi_value TestName(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t id = 0;
    napi_get_value_int32(env, args[0], &id);
    std::string name = auroraTestName(id);
    napi_value out = nullptr;
    napi_create_string_utf8(env, name.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value Version(napi_env env, napi_callback_info info)
{
    (void)info;
    std::string v = auroraVersion();
    napi_value out = nullptr;
    napi_create_string_utf8(env, v.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

// ---- GB7 官方口径元数据与复合分 ----
// 该负载是否属于官方多核 8 项(多核阶段按它过滤显示)
napi_value Gb7TestIsMulti(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t id = -1;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_int32(env, args[0], &id);
    }
    napi_value out = nullptr;
    napi_get_boolean(env, gb7TestIsMulti(id), &out);
    return out;
}

// 该负载在注册表里是不是计分项(gb7TestScored(id), 即 ENTRIES[id].scored)
//   为什么必须单独导出一个: "这一项得 0 分"有两种完全不同的原因, 界面上必须分得开 ——
//     ① scored=false(例如 Structure from Motion, k=0, 口径不可复核, 故意剔除出复合分):
//        得 0 是设计如此, 是中性事实;
//     ② scored=true 但没跑出吞吐(例如 Clang 的 chibicc 33 轮全失败): 这是运行失败。
//   以前 ArkTS 只能看 score <= 0, 于是把这两种状态写成了同一个"未计分"标签。
//   判据来自注册表本身(唯一来源), 不含任何按机型/SoC 的分支, 也不改任何数值与计分。
napi_value Gb7TestIsScored(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t id = -1;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_int32(env, args[0], &id);
    }
    napi_value out = nullptr;
    napi_get_boolean(env, gb7TestScored(id), &out);
    return out;
}

// 该项的单位换算与系数说明(UI 展示)
napi_value Gb7TestBasis(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t id = -1;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_int32(env, args[0], &id);
    }
    std::string basis = gb7TestBasis(id);
    napi_value out = nullptr;
    napi_create_string_utf8(env, basis.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

// 可选参数: number[] 单项分(按负载 id 索引); 不传则用最近一次 runGb7 的结果
napi_value Gb7CompositeImpl(napi_env env, napi_callback_info info, bool multi)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double buf[32];
    double* scores = nullptr;
    uint32_t n = 0;
    bool isArray = false;
    if (argc > 0 && args[0] != nullptr && napi_is_array(env, args[0], &isArray) == napi_ok && isArray &&
        napi_get_array_length(env, args[0], &n) == napi_ok && n > 0) {
        if (n > 32) {
            n = 32;
        }
        for (uint32_t i = 0; i < n; ++i) {
            napi_value v = nullptr;
            buf[i] = 0.0;
            if (napi_get_element(env, args[0], i, &v) == napi_ok) {
                napi_get_value_double(env, v, &buf[i]);
            }
        }
        scores = buf;
    }
    std::string json = multi ? gb7CompositeMulti(scores, (int)n) : gb7CompositeSingle(scores, (int)n);
    napi_value out = nullptr;
    napi_create_string_utf8(env, json.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

// 单核复合分 = 已计分项几何平均(SfM 未计分被剔除)
napi_value Gb7CompositeSingle(napi_env env, napi_callback_info info)
{
    return Gb7CompositeImpl(env, info, false);
}

// 多核复合分 = 官方多核 8 项几何平均
napi_value Gb7CompositeMulti(napi_env env, napi_callback_info info)
{
    return Gb7CompositeImpl(env, info, true);
}

napi_value RunTest(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t id = 0;
    int32_t threads = 1;
    napi_get_value_int32(env, args[0], &id);
    napi_get_value_int32(env, args[1], &threads);
    if (threads < 1) {
        threads = 1;
    }
    auto* w = new AsyncTestWork();
    w->env = env;
    w->id = id;
    w->threads = threads;
    napi_value promise = nullptr;
    napi_create_promise(env, &w->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "aurorabench", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName, ExecuteTest, CompleteTest, w, &w->work);
    napi_queue_async_work(env, w->work);
    return promise;
}

// ===========================================================================
// 崩溃取证装置的 napi 接线(只记录, 不改任何负载行为)
// ===========================================================================

// setLogDir(filesDir: string): boolean —— ArkTS 侧传 context.filesDir。
// 在这里把 <filesDir>/native_crash.txt 用 O_APPEND 打开并常驻 fd,
// 崩溃时处理器只 write() 这个 fd, 不做 open/malloc。
napi_value SetLogDir(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string dir = jsStringArg(env, argc > 0 ? args[0] : nullptr);
    const bool ok = (auroraSetLogDir(dir.empty() ? nullptr : dir.c_str()) != 0);
    napi_value out = nullptr;
    napi_get_boolean(env, ok, &out);
    return out;
}

// takeNativeCrashReport(): string —— 启动自检。空串 = 磁盘上没有崩溃记录。
napi_value TakeNativeCrashReport(napi_env env, napi_callback_info info)
{
    (void)info;
    const int cap = 64 * 1024;
    std::string buf;
    buf.resize(static_cast<size_t>(cap));
    const int n = auroraTakeCrashReport(buf.data(), cap);
    napi_value out = nullptr;
    if (n <= 0) {
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &out);
    } else {
        napi_create_string_utf8(env, buf.data(), static_cast<size_t>(n), &out);
    }
    return out;
}

// clearNativeCrashReport(): void —— 横幅展示完清盘(下次启动就不会重复报)。
napi_value ClearNativeCrashReport(napi_env env, napi_callback_info info)
{
    (void)info;
    auroraClearCrashReport();
    napi_value out = nullptr;
    napi_get_undefined(env, &out);
    return out;
}

// nativeCrashLogPath(): string
napi_value NativeCrashLogPath(napi_env env, napi_callback_info info)
{
    (void)info;
    const char* p = auroraCrashLogPath();
    napi_value out = nullptr;
    napi_create_string_utf8(env, p != nullptr ? p : "", NAPI_AUTO_LENGTH, &out);
    return out;
}

// nativeCrashGuardReady(): boolean —— 信号处理器是否已装好(用于自检显示)。
napi_value NativeCrashGuardReady(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out = nullptr;
    napi_get_boolean(env, auroraCrashGuardReady() != 0, &out);
    return out;
}

// sampleMemory(): void —— 采样一次内存足迹(建议每项开跑前调)
napi_value SampleMemory(napi_env env, napi_callback_info info)
{
    (void)info;
    auroraSampleMemory();
    napi_value out = nullptr;
    napi_get_undefined(env, &out);
    return out;
}

// lastMemorySample(): string —— 最近一次采样, 空串 = 还没采过
napi_value LastMemorySample(napi_env env, napi_callback_info info)
{
    (void)info;
    char buf[256];
    const int n = auroraLastMemory(buf, (int)sizeof(buf));
    napi_value out = nullptr;
    if (n <= 0) {
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &out);
    } else {
        napi_create_string_utf8(env, buf, static_cast<size_t>(n), &out);
    }
    return out;
}

// itemMemoryReport(): string —— 逐项地址空间归因(VmSize/VmRSS/VmHWM + 相对上一项的增量)。
// 旁路诊断: native 在每个 item 标记点采一次 /proc/self/status, 这里把整表取出来。
// 空串 = 本次会话还没跑过任何一项(或没采到)。
// 它不计分, 也不影响任何负载; 存在的唯一理由是回答一个真机问题:
//   「这一轮的虚拟地址空间到底是哪一项撑起来的」——实测 VmSize 到过 14 GB 而 VmRSS 只有 200 MB。
// 同一份渲染函数也用在崩溃记录里(两处同源, 不会自相矛盾)。
napi_value ItemMemoryReport(napi_env env, napi_callback_info info)
{
    (void)info;
    const int cap = 32 * 1024;
    std::string buf;
    buf.resize(static_cast<size_t>(cap));
    const int n = auroraItemMemoryReport(buf.data(), cap);
    napi_value out = nullptr;
    if (n <= 0) {
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &out);
    } else {
        napi_create_string_utf8(env, buf.data(), static_cast<size_t>(n), &out);
    }
    return out;
}

// itemMemoryLogPath(): string —— 逐项账目落盘文件(逐行追加, 96 KB 上限)。
// 真机上进程最可能是被 SIGKILL(连崩溃记录都没有), 那份文件是盘上唯一的地址空间归因。
// 空串 = 还没调 setLogDir。
napi_value ItemMemoryLogPath(napi_env env, napi_callback_info info)
{
    (void)info;
    const char* p = auroraItemMemoryLogPath();
    napi_value out = nullptr;
    napi_create_string_utf8(env, p != nullptr ? p : "", NAPI_AUTO_LENGTH, &out);
    return out;
}

// takeNativeHangReport(): string —— 卡死取证。空串 = 磁盘上没有卡死记录。
napi_value TakeNativeHangReport(napi_env env, napi_callback_info info)
{
    (void)info;
    const int cap = 64 * 1024;
    std::string buf;
    buf.resize(static_cast<size_t>(cap));
    const int n = auroraTakeHangReport(buf.data(), cap);
    napi_value out = nullptr;
    if (n <= 0) {
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &out);
    } else {
        napi_create_string_utf8(env, buf.data(), static_cast<size_t>(n), &out);
    }
    return out;
}

// clearNativeHangReport(): void
napi_value ClearNativeHangReport(napi_env env, napi_callback_info info)
{
    (void)info;
    auroraClearHangReport();
    napi_value out = nullptr;
    napi_get_undefined(env, &out);
    return out;
}

// nativeHangLogPath(): string
napi_value NativeHangLogPath(napi_env env, napi_callback_info info)
{
    (void)info;
    const char* p = auroraHangLogPath();
    napi_value out = nullptr;
    napi_create_string_utf8(env, p != nullptr ? p : "", NAPI_AUTO_LENGTH, &out);
    return out;
}

// hangStatus(): string —— 一行状态(看门狗开关/阈值/已采样次数/展开器来源/当前项与已运行秒数)
napi_value HangStatus(napi_env env, napi_callback_info info)
{
    (void)info;
    char buf[512];
    const int n = auroraHangStatus(buf, (int)sizeof(buf));
    napi_value out = nullptr;
    if (n <= 0) {
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &out);
    } else {
        napi_create_string_utf8(env, buf, static_cast<size_t>(n), &out);
    }
    return out;
}

// hangSelfTest(seconds: number): boolean —— 起一个空转线程并标成当前项,
// 30 秒后看门狗应当自动向它要栈并写进 native_hang.txt。
napi_value HangSelfTest(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t seconds = 45;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_int32(env, args[0], &seconds);
    }
    napi_value out = nullptr;
    napi_get_boolean(env, auroraHangSelfTest(seconds) != 0, &out);
    return out;
}

// crashSelfTest(mode: number): void —— 真机自检钩子(排障用):
// 0 = 只写一条链路自检记录(不崩, 用来验证 setLogDir/takeNativeCrashReport 通了);
// 1 = 故意空指针写触发 SIGSEGV(进程会退出, 走完整崩溃取证路径);
// 2 = raise(SIGABRT)。
napi_value CrashSelfTest(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t mode = 0;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_int32(env, args[0], &mode);
    }
    auroraCrashSelfTest(mode);
    napi_value out = nullptr;
    napi_get_undefined(env, &out);
    return out;
}

// ===========================================================================
// 超线程(SMT): napi 接线
//
// 口径(与 cpu_affinity.h 的文件头严格一致):
//   * 开关只决定多核阶段使用哪些逻辑核 —— 线程数(ArkTS 侧取 smtThreadsFor)、
//     大核簇掩码、池线程落点分配; 不改任何负载的算法 / 工作量 / metric / 计分公式;
//   * 单核阶段完全不受影响(仍然只用一个线程跑在一个核上);
//   * 读不到拓扑时不绑错、不假装: smtCapabilities().known = false, 线程数维持全部逻辑核,
//     界面按 known 字段注明"拓扑未知, SMT 开关无效"。
//   * 默认值 = 开, 且 native 侧的初值就是开(见 cpu_affinity.h 的 smtFlag), 因此不调用
//     setSmtEnabled 时行为与历史完全一致。
// ===========================================================================

// smtCapabilities(): string —— 一行 JSON, 字段见 types/libaurorabench/index.d.ts
// {"ok":true,"known":bool,"logical":n,"physical":n,"smt":bool,"smtPossible":bool,
//  "smtEnabled":bool,"source":"...","threadsMulti":n,"threadsMultiFull":n,
//  "threadCap":n,"requestedAsIs":bool,
//  "effectiveMask":"0x..","physicalOfCpu":[..],"text":"..."}
// 2026-10-06 threadsMulti 现在是实际会开的线程数(已过可用核集合/物理核的夹子),
//   threadsMultiFull 才是"不看可用核集合时本应几个"; threadCap 是那个夹子(< = 0 表示不限制)。
//   以前 threadsMulti = 全机口径(真机 14), 而实际只会开 9 个 —— 界面就在骗人。
napi_value SmtCapabilities(napi_env env, napi_callback_info info)
{
    (void)info;
    const AuroraCpuTopoInfo& t = auroraSmtTopology();
    const AuroraCpuEffectiveSet& es = auroraEffectiveSet();
    const int on = auroraSmtEnabled() ? 1 : 0;
    const int logical = t.logical;
    const int physical = t.physical;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"known\":%s,\"logical\":%d,\"physical\":%d,\"smt\":%s,"
             "\"smtPossible\":%s,\"smtEnabled\":%s,\"threadsMulti\":%d,\"threadsMultiFull\":%d,"
             "\"threadCap\":%d,\"requestedAsIs\":%s,"
             "\"effectiveMask\":\"0x%llx\"",
             t.known ? "true" : "false", logical, physical,
             t.smtPossible ? "true" : "false",
             t.smtPossible ? "true" : "false",
             on ? "true" : "false",
             // threadsMulti = 多核阶段实际会开的线程数(auroraSmtThreadsCapped):
             //   可用核集合比全机口径窄时(真机 allowed=0-8 而逻辑核 14), 这里就是被夹后的 9。
             // threadsMultiFull = 不看可用核集合时本应几个(真机 14) —— 两个数都来自本进程读到的
             //   可用核集合, 没有任何按机型/SoC 的分支。
             auroraSmtThreadsCapped(),
             auroraSmtThreads(),
             auroraThreadCap(),
             // requestedAsIs: ArkTS 侧把用户请求的线程数原样传下去是否等于 native 的选择。
             //   口径收敛成"请求全部逻辑核时, native 是否会原样放行"(开关关 / 可用核集合更窄都会
             //   被夹, 那时为 false) —— 直接问 native 那一个函数, 不再自己拼条件。
             (logical > 0 && auroraSmtThreadsFor(logical) == logical) ? "true" : "false",
             es.mask);
    std::string out(buf);
    out += ",\"source\":\"" + escapeJson(std::string(t.sourceText)) + "\"";
    // probes: 逐路径实测结果(在 App 自己的 SELinux 域下每个候选文件到底读到了什么 + errno)。
    // 值 13 = EACCES(权限不足), 2 = ENOENT(路径不存在), 0 = 读到了, -1/-2 = 读到了但内容为空/异常。
    out += ",\"probes\":\"" + escapeJson(std::string(t.probeText)) + "\"";
    // 2026-10-07 新增(纯新增字段, 老字段语义一个都不动)
    //   effectiveCount = 实际使用集合大小(与 threadsMulti/text 同一个数, 界面上不再需要自己算);
    //   switchHint     = 开关的真实约束说明(含 physical / effectiveCount / cap 三个数),
    //                    界面把它原样显示出来即可 —— 措辞只有 native 这一处。
    out += ",\"effectiveCount\":" + std::to_string(es.count);
    out += ",\"switchHint\":\"" + escapeJson(auroraSmtSwitchHint()) + "\"";
    out += ",\"text\":\"" + escapeJson(auroraSmtTopologyText()) + "\"";
    out += ",\"physicalOfCpu\":[";
    for (int c = 0; c < logical && c < 64; ++c) {
        char one[16];
        snprintf(one, sizeof(one), "%s%d", (c == 0 ? "" : ","),
                 (c < kMaxTopoCpus ? t.physicalOfCpu[c] : -1));
        out += one;
    }
    out += "]}";
    napi_value v = nullptr;
    napi_create_string_utf8(env, out.c_str(), NAPI_AUTO_LENGTH, &v);
    return v;
}

// ===========================================================================
// 芯片判读探测: 逐核原始频率读数 + /proc/cpuinfo 的 MIDR(只读)
//
// 用途(与 cpu_affinity.h 的 aurora_chip_probe 一节严格对应):
//   * 把 cpu0..cpuN-1 的 cpufreq/cpuinfo_max_freq 逐个读出来, 连"读失败"的核
//     也带它自己的 errno 一起上报 —— 目的是让"是漏了核还是真的只有三档"一眼可见;
//   * 把"绑核用的频率表"(readCoreMaxFreqKhz 在第一个打不开的核处 break 得到的那张表)
//     与"逐核探测的全集"摆在一起对比: probeMaxKhz > tableMaxKhz 就是"漏掉了更高频的核",
//     也就是"大核簇可能建在错误的频率档上"的硬证据;
//   * /proc/cpuinfo 上的 CPU implementer / part(MIDR) 是最硬的 SoC 指纹, 与频率档互相独立;
//   * 本函数不计分, 不改任何全局状态, 也不碰绑核规则 —— 纯只读探测。
//
// 数值口径: 频率一律 kHz(与 sysfs 原文一致, 不做任何四舍五入);
//           err 字段: 0 = 读到并解析成功; 13 = EACCES(权限不足); 2 = ENOENT(路径不存在);
//                     -1 = 打得开但内容为空; -2 = 读到内容但不是正的十进制数。
// ===========================================================================
napi_value CpuIdentityProbe(napi_env env, napi_callback_info info)
{
    (void)info;
    using namespace aurora_chip_probe;
    const AuroraCpuFreqProbe& f = cpuFreqProbeCached();
    const AuroraCpuinfoProbe& ci = cpuinfoProbeCached();
    const AuroraCpuTopoInfo& t = auroraSmtTopology();

    char num[192];
    std::string out = "{\"ok\":true";

    // ---- CPU 拓扑(与 smtCapabilities 同一份数据, 这里只挑判读用得上的几个) ----
    snprintf(num, sizeof(num),
             ",\"topo\":{\"logical\":%d,\"physical\":%d,\"smtPossible\":%s,\"known\":%s}",
             t.logical, t.physical, t.smtPossible ? "true" : "false", t.known ? "true" : "false");
    out += num;
    out += ",\"topoSource\":\"" + escapeJson(std::string(t.sourceText)) + "\"";

    // ---- 逐核频率(本节的重点) ----
    snprintf(num, sizeof(num),
             ",\"freq\":{\"rangeSource\":%d,\"rangeErrno\":%d,\"scanCount\":%d,\"okCount\":%d,"
             "\"firstFailCpu\":%d,\"firstFailErr\":%d,"
             "\"tableCores\":%d,\"tableMaxKhz\":%d,\"tableMaxCpu\":%d,"
             "\"probeMaxKhz\":%d,\"probeMaxCpu\":%d,"
             "\"truncated\":%s,\"missedFaster\":%s,\"physCount\":%d",
             f.rangeSource, (f.rangeSource == 0) ? f.presentErrno : f.possibleErrno,
             f.scanCount, f.okCount, f.firstFailCpu, f.firstFailErr,
             f.tableCores, f.tableMaxKhz, f.tableMaxCpu,
             f.probeMaxKhz, f.probeMaxCpu,
             f.truncated ? "true" : "false", f.missedFaster ? "true" : "false", f.physCount);
    out += num;
    out += ",\"range\":\"" + escapeJson(std::string(f.rangeText)) + "\"";
    out += ",\"perCore\":\"" + escapeJson(std::string(f.perCoreText)) + "\"";
    out += ",\"verdict\":\"" + escapeJson(std::string(f.verdictText)) + "\"";
    // 逐核明细(核号 / 原始字符串 / kHz / errno)
    out += ",\"cores\":[";
    for (int i = 0; i < f.scanCount; ++i) {
        char one[160];
        snprintf(one, sizeof(one),
                 "%s{\"cpu\":%d,\"raw\":\"%s\",\"khz\":%d,\"err\":%d}",
                 (i == 0 ? "" : ","), f.s[i].cpu, escapeJson(std::string(f.s[i].raw)).c_str(),
                 f.s[i].khz, f.s[i].err);
        out += one;
    }
    out += "]";
    // 频率档(逐核口径 + 物理核归并口径)
    out += ",\"tiers\":[";
    for (int i = 0; i < f.tierN; ++i) {
        char one[64];
        snprintf(one, sizeof(one), "%s{\"khz\":%d,\"count\":%d}", (i == 0 ? "" : ","),
                 f.tierKhz[i], f.tierCount[i]);
        out += one;
    }
    out += "],\"physTiers\":[";
    for (int i = 0; i < f.physTierN; ++i) {
        char one[64];
        snprintf(one, sizeof(one), "%s{\"khz\":%d,\"count\":%d}", (i == 0 ? "" : ","),
                 f.physTierKhz[i], f.physTierCount[i]);
        out += one;
    }
    out += "]}";

    // ---- /proc/cpuinfo + MIDR ----
    snprintf(num, sizeof(num),
             ",\"cpuinfo\":{\"ok\":%s,\"errno\":%d,\"processorLines\":%d,\"comboCount\":%d",
             ci.ok ? "true" : "false", ci.openErrno, ci.processorLines, ci.comboCount);
    out += num;
    out += ",\"hardware\":\"" + escapeJson(std::string(ci.hardware)) + "\"";
    out += ",\"modelName\":\"" + escapeJson(std::string(ci.modelName)) + "\"";
    out += ",\"partDecimal\":\"" + escapeJson(std::string(ci.partDecimal)) + "\"";
    out += ",\"text\":\"" + escapeJson(std::string(ci.text)) + "\"";
    out += ",\"midr\":[";
    for (int i = 0; i < ci.comboCount; ++i) {
        char one[256];
        snprintf(one, sizeof(one),
                 "%s{\"implementer\":\"%s\",\"architecture\":\"%s\",\"variant\":\"%s\","
                 "\"part\":\"%s\",\"partDecimal\":%d,\"revision\":\"%s\",\"count\":%d}",
                 (i == 0 ? "" : ","),
                 escapeJson(std::string(ci.combo[i].implementer)).c_str(),
                 escapeJson(std::string(ci.combo[i].architecture)).c_str(),
                 escapeJson(std::string(ci.combo[i].variant)).c_str(),
                 escapeJson(std::string(ci.combo[i].part)).c_str(),
                 partNumberToDecimal(ci.combo[i].part),
                 escapeJson(std::string(ci.combo[i].revision)).c_str(),
                 ci.combo[i].count);
        out += one;
    }
    out += "]}";
    out += "}";
    napi_value v = nullptr;
    napi_create_string_utf8(env, out.c_str(), NAPI_AUTO_LENGTH, &v);
    return v;
}

// smtSetEnabled(on: boolean): boolean —— 写入开关, 返回写入后的状态
napi_value SmtSetEnabled(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool on = true;   // 默认开: 不传参数时就是"开"
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_bool(env, args[0], &on);
    }
    const int now = auroraSetSmtEnabled(on ? 1 : 0);
    napi_value v = nullptr;
    napi_get_boolean(env, now != 0, &v);
    return v;
}

// smtEnabled(): boolean
napi_value SmtEnabled(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value v = nullptr;
    napi_get_boolean(env, auroraSmtEnabled() != 0, &v);
    return v;
}

// smtThreadsFor(requested: number): number —— 把"用户请求的线程数"过一遍 SMT 口径:
//   开关开(默认) -> 原样返回 requested(与历史行为一致);
//   开关关       -> 返回物理核数(= 实际使用集合的大小);
//   拓扑未知     -> 原样返回 requested(1:1, 开关无效)。
// 单核阶段(requested <= 1)永远原样返回 1。
napi_value SmtThreadsFor(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t requested = 1;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_int32(env, args[0], &requested);
    }
    const int32_t out32 = auroraSmtThreadsFor(requested);
    napi_value v = nullptr;
    napi_create_int32(env, out32, &v);
    return v;
}

// smtSwitchHint(): string —— 超线程(SMT)开关的真实约束说明(中文一行, 不计分)。
//   为什么放在 native: 文案里的数字(逻辑核 / 物理核 / 可用核集合 / 开关前后各几个线程)
//   全部来自 native 读到的同一份拓扑与可用核集合 —— 写在 native 里, ArkTS 只把它显示出来,
//   就不会出现"界面说 9 个线程、native 实际只开 1 个"这种两边各说各话的情况。
//   要求(用户 2026-10-07): 关掉开关会少几个线程必须在开跑之前就能看到, 不许靠用户自己撞。
napi_value SmtSwitchHint(napi_env env, napi_callback_info info)
{
    (void)info;
    const std::string hint = auroraSmtSwitchHint();
    napi_value v = nullptr;
    napi_create_string_utf8(env, hint.c_str(), NAPI_AUTO_LENGTH, &v);
    return v;
}

// ceilingText(): string —— 「本机性能天花板」一行结论(中文一句, 不计分)。
//   为什么放在 native: 这句话里的每一个数(全机最快档在哪几核多少 MHz、内核实测允许哪几核、
//   生效快簇是哪几核、多核最多几线程、运行时频率占标称多少)都来自本进程自己读到的
//   sysfs/procfs 与采样结果。写在 native 里, ArkTS 只负责把它显示出来 —— 首页 / 结果页 /
//   报告三处显示的是同一句话, 不可能出现"三个地方各说各话"。
//   一句话要回答的是: 这台机器到底被什么卡住(芯片有更快的地方 / 系统不让我们进去 / 我们
//   实际能到哪一档)。读不到的部分一律写"读不到", 不用 0 冒充。
napi_value CeilingText(napi_env env, napi_callback_info info)
{
    (void)info;
    char buf[2048];
    buf[0] = '\0';
    (void)auroraCeilingText(buf, (int)sizeof(buf));
    napi_value v = nullptr;
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &v);
    return v;
}

// setCurrentItem(phase: string, item: string, index: number, total: number): void
// napi 内部(见 ExecuteGb7Body/ExecuteCoreMark/ExecuteTest)已经自动标记 native 负载;
// 这个导出是给 ArkTS 侧标记“native 之外的阶段”(例如 CS1 GPU / 自定义阶段)用。
napi_value SetCurrentItem(napi_env env, napi_callback_info info)
{
    size_t argc = 4;
    napi_value args[4] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string phase = jsStringArg(env, argc > 0 ? args[0] : nullptr);
    const std::string item = jsStringArg(env, argc > 1 ? args[1] : nullptr);
    int32_t index = 0;
    int32_t total = 0;
    if (argc > 2 && args[2] != nullptr) {
        napi_get_value_int32(env, args[2], &index);
    }
    if (argc > 3 && args[3] != nullptr) {
        napi_get_value_int32(env, args[3], &total);
    }
    auroraSetCurrentItem(phase.c_str(), item.c_str(), index, total);
    napi_value out = nullptr;
    napi_get_undefined(env, &out);
    return out;
}

// setIdleMarker(): void —— 把取证标记置成"无负载运行中"(空闲)。
//   为什么必须导出: CS1 GPU 阶段的每一项是在 **libauroragpu7.so** 里跑完的, 而"当前项"
//   标记住在本模块(crash_guard.cpp)里。ArkTS 只在本项开跑前调一次 setCurrentItem,
//   最后一项跑完之后就再没有人把标记清成空闲 —— 看门狗于是把已经结束的 "Video Filter"
//   (index=11/11)当成"已跑 203 秒"的卡死, 每秒写一条现场(真机 native_hang.txt 160,606
//   字节就是这么来的)。
//   GPU 模块通过 napi_load_module("aurorabench") 拿到本模块的导出再调这个函数 ——
//   这样调到的一定是 ArkTS 正在用的那一个实例, 不存在"dlopen 出第二份副本、标记写到
//   别处去"的风险(dlopen 只作为兜底)。
//   语义与其它 native 阶段的收尾完全一致: 一律走 auroraSetIdleMarker()(它会把 g_idle 置 1,
//   看门狗据此不判定卡死), 不自己拼那两句空闲文案。
napi_value SetIdleMarker(napi_env env, napi_callback_info info)
{
    (void)info;
    auroraSetIdleMarker();
    napi_value out = nullptr;
    napi_get_undefined(env, &out);
    return out;
}

// ===========================================================================
// NPU(AI 加速器)跑分 —— 独立小节, 结果不参与 GB7 计分(实现见 npu_bench.cpp)
//
// 两个导出都返回 Promise<string>, 内容是 JSON 字符串(结构见 npu_bench.h 的注释):
//   npuProbe(): 第一步设备探测 —— 这台机器有没有向第三方应用开放 NPU
//   npuBench(scale?, warmup?, iters?, budgetMs?): 第二步推理跑分
// 走 napi 异步 worker: 跑分本体 1~3 秒, 放 UI 线程上会卡界面。
// 失败时 native 侧一定返回带原始 OH_NN_ReturnCode 的失败 JSON, 这里只负责把它原样
// 送到 ArkTS —— 不做任何"失败当 0 分"的转换。
// ===========================================================================
struct AsyncNpuWork {
    napi_env env = nullptr;
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    int mode = 0;          // 0 = 设备探测, 1 = 推理跑分
    int scale = 1;
    int warmup = 2;
    int iters = 10;
    int budgetMs = 1200;
    std::string result;
};

void ExecuteNpu(napi_env env, void* data)
{
    (void)env;
    auto* w = static_cast<AsyncNpuWork*>(data);
    // 崩溃取证的当前项标记: 走 NNRt 会进厂商驱动, 万一崩在那里, 证据文件里要能看出是哪一步。
    if (w->mode == 0) {
        auroraSetCurrentItem("NPU 探测", "NNRt 设备枚举", 1, 1);
    } else {
        auroraSetCurrentItem("NPU 跑分", "MATMUL 链推理", 1, 1);
    }
    try {
        if (w->mode == 0) {
            w->result = npuProbeJson();
        } else {
            NpuBenchRequest req;
            req.scale = w->scale;
            req.warmup = w->warmup;
            req.iters = w->iters;
            req.budgetMs = w->budgetMs;
            w->result = npuBenchJson(req);
        }
    } catch (const std::exception& e) {
        w->result = npuFailureJson(std::string("C++ 异常: ") + e.what());
    } catch (...) {
        w->result = npuFailureJson("未知 C++ 异常(可能是内存不足)");
    }
    markIdle();
}

void CompleteNpu(napi_env env, napi_status status, void* data)
{
    auto* w = static_cast<AsyncNpuWork*>(data);
    napi_value value = nullptr;
    if (status == napi_ok &&
        napi_create_string_utf8(env, w->result.c_str(), NAPI_AUTO_LENGTH, &value) == napi_ok) {
        napi_resolve_deferred(env, w->deferred, value);
    } else {
        // 连字符串都造不出来时也要让调用方看到原因, 不能静默 reject 一个空错误。
        napi_value message = nullptr;
        napi_value error = nullptr;
        napi_create_string_utf8(env, "NPU 结果字符串构造失败", NAPI_AUTO_LENGTH, &message);
        napi_create_error(env, nullptr, message, &error);
        napi_reject_deferred(env, w->deferred, error);
    }
    napi_delete_async_work(env, w->work);
    delete w;
}

// npuProbe(): Promise<string> —— 第一步: 设备探测
napi_value NpuProbe(napi_env env, napi_callback_info info)
{
    (void)info;
    auto* w = new AsyncNpuWork();
    w->env = env;
    w->mode = 0;
    napi_value promise = nullptr;
    napi_create_promise(env, &w->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "auroranpuprobe", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName, ExecuteNpu, CompleteNpu, w, &w->work);
    napi_queue_async_work(env, w->work);
    return promise;
}

// npuBench(scale?: number, warmup?: number, iters?: number, budgetMs?: number): Promise<string>
// 不传参数时: scale=1(中等规模, 约 100.7M 乘加/次), warmup=2, iters=10, budgetMs=1200(毫秒)。
napi_value NpuBench(napi_env env, napi_callback_info info)
{
    size_t argc = 4;
    napi_value args[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    auto* w = new AsyncNpuWork();
    w->env = env;
    w->mode = 1;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_int32(env, args[0], &w->scale);
    }
    if (argc > 1 && args[1] != nullptr) {
        napi_get_value_int32(env, args[1], &w->warmup);
    }
    if (argc > 2 && args[2] != nullptr) {
        napi_get_value_int32(env, args[2], &w->iters);
    }
    if (argc > 3 && args[3] != nullptr) {
        napi_get_value_int32(env, args[3], &w->budgetMs);
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &w->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "auroranpubench", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName, ExecuteNpu, CompleteNpu, w, &w->work);
    napi_queue_async_work(env, w->work);
    return promise;
}

// ===========================================================================
// 内核允许的核集合: napi 接线(只读探测, 旁路, 不计分)
//
// 为什么单独开一个导出: 上面 runGb7 的 cpuAllowed* 字段只回答"这一项跑的时候可用核集合
// 是什么"; 而"这台机器到底允不允许我们用最快的那几个核"是设备级的事实, 用户会在
// 「设备画像」里逐项对比两台机器 —— 所以这里给出一份设备级的快照(与 runGb7 同一份数据,
// 同一个单例, 不会各说各话)。
//
// 返回 JSON(字段名与 AuroraCpuAllowedSet / CpuTopology 严格对应):
//   {ok, allowed:{ok,count,mask,list,inThreadMask,mismatched,statusMaskOk,statusErrno,
//                 statusListFound,syscallOk,syscallErrno,memsFound,memsList,
//                 cgroupPath,cpusetDir,cpusetCpus,cpusetCpusErrno,cpusetEffective,
//                 cpusetEffErrno,text},
//    fast:{fallback,tierIndex,cores,maxKhz,mask,source,machineTopTierCores,
//          machineTopTierKhz,machineTopTierMask},
//    threads:{cap,basisFull}, line:"..."}
// ===========================================================================
napi_value CpuAffinityProbe(napi_env env, napi_callback_info info)
{
    (void)info;
    const aurora_cpu_detail::AuroraCpuAllowedSet& a = aurora_cpu_detail::allowedSetCached();
    char num[256];
    std::string out = "{\"ok\":true";
    snprintf(num, sizeof(num),
             ",\"allowed\":{\"ok\":%s,\"count\":%d,\"mask\":\"0x%llx\",\"inThreadMask\":%s,"
             "\"mismatched\":%s,\"statusMaskOk\":%s,\"statusErrno\":%d,"
             "\"statusListFound\":%s,\"syscallOk\":%s,\"syscallErrno\":%d,"
             "\"memsFound\":%s,\"cpusetCpusErrno\":%d,\"cpusetEffErrno\":%d}",
             a.ok ? "true" : "false", a.count, a.mask,
             a.inThreadMask ? "true" : "false",
             a.mismatched ? "true" : "false",
             a.statusMaskOk ? "true" : "false", a.statusErrno,
             a.statusListFound ? "true" : "false",
             a.syscallOk ? "true" : "false", a.syscallErrno,
             a.memsFound ? "true" : "false", a.cpusetCpusErrno, a.cpusetEffErrno);
    out += num;
    out += ",\"list\":\"" + escapeJson(std::string(a.listText)) + "\"";
    out += ",\"memsList\":\"" + escapeJson(std::string(a.memsList)) + "\"";
    out += ",\"cgroupPath\":\"" + escapeJson(std::string(a.cgroupPath)) + "\"";
    out += ",\"cpusetDir\":\"" + escapeJson(std::string(a.cpusetDir)) + "\"";
    out += ",\"cpusetCpus\":\"" + escapeJson(std::string(a.cpusetCpus)) + "\"";
    out += ",\"cpusetEffective\":\"" + escapeJson(std::string(a.cpusetEffective)) + "\"";
    out += ",\"source\":\"" + escapeJson(std::string(a.sourceText)) + "\"";
    out += ",\"text\":\"" + escapeJson(std::string(a.line)) + "\"";
    // ---- 双源原始读数 + 差集 + 实测探测 + 权威来源(2026-10 追加; 全是只读旁路) ----
    //  "不许只给一个 bool": 这里把三条读数的原文与逐位差集都原样交出去,
    //  界面/日志可以逐位核对; authority 明确写出"绑核决策用的是哪一个来源"。
    snprintf(num, sizeof(num),
             ",\"authority\":%d,\"probeRan\":%s,\"probeTid\":%d,\"probeUpper\":%d,"
             "\"probeAcceptedCount\":%d,\"probeAcceptedMask\":\"0x%llx\","
             "\"probeEinval\":%d,\"probeEperm\":%d,\"probeOtherFail\":%d,"
             "\"probeClampedCount\":%d,\"probeRestoreOk\":%s,"
             "\"syscallMask\":\"0x%llx\",\"statusMask\":\"0x%llx\",\"statusListMask\":\"0x%llx\","
             "\"statusMaskErrno\":%d,\"statusListErrno\":%d",
             a.authority, a.probeRan ? "true" : "false", a.probeTid, a.probeUpper,
             a.probeAcceptedCount, a.probeAcceptedMask,
             a.probeEinval, a.probeEperm, a.probeOtherFail, a.probeClampedCount,
             a.probeRestoreOk ? "true" : "false",
             a.syscallMask, a.statusMask, a.statusListMask,
             a.statusMaskErrno, a.statusListErrno);
    out += num;
    out += ",\"statusMaskRaw\":\"" + escapeJson(std::string(a.statusMaskRaw)) + "\"";
    out += ",\"statusListRaw\":\"" + escapeJson(std::string(a.statusListRaw)) + "\"";
    out += ",\"syscallCoreList\":\"" + escapeJson(std::string(a.syscallCoreList)) + "\"";
    out += ",\"statusHexCoreList\":\"" + escapeJson(std::string(a.statusHexCoreList)) + "\"";
    out += ",\"diffText\":\"" + escapeJson(std::string(a.diffText)) + "\"";
    out += ",\"probeText\":\"" + escapeJson(std::string(a.probeText)) + "\"";
    out += ",\"authorityText\":\"" + escapeJson(std::string(a.authorityText)) + "\"";
    out += ",\"tierText\":\"" + escapeJson(auroraCoreTierText()) + "\"";
    // ---- 生效快簇 + 全机最快档(两个概念分开报) ----
    snprintf(num, sizeof(num),
             ",\"fast\":{\"fallback\":%s,\"tierIndex\":%d,\"cores\":%d,\"maxKhz\":%d,"
             "\"mask\":\"0x%llx\",\"machineTopTierCores\":%d,\"machineTopTierKhz\":%d,"
             "\"machineTopTierMask\":\"0x%llx\"}",
             auroraFastClusterFallback() ? "true" : "false",
             auroraFastClusterTierIndex(),
             auroraFastClusterCoreCount(), auroraFastClusterMaxKhz(), auroraFastClusterMask(),
             auroraMachineTopTierCoreCount(), auroraMachineTopTierMaxKhz(),
             aurora_cpu_detail::topologyCached().machineTopTierMask);
    out += num;
    out += ",\"fastSource\":\"" + escapeJson(auroraFastClusterSourceText()) + "\"";
    // ---- 线程口径: cap = 本机多核阶段的实际上限(可用核集合 / 物理核数里更小的那个) ----
    snprintf(num, sizeof(num), ",\"threads\":{\"cap\":%d,\"basisFull\":%d}",
             auroraThreadCap(), auroraSmtThreads());
    out += num;
    out += ",\"line\":\"" + escapeJson(auroraAffinitySummaryText()) + "\"}";
    napi_value v = nullptr;
    napi_create_string_utf8(env, out.c_str(), NAPI_AUTO_LENGTH, &v);
    return v;
}
} // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"cpuCoreCount", nullptr, CpuCoreCount, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 内核允许的核集合 + 生效快簇 + 全机最快档(设备级只读快照; 见上面的 CpuAffinityProbe)
        {"cpuAffinityProbe", nullptr, CpuAffinityProbe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"testCount", nullptr, TestCount, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"testName", nullptr, TestName, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runTest", nullptr, RunTest, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"version", nullptr, Version, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runCoreMark", nullptr, RunCoreMark, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"gb7Count", nullptr, Gb7Count, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"gb7Name", nullptr, Gb7Name, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runGb7", nullptr, RunGb7, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"gb7TestIsMulti", nullptr, Gb7TestIsMulti, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 该项是否计分项(注册表 scored): 界面据此把"按政策不计分"与"运行失败"分开
        {"gb7TestIsScored", nullptr, Gb7TestIsScored, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"gb7TestBasis", nullptr, Gb7TestBasis, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"gb7CompositeSingle", nullptr, Gb7CompositeSingle, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"gb7CompositeMulti", nullptr, Gb7CompositeMulti, nullptr, nullptr, nullptr, napi_default, nullptr},
        // ---- 超线程(SMT): 拓扑读取 + 开关(默认开) ----
        {"smtCapabilities", nullptr, SmtCapabilities, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"smtSetEnabled", nullptr, SmtSetEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"smtEnabled", nullptr, SmtEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"smtThreadsFor", nullptr, SmtThreadsFor, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 开关的真实约束说明(中文一行; 三个数 physical / effectiveSet.count / cap 都在里面)
        {"smtSwitchHint", nullptr, SmtSwitchHint, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 「本机性能天花板」一行结论(首页 / 结果页 / 报告三处显示同一句话; 旁路, 不计分)
        {"ceilingText", nullptr, CeilingText, nullptr, nullptr, nullptr, napi_default, nullptr},
        // ---- 芯片判读探测: 逐核原始频率读数 + /proc/cpuinfo 的 MIDR(纯只读, 不参与计分) ----
        {"cpuIdentityProbe", nullptr, CpuIdentityProbe, nullptr, nullptr, nullptr, napi_default, nullptr},
        // ---- 崩溃取证 ----
        {"setLogDir", nullptr, SetLogDir, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"takeNativeCrashReport", nullptr, TakeNativeCrashReport, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"clearNativeCrashReport", nullptr, ClearNativeCrashReport, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeCrashLogPath", nullptr, NativeCrashLogPath, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeCrashGuardReady", nullptr, NativeCrashGuardReady, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setCurrentItem", nullptr, SetCurrentItem, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 收尾: 把标记置成空闲(CS1 GPU 阶段每项跑完 / 整个 run 结束时由 GPU 模块回调)
        {"setIdleMarker", nullptr, SetIdleMarker, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sampleMemory", nullptr, SampleMemory, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"lastMemorySample", nullptr, LastMemorySample, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 逐项地址空间归因(旁路: 不进分数、不影响负载)。崩溃记录里用的是同一份渲染函数。
        {"itemMemoryReport", nullptr, ItemMemoryReport, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"itemMemoryLogPath", nullptr, ItemMemoryLogPath, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"crashSelfTest", nullptr, CrashSelfTest, nullptr, nullptr, nullptr, napi_default, nullptr},
        // ---- 卡死取证(挂起看门狗 + 栈采样) ----
        {"takeNativeHangReport", nullptr, TakeNativeHangReport, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"clearNativeHangReport", nullptr, ClearNativeHangReport, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeHangLogPath", nullptr, NativeHangLogPath, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"hangStatus", nullptr, HangStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"hangSelfTest", nullptr, HangSelfTest, nullptr, nullptr, nullptr, napi_default, nullptr},
        // ---- NPU(AI 加速器)独立小节: 结果不参与 GB7 计分, 由 ArkTS 侧单独显示 ----
        {"npuProbe", nullptr, NpuProbe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"npuBench", nullptr, NpuBench, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    // ---- 存储 I/O 小节(独立文件 storage_bench.cpp; 结果不进 CS1 分数) ----
    // 导出 storageSetDir / storageInfo / storageRun / storageCleanup
    auroraStorageRegisterNapi(env, exports);
    // 参考分对照(referenceCompare / referenceRealUse)由独立模块 libauroraref.so 提供:
    // 它只读(只生成对照表, 不改任何分数), 因此不并进跑分主模块的导出表 ——
    // 独立 .so 也让"对照模块加载失败"没有能力把整轮跑分拖死(与工程对 NNRt 的结论一致)。
    return exports;
}
EXTERN_C_END

static napi_module auroraModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "aurorabench",
    .nm_priv = ((void*)0),
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterAuroraBench(void)
{
    napi_module_register(&auroraModule);
}
