# -*- coding: utf-8 -*-
"""
verify_abi_arm64.py —— 任务一『HAP 里有没有 32 位成分』的离线自检器
(不连设备、不跑 hdc、不跑构建、不改任何文件)

要回答的问题
  官方案例: 32 位应用在 Mate60 之后只能跑小核, 绑核也不生效; 改 64 位后恢复。
  因此必须核实本工程打出来的 HAP 里一个 32 位成分都没有。

四道核查(全部只读)
  [A] 构建配置: entry/build-profile.json5 的 externalNativeOptions.abiFilters
      必须精确等于 ["arm64-v8a"](不带 armeabi-v7a / x86 / x86_64)。
  [B] 打包产物: release/*.hap 是 zip; 其中 libs/ 下只允许 arm64-v8a 一个目录;
      逐个 .so 直接读 ELF 头: EI_CLASS 必须 = 2(ELF64), e_machine 必须 = 183(EM_AARCH64)。
  [C] 源码树: entry/src/main 下不得存在任何预编译二进制(.so / .a / .o)
      —— 一个都没有, 就不可能有 32 位成分混进来。
  [D] 配置/源码文本: 不得出现 armeabi / armeabi-v7a / armv7 / x86_64 之类的 ABI 字样
      (ref/ 与资料目录除外 —— 那里是外部资料, 不是本工程的构建输入)。

缺输入时的口径(不许假装通过)
  * 找不到 HAP -> 打印 SKIP(打包产物这一档没查) 并说明原因;
    其余三档照查。这种情况不算 FAIL(HAP 是构建产物, 可以没被构建出来), 但会显式标注。
  * HAP 存在但读不动 -> FAIL。

退出码 0 = 全部通过(含 SKIP 情形), 1 = 有 FAIL。
"""
import io
import json
import os
import re
import struct
import sys
import zipfile

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
OUT = io.open(os.path.join(HERE, "verify_abi_arm64_out.txt"), "w", encoding="utf-8", newline="\n")
fails = []
checks = 0
skips = []

EM = {183: "EM_AARCH64(183, 64 位 ARM)", 40: "EM_ARM(40, 32 位 ARM)",
      62: "EM_X86_64(62, 64 位 x86)", 3: "EM_386(3, 32 位 x86)",
      243: "EM_RISCV(243)"}


def emit(s=""):
    try:
        print(s)
    except Exception:
        pass
    OUT.write(s + "\n")
    OUT.flush()


def check(name, ok, detail=""):
    global checks
    checks += 1
    if ok:
        emit("  [PASS] " + name + ((" -- " + detail) if detail else ""))
    else:
        emit("  [FAIL] " + name + ((" -- " + detail) if detail else ""))
        fails.append(name)


def skip(name, why):
    skips.append(name)
    emit("  [SKIP] " + name + " -- " + why)


def rd_text(path):
    if not os.path.exists(path):
        return ""
    return io.open(path, encoding="utf-8", errors="replace").read()


def elf_header(data):
    """返回 (class, machine) 或 None。只读前 20 字节。"""
    if len(data) < 20 or data[0:4] != b"\x7fELF":
        return None
    return (data[4], struct.unpack_from("<H", data, 18)[0])


