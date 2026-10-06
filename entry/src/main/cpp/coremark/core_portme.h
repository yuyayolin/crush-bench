/* AuroraBench 的 CoreMark 移植层 (EEMBC CoreMark 官方负载, 逐字节同源) */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* 基本类型 */
typedef int8_t   ee_s8;
typedef int16_t  ee_s16;
typedef int32_t  ee_s32;
typedef uint8_t  ee_u8;
typedef uint16_t ee_u16;
typedef uint32_t ee_u32;
typedef intptr_t ee_ptr_int;
typedef size_t   ee_size_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define CORE_TICKS uint64_t

#define COMPILER_VERSION "Clang"
#define COMPILER_FLAGS   "-O2"

#define HAS_FLOAT   1
#define HAS_TIME_H  1
#define HAS_STDIO   0
#define HAS_PRINTF  0
#define USE_CLOCK   0
#define MAIN_HAS_NOARGC 1
#define MAIN_HAS_NORETURN 0
#define SEED_METHOD SEED_VOLATILE
#define MEM_METHOD  MEM_STATIC
#define MEM_LOCATION "STATIC"
#define MULTITHREAD 1
#define ITERATIONS 0

/* 目标测量时长(秒)。
   官方 CoreMark 的通行规则是 >=10 秒, 本 App 的 UI 明确要求 1.5~3 秒,
   所以这里把目标时长设为 2 秒。仅仅是"跑多久"的目标值 —— 迭代内容、算法、
   校验规则、种子全部保持官方原样(CRC 与迭代次数无关)。 */
#ifndef min_time
#define min_time 2
#endif

/* 标定出来的速率带有约 ±20% 的估计噪声(CPU 调频/预热), 目标时长上浮 10%,
   让实测时长稳定落在 [COREMARK_VALID_MIN_SECS, 3.0] 秒内。 */
#ifndef COREMARK_SCALE_MARGIN
#define COREMARK_SCALE_MARGIN 1.10
#endif

/* 有效结果的最短时长(秒) = 本 App 允许的时长区间下界。
   core_main.c 里官方那句 "Must execute for at least 10 secs" 的判定阈值改用它,
   否则按 min_time 判定时, 只要标定速率估计稍微偏乐观就会误判成 error 而不输出计分行。 */
#ifndef COREMARK_VALID_MIN_SECS
#define COREMARK_VALID_MIN_SECS 1.5
#endif

/* 自动标定阶段先跑多长(秒)来测出本机真实速率。
   官方原版是"跑到 >=1 秒, 再按 1+10/divisor 粗粒度放大到约 10 秒";
   这里改成"跑到 >=0.3 秒测出速率, 再线性缩放到 min_time"。
   0.3 秒足够让 CPU 调频/缓存进入稳态(估计更准), 而 core_main.c 里的标定步进已改成
   按实测速率比例跳变(x10 步进会过冲到阈值的 10 倍), 所以探测段本身只花约 0.3 秒,
   探测段 + 正式段合计稳定落在 2 秒出头。 */
#ifndef COREMARK_CALIBRATE_SECS
#define COREMARK_CALIBRATE_SECS 0.3
#endif

typedef struct CORE_PORTABLE_S
{
    ee_u8 portable_id;
} core_portable;

extern ee_u32 default_num_contexts;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

/* Function: align_mem
        把内存块指针对齐到 8 字节, core_matrix.c 依赖它。
   【必须在头文件里声明】这是真机 SIGSEGV 的直接原因: 原移植层只在 core_portme.c
   里定义了 align_mem(), 却没有任何可见声明, 于是 core_matrix.c 按 C99 隐式声明
   "int align_mem()" 调用它, 64 位返回值被截断成 32 位 int 再转回指针 →
   core_init_matrix() 里 A/B/C 全是野指针 → 往未映射地址写 → SEGV_MAPERR(si_code=1)。 */
void *align_mem(void *p);

int ee_printf(const char *fmt, ...);

/* 供 App 侧抓取结果(见 core_portme.c / coremark_runner.c) */
extern double g_coremark_score;
extern int    g_coremark_errors;
extern int    g_coremark_crc_ok;

/* CoreMark 官方输出的原文日志(按调用顺序拼接, 以 NUL 结尾) */
#define COREMARK_LOG_SIZE 4096
extern char   g_coremark_log[COREMARK_LOG_SIZE];
extern size_t g_coremark_log_len;

#endif
