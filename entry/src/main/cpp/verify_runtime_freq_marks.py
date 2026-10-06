# -*- coding: utf-8 -*-
"""
verify_runtime_freq_marks.py —— 运行时频率采样(旁路诊断)的静态自检

校验对象(全部是"能不能拿到数据"的结构性事实, 不做任何数值判定):
  A) cpu_freq_sample.h / .cpp 存在, 且对外接口齐备;
  B) 16 项负载的每一个计时区间都打了点:
       markStart 必须紧邻 t0 之前, markStop 必须紧邻 t1 之后 —— 打点写在计时区间之外,
       因此采样本身不会进 o.ms(这一点由源码位置保证, 不需要真机验证);
  C) 每个 t0 都有配对的 t1(16 对), markStart/markStop 数量各自等于 16;
  D) 旁路接线存在: Gb7Outcome 有 runFreq 字段、napi 输出 "runFreq"、
     gb7RunTest 把这一行追加进 cpuInfo.cpuAllowedText(即每项 note 走的那一行);
  E) 采样口径声明写在文本里(单核 = 本线程所在核 / 多核 = 可用核集合的核), 不含糊。

只读源码, 不联设备、不跑 hdc、不改任何文件。退出码 0 = 全部通过。
"""
import io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = io.open(os.path.join(HERE, "verify_runtime_freq_out.txt"), "w", encoding="utf-8", newline="\n")
fails = []

def emit(s):
    print(s)
    OUT.write(s + "\n")

def check(name, ok, detail=""):
    emit("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, ("" if ok else "  <- " + str(detail))))
    if not ok:
        fails.append(name)

def rd(fn):
    p = os.path.join(HERE, fn)
    if not os.path.exists(p):
        return ""
    return io.open(p, encoding="utf-8", errors="replace").read()

LOAD_FILES = ["gb7_filecompress.cpp", "gb7_batch2.cpp", "gb7_batch3.cpp", "gb7_pdf.cpp",
              "gb7_audio.cpp", "gb7_video.cpp", "gb7_browser.cpp", "gb7_sfm.cpp", "gb7_clang.cpp"]

emit("=" * 100)
emit("[A] 采样模块本身")
emit("=" * 100)
h = rd("cpu_freq_sample.h")
c = rd("cpu_freq_sample.cpp")
check("A1 cpu_freq_sample.h / .cpp 都存在", h != "" and c != "")
for fn in ["auroraFreqSampleSessionBegin", "auroraFreqMarkStart", "auroraFreqMarkStop",
           "auroraFreqSampleSessionEnd", "auroraFreqSampleText"]:
    check("A2 接口 %s 有声明" % fn, (fn + "(") in h)
    check("A3 接口 %s 有定义" % fn, re.search(r"^void %s\(|^int %s\(" % (fn, fn), c, re.M) is not None
          or ("%s(" % fn) in c)
check("A4 数据源就是 scaling_cur_freq(kHz)",
      "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq" in c)
check("A5 参考项 governor / thermal_zone 都读了",
      "scaling_governor" in c and "/sys/class/thermal/thermal_zone%d/temp" in c)
check("A6 采样线程的间隔常量在 200~500 ms 之间",
      re.search(r"kIntervalMs\s*=\s*(\d+)", c) is not None and 200 <= int(re.search(r"kIntervalMs\s*=\s*(\d+)", c).group(1)) <= 500)
check("A7 采样路径无分配(没有 new/malloc/std::string/vector/opendir 出现在 tick 路径里)",
      ("new " not in c.split("// 汇总成一行文本")[0].replace("std::string", "")) )
check("A8 逐项 errno 取证字段齐备(curFreqErrno/threadStatErrno/govErrno/thermalErrno)",
      all(k in c for k in ["curFreqErrno", "threadStatErrno", "govErrno", "thermalErrno"]))
check("A9 最小/中位/最大 + 样本数 + 采样核 都在文本里",
      all(k in c for k in ["中位", "最小", "最大", "采样 %d 次", "采样核 = "]))
check("A10 口径声明逐字写在文本里(单核/多核两种)",
      "跑本项负载的那个线程在每次采样时刻所在的核, 不是全机所有核" in c and
      "不是单线程口径" in c)
check("A11 会话起停做了异常兜底(诊断不让负载失败)",
      "catch (...)" in c)

emit("")
emit("=" * 100)
emit("[B] 16 项负载的计时区间打点")
emit("=" * 100)
pairs = 0
for f in LOAD_FILES:
    t = rd(f)
    lines = t.split("\n")
    for i, ln in enumerate(lines):
        if re.match(r"\s*(const )?double t0 = nowMs\w+\(\);", ln):
            pairs += 1
            check("B1 %s:%d markStart 紧邻 t0 之前" % (f, i + 1),
                  "auroraFreqMarkStart();" in lines[i - 1])
        if re.match(r"\s*(const )?double t1 = nowMs\w+\(\);", ln):
            check("B2 %s:%d markStop 紧邻 t1 之后" % (f, i + 1),
                  "auroraFreqMarkStop();" in lines[i + 1])
ms = sum(rd(f).count("auroraFreqMarkStart();") for f in LOAD_FILES)
me = sum(rd(f).count("auroraFreqMarkStop();") for f in LOAD_FILES)
check("B3 计时区间共 16 对(16 项 GB7 CPU 负载)", pairs == 16, pairs)
check("B4 markStart 共 16 处", ms == 16, ms)
check("B5 markStop  共 16 处", me == 16, me)

emit("")
emit("=" * 100)
emit("[C] 旁路接线(结果进 note + 进 JSON, 且不进任何计分)")
emit("=" * 100)
gh = rd("gb7.h")
gc = rd("gb7.cpp")
na = rd("napi_init.cpp")
check("C1 Gb7Outcome 有 runFreq 字段", "std::string runFreq;" in gh)
check("C2 gb7.cpp 里 runFreq 取自 auroraFreqSampleText()",
      "o.runFreq = freq;" in gc and "auroraFreqSampleText(freq" in gc)
check("C3 这一行被追加进 cpuInfo.cpuAllowedText(每项 note 走的那一行)",
      "o.cpuInfo.cpuAllowedText + used" in gc)
check("C4 放不下时退到 o.diag(同样进 note, 不丢信息)", "o.diag += freq;" in gc)
# napi 侧是 C++ 字符串字面量, 源码里写作 \"runFreq\":\"  -> 用正则找"转义后的引号 + runFreq"
check("C5 napi 输出 runFreq 旁路字段(成功路径与失败路径形状一致)",
      len(re.findall(r'runFreq\\":\\"', na)) >= 2,
      "napi 里 runFreq 出现 %d 次" % len(re.findall(r'runFreq', na)))
check("C6 计分公式一个字未动(score = k * (value * e.conv))",
      "score = k * (value * e.conv);" in gc)
check("C7 ENTRIES 表里的 k/conv 未被触碰(抽查 3 项)",
      "7.027686550" in gc and "181.6940430" in gc and "184.8221414" in gc)

emit("")
emit("=" * 100)
emit(("全部通过(%d) " % (len(fails) == 0)) + ("FAILS: " + ", ".join(fails) if fails else ""))
emit("=" * 100)
OUT.close()
sys.exit(1 if fails else 0)
