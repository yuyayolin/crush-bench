cmake_minimum_required(VERSION 3.5.0)
project(aurora_bench)

# 显式指定 C++17 —— 必须显式写, 不能靠工具链默认值:
# 本工程用的 clang 15 对 C++ 的默认标准是 gnu++14, 而代码里用到了 C++17 才有的语义
# (最典型的是把 std::string::data() 当可写缓冲用 —— C++14 返回 const char*, C++17 返回 char*)。
# 之前出现过一次真实事故: 各模块单独用 -std=c++17 自检都通过, 但整体构建没有这个开关,
# 于是 napi_init.cpp 里三处 std::string::data() 全部编译失败。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVE_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(${NATIVE_ROOT_PATH})
include_directories(${NATIVE_ROOT_PATH}/gpu)

# 让"纯 C 编译单元"也生成 .eh_frame:
# 卡死取证(crash_guard.cpp)用 _Unwind_Backtrace 抓调用栈, 而本 target 上 C++ 单元默认
# 带 .eh_frame、C 单元(coremark/lz4/miniz/chibicc)默认不带 —— 一旦卡死发生在这些 C 帧里,
# 展开器会在第一帧 C 处断掉。加上这个开关即可补齐(代价是体积略增)。
add_compile_options(-funwind-tables)

# EEMBC CoreMark 官方负载(逐字节同源, Apache-2.0)
set(COREMARK_SRC
    coremark/core_list_join.c
    coremark/core_matrix.c
    coremark/core_state.c
    coremark/core_util.c
    coremark/core_main.c
    coremark/core_portme.c
    coremark/coremark_runner.c)
set_source_files_properties(coremark/core_main.c PROPERTIES COMPILE_DEFINITIONS "main=coremark_main_entry")

# 把"隐式函数声明"与"指针被截断成整数再转回指针"升级为**编译错误**。
# 真实教训: CoreMark 的 align_mem() 只有定义、全工程没有原型, core_matrix.c 按 C99 隐式
# 声明当成 int align_mem() 调用 -> 64 位指针被截断到低 32 位 -> 三个矩阵指针全成野指针,
# 真机上每次点 CoreMark 必崩(SIGSEGV / si_code=1 SEGV_MAPERR, 崩在工作线程)。
# 这两个警告其实在编译期就报了, 只是被忽略; 升级成 error 后这类问题再也过不了构建。
# 只对 CoreMark 源生效: 第三方的 stb/lz4/chibicc 保持原样, 避免它们的历史告警挡住构建。
set_source_files_properties(${COREMARK_SRC} PROPERTIES COMPILE_OPTIONS
    "-Werror=implicit-function-declaration;-Werror=int-to-pointer-cast")

# GB7 复刻负载所需的真实开源库(LZ4 / zlib(sysroot 的 libz) / Zstandard / stb)
add_library(lz4 STATIC third_party/lz4.c)

# Zstandard: GB7 "File Compression" 官方定义里的第三种压缩格式(官方原文 LZ4/zlib/Zstandard)。
# 上游 facebook/zstd v1.5.6 (BSD-3-Clause, 许可证原文 third_party/zstd/LICENSE)。
# third_party/zstd.c 是官方 amalgamation 等价物(只 include lib/ 下的翻译单元, 源码逐字节未改)。
# 只对**第三方源**放开它自己的历史告警(-w); 调用方 gb7_filecompress.cpp 仍按 -Wall -Wextra 严格编译。
add_library(zstd STATIC third_party/zstd.c)
target_include_directories(zstd PRIVATE ${NATIVE_ROOT_PATH}/third_party/zstd)
target_compile_options(zstd PRIVATE -w)

# chibicc: GB7 "Clang" 负载内嵌的真实 C 编译器 (MIT, rui314/chibicc)
set(CHIBICC_SRC
    third_party/chibicc/codegen.c
    third_party/chibicc/hashmap.c
    third_party/chibicc/main.c
    third_party/chibicc/parse.c
    third_party/chibicc/preprocess.c
    third_party/chibicc/strings.c
    third_party/chibicc/tokenize.c
    third_party/chibicc/type.c
    third_party/chibicc/unicode.c)
set_source_files_properties(third_party/chibicc/main.c PROPERTIES COMPILE_DEFINITIONS "main=chibicc_main_entry")
add_library(chibicc STATIC ${CHIBICC_SRC})
target_include_directories(chibicc PRIVATE ${NATIVE_ROOT_PATH}/third_party/chibicc)

# storage_bench.cpp: 存储 I/O 小节(顺序读写 + 4K 随机读写 + 元数据)。
# 它是**独立小节**, 不参与 GB7 单项分/复合分, 不引用任何 gb7_*/gpu7/* 代码;
# 只用到 OHOS musl sysroot 里已有的头文件, 不引第三方依赖。
set(STORAGE_BENCH_SRC storage_bench.cpp)

# npu_bench.cpp: NPU(AI 加速器)小节 —— 用 NNRt 纯代码建 MATMUL 链模型, 编译后计时推理。
# 同样是**独立小节**: 不参与 GB7 单项分/复合分, 不引用任何 gb7_*/gpu7/* 代码, 不引第三方依赖。
# 重要: 它只用到 NNRt 的**头文件**(编译期); NNRt 的**库**是运行期 dlopen/dlsym 绑定的,
#       下面**故意没有**任何 neural_network 的 target_link_libraries —— 理由见后面的长注释。
set(NPU_BENCH_SRC npu_bench.cpp)

