// =============================================================================
// CS1 第四批之三: Clang —— 进程内运行真实的 chibicc C 编译器
// =============================================================================
//
// 本工程的 "Clang" 子项度量编译器吞吐(Klines/s)。真 Clang 无法塞进
// 手机 App, 因此这里内嵌 rui314/chibicc (MIT, 真实开源项目: 完整 词法/预处理/
// 语法/类型/代码生成) 的源码, 直接调用它的内部 API (tokenize -> preprocess ->
// parse -> codegen) 在内存里完整编译一份 2468 行的自包含 C 翻译单元, 反复多轮,
// 度量  metric = 源码总行数 x 迭代次数 / 1000 / 秒   (Klines/s, 越大越快)。
//
// 每一轮都是货真价实的完整编译: 词法 -> 预处理(展开全部 #define) -> 建 AST ->
// 生成汇编文本, 不跳过任何阶段。生成的汇编字节数与行数会累加进 volatile sink 并
// 做非空自检; 某轮失败(产出 0 字节/0 行, 或被 chibicc 的 error() 拒绝)时该轮不计
// 入成功次数, 且把失败次数打到 stderr(见 gb7RunClang 内注释), 不静默。
//
// -----------------------------------------------------------------------------
// 【CMakeLists.txt 集成说明 —— 请照抄】
// -----------------------------------------------------------------------------
// 1) 在 set(COREMARK_SRC ...) 之后加入 chibicc 的编译单元:
//
//    # chibicc: GB7 "Clang" 负载内嵌的真实 C 编译器 (MIT, rui314/chibicc)
//    set(CHIBICC_SRC
//        third_party/chibicc/codegen.c
//        third_party/chibicc/hashmap.c
//        third_party/chibicc/main.c
//        third_party/chibicc/parse.c
//        third_party/chibicc/preprocess.c
//        third_party/chibicc/strings.c
//        third_party/chibicc/tokenize.c
//        third_party/chibicc/type.c
//        third_party/chibicc/unicode.c)
//    set_source_files_properties(third_party/chibicc/main.c
//        PROPERTIES COMPILE_DEFINITIONS "main=chibicc_main_entry")
//    add_library(chibicc STATIC ${CHIBICC_SRC})
//    target_include_directories(chibicc PRIVATE
//        ${NATIVE_ROOT_PATH}/third_party/chibicc)
//
// 2) 把 gb7_clang.cpp 加进 aurorabench, 并把 chibicc 链进去:
//
//    add_library(aurorabench SHARED napi_init.cpp bench_cpu.cpp gb7.cpp
//        gb7_filecompress.cpp gb7_batch2.cpp gb7_batch3.cpp gb7_clang.cpp
//        ${COREMARK_SRC})
//    target_link_libraries(aurorabench PUBLIC lz4 z chibicc)
//
// 3) 需要的 -D 宏 / include 路径, 逐条:
//    * main=chibicc_main_entry —— 必须。chibicc 自带的 main() 与宿主 main 同名,
//      而 main.c 还定义了 preprocess.c/parse.c/codegen.c 需要的全局
//      include_paths / opt_fcommon / opt_fpic / base_file, 所以 main.c 不能删,
//      只能改名(与 coremark 的 "main=coremark_main_entry" 做法完全一致)。
//    * 不需要 -D_GNU_SOURCE: 9 个 .c 文件自己在 chibicc.h 第 1 行就
//      #define _POSIX_C_SOURCE 200809L, strndup/open_memstream/glob/ctime_r 等
//      在 OHOS musl 下均已暴露(已用 sysroot 头文件核实)。
//    * 不需要给 aurorabench 加 chibicc 目录的 include 路径: gb7_clang.cpp 不
//      include chibicc.h(避免它的 calloc/strdup 宏污染 C++ 代码), 只用
//      extern "C" 声明最小接口。只有 chibicc 自己的 target 需要该 include 路径。
//    * 不需要额外的 -std=: chibicc 需要 C11, 工程默认的 gnu11/gnu17 都行。
//      已实测 -std=gnu11 / -std=c11 / -std=c99 均零错误通过。
// 4) 【关键】这 9 个 .c 文件必须由 C 编译器编译(CMake 对 .c 默认如此, 与 coremark
//    相同)。不要把它们当 C++ 编译: chibicc 大量使用 C99 复合字面量(例如
//    "static Scope *scope = &(Scope){};")、指定初始化器和 void* 隐式转换, 这些在
//    C++ 下不成立; 而 gb7_clang.cpp 是按 extern "C" 声明这些符号的。
// 5) third_party/chibicc/gen_gb7_clang_source.py 只是离线生成器(生成下面那份 C
//    源码), 不是构建输入, 不要加进任何 target, 删掉也不影响编译。
//
// -----------------------------------------------------------------------------
// 【对 chibicc 源码打了哪些补丁 (逐条, 全部为集成所需, 不改任何算法)】
// -----------------------------------------------------------------------------
// (a) 新增 third_party/chibicc/aurora_glue.h, 并在 chibicc.h 末尾 #include 它。
//     它做两件事: 把 calloc/realloc/strdup/strndup 宏重定向到本文件实现的
//     bump arena; 声明 aurora_cc_on_error()。原因: chibicc 是批处理编译器, 全程
//     只分配不释放(靠进程退出回收)。本负载要在同一个长生命周期 App 进程里编译
//     上百次, 必须能在两轮之间整体回退分配器, 否则内存立刻爆掉。arena 返回的
//     内存全部已清零, calloc 语义不变。
// (b) strings.c: format() 由 open_memstream() 改为 vsnprintf() 两遍法, 输出字节
//     完全一致, 但缓冲区来自 arena。原因: chibicc 每生成一个标号名都会调用
//     format()(parse.c:new_unique_name), 而 open_memstream() 返回的缓冲区它从不
//     释放; musl 的 memstream cookie 还内嵌 BUFSIZ(8KiB) 缓冲, 每轮会泄漏几 MB。
// (c) tokenize.c: error()/error_at()/error_tok() 结尾的 exit(1) 改为
//     aurora_cc_on_error()(longjmp), 由本文件捕获。原因: 输入一旦被拒绝, exit(1)
//     会直接杀死宿主 App; 改为可捕获后按"该轮失败"处理并记录。
// (d) tokenize.c: 新增 aurora_cc_tokenize_memory(name, contents) —— 即
//     tokenize_file() 去掉文件读取, 直接在内存 buffer 上 canonicalize +
//     new_file + tokenize, 并重建 codegen 需要的 input_files 列表(顺带丢弃上一轮
//     的悬垂指针)。本负载全程不碰文件系统。
// (e) preprocess.c: init_macros() 开头清空 macros / pragma_once 两张散列表并复位
//     cond_incl / include_next_idx。原因: 这两张表存在 arena 里, arena 回退后必须
//     重建; 每轮重新调用 init_macros() 即可得到干净的预定义宏。
// (f) parse.c: 文件作用域由匿名的 "&(Scope){}" 改为具名 static base_scope(语义完全
//     等价), 并在 parse() 开头复位 base_scope 以及 locals/globals/current_fn/
//     gotos/labels/brk_label/cont_label/current_switch/builtin_alloca。原因同上:
//     这些状态跨越一轮编译, 却指向已被 arena 回退的内存。
// (g) tokenize.c: is_keyword() 的 static HashMap 提升为文件作用域 keyword_map,
//     parse.c: is_typename() 的 static HashMap 提升为文件作用域 typename_map,
//     preprocess.c: search_include_paths()/include_file() 的 static HashMap 提升为
//     文件作用域 include_path_cache/include_guards; 三个 .c 各新增一个
//     aurora_cc_reset_*_caches(), 由本文件在 arenaReset() 之后调用。
//     [这是本轮修掉的真机 0/46 失败根因] 这四张表是惰性建立的: 只在
//     map.capacity == 0 时填一次, 之后永远复用。它们的 buckets 数组同样是 arena
//     分配的, 于是 arena 回退之后 capacity 仍然 != 0(不会重建), 但 buckets 指向的
//     内存已经被下一轮编译复用 —— is_keyword()/is_typename() 从此按随机内容回答
//     "这个记号是不是关键字/类型名"。预热轮是干净的, 从第 2 轮开始解析随机错乱,
//     而 is_typename() 只被 parser 使用, 所以失败必然报在 parse 阶段:
//     真机上正是 "0/46 轮成功, last failure at 'parse'"。
//     复现: 把 arena 换成 libc malloc 后 46 轮全过(hashmap 自己那块堆内存从不失效),
//     换回 arena 后第 2 轮立刻在 parse 报
//     "gb7_clang_unit.c:38: typedef int (*aurora_binop)(int, int); ^ variable name
//     omitted"; 打上 (g) 之后连跑 12 轮 12/12 成功、产出 18 MB 汇编。
//     这与"内嵌 C 源码用了 chibicc 不支持的构造"无关: 那份 C 源码本身完全落在
//     chibicc 子集内(见下面的【chibicc 支持子集 / 限制清单】)。
//  (h) codegen.c: count() 里的函数局部 "static int i"(标号计数器)提升为文件作用域的
//     label_seq, 并新增 aurora_cc_reset_codegen_state(); 本文件在 arenaReset() 之后、
//     每一轮编译之前调用它(与 (e)(f)(g) 三组复位同一处、同一顺序)。
//     原因: codegen.c 跨轮存活的全部可变状态只有四处, 逐条如下 ——
//       output_file  由 codegen() 开头赋值, 陈旧值不会被解引用(仍一并清空, 失败轮
//                    不会留下一个已经 closed 的 FILE*);
//       depth        push/pop 的栈记账。emit_text() 在每个函数末尾 assert(depth == 0),
//                    push_args() 用 (depth + stack) % 2 决定是否补 8 字节对齐; 某一轮
//                    若在表达式中间被 error() longjmp 打断, depth 会停在非零值, 下一轮
//                    就从非零 depth 开始 —— assert 打开时该轮每个函数末尾直接 abort,
//                    关掉 assert 时实参压栈的对齐算错;
//       current_fn   正在生成的函数的 Obj*, 而 Obj 由 arena 分配: arena 回退后它就是
//                    悬垂指针, 不允许跨轮存活(被 emit_data/gen_expr 侧的 copy_struct_* /
//                    builtin_alloca / ND_RETURN 读取);
//       label_seq    count() 发放的 ".L.%d" 标号计数器, 跨轮只增不减。
//     说明: 这四处都不参与 AST 构建与汇编格式, 复位不改变任何一条指令的生成规则;
//     唯一可见差别是每一轮从 ".L.1" 重新开始编号, 因此各轮产出逐字节一致(便于复核)。
//     复位函数只写这 4 个变量, 不分配内存、不改算法。
//  (i) tokenize.c: 新增全局 "char aurora_cc_last_error[192]", 在 error() 与 verror_at()
//     (error_at/error_tok 的共同实现, 也是 codegen.c 那 5 处 error_tok 的必经之路)里把
//     消息原文按 "文件:行号: 正文" 抄一份; gb7_clang.cpp 在失败时把它连同
//     failed/ok/stage/arena 一起用 OH_LOG_Print 打到 hilog(标签 AuroraClang, LOG_ERROR)。
//     原因(实测, 不是推测): 真机上 App 的 stderr 不进 hilog —— 在 Clang 项刚跑完
//     (02:57)时 dump 整块 hilog 缓冲区(2.4 MB, 覆盖 09-30 08:54 → 10-04 02:58), 搜
//     gb7_clang / chibicc / last_stage 零命中, 连续抓 hdc hilog 流同样零命中; 也就是说
//     界面上那句 "see log" 原本指向空处, 失败原因无从查证。改成 hilog 后:
//       hdc shell "hilog -x" | Select-String AuroraClang
//     即可直接看到失败阶段 + chibicc 的错误原文 + arena 峰值。
//     本补丁不改 chibicc 的任何输出(仍照原样写 stderr), 只多抄一份副本; 也不影响
//     任何一轮的编译结果与汇编文本。
// 除上述 9 条外, chibicc 的 9 个 .c 文件与上游逐字节一致(附 LICENSE, MIT)。
//
// -----------------------------------------------------------------------------
// 【chibicc 支持子集 / 限制清单】(写内嵌 C 源码时的对照表, 依据见括号里的行号)
// -----------------------------------------------------------------------------
// 下面每一条都以 third_party/chibicc/ 的源码为准, 不是凭印象。
//
// A. 类型说明符 —— parse.c:declspec() 385-584
//    支持: void / _Bool / char / short / int / long / float / double /
//          long double(16 字节, type.c:18) / signed / unsigned 及其合法组合
//          (512-572); struct / union / enum / typeof / 已 typedef 的名字 (464-486)。
//    存储类: typedef / static / extern / inline / _Thread_local / __thread (410-411)。
//    被识别但忽略: const / volatile / auto / register / restrict / __restrict /
//          __restrict__ / _Noreturn (435-438)。
//    支持: _Atomic (441-449), _Alignas (451-462)。
//    不支持(会走到 571 行 "invalid type"): _Complex / _Imaginary /
//          _Decimal32/64/128 / __int128 / _BitInt / long long double —— 没有对应 case。
//
// B. 语句 —— parse.c:stmt() 1551-1764
//    支持: return, if/else, switch, case(含 GNU 范围 case a ... b, 1608-1615),
//          default, for, while, do-while, asm, goto(含 GNU goto *expr),
//          break/continue, 标号, 复合语句, 表达式语句。
//    break/continue 的作用域跟踪在 1639-1748; 局部声明在 compound_stmt 1775 判断。
//
// C. 表达式 —— parse.c:primary()/postfix()/unary() 2987/2811/2500
//    支持: 复合字面量 (2812-2828), 下标 [](2838), . 和 ->(2847/2853),
//          前后缀 ++/--(2861/2534), 函数调用, sizeof, _Alignof(3028),
//          _Generic(3040-3041), __builtin_types_compatible_p(3043),
//          __builtin_reg_class(3052), __builtin_compare_and_swap(3064),
//          __builtin_atomic_exchange(3076), GNU 语句表达式 ({ ... })(2990-2996),
//          GNU labels-as-values &&label(2542)。
//    不支持: __builtin_va_arg / __builtin_va_start / __builtin_offsetof /
//          __builtin_expect / __builtin_unreachable / __auto_type / __label__ /
//          __extension__ —— 源码里完全没有这些记号, 写了就是 "undefined variable"
//          或 "implicit declaration of a function"。
//
// D. 声明与初始化 —— parse.c:struct_members() 2548 / designation() 1029
//    支持: 位域 (2581-2584), 柔性数组成员 (2593-2596), 匿名 struct/union 成员
//          (2558-2567), 指定初始化器 .name = 与 [i] = 与 GNU 范围 [a ... b]
//          (983-1068), 变长数组 VLA (TY_VLA), 用字符串字面量初始化数组 (926)。
//    支持: __attribute__((packed)) 与 __attribute__((aligned(N)))(2603-2631);
//          其它 attribute 一律 error_tok("unknown attribute")。
//    不支持: _Static_assert(关键字表里没有: tokenize.c:162-171 / parse.c:1505-1511),
//          K&R 老式参数表(func_params 588-638 只接受 declspec declarator)。
//
// E. 词法 —— tokenize.c:490-637
//    整数: 十进制 / 0x(十六进制) / 0 开头(八进制) / 0b(二进制, 350);
//          后缀 U L LL 及其大小写组合。不支持数字分隔符撇号(C23 的 1'000'000)。
//    浮点: 交给 strtold(), 后缀 f/F/l/L(434-445); 词法阶段先按"预处理数字"收,
//          展开后必须被 strtold 整个吃掉, 否则 error "invalid numeric constant"(448)。
//    字符常量: 'x' / u'x' / L'x' / U'x'(586-613)。多字符常量如 'ab' 不会被拒绝,
//          但只会取第一个字符的值(321-340 用 strchr 找收尾引号)。本负载只用单字符。
//    字符串: "..." / u8"..." / u"..." / L"..." / U"..."(551-583), 支持相邻字面量拼接。
//    注释: 行注释与块注释(502-518)。
//    转义: 八进制 1-3 位、十六进制、以及 a b t n v f r e(180-231)。
//    标识符: UTF-8 解码 + unicode.c 的 is_ident1/is_ident2, 非 ASCII 也认。
//
// F. 预处理器 —— preprocess.c
//    支持: #include / #include_next / #define(对象式 + 函数式 + ... 可变参数 +
//          __VA_ARGS__ + __VA_OPT__)/ #undef / #if #ifdef #ifndef #elif #else #endif /
//          #line / #pragma once / #error(983)/ # 与 ## 运算符 / defined() /
//          __FILE__ __LINE__ __COUNTER__ __TIMESTAMP__ __BASE_FILE__ __DATE__ __TIME__。
//    不支持: __has_include / __has_attribute / __has_builtin / _Pragma / 除 once 以外
//          的 #pragma 形式 —— 源码里搜不到任何实现。
//
// G. 结论(内嵌的那份 2468 行 C 源码)
//    它只用到 A-F 里"支持"的部分: 结构体/指针/数组/函数指针/位域/枚举/typedef/
//    递归/switch/for/while/do-while/三目/复合赋值/取模/位运算/双精度浮点/字符串
//    字面量/静态数组, 预处理器只用 #define(对象式 + 函数式)。没有 #include,
//    没有 _Generic/typeof/复合字面量/语句表达式/指定初始化器/_Static_assert/
//    可变参数宏/长双精度/联合体/内联汇编。已用真实 chibicc 完整跑通(证据见 (g)),
//    因此无需为兼容性改写内嵌源码。
//
// -----------------------------------------------------------------------------
// 【已知保真度妥协与风险点】
// -----------------------------------------------------------------------------
// * chibicc 的目标平台是 x86-64 + glibc; 这里只取它产出的汇编文本(用于计数),
//   不汇编也不链接, 因此在 aarch64/OHOS 上跑没有问题, 但"生成代码的质量"与官方
//   Clang 不可比。
// * 被测 C 源码没有 #include(不向 chibicc 传系统头路径), 也不调用 libc, 数据全部
//   放在静态内存池里; 因此它比真实翻译单元的"行"更轻, 这里的 Klines/s 绝对值会
//   明显高于官方参考值 2.78 Klines/s。按要求不做任何针对设备的标定系数修正。
// * chibicc 的状态是进程级全局, 本负载只能单线程运行(parallelism = 1.0,
//   threads 参数忽略)。
// * 迭代次数按预热轮的耗时标定到约 1.5 s(上限 600 轮), 极快的设备上总耗时也不会
//   失控; 内存由 arena 回退兜住, 与迭代次数无关(单轮 ~20 MB 量级, 整体 < 200 MB)。
// =============================================================================

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* open_memstream() */
#endif

