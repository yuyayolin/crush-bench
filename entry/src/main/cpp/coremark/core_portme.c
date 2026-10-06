/* AuroraBench 的 CoreMark 移植层实现 */
#include "core_portme.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CoreMark 官方标准配置: 种子 0x0 / 0x0 / 0x66, 迭代数 0(自动标定到达标时长) */
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
volatile ee_s32 seed4_volatile = 0;
volatile ee_s32 seed5_volatile = 0;

/* CoreMark 需要的外部变量 */
ee_u32 default_num_contexts = 1;

/* Function: align_mem
        内存对齐辅助(官方移植层要求)。声明在 core_portme.h —— 没有声明的话
        core_matrix.c 会按隐式 int 调用, 在 64 位平台上把指针截断成 32 位。 */
void *align_mem(void *p)
{
    ee_ptr_int aligned = (ee_ptr_int)p;
    aligned = (aligned + 7) & ~(ee_ptr_int)7;
    return (void *)aligned;
}

/* ------------------------------------------------------------------
   结果抓取(UI 依赖这三个全局量)
   ------------------------------------------------------------------ */
double g_coremark_score = 0.0;
int    g_coremark_errors = -1; /* -1 = 未知, >=0 = 官方报出的错误条数 */
int    g_coremark_crc_ok = 1;

char   g_coremark_log[COREMARK_LOG_SIZE];
size_t g_coremark_log_len = 0;

static void log_append(const char *s, size_t n)
{
    if (n > (size_t)(COREMARK_LOG_SIZE - 1) - g_coremark_log_len)
        n = (size_t)(COREMARK_LOG_SIZE - 1) - g_coremark_log_len;
    if (n > 0)
    {
        memcpy(g_coremark_log + g_coremark_log_len, s, n);
        g_coremark_log_len += n;
    }
    g_coremark_log[g_coremark_log_len] = '\0';
}

/* 逐条解析官方输出的文本(不再靠 va_arg 猜类型) */
static void capture_line(const char *line)
{
    if (strncmp(line, "CoreMark 1.0 : ", 15) == 0)
    {
        /* 官方计分行: "CoreMark 1.0 : <Iterations/Sec> / <version> <flags> / <mem>" */
        g_coremark_score = strtod(line + 15, NULL);
    }
    else if (strstr(line, "Correct operation validated") != NULL)
    {
        /* 全部已知 CRC 命中 + 数据类型检查通过 */
        g_coremark_crc_ok = 1;
        g_coremark_errors = 0;
    }
    else if (strstr(line, "Errors detected") != NULL
             || strstr(line, "Cannot validate operation") != NULL)
    {
        g_coremark_crc_ok = 0;
    }
    else if (strstr(line, "ERROR!") != NULL)
    {
        /* 每条 "[n]ERROR! xxx crc 0x.... - should be 0x...." 都是一个被官方
           校验规则判定的错误, 累加起来就是官方 total_errors 的可观测部分。 */
        if (g_coremark_errors < 0)
            g_coremark_errors = 0;
        g_coremark_errors++;
    }
}

int ee_printf(const char *fmt, ...)
{
    char    buf[512];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return n;
    buf[sizeof(buf) - 1] = '\0';
    capture_line(buf);
    log_append(buf, strlen(buf));
    return n;
}

/* ------------------------------------------------------------------
   时序: 官方契约是"get_time() 返回自上次 start_time() 以来经过的 ticks"。
   原来的实现让 start/stop 变成空函数、get_time() 返回 CLOCK_MONOTONIC 的
   绝对时间, 于是标定循环拿到的是"开机以来的秒数", 计分处又拿绝对时间当分母。
   ------------------------------------------------------------------ */
static CORE_TICKS start_time_val = 0;
static CORE_TICKS stop_time_val  = 0;

/* 单调时钟原始读数, 单位纳秒(1 tick = 1ns)。只在本文件内部使用 */
static CORE_TICKS read_monotonic_ticks(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (CORE_TICKS)ts.tv_sec * 1000000000ull + (CORE_TICKS)ts.tv_nsec;
}

void start_time(void)
{
    start_time_val = read_monotonic_ticks();
}

void stop_time(void)
{
    stop_time_val = read_monotonic_ticks();
}

CORE_TICKS get_time(void)
{
    return (CORE_TICKS)(stop_time_val - start_time_val);
}

double time_in_secs(CORE_TICKS ticks)
{
    return (double)ticks / 1000000000.0;
}

void portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)argc;
    (void)argv;
    if (p != NULL)
    {
        p->portable_id = 1;
    }
}

void portable_fini(core_portable *p)
{
    if (p != NULL)
    {
        p->portable_id = 0;
    }
}