# cpu_freq_sample.cpp: 运行时**实际频率**采样(sysfs scaling_cur_freq, 后台线程 200ms 一次)。
# 纯旁路诊断: 不参与任何计分, 也不改变任何负载的行为; 详细口径见 cpu_freq_sample.h。
add_library(aurorabench SHARED napi_init.cpp crash_guard.cpp bench_cpu.cpp cpu_freq_sample.cpp gb7.cpp gb7_filecompress.cpp gb7_batch2.cpp
    gb7_batch3.cpp gb7_pdf.cpp gb7_audio.cpp gb7_video.cpp gb7_browser.cpp gb7_sfm.cpp gb7_clang.cpp
    ${STORAGE_BENCH_SRC} ${NPU_BENCH_SRC} ${COREMARK_SRC})
target_include_directories(aurorabench PRIVATE ${NATIVE_ROOT_PATH}/coremark ${NATIVE_ROOT_PATH})
target_link_libraries(aurorabench PUBLIC lz4 z zstd chibicc)
target_link_libraries(aurorabench PUBLIC libace_napi.z.so libhilog_ndk.z.so)
# ============================================================================
# NNRt(Neural Network Runtime)NDK: **有意不写进 target_link_libraries**, 改为运行期 dlopen。
# 这一行以后也**不要**再加回来。理由:
#
#   1) sysroot 里的 libneural_network_runtime.so / libneural_network_core.so 是**链接桩**
#      (用 llvm-objdump 核实: 每个导出函数体只有一条 ret, 且没有任何 DT_NEEDED)。
#      硬链之后 libaurorabench.so 就会带上对它们的**硬 DT_NEEDED**。
#   2) 一旦真机 /system/lib64 里没有这两个库(或 soname 对不上), 失效方式是
#      **整个 requireNapi("aurorabench") 加载失败** —— 一个可选的 NPU 小节把
#      GB7 / CoreMark 全部一起拖死。这是最糟的失效方式, 不能接受。
#   3) 本工程已有同样的先例: crash_guard.cpp 对 _Unwind_Backtrace 用的是 dlsym + 降级,
#      理由一字不差 —— **可选功能不该有让主功能加载失败的能力**。
#
#   做法(npu_bench.cpp 里实现): 首次进入 probe/bench 时
#      dlopen("libneural_network_runtime.so", RTLD_NOW|RTLD_LOCAL) 与
#      dlopen("libneural_network_core.so",    RTLD_NOW|RTLD_LOCAL),
#      再用 dlsym 绑定 38 个函数指针(31 个来自 core, 7 个来自 runtime), 结果缓存一次。
#      任何一步失败 -> 本小节返回 status:"NNRT_UNAVAILABLE" + dlerror 原文 + 缺失符号名,
#      不影响其它任何跑分项, 更不会让模块加载失败。
#
#   dlopen/dlsym 由 libc(libdl 已并入)提供, 是 NDK 的既有接口, 无需额外链接任何库;
#   在产物里它们是**未定义符号 U**, 由运行期解析 —— 这正是我们要的效果。
# ============================================================================

add_library(auroragpu SHARED gpu/gpu_napi.cpp gpu/gpu_renderer.cpp)
target_link_libraries(auroragpu PUBLIC libace_napi.z.so libace_ndk.z.so libhilog_ndk.z.so libEGL.so libGLESv3.so libnative_window.so)

# GPU 负载套件(11 项), 独立 napi 模块 libauroragpu7.so
add_library(auroragpu7 SHARED gpu7/gpu7_napi.cpp gpu7/gpu7_renderer.cpp)
target_include_directories(auroragpu7 PRIVATE ${NATIVE_ROOT_PATH}/gpu7)
target_link_libraries(auroragpu7 PUBLIC libace_napi.z.so libace_ndk.z.so libhilog_ndk.z.so libEGL.so libGLESv3.so libnative_window.so)

# 3DMark Steel Nomad 风格的 GPU 小节 "Aurora Nomad Light" (SNL), 独立 napi 模块 libaurorasn.so
#   * 独立小节: 不并入 GB7 的 GPU 11 项 / CPU 16 项, 不参与任何 GB7 分数;
#   * 与 libauroragpu7.so 一样**只依赖 napi + EGL/GLES3**, 不引用 gb7_*/gpu7/* 的任何代码,
#     也不给 libaurorabench.so 加任何硬依赖 —— 本模块加载失败不能拖死跑分主模块
#     (与工程对 NNRt 的结论一致, 见上面那段长注释);
#   * 它只 include cpu_affinity.h 读**只读**的拓扑/频率事实(用于结果里的"尺子有没有
#     被喂满"), 那个头文件是 header-only 的, 不引入任何链接依赖。
add_library(aurorasn SHARED sn/sn_napi.cpp sn/sn_renderer.cpp)
target_include_directories(aurorasn PRIVATE ${NATIVE_ROOT_PATH}/sn)
target_link_libraries(aurorasn PUBLIC libace_napi.z.so libace_ndk.z.so libhilog_ndk.z.so libEGL.so libGLESv3.so libnative_window.so)

# 参考分对照模块 libauroraref.so: **只读**, 不跑负载 / 不改分数 / 不参与计分。
#   * 只做两件事: ① 把本机结果与公开真值并列(带来源与来源种类, 第三方绝不写成官方);
#     ② 给每一项一句话真实用途 + 计分口径解释;
#   * 不引用任何 gb7_*/gpu7_*/sn_* 代码, 也不给 libaurorabench.so 加硬依赖
#     (可选功能不该有让主功能加载失败的能力 —— 与工程对 NNRt 的结论一致)。
add_library(auroraref SHARED reference_napi.cpp reference_compare.cpp)
target_link_libraries(auroraref PUBLIC libace_napi.z.so libace_ndk.z.so libhilog_ndk.z.so)