#include "gb7.h"

#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>
#include <string>
#include <vector>

// 补丁清单 (i): 失败诊断走 hilog。真机上 App 的 stderr 不进 hilog(实测: 整块
// hilog 缓冲区里搜不到本文件任何一行 fprintf), 所以失败原因必须走 OH_LOG_Print。
// aurorabench 已经链接 libhilog_ndk.z.so(见 CMakeLists.txt), 无需改构建。
#include <hilog/log.h>

// -----------------------------------------------------------------------------
// chibicc 的最小 C 接口声明。
// chibicc 的 .c 文件按 C 编译, 所以这里必须是 extern "C"; 指针类型用不透明结构体
// 表示, 与 chibicc.h 里的 Token*/Obj* 完全 ABI 等价。(本文件不 include chibicc.h,
// 以免它重定向 calloc/strdup 的宏污染这里的 C++ 代码。)
// -----------------------------------------------------------------------------
extern "C" {
typedef struct AuroraCcFile AuroraCcFile;
typedef struct AuroraCcToken AuroraCcToken;
typedef struct AuroraCcObj AuroraCcObj;

void init_macros(void);
AuroraCcToken *aurora_cc_tokenize_memory(char *name, char *contents);
AuroraCcToken *preprocess(AuroraCcToken *tok);
AuroraCcObj *parse(AuroraCcToken *tok);
void codegen(AuroraCcObj *prog, FILE *out);

// chibicc 的惰性散列表(chibicc.h 的 "Cache resets" 一节)。它们的 buckets 是从本
// 文件的 arena 分配的, 所以 arenaReset() 之后必须一并丢弃, 否则第二轮开始
// is_keyword()/is_typename() 会读到被复用的内存。
void aurora_cc_reset_tokenize_caches(void);
void aurora_cc_reset_parse_caches(void);
void aurora_cc_reset_preprocess_caches(void);
// codegen.c 专属的那一份(补丁清单 (h)): output_file / depth / current_fn / 标号计数器。
void aurora_cc_reset_codegen_state(void);

// chibicc 最近一次 error()/error_at()/error_tok() 的原文(补丁清单 (i))。
// 真机上 App 的 stderr 不进 hilog, 所以失败时由本文件把它转发到 hilog。
extern char aurora_cc_last_error[192];

// 由本文件实现, 供 aurora_glue.h 的宏与 chibicc 的 error() 调用。
void *aurora_cc_calloc(size_t nmemb, size_t size);
void *aurora_cc_realloc(void *ptr, size_t size);
char *aurora_cc_strdup(const char *s);
char *aurora_cc_strndup(const char *s, size_t n);
void aurora_cc_on_error(void) __attribute__((__noreturn__));
}

// =============================================================================
// 1. 可回退的 bump arena
//    chibicc 的所有分配(calloc/realloc/strdup/strndup)都落到这里。arena 由若干
//    malloc 出来的 chunk 串成; "回退"只是把游标拨回第一个 chunk 的开头, 不还给
//    系统, 因此两轮之间没有 malloc/free 抖动, 峰值内存 = 单轮峰值 + 一个 chunk 的
//    余量。每次分配在头部藏 16 字节长度(realloc 用)并整体清零(calloc 语义)。
// =============================================================================
namespace {

const size_t kAlign = 16;
const size_t kHeader = 16;
const size_t kChunkFirst = 4u << 20;   // 首个 chunk 4 MiB
const size_t kChunkMax = 16u << 20;    // 单个 chunk 上限 16 MiB
const size_t kMaxSingle = 32u << 20;   // 单次分配上限(超出视为失败)

struct ArenaChunk {
    unsigned char *base;
    size_t cap;
    ArenaChunk *next;
};

ArenaChunk *g_chunkHead = nullptr;
ArenaChunk *g_chunkCur = nullptr;
size_t g_chunkOff = 0;
uint64_t g_arenaBytes = 0; // 已向系统申请的总字节数(仅用于日志/诊断)

bool arenaPushChunk(size_t cap)
{
    ArenaChunk *chunk = (ArenaChunk *)malloc(sizeof(ArenaChunk));
    if (chunk == nullptr) {
        return false;
    }
    chunk->base = (unsigned char *)malloc(cap);
    if (chunk->base == nullptr) {
        free(chunk);
        return false;
    }
    chunk->cap = cap;
    chunk->next = nullptr;
    if (g_chunkCur != nullptr) {
        g_chunkCur->next = chunk;
    } else {
        g_chunkHead = chunk;
    }
    g_chunkCur = chunk;
    g_chunkOff = 0;
    g_arenaBytes += (uint64_t)cap;
    return true;
}

void *arenaRaw(size_t total)
{
    // 对齐: malloc 给出的 chunk 基址是 16 字节对齐的, 只要每次分配的大小也是 16 的
    // 倍数, 每个返回指针就都满足 max_align_t(chibicc 的 Token 里有 long double)。
    total = (total + (kAlign - 1)) & ~(kAlign - 1);
    if (g_chunkCur == nullptr) {
        if (!arenaPushChunk(kChunkFirst)) {
            return nullptr;
        }
    }
    if (g_chunkOff + total > g_chunkCur->cap) {
        for (;;) {
            if (g_chunkCur->next == nullptr) {
                size_t cap = (g_chunkCur->cap < kChunkMax) ? (g_chunkCur->cap * 2) : kChunkMax;
                if (cap < total) {
                    cap = total;
                }
                if (!arenaPushChunk(cap)) {
                    return nullptr;
                }
                break;
            }
            g_chunkCur = g_chunkCur->next;
            g_chunkOff = 0;
            if (g_chunkCur->cap >= total) {
                break;
            }
        }
    }
    unsigned char *p = g_chunkCur->base + g_chunkOff;
    g_chunkOff += total;
    memset(p, 0, total);
    return (void *)p;
}

// 带 16 字节长度头的分配; 保证 16 字节对齐且全零。
void *arenaPayload(size_t need)
{
    if (need > kMaxSingle) {
        return nullptr;
    }
    unsigned char *base = (unsigned char *)arenaRaw(need + kHeader);
    if (base == nullptr) {
        return nullptr;
    }
    *(size_t *)base = need;
    return (void *)(base + kHeader);
}

// 把 arena 拨回起点(= "释放"上一轮编译产生的全部对象)。
void arenaReset()
{
    g_chunkCur = g_chunkHead;
    g_chunkOff = 0;
}

// 把 arena 的所有 chunk 还给系统, 回到"还没分配过"的状态。
// 只在一轮编译"失败"之后调用: 那一轮产生的对象全部作废(chibicc 是 C 代码, 没有
// 析构, 且下一轮会重建全部全局状态), 因此直接释放是安全的。这样即便某一轮是因为
// 内存申请失败而失败, 也不会让之后每一轮都继续失败(真机上表现为整项永远是 0)。
void arenaDropAll()
{
    ArenaChunk *c = g_chunkHead;
    while (c != nullptr) {
        ArenaChunk *next = c->next;
        free(c->base);
        free(c);
        c = next;
    }
    g_chunkHead = nullptr;
    g_chunkCur = nullptr;
    g_chunkOff = 0;
    g_arenaBytes = 0;
}

// --- 错误逃逸: chibicc 的 error() 走这里, 用 longjmp 回到 compileOnce ---
jmp_buf g_errorJmp;
volatile int g_errorArmed = 0;

// --- 诊断: 最近一轮编译死在哪个阶段(0 = 这一轮没失败) ---
// longjmp 会直接跳过 chibicc 的 C 栈帧, 所以这个标记必须是全局的
// (compileOnce 的局部变量在 longjmp 之后的值是不确定的)。失败原因会随 o.unit
// 一起返回, 界面可见; chibicc 自己的错误文本仍然照常打到 stderr(hilog)。
enum {
    kCcStageNone = 0,
    kCcStageTokenize = 1,
    kCcStagePreprocess = 2,
    kCcStageParse = 3,
    kCcStageCodegen = 4,
    kCcStageOutput = 5
};
int g_compileStage = kCcStageNone;

const char* ccStageName(int stage)
{
    switch (stage) {
        case kCcStageTokenize: return "tokenize";
        case kCcStagePreprocess: return "preprocess";
        case kCcStageParse: return "parse";
        case kCcStageCodegen: return "codegen";
        case kCcStageOutput: return "codegen-output";
        default: return "none";
    }
}

// --- 失败归因(2026-10-05): "阶段"只说死在哪一步, 这里再记"死在哪一个出口" ---------
// 为什么必须单独记: 真机上连续多轮都是 "阶段=codegen + err=(chibicc 未给出错误信息)",
// 而 codegen 附近不写 chibicc 错误原文的出口不止一个 —— compileOnceInner 里
// 每一个 return 0 都是一个出口, 其中只有一部分会经过 chibicc 的 error()。
//   g_failWhy    出口标签(定长、可枚举、可 grep)
//   g_failDetail 该出口的关键数字(轮次 / arena / 长度 / fflush·ferror·fclose / errno)
// 两者都随 o.unit 一起进 runlog.jsonl 与结果截图, 所以即使 hilog 缓冲区被冲掉也还在。
// 约定: 只覆盖"最近一次失败", 成功轮不清空(报告只在有失败时才读它)。
const char* g_failWhy = "none";
char g_failDetail[192] = {0};
uint64_t g_failRound = 0;   // 最近一次失败发生在第几次尝试(1 = 预热轮, 2.. = 计时区间)
uint64_t g_attemptSeq = 0;  // 到当前为止一共尝试过多少次编译(含预热轮)

void ccSetFail(const char* why, const char* detail)
{
    g_failWhy = why;
    g_failRound = g_attemptSeq;
    snprintf(g_failDetail, sizeof(g_failDetail), "%s", (detail != nullptr) ? detail : "");
}

// --- 把 chibicc 的错误原文压成"一行 + 定长", 供 o.unit 使用 -------------------
// 为什么还要单独做一次: aurora_cc_last_error 是补丁 (i) 把 chibicc 的 error()/verror_at()
// 原文原样抄下来的(最多 191 字符), 而 o.unit 会进 runlog.jsonl(一行一条记录)与结果截图。
// 它可能
//   1) 很长 —— 直接把界面撑坏;
//   2) 含换行 —— chibicc 的 verror_at() 会把"出错那一行源码 + ^ 指示符"整块打出来,
//      换行会让 runlog 的一行记录变成好几行, 破坏按行解析。
// 所以这里统一: 所有空白(空格/制表/CR/LF)压成一个空格 + 去首尾空白 + 按字节截断。
// 截断按 UTF-8 边界收口: 不把一个多字节字符切成半个(否则 JSON 里会出现非法字节)。
// 返回 true = 因为长度上限被截断了(调用方据此补一个 "...")。
bool ccFlatError(const char* src, char* dst, int cap)
{
    if (dst == nullptr || cap <= 0) {
        return false;
    }
    dst[0] = '\0';
    if (src == nullptr) {
        return false;
    }
    int n = 0;
    bool pendingSpace = false;
    bool cut = false;
    for (const char* p = src; *p != '\0'; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            pendingSpace = (n > 0);   // 首尾空白直接丢掉
            continue;
        }
        if (pendingSpace) {
            if (n > cap - 2) {        // 连分隔空格都放不下了
                cut = true;
                break;
            }
            dst[n++] = ' ';
            pendingSpace = false;
        }
        int width = 1;                // UTF-8 序列长度(非法首字节按单字节处理)
        if (c >= 0xF0) {
            width = 4;
        } else if (c >= 0xE0) {
            width = 3;
        } else if (c >= 0xC0) {
            width = 2;
        }
        bool complete = true;
        for (int k = 1; k < width; ++k) {
            const unsigned char nx = (unsigned char)p[k];
            if (nx == '\0' || (nx & 0xC0) != 0x80) {
                complete = false;     // 源串本身就在半个字符上断了
                break;
            }
        }
        if (!complete) {
            cut = true;
            break;
        }
        if (n + width > cap - 1) {    // 放不下整个字符 -> 宁可少一个字, 不切半个
            cut = true;
            break;
        }
        for (int k = 0; k < width; ++k) {
            dst[n++] = p[k];
        }
        p += width - 1;
    }
    dst[n] = '\0';
    return cut;
}

