// AuroraBench 可移植参考实现
// 与鸿蒙版使用完全相同的负载源码(../entry/src/main/cpp/bench_cpu.cpp),
// 可在 Android NDK / iOS / Windows / Linux / macOS 上编译, 用于跨平台分数对照。
#include "bench.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    int threads = (int)std::thread::hardware_concurrency();
    if (threads <= 0) {
        threads = 4;
    }
    const int n = auroraTestCount();
    printf("AuroraBench portable reference\n");
    printf("cores detected: %d\n", threads);
    printf("\n== single-core ==\n");
    double singleProduct = 0.0;
    for (int i = 0; i < n; ++i) {
        BenchOutcome r = auroraRunTest(i, 1);
        printf("%-14s %10.2f ms  %10.2f %-8s  score %8.1f\n", r.name.c_str(), r.ms, r.metric, r.unit.c_str(), r.score);
        singleProduct += (r.score > 0.0) ? std::log(r.score) : 0.0;
    }
    printf("single-core composite: %.1f\n", std::exp(singleProduct / (double)n));
    printf("\n== multi-core (%d threads) ==\n", threads);
    double multiProduct = 0.0;
    for (int i = 0; i < n; ++i) {
        BenchOutcome r = auroraRunTest(i, threads);
        printf("%-14s %10.2f ms  %10.2f %-8s  score %8.1f\n", r.name.c_str(), r.ms, r.metric, r.unit.c_str(), r.score);
        multiProduct += (r.score > 0.0) ? std::log(r.score) : 0.0;
    }
    printf("multi-core composite: %.1f\n", std::exp(multiProduct / (double)n));
    printf("\nnote: GPU scenes require a platform graphics context; see gpu/gpu_renderer.cpp\n");
    (void)argc;
    (void)argv;
    return 0;
}