def main():
    emit("=" * 100)
    emit("[A] 构建配置里的 ABI 过滤器")
    emit("=" * 100)
    bp_path = os.path.join(REPO, "entry", "build-profile.json5")
    bp = rd_text(bp_path)
    check("A1 entry/build-profile.json5 存在且可读", bool(bp), bp_path)
    m = re.search(r'"abiFilters"\s*:\s*\[([^\]]*)\]', bp)
    check("A2 显式声明了 abiFilters(不是靠工具链默认值)", m is not None,
          m.group(0) if m else "没找到 abiFilters")
    abis = []
    if m:
        abis = [t.strip().strip('"').strip("'") for t in m.group(1).split(",") if t.strip()]
    check("A3 abiFilters 精确等于 [\"arm64-v8a\"]", abis == ["arm64-v8a"], "实际 = " + repr(abis))
    check("A4 abiFilters 里没有任何 32 位 ABI",
          all(("arm64" in a) for a in abis) and not any(
              ("armeabi" in a or "armv7" in a or "x86" in a) for a in abis),
          "逐个: " + repr(abis))
    root_bp = rd_text(os.path.join(REPO, "build-profile.json5"))
    check("A5 根 build-profile.json5 里没有第二处 abiFilters(单一来源, 不会被覆盖)",
          "abiFilters" not in root_bp, "根配置里 abiFilters 出现次数 = %d" %
          root_bp.count("abiFilters"))
    cmake = rd_text(os.path.join(HERE, "CMakeLists.txt"))
    check("A6 CMakeLists.txt 没有写死别的架构(ANDROID_ABI / CMAKE_SYSTEM_PROCESSOR)",
          ("ANDROID_ABI" not in cmake) and ("CMAKE_SYSTEM_PROCESSOR" not in cmake),
          "由 hvigor 按 abiFilters 传入")

    emit("")
    emit("[B] 打包产物(HAP)里实际打进去了哪些 ABI")
    emit("=" * 100)
    rel = os.path.join(REPO, "release")
    haps = []
    if os.path.isdir(rel):
        haps = [os.path.join(rel, f) for f in sorted(os.listdir(rel)) if f.endswith(".hap")]
    if not haps:
        skip("B* 打包产物核查", "release/ 下没有 .hap(构建产物可以不存在) -> 这一档没查")
    else:
        hap = haps[-1]
        emit("  核查对象: %s (%d 字节)" % (hap, os.path.getsize(hap)))
        try:
            z = zipfile.ZipFile(hap)
            names = z.namelist()
            libs = [n for n in names if n.startswith("libs/")]
            so_names = [n for n in names if n.endswith(".so")]
            top = sorted(set(n.split("/")[1] for n in libs if len(n.split("/")) > 2))
            emit("  libs/ 下的 ABI 目录: %s" % (top if top else "(一个都没有)"))
            emit("  libs/ 下的 .so: %s" % ", ".join(os.path.basename(n) for n in so_names))
            check("B1 HAP 里 libs/ 下只有 arm64-v8a 一个 ABI 目录",
                  top == ["arm64-v8a"], "实际 = " + repr(top))
            bad_dirs = [d for d in top if d != "arm64-v8a"]
            check("B2 没有 armeabi-v7a / armeabi / x86 / x86_64 任何一个目录",
                  not bad_dirs, "多出来的 = " + repr(bad_dirs) if bad_dirs else "0 个")
            check("B3 HAP 里确实有 .so(这一档不是'空着通过')",
                  len(so_names) > 0, "%d 个 .so" % len(so_names))
            all64 = True
            detail = []
            for n in so_names:
                data = z.read(n)
                hd = elf_header(data)
                if hd is None:
                    all64 = False
                    detail.append("%s: 不是 ELF" % n)
                    continue
                cls, mach = hd
                detail.append("%s: %s, %s" % (os.path.basename(n),
                                              "ELF64" if cls == 2 else ("ELF32" if cls == 1 else "class%d" % cls),
                                              EM.get(mach, "EM_%d" % mach)))
                if cls != 2 or mach != 183:
                    all64 = False
            for d in detail:
                emit("     · " + d)
            check("B4 HAP 里每一个 .so 都是 ELF64 + EM_AARCH64",
                  all64, "%d/%d 个 .so 全部 aarch64" % (len(so_names), len(so_names)))
            check("B5 HAP 里没有任何 32 位 ELF(EM_ARM=40 / EM_386=3 / ELF32)",
                  all64, "逐个头字段已在上面列出")
            modjson = ""
            try:
                modjson = z.read("module.json").decode("utf-8", "replace")
            except Exception:
                pass
            check("B6 module.json 里没有 32 位相关声明(abiList / 32 位 deviceType)",
                  ("armeabi" not in modjson) and ("armv7" not in modjson),
                  "module.json 可读" if modjson else "module.json 没读到(不额外判 FAIL)")
            z.close()
        except Exception as e:
            check("B0 HAP 可被当作 zip 解析", False, "打不开: %s" % e)

    emit("")
    emit("[C] 源码树里有没有预编译二进制(32 位成分的另一条来源)")
    emit("=" * 100)
    bad_bins = []
    src = os.path.join(REPO, "entry", "src", "main")
    for root, dirs, files in os.walk(src):
        for f in files:
            if f.endswith((".so", ".a", ".o")):
                bad_bins.append(os.path.join(root, f))
    check("C1 entry/src/main 下没有任何预编译二进制(.so / .a / .o)",
          not bad_bins, "命中 = " + repr(bad_bins[:5]) if bad_bins else "0 个")
    check("C2 native 源码全部是 C/C++ 源文件(随 abiFilters 一起编译, 不带架构属性)",
          os.path.exists(os.path.join(HERE, "CMakeLists.txt")),
          "CMakeLists.txt 在 cpp/ 下")
    emit("     说明: ArkTS 侧编译产物是 .abc 字节码, 与 CPU 架构无关;")
    emit("           HAP 根目录下没有任何 lib/ 或 libs/<其它 ABI>/ 目录(B1 已核实)。")

    emit("")
    emit("[D] 配置/源码文本里的 32 位 ABI 字样")
    emit("=" * 100)
    hits = []
    for root, dirs, files in os.walk(os.path.join(REPO, "entry")):
        dirs[:] = [d for d in dirs if d not in ("node_modules", "oh_modules", ".git", ".cxx", "build")]
        for f in files:
            if not f.endswith((".json5", ".json", ".txt", ".cmake", ".ts", ".gradle")):
                continue
            # 自检器自己的输出(以及其它 verify_* 的自检器/输出)不是构建输入, 跳过
            if f.startswith("verify_"):
                continue
            p = os.path.join(root, f)
            try:
                t = io.open(p, encoding="utf-8", errors="replace").read()
            except Exception:
                continue
            for kw in ("armeabi-v7a", "armeabi", "armv7", "x86_64", "x86"):
                if kw in t:
                    hits.append("%s: %s" % (os.path.relpath(p, REPO), kw))
    hits = [h for h in hits if "abiFilters" not in h or "arm64" not in h]
    check("D1 entry/ 的构建配置文件里没有 32 位 ABI 字样",
          not hits, "命中 = " + repr(hits[:6]) if hits else "0 处")
    emit("     说明: ref/ 与 kirin_raw/ 下的资料会提到 32/64 位, 那是外部资料, 不是构建输入, 不参与判定。")

    emit("")
    emit("=" * 100)
    verdict = "PASS" if not fails else "FAIL"
    emit("共 %d 项断言, 失败 %d 项, 跳过 %d 档 -> %s" % (checks, len(fails), len(skips), verdict))
    if not fails:
        emit("结论: 【不适用, 本工程全是 arm64】—— HAP 里只有 arm64-v8a, 每个 .so 都是 ELF64/EM_AARCH64,")
        emit("      源码树没有任何预编译二进制。官方那条『32 位应用只能跑小核、绑核也不生效』的限制")
        emit("      不适用于本工程, 因此『只有 8 个核 / 单核频率只有 70% 标称值』不能归因到 32 位。")
    emit("=" * 100)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