// --- 自检 sink: 汇编字节数/行数, 防止 codegen 的结果被优化掉 ---
volatile uint64_t g_asmBytes = 0;
volatile uint64_t g_asmLines = 0;

} // namespace

// =============================================================================
// 分配失败诊断(2026-10-07) —— 只加诊断, 分配算法一个字都没改
//
// 为什么必须加: 下面四条"分配失败"路径走的是 aurora_cc_on_error()(longjmp 逃逸), 它
// 不经过 chibicc 的 error()/verror_at(), 因此没有任何人往 aurora_cc_last_error 里写字。
// 真机上的表现就是 Clang 项永远 "0/33 轮编译成功 · metric=0.00 · 阶段=codegen ·
// err=(chibicc 未给出错误信息)" —— 四种完全不同的内存故障被糊成同一句话, 分不出是哪一种
// (空 err + stage=codegen 只能来自这四条, 所以这里必须一条一条写清楚)。
//
// 写法: 与 chibicc 的 error() 共用同一个 191 字节缓冲(aurora_cc_last_error), 下游
// ccFlatError() 会照常把它压成一行、按 80 字节截断, 所以标签与关键数字写在最前面。
// 只读地带上 g_arenaBytes / kChunkMax / kMaxSingle 三个数, 便于一眼看出"是单次太大还是
// arena 整体没要下来"; 三者都是本进程自己的常量/计数, 与机型 / SoC 无关。
// =============================================================================
void ccNoteAllocFailure(const char* label, size_t need, size_t nmemb, size_t size)
{
    snprintf(aurora_cc_last_error, sizeof(aurora_cc_last_error),
             "%s: need=%lluB nmemb=%llu size=%llu arenaBytes=%lluMiB kMaxSingle=%lluMiB kChunkMax=%lluMiB",
             label,
             (unsigned long long)need,
             (unsigned long long)nmemb,
             (unsigned long long)size,
             (unsigned long long)(g_arenaBytes >> 20),
             (unsigned long long)(kMaxSingle >> 20),
             (unsigned long long)(kChunkMax >> 20));
    // 2026-10-05: 同一个出口再登记一次到 g_failWhy/g_failDetail。分配失败是
    // "出口 A(longjmp)"的一个子类, 单独打标签才能在 o.unit 里与 chibicc 的语法错误
    // 区分开(chibicc 的错误原文仍然照常写在 aurora_cc_last_error 里, 两者并存)。
    char d[176];
    snprintf(d, sizeof(d),
             "%s need=%lluB nmemb=%llu size=%llu arenaBytes=%lluMiB",
             label, (unsigned long long)need, (unsigned long long)nmemb,
             (unsigned long long)size, (unsigned long long)(g_arenaBytes >> 20));
    ccSetFail("cc-alloc-failed", d);
}

extern "C" void *aurora_cc_calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0) {
        nmemb = 1;
        size = 1;
    }
    if (nmemb > (SIZE_MAX - kHeader) / size) {
        // 静默分支 1/4 nmemb*size 乘法溢出(整数回绕会让"要大块"变成"要小块")
        ccNoteAllocFailure("cc-calloc size overflow (nmemb*size)", 0, nmemb, size);
        aurora_cc_on_error(); // 乘法溢出
    }
    const size_t need = nmemb * size;
    void *p = arenaPayload(need);
    if (p == nullptr) {
        // 静默分支 2/4 arenaPayload 返回 null 只有两种原因, 必须分开写:
        //   ① need > kMaxSingle(单次分配超上限)  ② arena 向系统要 chunk 失败(本机内存不够)
        ccNoteAllocFailure(need > kMaxSingle ? "cc-calloc single > kMaxSingle" : "arena exhausted",
                           need, nmemb, size);
        aurora_cc_on_error(); // arena 耗尽
    }
    return p;
}

extern "C" void *aurora_cc_realloc(void *ptr, size_t size)
{
    if (ptr == nullptr) {
        return aurora_cc_calloc(1, size == 0 ? 1 : size);
    }
    size_t oldNeed = *(size_t *)((unsigned char *)ptr - kHeader);
    if (size == 0) {
        size = 1;
    }
    if (size > kMaxSingle) {
        // 静默分支 3/4 realloc 请求超过单次分配上限
        ccNoteAllocFailure("cc-realloc size > kMaxSingle", size, 1, size);
        aurora_cc_on_error();
    }
    void *np = arenaPayload(size);
    if (np == nullptr) {
        // 静默分支 4/4(走到这里说明 size <= kMaxSingle, 所以只可能是 arena 整体耗尽)
        ccNoteAllocFailure("realloc arena exhausted", size, 1, size);
        aurora_cc_on_error();
    }
    size_t n = (oldNeed < size) ? oldNeed : size;
    if (n != 0) {
        memcpy(np, ptr, n);
    }
    return np;
}

extern "C" char *aurora_cc_strdup(const char *s)
{
    if (s == nullptr) {
        return nullptr;
    }
    size_t n = strlen(s);
    char *p = (char *)aurora_cc_calloc(n + 1, 1);
    memcpy(p, s, n + 1);
    return p;
}

extern "C" char *aurora_cc_strndup(const char *s, size_t n)
{
    if (s == nullptr) {
        return nullptr;
    }
    size_t len = 0;
    while (len < n && s[len] != '\0') {
        len++;
    }
    char *p = (char *)aurora_cc_calloc(len + 1, 1);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

extern "C" void aurora_cc_on_error(void)
{
    if (g_errorArmed != 0) {
        g_errorArmed = 0;
        longjmp(g_errorJmp, 1);
    }
    // 只可能出现在被保护的编译窗口之外; 明确暴露而不是静默继续。
    fprintf(stderr, "gb7_clang: chibicc fatal error outside a guarded compile\n");
    abort();
}

// =============================================================================
// 2. 被测的 C 翻译单元 (2468 行, 自包含, 无任何 #include)
//    结构体 / 指针 / 数组 / 递归 / switch / for-while / 函数指针(局部表 + 静态表) /
//    位域 / 枚举 / 位运算 / 字符串处理 / 双精度浮点 / 静态内存池。
//    这段文本由 third_party/chibicc/gen_gb7_clang_source.py 离线生成, 并已用
//    aarch64-linux-ohos clang (-std=c99 与 -std=c11, -Wall -Wextra) 零警告编译验证。
// =============================================================================
static const char kUnitSource[] =
R"GB7CLANGUNIT(/* ---------------------------------------------------------------------
 * Aurora GB7 "Clang" workload -- the C translation unit handed to the
 * embedded chibicc compiler.
 *
 * It is deliberately self contained: chibicc runs without any system
 * include path, so this file contains no #include directive and calls
 * nothing from libc.  Data lives in a fixed static pool instead of the
 * heap.  "#define" is the only preprocessor directive that is used.
 *
 * Content: structs, unions-free aggregates, pointers, arrays, recursion,
 * switch, for/while, function pointers (local and static tables), bit
 * fields, enumerations, bit twiddling, string handling and floating
 * point arithmetic.
 * --------------------------------------------------------------------- */

#define AURORA_POOL_BYTES 262144
#define AURORA_LIST_NODES 40
#define AURORA_ROUNDS 12
#define AURORA_MAX(a, b) ((a) > (b) ? (a) : (b))
#define AURORA_MIN(a, b) ((a) < (b) ? (a) : (b))
#define AURORA_ABS(v) ((v) < 0 ? -(v) : (v))
#define AURORA_SWAP_INT(a, b) do { int aurora_swap_tmp = (a); (a) = (b); (b) = aurora_swap_tmp; } while (0)
#define AURORA_IS_EVEN(v) (((v) & 1) == 0)

typedef unsigned char u8;
typedef signed char i8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef long i64;

typedef struct aurora_point { int x; int y; } aurora_point;
typedef struct aurora_entry { char name[16]; int score; aurora_point pos; } aurora_entry;
typedef struct aurora_pair { int first; int second; } aurora_pair;
typedef struct aurora_bits { unsigned int lo : 5; unsigned int mid : 11; unsigned int hi : 16; } aurora_bits;
typedef struct aurora_node aurora_node;
typedef struct aurora_ring { int head; int tail; int count; int slots[32]; } aurora_ring;
typedef int (*aurora_binop)(int, int);
typedef struct aurora_opinfo { const char *name; aurora_binop fn; } aurora_opinfo;

struct aurora_node { int key; int value; int height; aurora_node *left; aurora_node *right; };

enum aurora_state { AURORA_IDLE = 0, AURORA_RUN = 1, AURORA_WAIT = 2, AURORA_DONE = 3 };

static u8 aurora_pool[AURORA_POOL_BYTES];
static u64 aurora_pool_used;
static int aurora_failures;
static long aurora_checksum;
static const char *aurora_titles[6] = {
  "idle", "run", "wait", "done", "error", "blank"
};

static void *aurora_pool_alloc(u64 size) {
  u64 aligned = (size + 15ul) & ~15ul;
  if (aligned == 0ul) {
    aligned = 16ul;
  }
  if (aurora_pool_used + aligned > (u64)AURORA_POOL_BYTES) {
    aurora_failures = aurora_failures + 1;
    return 0;
  }
  {
    void *result = (void *)(aurora_pool + aurora_pool_used);
    aurora_pool_used = aurora_pool_used + aligned;
    return result;
  }
}

static void aurora_pool_reset(void) {
  aurora_pool_used = 0ul;
}

static int aurora_pool_bytes(void) {
  return (int)aurora_pool_used;
}

static aurora_node *aurora_node_new(int key, int value) {
  aurora_node *node = (aurora_node *)aurora_pool_alloc((u64)sizeof(aurora_node));
  if (node == 0) {
    return 0;
  }
  node->key = key;
  node->value = value;
  node->height = 1;
  node->left = 0;
  node->right = 0;
  return node;
}

static int aurora_sign(int value) {
  if (value > 0) {
    return 1;
  }
  if (value < 0) {
    return -1;
  }
  return 0;
}

static int aurora_clamp(int value, int low, int high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

static aurora_point aurora_point_make(int x, int y) {
  aurora_point p;
  p.x = x;
  p.y = y;
  return p;
}

static u32 k00_popcount(u32 v) {
  u32 count = 0;
  while (v != 0u) {
    count = count + (v & 1u);
    v = v >> 1;
  }
  return count;
}

static u32 k00_reverse_bits(u32 v) {
  u32 out = 0;
  int i = 0;
  while (i < 32) {
    out = (out << 1) | (v & 1u);
    v = v >> 1;
    i = i + 1;
  }
  return out;
}

static u32 k00_rotate_left(u32 v, int n) {
  int k = n & 31;
  if (k == 0) {
    return v;
  }
  return (v << k) | (v >> (32 - k));
}

static u32 k00_mix(u32 a, u32 b) {
  u32 x = a ^ (b << 7);
  u32 y = b ^ (a >> 3);
  return (x & 0x00FF00FFu) | (y & 0xFF00FF00u);
}

static u32 k00_crc32(const u8 *data, int len) {
  u32 crc = 0xFFFFFFFFu;
  int i = 0;
  while (i < len) {
    int bit = 0;
    crc = crc ^ (u32)data[i];
    while (bit < 8) {
      if ((crc & 1u) != 0u) {
        crc = (crc >> 1) ^ 0xEDB88320u;
      } else {
        crc = crc >> 1;
      }
      bit = bit + 1;
    }
    i = i + 1;
  }
  return crc ^ 0xFFFFFFFFu;
}

static int k00_kernel(void) {
  u8 buffer[64];
  u32 acc = 2166136261u;
  int i = 0;
  while (i < 64) {
    buffer[i] = (u8)((i * 37 + 11) & 0xFF);
    i = i + 1;
  }
  i = 0;
  while (i < 32) {
    u32 v = (u32)i * 2654435761u + 12345u;
    acc = acc + k00_popcount(v);
    acc = acc + k00_reverse_bits(v >> (i & 7));
    acc = acc + k00_rotate_left(v, i);
    acc = acc ^ k00_mix(v, acc);
    i = i + 1;
  }
  acc = acc ^ k00_crc32(buffer, 64);
  return (int)(acc & 0x7FFFFFFFu);
}

static aurora_node *k01_push(aurora_node *head, int value) {
  aurora_node *node = aurora_node_new(value, value * 3 + 1);
  if (node == 0) {
    return head;
  }
  node->right = head;
  return node;
}

static int k01_sum(const aurora_node *head) {
  int total = 0;
  const aurora_node *cur = head;
  while (cur != 0) {
    total = total + cur->value;
    cur = cur->right;
  }
  return total;
}

static int k01_length(const aurora_node *head) {
  int n = 0;
  const aurora_node *cur = head;
  while (cur != 0) {
    n = n + 1;
    cur = cur->right;
  }
  return n;
}

static aurora_node *k01_reverse(aurora_node *head) {
  aurora_node *prev = 0;
  aurora_node *cur = head;
  while (cur != 0) {
    aurora_node *next = cur->right;
    cur->right = prev;
    prev = cur;
    cur = next;
  }
  return prev;
}

static int k01_kernel(void) {
  aurora_node *head = 0;
  int i = 0;
  int total = 0;
  while (i < AURORA_LIST_NODES) {
    head = k01_push(head, i * 7 + 3);
    i = i + 1;
  }
  total = total + k01_sum(head);
  total = total + k01_length(head);
  head = k01_reverse(head);
  total = total + k01_sum(head);
  return total;
}

static aurora_node *k02_insert(aurora_node *root, int key, int value) {
  if (root == 0) {
    return aurora_node_new(key, value);
  }
  if (key < root->key) {
    root->left = k02_insert(root->left, key, value);
  } else if (key > root->key) {
    root->right = k02_insert(root->right, key, value);
  } else {
    root->value = root->value + value;
  }
  return root;
}

static int k02_depth(const aurora_node *root) {
  int left = 0;
  int right = 0;
  if (root == 0) {
    return 0;
  }
  left = k02_depth(root->left);
  right = k02_depth(root->right);
  if (left > right) {
    return left + 1;
  }
  return right + 1;
}

static int k02_find(const aurora_node *root, int key) {
  while (root != 0) {
    if (key == root->key) {
      return root->value;
    }
    if (key < root->key) {
      root = root->left;
    } else {
      root = root->right;
    }
  }
  return -1;
}

static int k02_sum(const aurora_node *root) {
  if (root == 0) {
    return 0;
  }
  return root->value + k02_sum(root->left) + k02_sum(root->right);
}

static int k02_kernel(void) {
  aurora_node *root = 0;
  int i = 0;
  int total = 0;
  while (i < 64) {
    root = k02_insert(root, (i * 29) % 61, i + 1);
    i = i + 1;
  }
  total = total + k02_depth(root);
  total = total + k02_sum(root);
  total = total + k02_find(root, 17);
  return total;
}

static void k03_swap(int *a, int *b) {
  AURORA_SWAP_INT(*a, *b);
}

static void k03_insertion_sort(int *data, int n) {
  int i = 1;
  while (i < n) {
    int j = i;
    while (j > 0 && data[j - 1] > data[j]) {
      k03_swap(&data[j - 1], &data[j]);
      j = j - 1;
    }
    i = i + 1;
  }
}

static void k03_quick_sort(int *data, int lo, int hi) {
  int i = 0;
  int j = 0;
  int pivot = 0;
  if (lo >= hi) {
    return;
  }
  pivot = data[lo + (hi - lo) / 2];
  i = lo;
  j = hi;
  while (i <= j) {
    while (data[i] < pivot) {
      i = i + 1;
    }
    while (data[j] > pivot) {
      j = j - 1;
    }
    if (i <= j) {
      k03_swap(&data[i], &data[j]);
      i = i + 1;
      j = j - 1;
    }
  }
  k03_quick_sort(data, lo, j);
  k03_quick_sort(data, i, hi);
}

static int k03_is_sorted(const int *data, int n) {
  int i = 1;
  while (i < n) {
    if (data[i - 1] > data[i]) {
      return 0;
    }
    i = i + 1;
  }
  return 1;
}

static void k03_merge(const int *a, int na, const int *b, int nb, int *out) {
  int i = 0;
  int j = 0;
  int k = 0;
  while (i < na && j < nb) {
    if (a[i] <= b[j]) {
      out[k] = a[i];
      i = i + 1;
    } else {
      out[k] = b[j];
      j = j + 1;
    }
    k = k + 1;
  }
  while (i < na) {
    out[k] = a[i];
    i = i + 1;
    k = k + 1;
  }
  while (j < nb) {
    out[k] = b[j];
    j = j + 1;
    k = k + 1;
  }
}

static int k03_kernel(void) {
  int data[48];
  int scratch[48];
  int merged[96];
  u32 state = 20240101u;
  int i = 0;
  while (i < 48) {
    state = state * 1103515245u + 12345u;
    data[i] = (int)((state >> 8) & 0x3FFu) - 512;
    scratch[i] = data[i];
    i = i + 1;
  }
  k03_insertion_sort(data, 48);
  k03_quick_sort(scratch, 0, 47);
  k03_merge(data, 24, scratch + 24, 24, merged);
  return k03_is_sorted(data, 48) + k03_is_sorted(scratch, 48) + merged[0] + merged[95];
}

static int k04_length(const char *s) {
  int n = 0;
  while (s[n] != 0) {
    n = n + 1;
  }
  return n;
}

static void k04_copy(char *dst, const char *src, int cap) {
  int i = 0;
  if (cap <= 0) {
    return;
  }
  while (i < cap - 1 && src[i] != 0) {
    dst[i] = src[i];
    i = i + 1;
  }
  dst[i] = 0;
}

static int k04_compare(const char *a, const char *b) {
  int i = 0;
  while (a[i] != 0 && a[i] == b[i]) {
    i = i + 1;
  }
  return (int)(u8)a[i] - (int)(u8)b[i];
}

static u32 k04_hash(const char *s) {
  u32 h = 2166136261u;
  int i = 0;
  while (s[i] != 0) {
    h = h ^ (u32)(u8)s[i];
    h = h * 16777619u;
    i = i + 1;
  }
  return h;
}

static int k04_to_decimal(long value, char *out, int cap) {
  char tmp[24];
  int n = 0;
  int i = 0;
  int negative = 0;
  unsigned long v = 0ul;
  if (cap <= 1) {
    return 0;
  }
  if (value < 0) {
    negative = 1;
    v = (unsigned long)(-(value + 1)) + 1ul;
  } else {
    v = (unsigned long)value;
  }
  do {
    tmp[n] = (char)('0' + (int)(v % 10ul));
    v = v / 10ul;
    n = n + 1;
  } while (v != 0ul && n < 22);
  if (negative != 0) {
    tmp[n] = '-';
    n = n + 1;
  }
  while (i < n && i < cap - 1) {
    out[i] = tmp[n - 1 - i];
    i = i + 1;
  }
  out[i] = 0;
  return i;
}

static int k04_index_of(const char *haystack, const char *needle) {
  int i = 0;
  int j = 0;
  if (needle[0] == 0) {
    return 0;
  }
  while (haystack[i] != 0) {
    j = 0;
    while (needle[j] != 0 && haystack[i + j] == needle[j]) {
      j = j + 1;
    }
    if (needle[j] == 0) {
      return i;
    }
    i = i + 1;
  }
  return -1;
}

static int k04_kernel(void) {
  char text[64];
  char number[24];
  int total = 0;
  total = total + k04_length("aurora benchmark suite");
  k04_copy(text, "aurora benchmark suite", 64);
  total = total + k04_compare(text, "aurora benchmark suite");
  total = total + (int)(k04_hash(text) & 0xFFFFu);
  total = total + k04_to_decimal(-123456789L, number, 24);
  total = total + k04_length(number);
  total = total + k04_index_of(text, "bench");
  return total;
}

#define k05_N 16

static void k05_mul(const int *a, const int *b, int *out) {
  int i = 0;
  while (i < k05_N) {
    int j = 0;
    while (j < k05_N) {
      int acc = 0;
      int k = 0;
      while (k < k05_N) {
        acc = acc + a[i * k05_N + k] * b[k * k05_N + j];
        k = k + 1;
      }
      out[i * k05_N + j] = acc;
      j = j + 1;
    }
    i = i + 1;
  }
}

static int k05_trace(const int *a) {
  int i = 0;
  int acc = 0;
  while (i < k05_N) {
    acc = acc + a[i * k05_N + i];
    i = i + 1;
  }
  return acc;
}

static void k05_transpose(const int *a, int *out) {
  int i = 0;
  while (i < k05_N) {
    int j = 0;
    while (j < k05_N) {
      out[j * k05_N + i] = a[i * k05_N + j];
      j = j + 1;
    }
    i = i + 1;
  }
}

static int k05_det(const int *m, int n) {
  int minor[64];
  int sign = 1;
  int acc = 0;
  int col = 0;
  if (n <= 1) {
    return m[0];
  }
  while (col < n) {
    int r = 1;
    int mi = 0;
    while (r < n) {
      int c = 0;
      while (c < n) {
        if (c != col) {
          minor[mi] = m[r * n + c];
          mi = mi + 1;
        }
        c = c + 1;
      }
      r = r + 1;
    }
    acc = acc + sign * m[col] * k05_det(minor, n - 1);
    sign = -sign;
    col = col + 1;
  }
  return acc;
}

static int k05_kernel(void) {
  int a[k05_N * k05_N];
  int b[k05_N * k05_N];
  int c[k05_N * k05_N];
  int t[k05_N * k05_N];
  int small[64];
  int i = 0;
  while (i < k05_N * k05_N) {
    a[i] = (i % 7) - 3;
    b[i] = ((i * 5) % 11) - 5;
    c[i] = 0;
    t[i] = 0;
    i = i + 1;
  }
  k05_mul(a, b, c);
  k05_transpose(c, t);
  i = 0;
  while (i < 64) {
    small[i] = ((i * 3) % 5) - 2;
    i = i + 1;
  }
  return k05_trace(t) + k05_det(small, 8);
}

#define k06_OP_HALT 0
#define k06_OP_PUSH 1
#define k06_OP_ADD 2
#define k06_OP_SUB 3
#define k06_OP_MUL 4
#define k06_OP_AND 5
#define k06_OP_OR 6
#define k06_OP_XOR 7
#define k06_OP_SHL 8
#define k06_OP_SHR 9
#define k06_OP_NOT 10
#define k06_OP_DUP 11
#define k06_OP_DROP 12

static int k06_run(const int *code, int len) {
  int stack[16];
  int sp = 0;
  int acc = 0;
  int pc = 0;
  while (pc < len) {
    int op = code[pc];
    pc = pc + 1;
    switch (op) {
    case k06_OP_HALT:
      pc = len;
      break;
    case k06_OP_PUSH:
      if (pc < len && sp < 16) {
        stack[sp] = code[pc];
        sp = sp + 1;
      }
      if (pc < len) {
        pc = pc + 1;
      }
      break;
    case k06_OP_ADD:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] + stack[sp];
      }
      break;
    case k06_OP_SUB:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] - stack[sp];
      }
      break;
    case k06_OP_MUL:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] * stack[sp];
      }
      break;
    case k06_OP_AND:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] & stack[sp];
      }
      break;
    case k06_OP_OR:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] | stack[sp];
      }
      break;
    case k06_OP_XOR:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] ^ stack[sp];
      }
      break;
    case k06_OP_SHL:
      if (sp >= 1) {
        stack[sp - 1] = stack[sp - 1] << (acc & 7);
      }
      break;
    case k06_OP_SHR:
      if (sp >= 1) {
        stack[sp - 1] = stack[sp - 1] >> (acc & 7);
      }
      break;
    case k06_OP_NOT:
      if (sp >= 1) {
        stack[sp - 1] = ~stack[sp - 1];
      }
      break;
    case k06_OP_DUP:
      if (sp >= 1 && sp < 16) {
        stack[sp] = stack[sp - 1];
        sp = sp + 1;
      }
      break;
    case k06_OP_DROP:
      if (sp >= 1) {
        sp = sp - 1;
      }
      break;
    default:
      acc = acc + op;
      break;
    }
    if (sp > 0) {
      acc = acc ^ stack[sp - 1];
    }
  }
  return acc + sp;
}

