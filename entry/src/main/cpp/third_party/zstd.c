// ============================================================================
// Zstandard (vendored, upstream facebook/zstd v1.5.6, BSD-3-Clause / GPL-2.0 dual)
//
// 本文件是 zstd 官方 "amalgamation" 等价物: 只把 lib/ 下编译 Zstandard 所需的全部
// 翻译单元 include 进来, 供工程当作**单个静态库**使用(third_party/zstd → libzstd.a,
// 本机 target 名 zstd)。源码逐字节取自官方 v1.5.6 tag, 未做任何修改;
// 许可证原文在 third_party/zstd/LICENSE (BSD-3-Clause) 与 COPYING (GPL-2.0)。
//
// 为什么需要它: GB7 "File Compression" 官方定义明确是 "LZ4, zlib, Zstandard" 三种格式
// (见 ref/geekbench7-cpu-workloads.txt 第 22-25 行), 而本工程此前只有 LZ4 + zlib;
// 缺 Zstandard 是该项每 MB 工作量比 GB7 轻一个数量级的主因之一。
//
// 编译开关(均为官方 supported configuration, 不改算法/不改输出格式):
//   ZSTD_LEGACY_SUPPORT=0   不链接 v0.1~v0.7 旧格式解码器(我们只压缩/解压自己产出的
//                           当代格式帧, 不需要回溯兼容 2016 年以前的帧)
//   ZSTD_MULTITHREAD        启用官方多线程压缩入口(单线程路径不受影响)
//   ZSTD_DISABLE_ASM        不引入 x86-64 汇编解码器(目标平台是 aarch64, 汇编用不上,
//                           且 .S 文件不在本工程构建清单里)
// 注意: 这里**不**定义任何"性能档位"宏, 压缩级别只由调用方(ZSTD_compress(...,ZSTD_CLEVEL_DEFAULT))
// 决定, 与设备无关。
// ============================================================================
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_MULTITHREAD 1
#define ZSTD_DISABLE_ASM 1

#include "zstd/common/debug.c"
#include "zstd/common/entropy_common.c"
#include "zstd/common/error_private.c"
#include "zstd/common/fse_decompress.c"
#include "zstd/common/pool.c"
#include "zstd/common/threading.c"
#include "zstd/common/xxhash.c"
#include "zstd/common/zstd_common.c"
#include "zstd/compress/fse_compress.c"
// hist.c 必须先于 huf_compress.c: HUF_compress 内部调用 HIST_count_wksp/HIST_countFast_wksp,
// 而这三个函数(HIST_count_simple / HIST_countFast_wksp / HIST_count_wksp)只定义在 hist.c 里。
// 少了这一行, 编译期一切正常、**链接期**才报 undefined symbol(实测 ld.lld 报三个 HIST_*),
// 是那种"看着编过了其实没链接成功"的坑 —— 加进来后三处引用全部落地。
// (与官方 amalgamation 一致: hist.c 属于 compress 组。)
#include "zstd/compress/hist.c"
#include "zstd/compress/huf_compress.c"
#include "zstd/compress/zstd_compress.c"
#include "zstd/compress/zstd_compress_literals.c"
#include "zstd/compress/zstd_compress_sequences.c"
#include "zstd/compress/zstd_compress_superblock.c"
#include "zstd/compress/zstd_double_fast.c"
#include "zstd/compress/zstd_fast.c"
#include "zstd/compress/zstd_lazy.c"
#include "zstd/compress/zstd_ldm.c"
#include "zstd/compress/zstd_opt.c"
#include "zstd/compress/zstdmt_compress.c"
#include "zstd/decompress/huf_decompress.c"
#include "zstd/decompress/zstd_ddict.c"
#include "zstd/decompress/zstd_decompress.c"
#include "zstd/decompress/zstd_decompress_block.c"
