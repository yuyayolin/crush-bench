/* AuroraBench: 运行官方 CoreMark 负载并取回标准分数(每秒迭代数) */
#include "core_portme.h"

extern int coremark_main_entry(void);

double coremark_run_official(void)
{
    g_coremark_score   = 0.0;
    g_coremark_errors  = -1;
    g_coremark_crc_ok  = 1;
    g_coremark_log_len = 0;
    g_coremark_log[0]  = '\0';
    coremark_main_entry();
    return g_coremark_score;
}

int coremark_crc_ok(void)
{
    return g_coremark_crc_ok;
}

int coremark_error_count(void)
{
    return g_coremark_errors;
}