static int k06_kernel(void) {
  int program[64];
  int i = 0;
  int total = 0;
  while (i < 64) {
    int selector = i % 7;
    if (selector == 0) {
      program[i] = k06_OP_PUSH;
    } else if (selector == 1) {
      program[i] = k06_OP_ADD;
    } else if (selector == 2) {
      program[i] = k06_OP_MUL;
    } else if (selector == 3) {
      program[i] = k06_OP_XOR;
    } else if (selector == 4) {
      program[i] = k06_OP_DUP;
    } else if (selector == 5) {
      program[i] = k06_OP_DROP;
    } else {
      program[i] = i - 3;
    }
    i = i + 1;
  }
  total = total + k06_run(program, 64);
  program[63] = k06_OP_HALT;
  total = total + k06_run(program, 64);
  return total;
}

#define k07_SLOTS 64

static u32 k07_hash_key(int key) {
  u32 v = (u32)key;
  v = v ^ (v >> 16);
  v = v * 2246822519u;
  v = v ^ (v >> 13);
  return v;
}

static void k07_put(int *keys, int *values, int key, int value) {
  u32 slot = k07_hash_key(key) % (u32)k07_SLOTS;
  int probe = 0;
  while (probe < k07_SLOTS) {
    int idx = (int)((slot + (u32)probe) % (u32)k07_SLOTS);
    if (keys[idx] == -1) {
      keys[idx] = key;
      values[idx] = value;
      return;
    }
    if (keys[idx] == key) {
      values[idx] = values[idx] + value;
      return;
    }
    probe = probe + 1;
  }
}

static int k07_get(const int *keys, const int *values, int key) {
  u32 slot = k07_hash_key(key) % (u32)k07_SLOTS;
  int probe = 0;
  while (probe < k07_SLOTS) {
    int idx = (int)((slot + (u32)probe) % (u32)k07_SLOTS);
    if (keys[idx] == -1) {
      return 0;
    }
    if (keys[idx] == key) {
      return values[idx];
    }
    probe = probe + 1;
  }
  return 0;
}

static int k07_kernel(void) {
  int keys[k07_SLOTS];
  int values[k07_SLOTS];
  int i = 0;
  int total = 0;
  while (i < k07_SLOTS) {
    keys[i] = -1;
    values[i] = 0;
    i = i + 1;
  }
  i = 0;
  while (i < 48) {
    k07_put(keys, values, (i * 17) % 97, i + 1);
    i = i + 1;
  }
  i = 0;
  while (i < 48) {
    total = total + k07_get(keys, values, (i * 17) % 97);
    i = i + 1;
  }
  return total;
}

#define k08_CAP 32

static void k08_init(aurora_ring *ring) {
  int i = 0;
  ring->head = 0;
  ring->tail = 0;
  ring->count = 0;
  while (i < k08_CAP) {
    ring->slots[i] = 0;
    i = i + 1;
  }
}

static int k08_push(aurora_ring *ring, int value) {
  if (ring->count >= k08_CAP) {
    return 0;
  }
  ring->slots[ring->tail] = value;
  ring->tail = (ring->tail + 1) % k08_CAP;
  ring->count = ring->count + 1;
  return 1;
}

static int k08_pop(aurora_ring *ring, int *out) {
  if (ring->count <= 0) {
    return 0;
  }
  *out = ring->slots[ring->head];
  ring->head = (ring->head + 1) % k08_CAP;
  ring->count = ring->count - 1;
  return 1;
}

static int k08_kernel(void) {
  aurora_ring ring;
  int value = 0;
  int total = 0;
  int i = 0;
  k08_init(&ring);
  while (i < 96) {
    if (AURORA_IS_EVEN(i)) {
      if (k08_push(&ring, i * 3) == 0) {
        total = total + 1;
      }
    } else {
      if (k08_pop(&ring, &value) != 0) {
        total = total + value;
      }
    }
    i = i + 1;
  }
  while (k08_pop(&ring, &value) != 0) {
    total = total + value;
  }
  return total;
}

static int k09_op_add(int a, int b) { return a + b; }
static int k09_op_sub(int a, int b) { return a - b; }
static int k09_op_mul(int a, int b) { return a * b; }
static int k09_op_div(int a, int b) { if (b == 0) { return 0; } return a / b; }
static int k09_op_mod(int a, int b) { if (b == 0) { return 0; } return a % b; }
static int k09_op_and(int a, int b) { return a & b; }
static int k09_op_or(int a, int b) { return a | b; }
static int k09_op_xor(int a, int b) { return a ^ b; }

static int k09_dispatch(int op, int a, int b) {
  aurora_binop table[8];
  table[0] = k09_op_add;
  table[1] = k09_op_sub;
  table[2] = k09_op_mul;
  table[3] = k09_op_div;
  table[4] = k09_op_mod;
  table[5] = k09_op_and;
  table[6] = k09_op_or;
  table[7] = k09_op_xor;
  if (op < 0 || op >= 8) {
    return 0;
  }
  return table[op](a, b);
}

static int k09_kernel(void) {
  int i = 0;
  int total = 0;
  while (i < 64) {
    total = total + k09_dispatch(i % 8, i * 3 - 20, i + 3);
    i = i + 1;
  }
  return total;
}

static u32 k10_pack(const aurora_bits *bits) {
  return ((u32)bits->lo << 27) | ((u32)bits->mid << 16) | (u32)bits->hi;
}

static void k10_unpack(u32 word, aurora_bits *bits) {
  bits->lo = (unsigned int)((word >> 27) & 31u);
  bits->mid = (unsigned int)((word >> 16) & 2047u);
  bits->hi = (unsigned int)(word & 65535u);
}

