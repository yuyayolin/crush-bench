#ifndef AURORA_STORAGE_BENCH_H
#define AURORA_STORAGE_BENCH_H

#include <string>

#include "napi/native_api.h"

// ============================================================================
//  storage_bench.h — 「极光跑分」存储 I/O 小节(顺序读写 + 4K 随机读写 + 元数据)
//
//   本小节的结果 不进入 CS1 单项分 / 复合分 
//     GB7 没有存储项, 混进去就毁掉可比性。这里给的是独立的一组硬件指标,
//     由 ArkTS 侧接线到 UI 的独立小节。
//
//  ---------------------------------------------------------------------------
//  ArkTS 侧接线(参考 setLogDir 的做法, 路径一律由 ArkTS 传入, native 不猜路径):
//
//    import { storageSetDir, storageRun, storageInfo, storageCleanup }
//        from 'libaurorabench.so';
//
//    storageSetDir(context.filesDir);            // 或 context.cacheDir
//    const info = storageInfo();                 // 同步, 只读探测(空间/能力), 不写盘
//    const json = await storageRun('{"seqMiB":256}');   // 异步, 不阻塞 UI 线程
//
//    native 只在 <dir>/storage_bench/ 下放临时文件, 不碰用户可见目录;
//    无论成功/失败/异常/提前中止, 退出时都会删干净, 并调用 storageCleanup() 兜底。
//  ---------------------------------------------------------------------------
//
//  度量项与口径(逐条, 详见 storage_bench.cpp 文件头与结果 JSON 的 notes):
//    ① 顺序写(不 fsync)  —— 只到 write() 返回, 数字是上界
//    ② 顺序写(fsync 落盘) —— write + fsync 的总时间, 更接近真实
//    ③ 顺序读(页缓存命中) —— 刚写完的对照值, 明确标注"被放大"
//    ④ 顺序读(绕过页缓存) —— 优先 O_DIRECT, 退 posix_fadvise(DONTNEED)
//    ⑤ 4K 随机读 (IOPS)   —— 固定种子 PRNG, 覆盖整个文件区间(均匀采样)
//    ⑥ 4K 随机写 (IOPS)   —— 同样给"页缓存"与"绕过缓存"两个口径
//    ⑦ 文件创建/删除      —— N 个 4KB 小文件, 报 文件/s(元数据, 受 FS/缓存影响大)
//    每项都带耗时; 结果里有总耗时 totalMs 与预算 stoppedEarly。
// ============================================================================

// 设置沙箱根目录(通常传 context.filesDir 或 context.cacheDir)。
// 返回 1 = 可用(目录存在且能读)。调用时会顺手清掉上一次进程被杀/断电残留在
// <dir>/storage_bench 下的临时文件 —— 这是"即使中途失败也要清理"的兜底。
int auroraStorageSetDir(const char* dir);

// 只读探测(不写任何文件): statvfs 空间 / /proc/meminfo / 编译期与运行期能力。
// 返回 JSON 字符串。用于 UI 在开跑前显示"能不能跑、空间够不够"。
std::string auroraStorageInfoJson();

// 跑完整小节并返回结果 JSON。optionsJson 是扁平、值全为数字的 JSON 对象,
// 支持的键(全部可选): seqMiB / randOps / smallFiles / smallFileBytes / seed /
// budgetMs / marginMiB。传空串即全默认。生效值会原样回写在结果的 config 里。
std::string auroraStorageRunJson(const std::string& optionsJson);

// 删除残留临时文件与目录。返回 1 = 已清干净(残留条目为 0)。
int auroraStorageCleanupNow();

// napi 导出注册(由 napi_init.cpp 的 Init 调用一次):
//   storageSetDir(dir: string): boolean
//   storageInfo(): string
//   storageRun(optionsJson?: string): Promise<string>
//   storageCleanup(): boolean
void auroraStorageRegisterNapi(napi_env env, napi_value exports);

#endif // AURORA_STORAGE_BENCH_H
