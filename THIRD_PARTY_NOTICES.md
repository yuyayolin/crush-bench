# 第三方组件与许可声明

本仓库包含下列第三方代码。它们**不是**本项目自有代码，各自按自己的许可发布。
分发本软件的二进制（`.hap`）或源码时，请连同本文件一起保留。

| 组件 | 位置 | 许可 | 版权 |
|---|---|---|---|
| chibicc | `entry/src/main/cpp/third_party/chibicc/` | MIT | Copyright (c) 2019 Rui Ueyama |
| Zstandard (zstd) | `entry/src/main/cpp/third_party/zstd/` | BSD-3-Clause | Copyright (c) Meta Platforms, Inc. and affiliates |
| LZ4 | `entry/src/main/cpp/third_party/lz4.{c,h}` | BSD-2-Clause | Copyright (c) Yann Collet |
| miniz | `entry/src/main/cpp/third_party/miniz.{c,h}` | Public domain / MIT（见文件头） | RAD Game Tools / Valve Software / Rich Geldreich |
| stb_image / stb_image_write | `entry/src/main/cpp/third_party/stb_image*.h` | Public domain / MIT（见文件头） | Sean Barrett (nothings.org) |
| CoreMark | `entry/src/main/cpp/coremark/` | Apache-2.0（见该目录 LICENSE.md） | Copyright (c) 2018 EEMBC |
| libc++ | 由 HarmonyOS SDK 提供，打包为 `libs/arm64-v8a/libc++_shared.so` | Apache-2.0 WITH LLVM-exception | LLVM Project |

## 需要单独注意的两条

### 1. CoreMark（EEMBC）

CoreMark 的许可里除了 Apache-2.0 还有一条**成绩发布规则**：EEMBC 对 "CoreMark" 这个名字的用法、
以及公开成绩时必须同时给出的信息（编译器、编译选项、运行规则等）有明确要求。

本 App 运行 CoreMark 只是**作为一项稳定性 / 功能自检**，它的结果**不参与任何计分**，
界面上也不以 "CoreMark score" 的形式对外发布。如果你要单独把本 App 跑出的 CoreMark 数字公开，
请先去 EEMBC 官网核对那份发布规则。

### 2. 与本项目无关的第三方跑分软件

本项目的测试套件、单位、刻度**全部由本工程自定**。详见 README 里的"与第三方跑分的关系"一节。