static int k10_kernel(void) {
  aurora_bits bits;
  u32 word = 0u;
  int i = 0;
  int total = 0;
  bits.lo = 0;
  bits.mid = 0;
  bits.hi = 0;
  while (i < 32) {
    k10_unpack((u32)i * 2654435761u, &bits);
    word = word ^ k10_pack(&bits);
    total = total + (int)bits.lo + (int)bits.mid + (int)bits.hi;
    i = i + 1;
  }
  return total + (int)(word & 0xFFFFu);
}

static int k11_gcd(int a, int b) {
  int x = a;
  int y = b;
  while (y != 0) {
    int t = x % y;
    x = y;
    y = t;
  }
  if (x < 0) {
    return -x;
  }
  return x;
}

static long k11_modexp(long base, long exp, long mod) {
  long result = 1;
  long b = 0;
  long e = exp;
  if (mod <= 1) {
    return 0;
  }
  b = base % mod;
  if (b < 0) {
    b = b + mod;
  }
  while (e > 0) {
    if ((e & 1) != 0) {
      result = (result * b) % mod;
    }
    b = (b * b) % mod;
    e = e >> 1;
  }
  return result;
}

static int k11_fib(int n) {
  if (n < 2) {
    return n;
  }
  return k11_fib(n - 1) + k11_fib(n - 2);
}

static int k11_sieve(u8 *flags, int limit) {
  int count = 0;
  int i = 2;
  while (i < limit) {
    flags[i] = 1;
    i = i + 1;
  }
  i = 2;
  while (i * i < limit) {
    if (flags[i] != 0) {
      int j = i * i;
      while (j < limit) {
        flags[j] = 0;
        j = j + i;
      }
    }
    i = i + 1;
  }
  i = 2;
  while (i < limit) {
    count = count + flags[i];
    i = i + 1;
  }
  return count;
}

static int k11_kernel(void) {
  u8 flags[256];
  int total = 0;
  int i = 0;
  while (i < 24) {
    total = total + k11_gcd(i * 13 + 7, 91);
    i = i + 1;
  }
  total = total + k11_fib(18);
  total = total + k11_sieve(flags, 256);
  total = total + (int)k11_modexp(7, 129, 1000003);
  return total;
}

static int k12_is_digit(char c) {
  return c >= '0' && c <= '9';
}

static int k12_is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int k12_is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n';
}

static int k12_scan(const char *text, int *out, int cap) {
  int i = 0;
  int n = 0;
  while (text[i] != 0) {
    char c = text[i];
    if (k12_is_space(c)) {
      i = i + 1;
    } else if (k12_is_digit(c)) {
      int value = 0;
      while (k12_is_digit(text[i])) {
        value = value * 10 + (int)(text[i] - '0');
        i = i + 1;
      }
      if (n < cap) {
        out[n] = value;
        n = n + 1;
      }
    } else if (k12_is_alpha(c)) {
      int hash = 0;
      while (k12_is_alpha(text[i]) || k12_is_digit(text[i])) {
        hash = (hash * 31 + (int)(u8)text[i]) & 0x7FFFFFFF;
        i = i + 1;
      }
      if (n < cap) {
        out[n] = hash;
        n = n + 1;
      }
    } else {
      i = i + 1;
    }
  }
  return n;
}

static int k12_kernel(void) {
  int tokens[32];
  int n = 0;
  int total = 0;
  int i = 0;
  n = k12_scan("alpha 12 beta_3 4096 gamma 7 delta42", tokens, 32);
  total = total + n;
  while (i < n) {
    total = total + (tokens[i] & 0xFF);
    i = i + 1;
  }
  return total;
}

static double k13_distance2(double ax, double ay, double bx, double by) {
  double dx = ax - bx;
  double dy = ay - by;
  return dx * dx + dy * dy;
}

static double k13_sqrt_newton(double value) {
  double guess = value;
  int i = 0;
  if (value <= 0.0) {
    return 0.0;
  }
  if (guess < 1.0) {
    guess = 1.0;
  }
  while (i < 24) {
    guess = 0.5 * (guess + value / guess);
    i = i + 1;
  }
  return guess;
}

static double k13_area(const aurora_point *pts, int n) {
  double area = 0.0;
  int i = 0;
  if (n < 3) {
    return 0.0;
  }
  while (i < n) {
    int j = (i + 1) % n;
    area = area + (double)pts[i].x * (double)pts[j].y - (double)pts[j].x * (double)pts[i].y;
    i = i + 1;
  }
  return area * 0.5;
}

static int k13_kernel(void) {
  aurora_point pts[12];
  double total = 0.0;
  int i = 0;
  while (i < 12) {
    pts[i] = aurora_point_make(i * 3 - 7, (i * i) % 11 - 5);
    i = i + 1;
  }
  total = total + k13_area(pts, 12);
  i = 0;
  while (i < 12) {
    total = total + k13_sqrt_newton(k13_distance2(0.0, 0.0, (double)pts[i].x, (double)pts[i].y) + 1.0);
    i = i + 1;
  }
  return (int)total;
}

static int k14_tbl_add(int a, int b) { return a + b; }
static int k14_tbl_sub(int a, int b) { return a - b; }
static int k14_tbl_mul(int a, int b) { return a * b; }
static int k14_tbl_max(int a, int b) { return AURORA_MAX(a, b); }
static int k14_tbl_min(int a, int b) { return AURORA_MIN(a, b); }
static int k14_tbl_or(int a, int b) { return a | b; }
static int k14_tbl_and(int a, int b) { return a & b; }
static int k14_tbl_xor(int a, int b) { return a ^ b; }

static const aurora_opinfo k14_ops[8] = {
  { "add", k14_tbl_add },
  { "sub", k14_tbl_sub },
  { "mul", k14_tbl_mul },
  { "max", k14_tbl_max },
  { "min", k14_tbl_min },
  { "or", k14_tbl_or },
  { "and", k14_tbl_and },
  { "xor", k14_tbl_xor }
};

static int k14_kernel(void) {
  int i = 0;
  int total = 0;
  while (i < 8) {
    aurora_binop fn = k14_ops[i].fn;
    total = total + fn(i * 5 + 1, i + 2);
    total = total + (int)(u8)k14_ops[i].name[0];
    i = i + 1;
  }
  return total;
}

static int k15_step(int state, int input) {
  switch (state) {
  case AURORA_IDLE:
    if (input > 0) {
      return AURORA_RUN;
    }
    return AURORA_IDLE;
  case AURORA_RUN:
    if (input < 0) {
      return AURORA_WAIT;
    }
    if (input == 0) {
      return AURORA_DONE;
    }
    return AURORA_RUN;
  case AURORA_WAIT:
    if (input > 16) {
      return AURORA_DONE;
    }
    if (input > 0) {
      return AURORA_RUN;
    }
    return AURORA_WAIT;
  case AURORA_DONE:
    return AURORA_IDLE;
  default:
    return AURORA_IDLE;
  }
}

static int k15_label(int state) {
  switch (state) {
  case AURORA_IDLE:
    return 0;
  case AURORA_RUN:
    return 1;
  case AURORA_WAIT:
    return 2;
  case AURORA_DONE:
    return 3;
  default:
    return 4;
  }
}

static int k15_kernel(void) {
  int state = AURORA_IDLE;
  int total = 0;
  int i = 0;
  while (i < 96) {
    int input = (i * 7) % 23 - 4;
    state = k15_step(state, input);
    total = total + k15_label(state);
    total = total + (int)(u8)aurora_titles[k15_label(state) % 6][0];
    total = total + aurora_sign(input) + aurora_clamp(input, -3, 3);
    i = i + 1;
  }
  return total;
}

static void k16_fill(aurora_entry *entry, const char *name, int score, int x, int y) {
  int i = 0;
  while (i < 15 && name[i] != 0) {
    entry->name[i] = name[i];
    i = i + 1;
  }
  entry->name[i] = 0;
  entry->score = score;
  entry->pos = aurora_point_make(x, y);
}

static int k16_rank(const aurora_entry *entries, int n) {
  int best = -1;
  int best_index = -1;
  int i = 0;
  while (i < n) {
    if (entries[i].score > best) {
      best = entries[i].score;
      best_index = i;
    }
    i = i + 1;
  }
  return best_index;
}

static int k16_kernel(void) {
  aurora_entry entries[16];
  aurora_pair pairs[16];
  int i = 0;
  int total = 0;
  while (i < 16) {
    entries[i].name[0] = 0;
    entries[i].score = 0;
    entries[i].pos = aurora_point_make(0, 0);
    pairs[i].first = i * 3;
    pairs[i].second = i * 5;
    i = i + 1;
  }
  k16_fill(&entries[0], "aurora", 90, 1, 2);
  k16_fill(&entries[1], "bench", 42, 3, 4);
  k16_fill(&entries[2], "clang", 77, 5, 6);
  i = 3;
  while (i < 16) {
    k16_fill(&entries[i], "unit", i * 7, i, i * 2);
    i = i + 1;
  }
  total = total + k16_rank(entries, 16);
  i = 0;
  while (i < 16) {
    total = total + entries[i].score + entries[i].pos.x + pairs[i].first + pairs[i].second;
    i = i + 1;
  }
  return total;
}

static u32 k17_popcount(u32 v) {
  u32 count = 0;
  while (v != 0u) {
    count = count + (v & 1u);
    v = v >> 1;
  }
  return count;
}

static u32 k17_reverse_bits(u32 v) {
  u32 out = 0;
  int i = 0;
  while (i < 32) {
    out = (out << 1) | (v & 1u);
    v = v >> 1;
    i = i + 1;
  }
  return out;
}

static u32 k17_rotate_left(u32 v, int n) {
  int k = n & 31;
  if (k == 0) {
    return v;
  }
  return (v << k) | (v >> (32 - k));
}

static u32 k17_mix(u32 a, u32 b) {
  u32 x = a ^ (b << 7);
  u32 y = b ^ (a >> 3);
  return (x & 0x00FF00FFu) | (y & 0xFF00FF00u);
}

static u32 k17_crc32(const u8 *data, int len) {
  u32 crc = 0xFFFFFFFFu;
  int i = 0;
  while (i < len) {
    int bit = 0;
    crc = crc ^ (u32)data[i];
    while (bit < 8) {
      if ((crc & 1u) != 0u) {
        crc = (crc >> 1) ^ 0xEDB88320u;
      } else {
        crc = crc >> 1;
      }
      bit = bit + 1;
    }
    i = i + 1;
  }
  return crc ^ 0xFFFFFFFFu;
}

static int k17_kernel(void) {
  u8 buffer[64];
  u32 acc = 2166136261u;
  int i = 0;
  while (i < 64) {
    buffer[i] = (u8)((i * 37 + 11) & 0xFF);
    i = i + 1;
  }
  i = 0;
  while (i < 32) {
    u32 v = (u32)i * 2654435761u + 12345u;
    acc = acc + k17_popcount(v);
    acc = acc + k17_reverse_bits(v >> (i & 7));
    acc = acc + k17_rotate_left(v, i);
    acc = acc ^ k17_mix(v, acc);
    i = i + 1;
  }
  acc = acc ^ k17_crc32(buffer, 64);
  return (int)(acc & 0x7FFFFFFFu);
}

static aurora_node *k18_push(aurora_node *head, int value) {
  aurora_node *node = aurora_node_new(value, value * 3 + 1);
  if (node == 0) {
    return head;
  }
  node->right = head;
  return node;
}

static int k18_sum(const aurora_node *head) {
  int total = 0;
  const aurora_node *cur = head;
  while (cur != 0) {
    total = total + cur->value;
    cur = cur->right;
  }
  return total;
}

static int k18_length(const aurora_node *head) {
  int n = 0;
  const aurora_node *cur = head;
  while (cur != 0) {
    n = n + 1;
    cur = cur->right;
  }
  return n;
}

static aurora_node *k18_reverse(aurora_node *head) {
  aurora_node *prev = 0;
  aurora_node *cur = head;
  while (cur != 0) {
    aurora_node *next = cur->right;
    cur->right = prev;
    prev = cur;
    cur = next;
  }
  return prev;
}

static int k18_kernel(void) {
  aurora_node *head = 0;
  int i = 0;
  int total = 0;
  while (i < AURORA_LIST_NODES) {
    head = k18_push(head, i * 7 + 3);
    i = i + 1;
  }
  total = total + k18_sum(head);
  total = total + k18_length(head);
  head = k18_reverse(head);
  total = total + k18_sum(head);
  return total;
}

static aurora_node *k19_insert(aurora_node *root, int key, int value) {
  if (root == 0) {
    return aurora_node_new(key, value);
  }
  if (key < root->key) {
    root->left = k19_insert(root->left, key, value);
  } else if (key > root->key) {
    root->right = k19_insert(root->right, key, value);
  } else {
    root->value = root->value + value;
  }
  return root;
}

static int k19_depth(const aurora_node *root) {
  int left = 0;
  int right = 0;
  if (root == 0) {
    return 0;
  }
  left = k19_depth(root->left);
  right = k19_depth(root->right);
  if (left > right) {
    return left + 1;
  }
  return right + 1;
}

static int k19_find(const aurora_node *root, int key) {
  while (root != 0) {
    if (key == root->key) {
      return root->value;
    }
    if (key < root->key) {
      root = root->left;
    } else {
      root = root->right;
    }
  }
  return -1;
}

static int k19_sum(const aurora_node *root) {
  if (root == 0) {
    return 0;
  }
  return root->value + k19_sum(root->left) + k19_sum(root->right);
}

static int k19_kernel(void) {
  aurora_node *root = 0;
  int i = 0;
  int total = 0;
  while (i < 64) {
    root = k19_insert(root, (i * 29) % 61, i + 1);
    i = i + 1;
  }
  total = total + k19_depth(root);
  total = total + k19_sum(root);
  total = total + k19_find(root, 17);
  return total;
}

static void k20_swap(int *a, int *b) {
  AURORA_SWAP_INT(*a, *b);
}

static void k20_insertion_sort(int *data, int n) {
  int i = 1;
  while (i < n) {
    int j = i;
    while (j > 0 && data[j - 1] > data[j]) {
      k20_swap(&data[j - 1], &data[j]);
      j = j - 1;
    }
    i = i + 1;
  }
}

static void k20_quick_sort(int *data, int lo, int hi) {
  int i = 0;
  int j = 0;
  int pivot = 0;
  if (lo >= hi) {
    return;
  }
  pivot = data[lo + (hi - lo) / 2];
  i = lo;
  j = hi;
  while (i <= j) {
    while (data[i] < pivot) {
      i = i + 1;
    }
    while (data[j] > pivot) {
      j = j - 1;
    }
    if (i <= j) {
      k20_swap(&data[i], &data[j]);
      i = i + 1;
      j = j - 1;
    }
  }
  k20_quick_sort(data, lo, j);
  k20_quick_sort(data, i, hi);
}

static int k20_is_sorted(const int *data, int n) {
  int i = 1;
  while (i < n) {
    if (data[i - 1] > data[i]) {
      return 0;
    }
    i = i + 1;
  }
  return 1;
}

static void k20_merge(const int *a, int na, const int *b, int nb, int *out) {
  int i = 0;
  int j = 0;
  int k = 0;
  while (i < na && j < nb) {
    if (a[i] <= b[j]) {
      out[k] = a[i];
      i = i + 1;
    } else {
      out[k] = b[j];
      j = j + 1;
    }
    k = k + 1;
  }
  while (i < na) {
    out[k] = a[i];
    i = i + 1;
    k = k + 1;
  }
  while (j < nb) {
    out[k] = b[j];
    j = j + 1;
    k = k + 1;
  }
}

static int k20_kernel(void) {
  int data[48];
  int scratch[48];
  int merged[96];
  u32 state = 20240101u;
  int i = 0;
  while (i < 48) {
    state = state * 1103515245u + 12345u;
    data[i] = (int)((state >> 8) & 0x3FFu) - 512;
    scratch[i] = data[i];
    i = i + 1;
  }
  k20_insertion_sort(data, 48);
  k20_quick_sort(scratch, 0, 47);
  k20_merge(data, 24, scratch + 24, 24, merged);
  return k20_is_sorted(data, 48) + k20_is_sorted(scratch, 48) + merged[0] + merged[95];
}

static int k21_length(const char *s) {
  int n = 0;
  while (s[n] != 0) {
    n = n + 1;
  }
  return n;
}

static void k21_copy(char *dst, const char *src, int cap) {
  int i = 0;
  if (cap <= 0) {
    return;
  }
  while (i < cap - 1 && src[i] != 0) {
    dst[i] = src[i];
    i = i + 1;
  }
  dst[i] = 0;
}

static int k21_compare(const char *a, const char *b) {
  int i = 0;
  while (a[i] != 0 && a[i] == b[i]) {
    i = i + 1;
  }
  return (int)(u8)a[i] - (int)(u8)b[i];
}

static u32 k21_hash(const char *s) {
  u32 h = 2166136261u;
  int i = 0;
  while (s[i] != 0) {
    h = h ^ (u32)(u8)s[i];
    h = h * 16777619u;
    i = i + 1;
  }
  return h;
}

static int k21_to_decimal(long value, char *out, int cap) {
  char tmp[24];
  int n = 0;
  int i = 0;
  int negative = 0;
  unsigned long v = 0ul;
  if (cap <= 1) {
    return 0;
  }
  if (value < 0) {
    negative = 1;
    v = (unsigned long)(-(value + 1)) + 1ul;
  } else {
    v = (unsigned long)value;
  }
  do {
    tmp[n] = (char)('0' + (int)(v % 10ul));
    v = v / 10ul;
    n = n + 1;
  } while (v != 0ul && n < 22);
  if (negative != 0) {
    tmp[n] = '-';
    n = n + 1;
  }
  while (i < n && i < cap - 1) {
    out[i] = tmp[n - 1 - i];
    i = i + 1;
  }
  out[i] = 0;
  return i;
}

static int k21_index_of(const char *haystack, const char *needle) {
  int i = 0;
  int j = 0;
  if (needle[0] == 0) {
    return 0;
  }
  while (haystack[i] != 0) {
    j = 0;
    while (needle[j] != 0 && haystack[i + j] == needle[j]) {
      j = j + 1;
    }
    if (needle[j] == 0) {
      return i;
    }
    i = i + 1;
  }
  return -1;
}

static int k21_kernel(void) {
  char text[64];
  char number[24];
  int total = 0;
  total = total + k21_length("aurora benchmark suite");
  k21_copy(text, "aurora benchmark suite", 64);
  total = total + k21_compare(text, "aurora benchmark suite");
  total = total + (int)(k21_hash(text) & 0xFFFFu);
  total = total + k21_to_decimal(-123456789L, number, 24);
  total = total + k21_length(number);
  total = total + k21_index_of(text, "bench");
  return total;
}

#define k22_N 16

static void k22_mul(const int *a, const int *b, int *out) {
  int i = 0;
  while (i < k22_N) {
    int j = 0;
    while (j < k22_N) {
      int acc = 0;
      int k = 0;
      while (k < k22_N) {
        acc = acc + a[i * k22_N + k] * b[k * k22_N + j];
        k = k + 1;
      }
      out[i * k22_N + j] = acc;
      j = j + 1;
    }
    i = i + 1;
  }
}

static int k22_trace(const int *a) {
  int i = 0;
  int acc = 0;
  while (i < k22_N) {
    acc = acc + a[i * k22_N + i];
    i = i + 1;
  }
  return acc;
}

static void k22_transpose(const int *a, int *out) {
  int i = 0;
  while (i < k22_N) {
    int j = 0;
    while (j < k22_N) {
      out[j * k22_N + i] = a[i * k22_N + j];
      j = j + 1;
    }
    i = i + 1;
  }
}

static int k22_det(const int *m, int n) {
  int minor[64];
  int sign = 1;
  int acc = 0;
  int col = 0;
  if (n <= 1) {
    return m[0];
  }
  while (col < n) {
    int r = 1;
    int mi = 0;
    while (r < n) {
      int c = 0;
      while (c < n) {
        if (c != col) {
          minor[mi] = m[r * n + c];
          mi = mi + 1;
        }
        c = c + 1;
      }
      r = r + 1;
    }
    acc = acc + sign * m[col] * k22_det(minor, n - 1);
    sign = -sign;
    col = col + 1;
  }
  return acc;
}

static int k22_kernel(void) {
  int a[k22_N * k22_N];
  int b[k22_N * k22_N];
  int c[k22_N * k22_N];
  int t[k22_N * k22_N];
  int small[64];
  int i = 0;
  while (i < k22_N * k22_N) {
    a[i] = (i % 7) - 3;
    b[i] = ((i * 5) % 11) - 5;
    c[i] = 0;
    t[i] = 0;
    i = i + 1;
  }
  k22_mul(a, b, c);
  k22_transpose(c, t);
  i = 0;
  while (i < 64) {
    small[i] = ((i * 3) % 5) - 2;
    i = i + 1;
  }
  return k22_trace(t) + k22_det(small, 8);
}

#define k23_OP_HALT 0
#define k23_OP_PUSH 1
#define k23_OP_ADD 2
#define k23_OP_SUB 3
#define k23_OP_MUL 4
#define k23_OP_AND 5
#define k23_OP_OR 6
#define k23_OP_XOR 7
#define k23_OP_SHL 8
#define k23_OP_SHR 9
#define k23_OP_NOT 10
#define k23_OP_DUP 11
#define k23_OP_DROP 12

static int k23_run(const int *code, int len) {
  int stack[16];
  int sp = 0;
  int acc = 0;
  int pc = 0;
  while (pc < len) {
    int op = code[pc];
    pc = pc + 1;
    switch (op) {
    case k23_OP_HALT:
      pc = len;
      break;
    case k23_OP_PUSH:
      if (pc < len && sp < 16) {
        stack[sp] = code[pc];
        sp = sp + 1;
      }
      if (pc < len) {
        pc = pc + 1;
      }
      break;
    case k23_OP_ADD:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] + stack[sp];
      }
      break;
    case k23_OP_SUB:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] - stack[sp];
      }
      break;
    case k23_OP_MUL:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] * stack[sp];
      }
      break;
    case k23_OP_AND:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] & stack[sp];
      }
      break;
    case k23_OP_OR:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] | stack[sp];
      }
      break;
    case k23_OP_XOR:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] ^ stack[sp];
      }
      break;
    case k23_OP_SHL:
      if (sp >= 1) {
        stack[sp - 1] = stack[sp - 1] << (acc & 7);
      }
      break;
    case k23_OP_SHR:
      if (sp >= 1) {
        stack[sp - 1] = stack[sp - 1] >> (acc & 7);
      }
      break;
    case k23_OP_NOT:
      if (sp >= 1) {
        stack[sp - 1] = ~stack[sp - 1];
      }
      break;
    case k23_OP_DUP:
      if (sp >= 1 && sp < 16) {
        stack[sp] = stack[sp - 1];
        sp = sp + 1;
      }
      break;
    case k23_OP_DROP:
      if (sp >= 1) {
        sp = sp - 1;
      }
      break;
    default:
      acc = acc + op;
      break;
    }
    if (sp > 0) {
      acc = acc ^ stack[sp - 1];
    }
  }
  return acc + sp;
}

static int k23_kernel(void) {
  int program[64];
  int i = 0;
  int total = 0;
  while (i < 64) {
    int selector = i % 7;
    if (selector == 0) {
      program[i] = k23_OP_PUSH;
    } else if (selector == 1) {
      program[i] = k23_OP_ADD;
    } else if (selector == 2) {
      program[i] = k23_OP_MUL;
    } else if (selector == 3) {
      program[i] = k23_OP_XOR;
    } else if (selector == 4) {
      program[i] = k23_OP_DUP;
    } else if (selector == 5) {
      program[i] = k23_OP_DROP;
    } else {
      program[i] = i - 3;
    }
    i = i + 1;
  }
  total = total + k23_run(program, 64);
  program[63] = k23_OP_HALT;
  total = total + k23_run(program, 64);
  return total;
}

#define k24_SLOTS 64

static u32 k24_hash_key(int key) {
  u32 v = (u32)key;
  v = v ^ (v >> 16);
  v = v * 2246822519u;
  v = v ^ (v >> 13);
  return v;
}

static void k24_put(int *keys, int *values, int key, int value) {
  u32 slot = k24_hash_key(key) % (u32)k24_SLOTS;
  int probe = 0;
  while (probe < k24_SLOTS) {
    int idx = (int)((slot + (u32)probe) % (u32)k24_SLOTS);
    if (keys[idx] == -1) {
      keys[idx] = key;
      values[idx] = value;
      return;
    }
    if (keys[idx] == key) {
      values[idx] = values[idx] + value;
      return;
    }
    probe = probe + 1;
  }
}

static int k24_get(const int *keys, const int *values, int key) {
  u32 slot = k24_hash_key(key) % (u32)k24_SLOTS;
  int probe = 0;
  while (probe < k24_SLOTS) {
    int idx = (int)((slot + (u32)probe) % (u32)k24_SLOTS);
    if (keys[idx] == -1) {
      return 0;
    }
    if (keys[idx] == key) {
      return values[idx];
    }
    probe = probe + 1;
  }
  return 0;
}

static int k24_kernel(void) {
  int keys[k24_SLOTS];
  int values[k24_SLOTS];
  int i = 0;
  int total = 0;
  while (i < k24_SLOTS) {
    keys[i] = -1;
    values[i] = 0;
    i = i + 1;
  }
  i = 0;
  while (i < 48) {
    k24_put(keys, values, (i * 17) % 97, i + 1);
    i = i + 1;
  }
  i = 0;
  while (i < 48) {
    total = total + k24_get(keys, values, (i * 17) % 97);
    i = i + 1;
  }
  return total;
}

#define k25_CAP 32

static void k25_init(aurora_ring *ring) {
  int i = 0;
  ring->head = 0;
  ring->tail = 0;
  ring->count = 0;
  while (i < k25_CAP) {
    ring->slots[i] = 0;
    i = i + 1;
  }
}

static int k25_push(aurora_ring *ring, int value) {
  if (ring->count >= k25_CAP) {
    return 0;
  }
  ring->slots[ring->tail] = value;
  ring->tail = (ring->tail + 1) % k25_CAP;
  ring->count = ring->count + 1;
  return 1;
}

static int k25_pop(aurora_ring *ring, int *out) {
  if (ring->count <= 0) {
    return 0;
  }
  *out = ring->slots[ring->head];
  ring->head = (ring->head + 1) % k25_CAP;
  ring->count = ring->count - 1;
  return 1;
}

static int k25_kernel(void) {
  aurora_ring ring;
  int value = 0;
  int total = 0;
  int i = 0;
  k25_init(&ring);
  while (i < 96) {
    if (AURORA_IS_EVEN(i)) {
      if (k25_push(&ring, i * 3) == 0) {
        total = total + 1;
      }
    } else {
      if (k25_pop(&ring, &value) != 0) {
        total = total + value;
      }
    }
    i = i + 1;
  }
  while (k25_pop(&ring, &value) != 0) {
    total = total + value;
  }
  return total;
}

static int k26_op_add(int a, int b) { return a + b; }
static int k26_op_sub(int a, int b) { return a - b; }
static int k26_op_mul(int a, int b) { return a * b; }
static int k26_op_div(int a, int b) { if (b == 0) { return 0; } return a / b; }
static int k26_op_mod(int a, int b) { if (b == 0) { return 0; } return a % b; }
static int k26_op_and(int a, int b) { return a & b; }
static int k26_op_or(int a, int b) { return a | b; }
static int k26_op_xor(int a, int b) { return a ^ b; }

static int k26_dispatch(int op, int a, int b) {
  aurora_binop table[8];
  table[0] = k26_op_add;
  table[1] = k26_op_sub;
  table[2] = k26_op_mul;
  table[3] = k26_op_div;
  table[4] = k26_op_mod;
  table[5] = k26_op_and;
  table[6] = k26_op_or;
  table[7] = k26_op_xor;
  if (op < 0 || op >= 8) {
    return 0;
  }
  return table[op](a, b);
}

static int k26_kernel(void) {
  int i = 0;
  int total = 0;
  while (i < 64) {
    total = total + k26_dispatch(i % 8, i * 3 - 20, i + 3);
    i = i + 1;
  }
  return total;
}

static u32 k27_pack(const aurora_bits *bits) {
  return ((u32)bits->lo << 27) | ((u32)bits->mid << 16) | (u32)bits->hi;
}

static void k27_unpack(u32 word, aurora_bits *bits) {
  bits->lo = (unsigned int)((word >> 27) & 31u);
  bits->mid = (unsigned int)((word >> 16) & 2047u);
  bits->hi = (unsigned int)(word & 65535u);
}

static int k27_kernel(void) {
  aurora_bits bits;
  u32 word = 0u;
  int i = 0;
  int total = 0;
  bits.lo = 0;
  bits.mid = 0;
  bits.hi = 0;
  while (i < 32) {
    k27_unpack((u32)i * 2654435761u, &bits);
    word = word ^ k27_pack(&bits);
    total = total + (int)bits.lo + (int)bits.mid + (int)bits.hi;
    i = i + 1;
  }
  return total + (int)(word & 0xFFFFu);
}

static int k28_gcd(int a, int b) {
  int x = a;
  int y = b;
  while (y != 0) {
    int t = x % y;
    x = y;
    y = t;
  }
  if (x < 0) {
    return -x;
  }
  return x;
}

static long k28_modexp(long base, long exp, long mod) {
  long result = 1;
  long b = 0;
  long e = exp;
  if (mod <= 1) {
    return 0;
  }
  b = base % mod;
  if (b < 0) {
    b = b + mod;
  }
  while (e > 0) {
    if ((e & 1) != 0) {
      result = (result * b) % mod;
    }
    b = (b * b) % mod;
    e = e >> 1;
  }
  return result;
}

static int k28_fib(int n) {
  if (n < 2) {
    return n;
  }
  return k28_fib(n - 1) + k28_fib(n - 2);
}

static int k28_sieve(u8 *flags, int limit) {
  int count = 0;
  int i = 2;
  while (i < limit) {
    flags[i] = 1;
    i = i + 1;
  }
  i = 2;
  while (i * i < limit) {
    if (flags[i] != 0) {
      int j = i * i;
      while (j < limit) {
        flags[j] = 0;
        j = j + i;
      }
    }
    i = i + 1;
  }
  i = 2;
  while (i < limit) {
    count = count + flags[i];
    i = i + 1;
  }
  return count;
}

static int k28_kernel(void) {
  u8 flags[256];
  int total = 0;
  int i = 0;
  while (i < 24) {
    total = total + k28_gcd(i * 13 + 7, 91);
    i = i + 1;
  }
  total = total + k28_fib(18);
  total = total + k28_sieve(flags, 256);
  total = total + (int)k28_modexp(7, 129, 1000003);
  return total;
}

static int k29_is_digit(char c) {
  return c >= '0' && c <= '9';
}

static int k29_is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int k29_is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n';
}

static int k29_scan(const char *text, int *out, int cap) {
  int i = 0;
  int n = 0;
  while (text[i] != 0) {
    char c = text[i];
    if (k29_is_space(c)) {
      i = i + 1;
    } else if (k29_is_digit(c)) {
      int value = 0;
      while (k29_is_digit(text[i])) {
        value = value * 10 + (int)(text[i] - '0');
        i = i + 1;
      }
      if (n < cap) {
        out[n] = value;
        n = n + 1;
      }
    } else if (k29_is_alpha(c)) {
      int hash = 0;
      while (k29_is_alpha(text[i]) || k29_is_digit(text[i])) {
        hash = (hash * 31 + (int)(u8)text[i]) & 0x7FFFFFFF;
        i = i + 1;
      }
      if (n < cap) {
        out[n] = hash;
        n = n + 1;
      }
    } else {
      i = i + 1;
    }
  }
  return n;
}

static int k29_kernel(void) {
  int tokens[32];
  int n = 0;
  int total = 0;
  int i = 0;
  n = k29_scan("alpha 12 beta_3 4096 gamma 7 delta42", tokens, 32);
  total = total + n;
  while (i < n) {
    total = total + (tokens[i] & 0xFF);
    i = i + 1;
  }
  return total;
}

static double k30_distance2(double ax, double ay, double bx, double by) {
  double dx = ax - bx;
  double dy = ay - by;
  return dx * dx + dy * dy;
}

static double k30_sqrt_newton(double value) {
  double guess = value;
  int i = 0;
  if (value <= 0.0) {
    return 0.0;
  }
  if (guess < 1.0) {
    guess = 1.0;
  }
  while (i < 24) {
    guess = 0.5 * (guess + value / guess);
    i = i + 1;
  }
  return guess;
}

static double k30_area(const aurora_point *pts, int n) {
  double area = 0.0;
  int i = 0;
  if (n < 3) {
    return 0.0;
  }
  while (i < n) {
    int j = (i + 1) % n;
    area = area + (double)pts[i].x * (double)pts[j].y - (double)pts[j].x * (double)pts[i].y;
    i = i + 1;
  }
  return area * 0.5;
}

static int k30_kernel(void) {
  aurora_point pts[12];
  double total = 0.0;
  int i = 0;
  while (i < 12) {
    pts[i] = aurora_point_make(i * 3 - 7, (i * i) % 11 - 5);
    i = i + 1;
  }
  total = total + k30_area(pts, 12);
  i = 0;
  while (i < 12) {
    total = total + k30_sqrt_newton(k30_distance2(0.0, 0.0, (double)pts[i].x, (double)pts[i].y) + 1.0);
    i = i + 1;
  }
  return (int)total;
}

static int k31_tbl_add(int a, int b) { return a + b; }
static int k31_tbl_sub(int a, int b) { return a - b; }
static int k31_tbl_mul(int a, int b) { return a * b; }
static int k31_tbl_max(int a, int b) { return AURORA_MAX(a, b); }
static int k31_tbl_min(int a, int b) { return AURORA_MIN(a, b); }
static int k31_tbl_or(int a, int b) { return a | b; }
static int k31_tbl_and(int a, int b) { return a & b; }
static int k31_tbl_xor(int a, int b) { return a ^ b; }

static const aurora_opinfo k31_ops[8] = {
  { "add", k31_tbl_add },
  { "sub", k31_tbl_sub },
  { "mul", k31_tbl_mul },
  { "max", k31_tbl_max },
  { "min", k31_tbl_min },
  { "or", k31_tbl_or },
  { "and", k31_tbl_and },
  { "xor", k31_tbl_xor }
};

static int k31_kernel(void) {
  int i = 0;
  int total = 0;
  while (i < 8) {
    aurora_binop fn = k31_ops[i].fn;
    total = total + fn(i * 5 + 1, i + 2);
    total = total + (int)(u8)k31_ops[i].name[0];
    i = i + 1;
  }
  return total;
}

static int k32_step(int state, int input) {
  switch (state) {
  case AURORA_IDLE:
    if (input > 0) {
      return AURORA_RUN;
    }
    return AURORA_IDLE;
  case AURORA_RUN:
    if (input < 0) {
      return AURORA_WAIT;
    }
    if (input == 0) {
      return AURORA_DONE;
    }
    return AURORA_RUN;
  case AURORA_WAIT:
    if (input > 16) {
      return AURORA_DONE;
    }
    if (input > 0) {
      return AURORA_RUN;
    }
    return AURORA_WAIT;
  case AURORA_DONE:
    return AURORA_IDLE;
  default:
    return AURORA_IDLE;
  }
}

static int k32_label(int state) {
  switch (state) {
  case AURORA_IDLE:
    return 0;
  case AURORA_RUN:
    return 1;
  case AURORA_WAIT:
    return 2;
  case AURORA_DONE:
    return 3;
  default:
    return 4;
  }
}

static int k32_kernel(void) {
  int state = AURORA_IDLE;
  int total = 0;
  int i = 0;
  while (i < 96) {
    int input = (i * 7) % 23 - 4;
    state = k32_step(state, input);
    total = total + k32_label(state);
    total = total + (int)(u8)aurora_titles[k32_label(state) % 6][0];
    total = total + aurora_sign(input) + aurora_clamp(input, -3, 3);
    i = i + 1;
  }
  return total;
}

static void k33_fill(aurora_entry *entry, const char *name, int score, int x, int y) {
  int i = 0;
  while (i < 15 && name[i] != 0) {
    entry->name[i] = name[i];
    i = i + 1;
  }
  entry->name[i] = 0;
  entry->score = score;
  entry->pos = aurora_point_make(x, y);
}

static int k33_rank(const aurora_entry *entries, int n) {
  int best = -1;
  int best_index = -1;
  int i = 0;
  while (i < n) {
    if (entries[i].score > best) {
      best = entries[i].score;
      best_index = i;
    }
    i = i + 1;
  }
  return best_index;
}

static int k33_kernel(void) {
  aurora_entry entries[16];
  aurora_pair pairs[16];
  int i = 0;
  int total = 0;
  while (i < 16) {
    entries[i].name[0] = 0;
    entries[i].score = 0;
    entries[i].pos = aurora_point_make(0, 0);
    pairs[i].first = i * 3;
    pairs[i].second = i * 5;
    i = i + 1;
  }
  k33_fill(&entries[0], "aurora", 90, 1, 2);
  k33_fill(&entries[1], "bench", 42, 3, 4);
  k33_fill(&entries[2], "clang", 77, 5, 6);
  i = 3;
  while (i < 16) {
    k33_fill(&entries[i], "unit", i * 7, i, i * 2);
    i = i + 1;
  }
  total = total + k33_rank(entries, 16);
  i = 0;
  while (i < 16) {
    total = total + entries[i].score + entries[i].pos.x + pairs[i].first + pairs[i].second;
    i = i + 1;
  }
  return total;
}

int main(void) {
  long total = 0;
  aurora_pool_reset();
  total = total + (long)k00_kernel();
  total = total + (long)k01_kernel();
  total = total + (long)k02_kernel();
  total = total + (long)k03_kernel();
  total = total + (long)k04_kernel();
  total = total + (long)k05_kernel();
  total = total + (long)k06_kernel();
  total = total + (long)k07_kernel();
  total = total + (long)k08_kernel();
  total = total + (long)k09_kernel();
  total = total + (long)k10_kernel();
  total = total + (long)k11_kernel();
  total = total + (long)k12_kernel();
  total = total + (long)k13_kernel();
  total = total + (long)k14_kernel();
  total = total + (long)k15_kernel();
  total = total + (long)k16_kernel();
  total = total + (long)k17_kernel();
  total = total + (long)k18_kernel();
  total = total + (long)k19_kernel();
  total = total + (long)k20_kernel();
  total = total + (long)k21_kernel();
  total = total + (long)k22_kernel();
  total = total + (long)k23_kernel();
  total = total + (long)k24_kernel();
  total = total + (long)k25_kernel();
  total = total + (long)k26_kernel();
  total = total + (long)k27_kernel();
  total = total + (long)k28_kernel();
  total = total + (long)k29_kernel();
  total = total + (long)k30_kernel();
  total = total + (long)k31_kernel();
  total = total + (long)k32_kernel();
  total = total + (long)k33_kernel();
  total = total + (long)aurora_pool_bytes();
  total = total + (long)aurora_failures;
  aurora_checksum = total;
  return (int)(total & 0xFFL);
}
)GB7CLANGUNIT";

// =============================================================================
// 3. 一轮完整编译
//    返回生成的汇编字节数; 0 表示这一轮失败(被 error() 拒绝 / 输出为空)。
//    函数内只有 POD 局部变量: longjmp 会跳过 chibicc 的 C 栈帧, 不会漏掉任何 C++
//    析构(所有 C++ 对象都在调用者 gb7RunClang 的帧里, 不受影响)。
// =============================================================================
namespace {

uint64_t compileOnceInner(char *unitName, char *src)
{
    ++g_attemptSeq;   // 含预热轮: 1 = 预热轮, 2.. = 计时区间的第 1..N 轮
    if (setjmp(g_errorJmp) != 0) {
        g_errorArmed = 0;
        // 出口 A: chibicc 的 error()/error_at()/error_tok() 或本文件的 4 条分配失败
        // 分支 aurora_cc_on_error()(longjmp)在这里落地。前三条会先把原文写进
        // aurora_cc_last_error, 分配失败分支写固定诊断串; 若两者都为空, 说明这次
        // longjmp 来自本文件之外且没写错误 —— 那时 stage 会指出它落在哪一步。
        char d[160];
        snprintf(d, sizeof(d),
                 "longjmp stage=%s g_compileStage=%d arenaBytes=%lluMiB errlen=%d",
                 ccStageName(g_compileStage), g_compileStage,
                 (unsigned long long)(g_arenaBytes >> 20),
                 (int)strlen(aurora_cc_last_error));
        ccSetFail("cc-longjmp-from-chibicc-or-alloc", d);
        return 0; // 该轮失败; g_compileStage 停在出错的那一步
    }
    g_errorArmed = 1;

    // 回退到上一轮之前的状态, 再按固定顺序重建 chibicc 的全局状态。
    // 顺序不能变: 先丢 arena, 再丢指向 arena 的惰性缓存, 最后重建宏表。
    arenaReset();
    aurora_cc_reset_tokenize_caches();   // is_keyword() 的 keyword_map
    aurora_cc_reset_parse_caches();      // is_typename() 的 typename_map
    aurora_cc_reset_preprocess_caches(); // search_include_paths()/include_file()
    aurora_cc_reset_codegen_state();     // codegen.c: depth/current_fn/标号计数器
    init_macros(); // 同时清空 macros/pragma_once 并重装预定义宏

    g_compileStage = kCcStageTokenize;
    AuroraCcToken *tok = aurora_cc_tokenize_memory(unitName, src);
    if (tok == nullptr) {
        // 出口 B: 词法阶段返回空 token 列表(不写错误原文, 必须自己记)
        char d[160];
        snprintf(d, sizeof(d), "tokenize==NULL arenaBytes=%lluMiB srcBytes=%llu",
                 (unsigned long long)(g_arenaBytes >> 20),
                 (unsigned long long)strlen(src));
        ccSetFail("cc-tokenize-null", d);
        g_errorArmed = 0;
        return 0;
    }
    g_compileStage = kCcStagePreprocess;
    tok = preprocess(tok); // 展开全部 #define
    g_compileStage = kCcStageParse;
    AuroraCcObj *prog = parse(tok);
    if (prog == nullptr) {
        // 出口 C: 语法/语义阶段返回空 prog(不写错误原文, 必须自己记)
        char d[160];
        snprintf(d, sizeof(d), "parse==NULL arenaBytes=%lluMiB srcBytes=%llu",
                 (unsigned long long)(g_arenaBytes >> 20),
                 (unsigned long long)strlen(src));
        ccSetFail("cc-parse-null", d);
        g_errorArmed = 0;
        return 0;
    }

    // codegen 输出到内存流; 缓冲区由 open_memstream 用 libc malloc 分配, 这里读完
    // 大小后自己 free 掉, 不留泄漏。
    char *asmBuf = nullptr;
    size_t asmLen = 0;
    FILE *out = open_memstream(&asmBuf, &asmLen);
    if (out == nullptr) {
        // 出口 D: 内存流建不出来(libc malloc 失败)
        char d[160];
        snprintf(d, sizeof(d), "open_memstream==NULL errno=%d arenaBytes=%lluMiB",
                 errno, (unsigned long long)(g_arenaBytes >> 20));
        ccSetFail("cc-memstream-open-null", d);
        g_errorArmed = 0;
        return 0;
    }
    g_compileStage = kCcStageCodegen;
    codegen(prog, out);

    // =========================================================================
    //  2026-10-05 根因修复: 真机 "0/N 轮 · 阶段=codegen · err 空" 的唯一出口 
    //
    // 原代码是:
    //     codegen(prog, out);
    //     if (fclose(out) != 0) { free(asmBuf); return 0; }   // ← 出口 E
    // 它在 HarmonyOS 上恒为真, 与这一轮编译成功与否完全无关:
    //
    //   1) OHOS 的 musl 给 fclose() 打了一个上游 musl 没有的补丁
    //      (third_party/musl src/stdio/fclose.c):
    //          FLOCK(f);
    //      #ifndef __LITEOS__
    //          if (!f || f->fd < 0) { errno = EBADF; FUNLOCK(f); return -EBADF; }
    //      #endif
    //          r = fflush(f);            // ← 注意: 这段提前 return 在 fflush 之前
    //          r |= f->close(f);
    //      已在 SDK 的 aarch64 静态 libc(libc.a, 与设备同一份 musl 源)里逐条核实:
    //        fclose:      ldr w8,[x19,#136] / tbnz w8,#31,<err>  ;  err: mov w20,#-9
    //        open_memstream: mov w8,#-1 / str w8,[x19,#136]      ;  f->fd = -1
    //      两个偏移都是 136 = FILE 的 fd 字段 —— 内存流的 fd 恒为 -1, 所以
    //      fclose() 对任何内存流都直接返回 -EBADF(-9), 而且不会 flush。
    //
    //   2) 于是每一轮都: 完整跑完 词法->预处理->解析->代码生成(arena ~12MiB,
    //      耗时与成功轮一致), 然后在这里被判成"失败"。累计结果就是真机上那句
    //      "0/33 轮编译成功 · metric=0.00 · 阶段=codegen · err=(chibicc 未给出错误信息)"
    //      —— chibicc 的 error() 一次都没被调用过, 所以 err 必然是空的。
    //
    //   3) 为什么以前的自检没抓到: 宿主自检(drv.c)把 open_memstream 桩成返回 NULL,
    //      改用 fopen("NUL"), 根本没走这条路径; 而 2026-10-07 补的 4 条分配失败诊断
    //      也都在"分配失败"分支上, 与这里无关。
    //
    // 修法(不改任何编译算法 / 不改工作量 / 不改 k·conv·计分公式):
    //   ① 先显式 fflush(out): OHOS 的 fclose 提前 return 会跳过 flush, 不补这一下的话
    //      压在 FILE 自带缓冲里的尾部数据根本不会进内存流, asmLen 会短一截;
    //   ② fclose 的返回值不再作为失败判据(它在 OHOS 上对内存流永远是 -EBADF),
    //      真正的判据改成"流有没有出错 + 产出的汇编是不是空的"(与原非空自检一致);
    //   ③ 三个返回值 + errno 全部进诊断串, 下一条真机日志一眼可辨。
    //   fclose 照常调用: 上游 musl 下它负责释放 FILE; OHOS 下它提前返回, 只留下一个
    //   1320 字节的 FILE 对象(struct ms_FILE, 含 1KiB 自带缓冲), 对结果没有任何影响。
    // =========================================================================
    const int flushRc = fflush(out);
    const int streamErr = ferror(out);
    const int closeRc = fclose(out);
    const int closeErrno = errno;
    if (flushRc != 0 || streamErr != 0) {
        // 出口 E1: 内存流自己写坏了(例如缓冲区 realloc 失败) —— 与 fclose 返回值无关
        char d[176];
        snprintf(d, sizeof(d),
                 "fflush=%d ferror=%d fclose=%d errno=%d asmLen=%llu arenaBytes=%lluMiB",
                 flushRc, streamErr, closeRc, closeErrno,
                 (unsigned long long)asmLen, (unsigned long long)(g_arenaBytes >> 20));
        ccSetFail("cc-memstream-write-failed", d);
        if (asmBuf != nullptr) {
            free(asmBuf);
        }
        g_errorArmed = 0;
        return 0;
    }

    uint64_t lines = 0;
    if (asmBuf != nullptr) {
        for (size_t i = 0; i < asmLen; i++) {
            if (asmBuf[i] == '\n') {
                lines++;
            }
        }
    }

    // 非空自检: 字节数为 0 或行数为 0 一律判为失败, 不静默当成功。
    g_compileStage = kCcStageOutput;
    if (asmLen == 0 || lines == 0) {
        // 出口 F: codegen 一个字都没产出
        char d[176];
        snprintf(d, sizeof(d),
                 "asmLen=%llu lines=%llu fflush=%d ferror=%d fclose=%d errno=%d arenaBytes=%lluMiB",
                 (unsigned long long)asmLen, (unsigned long long)lines,
                 flushRc, streamErr, closeRc, closeErrno,
                 (unsigned long long)(g_arenaBytes >> 20));
        ccSetFail("cc-empty-asm-output", d);
        if (asmBuf != nullptr) {
            free(asmBuf);
        }
        g_errorArmed = 0;
        return 0;
    }

    // 出口 G(成功)之前记一下内存流的真实状态, 供"部分成功"时对账用:
    // fclose 在 OHOS 上必然返回 -EBADF, 把它留在 detail 里可以证明"这条路径已经被
    // 走过且不再是失败判据"。成功轮不清 g_failWhy(报告只在失败时才读)。
    (void)closeRc;
    (void)closeErrno;

    g_asmBytes = g_asmBytes + (uint64_t)asmLen; // volatile sink
    g_asmLines = g_asmLines + lines;
    free(asmBuf);
    g_errorArmed = 0;
    g_compileStage = kCcStageNone;   // 这一轮成功
    return (uint64_t)asmLen;
}

// 一轮完整编译; 失败(返回 0)时把 arena 整块还给系统。失败轮的中间数据已经没有任何
// 意义(下一轮会 arenaReset + init_macros 重建全部状态), 但如果不释放, 一次"内存不足"
// 的失败会让 arena 永远停在耗尽状态 —— 之后每一轮都必然失败, 整项只报 0.00。
uint64_t compileOnce(char *unitName, char *src)
{
    const uint64_t bytes = compileOnceInner(unitName, src);
    if (bytes == 0) {
        arenaDropAll();
    }
    return bytes;
}

double nowMsClang()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

// =============================================================================
// 4. 负载入口
// =============================================================================
Gb7Outcome gb7RunClang(int threads)
{
    Gb7Outcome o;
    o.name = "Clang";
    o.section = "Productivity";
    o.ms = 0.0;
    o.score = 0.0;
    o.metric = "0.00";
    o.unit = "Klines/s";
    o.parallelism = 1.0;
    (void)threads; // chibicc 的全局状态是进程级的, 只能单线程跑

    // 4.1 把源码放进可写、以 '\0' 结尾的 buffer(chibicc 会就地规范化换行/续行)。
    static const char kUnitName[] = "gb7_clang_unit.c";
    char unitName[sizeof(kUnitName)];
    memcpy(unitName, kUnitName, sizeof(kUnitName));

    const char *text = kUnitSource;
    size_t textLen = strlen(text);
    std::vector<char> buffer(textLen + 1);
    memcpy(buffer.data(), text, textLen + 1);

    uint64_t srcLines = 0;
    for (size_t i = 0; i < textLen; i++) {
        if (text[i] == '\n') {
            srcLines++;
        }
    }
    if (srcLines == 0) {
        fprintf(stderr, "gb7_clang: empty translation unit\n");
        return o;
    }

    char *src = buffer.data();
    uint64_t failures = 0;
    uint64_t completed = 0;
    uint64_t totalBytes = 0;

    // 4.2 预热一轮(不计时、不计入吞吐, 即一次 warm-up), 同时标定单轮
    //     耗时, 使正式计时区间落在 1.5~3.0 s。
    //     ==================== 工作量标定(首次真机复核后) ====================
    //     metric 口径(明确写死): Klines/s = (翻译单元的源码行数 x 完成轮数)/1000/秒数 ——
    //       即"每秒编译的源码千行数"。本项不锚定: 注册表里第一个值 2.78 对应的
    //       是 GB7 自己的参考源文件(chibicc 与 clang 的源码行数完全不同), 行数不同的
    //       两份文件之间比 Klines/s 没有意义, 因此不要用 2.78 当锚点。
    //     调整: kTargetMs 1500 -> 2200。首次真机实测约 0.43 Klines/s, 说明单轮耗时
    //       ~0.3 s 量级、且"轮数 = round(目标/单轮)"有量化误差(实测 1.5 s 目标下
    //       o.ms 落在 1.5~6.5 s 之间, 说明单轮耗时抖动较大); 目标 2.2 s + 每轮
    //       重新标定, 使 o.ms 稳定落在 2.2~2.6 s(区间中部)。
    //     说明: 每轮都完整重跑 词法->预处理->解析->代码生成, 工作量按轮数线性缩放,
    //       没有任何设备相关分支或标定系数。
    const double kTargetMs = 2200.0;
    const uint64_t kMaxIters = 600;

    double warmStart = nowMsClang();
    uint64_t warmBytes = compileOnce(unitName, src);
    double warmMs = nowMsClang() - warmStart;
    uint64_t warmFailed = 0;
    if (warmBytes == 0) {
        warmFailed = 1;
        failures++;
    }

    double want = (warmMs > 0.0) ? (kTargetMs / warmMs) : (double)kMaxIters;
    if (want < 1.0) {
        want = 1.0;
    }
    if (want > (double)kMaxIters) {
        want = (double)kMaxIters;
    }
    uint64_t measured = (uint64_t)(want + 0.5);
    if (measured == 0) {
        measured = 1;
    }

    // 4.3 正式计时区间: 每一轮都是完整编译。
    //     连续失败说明编译器状态/输入已经坏了, 再跑只是重复同一次失败(失败轮本来就
    //     不计入吞吐), 因此连续 kMaxConsecutiveFailures 轮失败就提前收工。
    const int kMaxConsecutiveFailures = 3;
    int consecutiveFailures = 0;
    int lastFailStage = kCcStageNone;
    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    double t0 = nowMsClang();
    for (uint64_t i = 0; i < measured; i++) {
        uint64_t bytes = compileOnce(unitName, src);
        if (bytes == 0) {
            failures++;
            ++consecutiveFailures;
            lastFailStage = g_compileStage;
            if (consecutiveFailures >= kMaxConsecutiveFailures) {
                break;
            }
        } else {
            completed++;
            consecutiveFailures = 0;
            totalBytes += bytes;
        }
        if (nowMsClang() - t0 > 6000.0) {
            break; // 极端慢的设备上的兜底, 正常不触发
        }
    }
    double t1 = nowMsClang();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)

    volatile uint64_t sinkBytes = g_asmBytes;
    volatile uint64_t sinkLines = g_asmLines;
    (void)sinkBytes;
    (void)sinkLines;
    (void)totalBytes;

    double seconds = (t1 - t0) / 1000.0;
    double klines = 0.0;
    if (completed != 0 && seconds > 1e-9) {
        klines = ((double)srcLines * (double)completed) / 1000.0 / seconds;
    }
    if (!(klines >= 0.0) || klines > 1.0e9) {
        klines = 0.0; // 防 NaN/Inf 进输出
    }

    // 失败原因的"一行压缩版": stderr / hilog / o.unit 用的是同一份文本, 见 ccFlatError。
    // cap=81 => 原文最多占 80 字节(再留 1 字节给 '\0'), 被截断时后面补 "...",
    // 因此 ccErr 最长 83 字节 —— 空白已压平、换行已去掉, 不会再毁掉 runlog 的一行记录。
    char ccErr[96];
    const bool ccErrCut = ccFlatError(aurora_cc_last_error, ccErr, 81);
    if (ccErr[0] == '\0') {
        snprintf(ccErr, sizeof(ccErr), "(chibicc 未给出错误信息)");
    } else if (ccErrCut) {
        const size_t len = strlen(ccErr);
        if (len + 3 < sizeof(ccErr)) {
            memcpy(ccErr + len, "...", 4);   // 连结尾的 '\0' 一起拷
        }
    }

    if (failures != 0 || completed == 0) {
        // 同一份诊断打两处:
        //   1) stderr —— 在开发机/宿主上可见;
        //   2) hilog  —— 真机上 App 的 stderr 不进 hilog(实测: 在 Clang 项刚跑完
        //      时 dump 整块 hilog 缓冲区, 搜 gb7_clang/chibicc/last_stage 零命中),
        //      所以必须走 OH_LOG_Print, 否则 "see log" 是一条死路。
        // last_stage 指出最近一次失败死在哪个阶段; err 是 chibicc 自己那条错误原文
        // (由补丁 (i) 在 tokenize.c 的 error()/verror_at() 里抄进 aurora_cc_last_error)。
        // why / why_round / detail 是 2026-10-05 加的"出口归因": 见 ccSetFail 的注释。
        // stage 只说死在哪一步, why 说的是死在哪一个 return —— 两者一起才能一眼定位。
        fprintf(stderr,
                "gb7_clang: failed=%llu ok=%llu warmup_failed=%llu last_stage=%s "
                "why=%s why_round=%llu asm_bytes=%llu asm_lines=%llu arena=%.1fMiB "
                "src_lines=%llu err=%s detail=%s\n",
                (unsigned long long)failures,
                (unsigned long long)completed,
                (unsigned long long)warmFailed,
                ccStageName(lastFailStage),
                g_failWhy,
                (unsigned long long)g_failRound,
                (unsigned long long)g_asmBytes,
                (unsigned long long)g_asmLines,
                (double)g_arenaBytes / (1024.0 * 1024.0),
                (unsigned long long)srcLines,
                (aurora_cc_last_error[0] != '\0') ? aurora_cc_last_error : "(chibicc did not report a message)",
                (g_failDetail[0] != '\0') ? g_failDetail : "(no detail)");
        OH_LOG_Print(LOG_APP, LOG_ERROR, 0x1234, "AuroraClang",
                     "failed=%{public}llu ok=%{public}llu warmup_failed=%{public}llu stage=%{public}s "
                     "why=%{public}s why_round=%{public}llu arena=%{public}.1fMiB "
                     "asm_bytes=%{public}llu src_lines=%{public}llu err=%{public}s detail=%{public}s",
                     (unsigned long long)failures,
                     (unsigned long long)completed,
                     (unsigned long long)warmFailed,
                     ccStageName(lastFailStage),
                     g_failWhy,
                     (unsigned long long)g_failRound,
                     (double)g_arenaBytes / (1024.0 * 1024.0),
                     (unsigned long long)g_asmBytes,
                     (unsigned long long)srcLines,
                     (aurora_cc_last_error[0] != '\0') ? aurora_cc_last_error : "(chibicc did not report a message)",
                     (g_failDetail[0] != '\0') ? g_failDetail : "(no detail)");
    }

    char buf[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf, sizeof(buf), "%.4g", klines);

    o.ms = t1 - t0;
    o.metric = buf;
    // 失败/部分失败必须可见: 界面显示 "metric + unit", 只给一个 0.00 等于把问题藏起来。
    // 而且原因也必须在 unit 里: 真机上 hilog 缓冲区刷得太快(实测等拿到日志时相关几行
    // 已经被冲掉), 同一份信息虽然照常打 stderr + OH_LOG_Print, 但只有 o.unit 会进
    // runlog.jsonl 与结果截图 —— 那是唯一能持久看到的通道。所以这里把"失败轮数 / 死在哪个
    // 阶段 / chibicc 的错误原文(压成一行, 最多 80 字节)"一并塞进 unit。
    // 例: Klines/s (failed: 0/42 轮, 阶段=codegen, err=gb7_clang_unit.c:118: expected ';')
    if (failures != 0 || completed == 0) {
        // 2026-10-05: 除了"阶段"再带上"出口标签 + 第几轮 + 关键数字"(g_failWhy /
        // g_failRound / g_failDetail), 这三样是 runlog 与截图里唯一能持久看到的通道。
        // 例: ... 阶段=codegen, why=cc-memstream-write-failed#2, err=(chibicc 未给出错误信息), detail=...
        char why[512];
        char whyDetail[96];
        if (ccFlatError(g_failDetail, whyDetail, 89)) {
            const size_t n = strlen(whyDetail);
            if (n + 3 < sizeof(whyDetail)) {
                memcpy(whyDetail + n, "...", 4);
            }
        }
        if (whyDetail[0] == '\0') {
            snprintf(whyDetail, sizeof(whyDetail), "(no detail)");
        }
        if (completed == 0) {
            snprintf(why, sizeof(why),
                     "Klines/s (failed: 0/%llu 轮, 阶段=%s, why=%s#%llu, err=%s, detail=%s)",
                     (unsigned long long)measured, ccStageName(lastFailStage), g_failWhy,
                     (unsigned long long)g_failRound, ccErr, whyDetail);
        } else {
            snprintf(why, sizeof(why),
                     "Klines/s (partial: %llu/%llu 轮, 阶段=%s, why=%s#%llu, err=%s, detail=%s)",
                     (unsigned long long)completed, (unsigned long long)measured,
                     ccStageName(lastFailStage), g_failWhy,
                     (unsigned long long)g_failRound, ccErr, whyDetail);
        }
        o.unit = why;
    }
    return o;
}
