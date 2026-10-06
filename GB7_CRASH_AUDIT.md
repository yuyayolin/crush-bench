# gb7 负载静态审计报告(真机闪退定位)

> 结论先行: **9 个负载文件里没有找到"按旧尺寸写死的缓冲被标定放大后越界"这一类确定性缺陷。**
> 固定容量数组与最大索引表达式逐条核对全部 PASS(见 \`verify_gb7_arrays.py\` 输出 792 行)。
> 已修 3 处(3 个文件), 全是"潜在越界/未定义行为"的兜底, 不改口径、不改并行结构、不改计分;
> 另有 4 处**不该由我单方面改**的缺陷/风险列在第 5 节, 等你决定。

- 审计范围: \`entry/src/main/cpp/gb7_filecompress.cpp / gb7_batch2.cpp / gb7_batch3.cpp / gb7_pdf.cpp /
  gb7_audio.cpp / gb7_video.cpp / gb7_browser.cpp / gb7_sfm.cpp / gb7_clang.cpp\`(共 15,175 行)
- 未改动: \`gb7.h / gb7.cpp / napi_init.cpp / CMakeLists.txt / Index.ets / gb7_parallel.h\`(只读引用)
- 交叉编译: aarch64-linux-ohos, clang 15.0.4, \`-std=c++17 -c -O2 -fPIC -pthread -Wall\`
  → **9/9 文件 exit=0, 零 error 零 warning**; 额外用 \`-Wall -Wextra\` 复核也是 9/9 干净
  (唯一 26 条 \`-Wextra\` 提示来自 \`third_party/stb_image_write.h\` 自身, 不在可改范围)
- 本机无设备无模拟器, 也没有可用的 host C++ 工具链(无 MSVC/MinGW 头, 交叉 clang 缺 host libc++ 头),
  因此**没有动态验证**, 全部结论都是静态推导 + 脚本核对。

---

## 1. 交付物

| 文件 | 作用 |
|---|---|
| \`gb7_*.cpp\`(3 个文件 4 处改动) | 第 2 节列出的兜底修复 |
| \`entry/src/main/cpp/verify_gb7_arrays.py\` | 固定容量数组 vs 最大索引表达式逐条核对 + 标定常量回读校验 |
| \`entry/src/main/cpp/verify_gb7_memory.py\` | 峰值内存核算(单核/14 线程) + 大缓冲尺寸来源核对 |
| \`entry/src/main/cpp/verify_out.txt\` / \`verify_mem_out.txt\` | 两个脚本的输出(UTF-8, 可直接查看) |

运行方式(任一目录):
\`\`\`
python entry/src/main/cpp/verify_gb7_arrays.py     # 退出码 0 = 全部 PASS
python entry/src/main/cpp/verify_gb7_memory.py     # 退出码 0 = 全部 PASS
\`\`\`

---

## 2. 逐文件审计结果: 尺寸相关的越界 / 空指针 / 除零

### 2.1 核对方法(可复现)

1. 从源码里重新解析标定常量(pageSize=1MiB, pages=192, texW/texH=4096, 大地图 896, photos=7/4,
   samples=14, steps=360, kVFrameCount4=4, seconds=1, kPageCount=2, kMaxFeat=560, kTargetMs=2200)
   —— 共 15 条, 全部与报告一致(脚本第 [1] 节)。
2. 抽出每个固定容量声明(栈数组 / \`std::array\` / 静态表 / 固定成员 / 按常量算出的 vector),
   写出它的**索引上界表达式**, 与容量比对(脚本第 [2] 节, 共 71 条)。
3. 全文件扫描每一处 \`name[...] = ...\` 写入点并按名归类(脚本第 [3] 节), 确认没有"漏网的数组"。

### 2.2 全部结论

**没有任何一项是"容量 < 索引上界"。** 逐条明细见 \`verify_out.txt\`, 这里只列与标定直接相关、
最容易出问题的那一批:

| 文件 | 结构 | 容量 | 索引上界 | 结论 |
|---|---|---|---|---|
| gb7_batch2 | \`text[pageSize*pages]\` | 1MiB*192 = 192MiB | \`i<text.size()\` 且 \`i+len+2<size\` 才 memcpy | PASS(8MiB→192MiB 是同一缓冲, 生成时有边界检查) |
| gb7_batch2 | \`partial[pages]\` | 192 | 任务区间起点 s<pages | PASS(槽位随页数一起从 128 变 192) |
| gb7_batch2 | \`tex[texW*texH*4]\` | 64MiB | \`((y*texW+x)*4+c)\` | PASS(纹理 2048²→4096² 全部由 texW/texH 推导, 没有 2048 假设) |
| gb7_batch2 | \`partial[blocksY]\` | 1024 | rs<blocksY | PASS(块行数由 texH/4 推导) |
| gb7_batch2 | 地图 \`cost[w*h]\` / \`dist[w*h]\` | 896²*4B / 896²*8B | 邻域 v=vy*w+vx 且 vx<w,vy<h | PASS(640→896 全部按 w/h 走) |
| gb7_batch2 | 路线端点 \`% (w-4)\` | - | 896-4=892≠0 | PASS(无除零; 起终点落在图内) |
| gb7_batch3 | \`img/work/hdr/ldr/out[w*h*c]\` | 2.25/18/42/10.5/1.7 MB | \`((y*w+x)*c+i)\` | PASS(全部按 w/h 分配) |
| gb7_batch3 | \`photoAcc[photos]\` / \`rowAcc[h]\` | 7 / 2000 | p<photos, y<h | PASS(槽位随标定量一起变) |
| gb7_batch3 | \`spheres[6]\` + \`acc[c]/samples\` | 6 | k<6, samples=14≠0 | PASS(采样 24→14 不产生除零; 14 是编译期常量, 编译器可见非零) |
| gb7_batch3 | \`head[64³]\`/\`next[8192]\` | 262144/8192 | cellIdx 的 cx,cy,cz 先夹到 [0,63] | PASS(步数 600→360 与网格无关) |
| gb7_audio | \`pcm[totalFrames*2]\` | 960000 | \`((blk*4096+i)*2+1)\`, blk<blocks=11 | PASS(60s→1s 后最大下标 90000 级, 缓冲按真实样本数分配) |
| gb7_audio | \`blockBuf[2][4096]\`/\`ks[256]\`/\`lpc[16]\`/\`scratch[4096]\` | 固定 | i<4096, parts<=256, order<=12 | PASS(块大小与阶数常量都没有被标定改动) |
| gb7_video | \`refY/recY[kVLumaSize4]\` | 921600 | 运动补偿前 clampVecV4 夹取; 色度单独夹取 | PASS(帧数 90→4 不改缓冲尺寸; 分辨率未改) |
| gb7_video | \`BitWriterV4 buf(4MiB)\` | 4MiB | 每次写位前判 \`byteIdx>=buf.size()\` | PASS(固定容量但**每次访问都查界**, 溢出只置 over 标志并 break) |
| gb7_pdf | \`cov[kPgW]\`/\`pix[kPgW*kPgH*3]\` | 1275 / 6.0MB | x<=ibMax=cx1-1<=1274, row<=1649 | PASS(页数 5→2 只减少渲染页数, 缓冲尺寸不变) |
| gb7_pdf | \`PathB.xy\`/\`edges\` | 400000 点 / 600000 边 | 每次 push 前判上限, 超限置 overflow/丢弃 | PASS |
| gb7_pdf | \`kGlyphBits[47][7]\` | 47 | glyphIndex()∈[0,46] | PASS |
| gb7_browser | \`PageSlot.fb[1024*768]\` | 786432 | 所有写入都过 fbBlend/fbFill 并逐坐标夹取 | PASS |
| gb7_browser | \`nm[25]/av[161]/cls[40]/idb[32]/sty[200]/buf[24]\` | 各含结束符位 | 全部"先判 < 上限-1 再 memcpy + 写 0" | PASS |
| gb7_browser | \`curOk/nxt/chain[kMaxDepth+4]\` | 68 | L<kCap | PASS |
| gb7_sfm | \`fs.desc[n*4]\`/\`fs.angle[n]\` | n<=560 | i<nFeat | PASS(见 2.3-(c)) |
| gb7_sfm | \`parent/used[10*560]\` | 5600 | na=ia*kMaxFeat+a, a<560 | PASS(索引空间随 kMaxFeat 一起从 900 变 560, 且 ufFind 前还有 \`mt.a<kMaxFeat\` 检查) |
| gb7_sfm | \`hw.t1..t3/resp/tmp/blur[1280*960]\` | 5*4.7MB+1.2MB | base=y*W+x | PASS |
| gb7_clang | \`aurora_pool[262144]\` | 256KiB | \`used+aligned>POOL_BYTES\` 则返回 0 | PASS(失败返回空指针, 调用方判空) |
| gb7_filecompress | \`Sha1::w[80]/buf[64]\`, \`outs[slot]\` | 80/64/池大小 | i<80, bufLen<64, 槽来自 freeSlots | PASS |

### 2.3 并行路径专项

- **File Compression**: 输出缓冲池 \`outs[min(threads,12)]\` + \`freeSlots\`, 取一还一, 不会空池取。
- **Navigation / Text / Asset / Photo / Browser / SfM / Audio / Video**: 每个并行调用只写"自己的槽",
  槽数组尺寸 = 任务数(随标定量增长), 归约在并行区外串行做。
- **PDF Viewer(多核)**: worker 池大小 = parThreads, 而 \`gb7ParallelFor\` 的并发 body 数 <= parThreads,
  池永不空。**但原代码用 \`freeWorkers.back()\` 直接取, 空池就是越界读(UB/SIGSEGV)** —— 见第 3 节改动 (b)。

### 2.4 除零 / 取模零专项

标定后变小或变 0 的除数逐个核对过:
- Ray Tracer \`acc[c]/samples\`(24→14)、\`/w\`, \`/h\`: 全部非零, 且 samples 是编译期常量。
- PDF \`/kSubScan\`(8)、\`/maxd\`(=cx²+cy², 1275/1650 非零)、\`/det\`(有 \`|det|>1e-12\` 守卫)、
  \`invert3/solveCholesky\`(失败走重试+jitter)。
- Audio \`/beatSamples\`(24000)、\`/transientLen\`(288)、\`/err\`(\`err>1e-9\` 守卫)、\`>>k\`(k>=0)。
- SFM \`/s0,/s1\`(有 \`>1e-300\` 守卫)、\`/d\`(有 \`>1e-9\` 守卫)、\`/var\`(\`var>1e-12\` 守卫)、
  索引空间除法 \`nd/kMaxFeat\`(560≠0)。
- Clang \`kTargetMs/warmMs\`(warmMs>0 才除)、\`srcLines\`(==0 提前返回)。
- 结论: **没有发现因标定(数量变小)而产生的除零/取模零**。

---

## 3. 本轮改动(4 处, 只做兜底, 不改口径/常量/并行结构/计分)

### (a) \`gb7_pdf.cpp\` —— 多核 worker 池"空池取元素"改成安全获取/归还
- 位置: \`gb7RunPdfViewer\` 多核路径(原 3055-3068 行)。
- 形态: \`w = freeWorkers.back(); freeWorkers.pop_back();\` —— 池空时 \`back()\` 是越界读(UB)。
- 为什么真机会崩: 空 vector 的 \`back()\` 读未分配内存, 在真机上就是 SIGSEGV; 而且它在
  \`gb7ParallelFor\` 的 worker 线程里 —— 崩溃点与负载名对不上, 面包屑会指向 PDF 这项。
- 修法: 抽出 \`acquireWorker/releaseWorker\`; 池空时用**本工作单元的私有 Raster**(仍然渲染进共享的
  \`ras.pix\`, 结果逐位相同)顶上, 且私有兜底不还池(所有权在 \`unique_ptr\`)。
  正确不变式下这条分支永不执行 —— 纯粹把 UB 变成定义行为。

### (b) \`gb7_batch3.cpp\` —— Photo Library 解码图采样点加前置守卫
- 位置: \`gb7RunPhotoLibrary\`(原 127 行 \`if (decoded != nullptr)\`)。
- 形态: 色调趟按 \`y+=4 / x+=4\` 采样后读 \`decoded[idx..idx+2]\`。**只要 dw/dh 不被 4 整除,
  \`x+3\` 就跨到下一行, 最后一行就跨出缓冲 -> 堆越界读。** 标定把单张从 12MP(4000x3000)改成
  1024x768 —— 1024/768 恰好都能被 4 整除, 所以**现在安全**; 但如果以后再调这张图的尺寸
  (或出现非 4 倍数尺寸), 这里立刻越界。
- 修法: \`if (decoded != nullptr && dw >= 4 && dh >= 4)\`。当前尺寸下恒真, 一帧结果、一个计数都不变;
  只是不再依赖"1024x768 恰好被 4 整除"这个标定巧合。注释已写明"因为标定把 12MP 改成 1024x768"。

### (c) \`gb7_sfm.cpp\` —— 特征网格槽位下标加当前特征数上界检查
- 位置: \`detectFeatures\` 的 12px 网格分散趟(原 917-918 行)。
- 形态: \`ws.grid[...]\` 里存的是"写入时的 fs.n", 随后被当作 \`fs.x/fs.y\` 的下标用。
  标定把每图特征数 900→560, 两个数字都挂在 \`kMaxFeat\` 上(遍历上界 kMaxFeat、写入 fi=fs.n<kMaxFeat),
  正常不会越界; 但 \`fs.x/fs.y\` 是按 fs.n 增长的 vector, **一旦槽位里出现 >= 当前 fs.n 的下标就是堆越界读**。
- 修法: \`if (fi < 0 || fi >= fs.n) { continue; }\`。正常路径永不命中, 不改任何特征/描述子/匹配结果。
  (同一负载上一轮已经修过一次同类问题: \`fs.angle\` 曾经是空 vector 却被写 —— 空指针解引用。
  说明这个文件的"按新尺寸重算了下标、但没重算容量的地方"确实是危险区, 所以我在这里加了断言式守卫。)

### (d) 没有改的地方(有意为之)
- 所有标定常量、metric 公式、计分、并行分解方式、\`gb7_parallel.h\` 调用方式: **一字未动**。
- File Compression 的 \`h1/h2/h3\` 数据竞争(见 5.1): 它是"确定性缺陷", 但修它必须给并行任务加
  同步/私有槽 —— 属于改并行结构, 按约束留给你决定。

---

## 4. 峰值内存表(单核 / 14 线程)

口径: 单核 = threads=1; 14 线程 = 每个 worker 的**私有**缓冲各一份(与各负载的并行分解一致)。
完整可复现表格见 \`verify_mem_out.txt\`(脚本把每一项的分配来源与尺寸表达式都列了出来)。

| 负载 | 主要分配 | 单核 MB | 14 线程 MB |
|---|---|---|---|
| **Text Processing** | 语料 1MiB×192 页, **单块连续 192 MiB** | **192.0** | **192.0** |
| **Asset Compression** | RGBA8 纹理 4096²×4B, **单块连续 64 MiB** | **64.0** | **64.0** |
| File Compression | 语料 12MiB + 输出缓冲池 min(threads,12) 槽×4.2MiB | 16.2 | 62.4 |
| Navigation | 地图 3.3MB + 14×(dist 6.4MB + 二叉堆 ~1.6MB) | 21.7 | 115.3(最坏 ~271) |
| Photo Library | 源图 2.25MB + min(threads,7)×(jpeg+decoded 各≈2.36MB) | 7.0 | 35.3 |
| Photo Editor | 源图 18MB + work 18MB(所有线程共享) | 36.0 | 36.0 |
| HDR | hdr float 42.2MB + ldr 10.5MB(所有线程共享) | 52.7 | 52.7 |
| Ray Tracer | 输出 1.7MB | 1.7 | 1.8 |
| Game Physics | 6×float[8192] + head[64³] + next(保持串行) | 1.2 | 1.2 |
| PDF Viewer | 页位图 6MB + 主 edges/PathB + 13 份 worker 暂存 | 26.1 | 198.8(一页真有 60 万边时 ~250) |
| HTML5 Browser | min(threads,8) 槽 × (帧缓冲 3MB + DOM/arena/boxes ≈3.4MB) | 6.4 | 51.2 |
| Audio Encoder | PCM 1.83MB + 14×私有位流/块缓冲 | 2.1 | 6.0 |
| Video Enc/Dec | y/u/v + ref×3 + rec×3 + MbStore 2.6MB + 码流 4MiB | 15.7 | 18.3 |
| SfM | hw 5×图像级 float[1280×960] 19.5MB + images 12.3MB + 场景/特征/BA | 70.0 | 70.3 |
| Clang | bump arena(单轮 ~20MB, chunk 上限 16MB) | 40.0 | 40.0 |

**红线判定(任务给定: 单核 > 400MB / 多核 > 800MB 标红): 没有任何一项越过红线。**
本机这台设备上, 峰值最高的三项是 Text Processing(192)、PDF Viewer 并行(198.8)、Navigation 并行(115)。

### 4.1 内存保险方案(按你的要求**没有动手**, 只给方案与影响评估)

1. **Text Processing 的 192 MiB 语料(收益最大)**
   - 现状: \`std::vector<uint8_t> text(pageSize*pages)\` 一次性连续 192 MiB, 然后 192 页各自扫描自己那 1 MiB。
   - 方案 A(推荐, **零耗时影响**): 语料本来就是"确定性伪随机生成的词流", 生成函数只依赖
     \`(全局 32 位 PRNG 状态, 页号)\`。把"生成全部 192 页"改成"生成当前页那 1 MiB"——
     只要在生成第 p 页前把 PRNG 状态推进到"前 p 页消耗掉的抽签次数", 产出的**字节和现在逐位相同**,
     每页扫描的字节数、页数、pages/s 分子分母**完全不变**, 内存从 192 MiB 降到 **1 MiB**。
     代价: 需要把 \`words[xsB2(s)%16]\` 的抽签次数按页缓存一张前缀表(192 个 uint64), 一次性 O(1)
     推进(或用"每页一个派生种子 + 保持逐位一致"的写法, 但那会改变字节内容 → 不可取)。
     **这一步需要真机跑一次单核/多核对照**(字节应由 hash 校验: 现有 \`sink\` 就是校验值, 前后必须相等)。
   - 方案 B(次选, 会略微改变内存-耗时权衡): 保留连续缓冲但改成分块复用
     (例如 16 MiB 窗口滚动, 每页在自己的窗口里重生成) —— 仍然是同一份字节, 内存降到 16 MiB,
     但生成成本从"一次性"变成"每页重生成自己那一段", 生成耗时与页数成正比(而不是一次性),
     可能把 o.ms 抬高几毫秒; **必须真机核对 o.ms 变化**。
   - 方案 C(不推荐): 直接把 pages 从 192 调小 —— 这是改工作量常量, 明确禁止。

2. **Asset Compression 的 64 MiB 纹理**: 纹理是"按像素位置确定性生成"的
   (\`x*255/texW\`、\`y*255/texH\`、\`x^y\`、\`xsB2(s)\`), 但 \`xsB2(s)\` 是全图串行推进的,
   所以"按需生成某一区块"需要把 PRNG 也做成可跳跃的(比 Text 那项麻烦)。
   更省事的是"按块行滚动生成 + 就地编码该块行"——但这会改变"先全图生成再编码"的访存局部性,
   对 MB/s 有影响。**建议: 这一项先不动**(它只有 64 MiB, 且是全项目唯一"性能余量比目标更大"的负载)。

3. **PDF Viewer 并行 198.8 MB**: \`kMaxEdges=600000\` 是**悲观上限**, 每个 worker 只有在
   该行带真的碰到 60 万条边的路径时才会把 \`edges\` 撑到 13.7 MB。当前生成的内容流(2 页, 每页
   约 100 个路径操作、贝塞尔细分深度 <=10)离这个上限很远, 实际占用远低于表中数字。
   真要降: 把 \`kMaxEdges\`/\`kMaxPathPts\` 按"实际内容流的最大边数"重新定一个够用的值
   (属于改"防御上限", 不改工作量与 metric), 或者让 worker 的 \`edges\` 用 \`shrink_to_fit\` 后
   长期复用。**建议: 先不动, 等真机确认 PDF Viewer 是不是元凶。**

4. **Navigation 并行的二叉堆**: dist 6.4 MB×14 是硬开销(数据分解的必然结果);
   二叉堆换成"配对堆/四点堆"可以省一点, 但那是改算法。**建议: 不动。**

---

## 5. 确定性缺陷 / 风险(我**没有硬改**的部分, 请你决定)

### 5.1 【确定性缺陷·数据竞争】File Compression 的 \`h1/h2/h3\`
\`gb7_filecompress.cpp\` 265-292 行: 并行任务体里 \`h1 = sha1Of(out);\` 而 \`h1/h2/h3\` 是**普通
\`uint64_t\` 局部变量**, 12 个任务可能被两个线程同时执行 → 对同一变量的并发读改写 = 数据竞争(UB)。
- 真机后果: 8 字节对齐的标量存储, AArch64 上不会撕裂, 所以**不会崩**, 最坏是"最后写入者胜"导致的
  校验值不确定; 但 UB 就是 UB, TSAN 下会报。
- 修法(不改并行结构, 只是把"共享累加"换成"任务自己的槽"): 用 \`std::vector<uint64_t> taskHash(12,0)\`,
  每个 case 写 \`taskHash[k]\`, 并行区结束后 \`sink = taskHash[0]^taskHash[1]^...\`。
  sink 本身只是"防止结果被优化掉", 不参与 metric/计分, 所以对成绩零影响。
- 我为什么不改: 它确实要动"12 个任务各自的输出槽"这段并行区代码, 属于并行结构微调,
  按约束(不许改并行结构)留给你拍板。**建议: 采纳**, 这是纯收益。

### 5.2 【确定性缺陷·工作量/结果不正确】Ray Tracer 并行路径的抖动种子
\`gb7_batch3.cpp\` 423-434 行: 每个工作单元开始处 \`uint32_t rs = 0x9E3779B9 ^ (y*2654435761u);\`
—— 同一条扫描线的**左半和右半拿到同一个种子**, 于是左右两半的采样抖动序列完全相同(镜像重复),
而串行路径是一条连续 PRNG 流。
- 后果: 不崩、不改候选口径, 但并行/串行图像不同(注释里写的"每行消耗的随机数个数与串行完全一致"
  不成立), 对"单核/多核对照"是个瑕疵。
- 修法: 种子再混入半行标志, 例如 \`rs = 0x9E3779B9 ^ (y*2654435761u) ^ (half ? 0x85EBCA6Bu : 0u)\`。
- 我为什么不改: 它会**改变像素结果**(虽然口径不变), 属于"改负载输出", 需要你同意。

### 5.3 【风险·量级未标定】Navigation / PDF 的内存与二叉堆规模
见 4.1-4。Navigation 并行的二叉堆规模没有实测数据, 我在表里给了"典型 ~113MB / 最坏 ~271MB"的区间。

### 5.4 【风险·真机才可见】Photo Library 的 jpeg 缓冲尺寸
\`jpeg\` 向量由 stb 的 \`writeToVector\` 逐步追加, 无上限。1024x768 q=88 实测量级应是几百 KB,
但如果 stb 写出异常大的流(理论上限 3 字节/像素 = 2.36 MB), 这个是动态增长、不会崩。**不需要改。**

---

## 6. 最可能的崩溃项排序(前 3)与理由

> 前提: 单核跑 16 项、多核跑官方 8 项, 两个阶段**共有的 7 项**是
> File Compression / Text Processing / Asset Compression / Photo Library / Photo Editor / HDR / Ray Tracer。
> "两个按钮都闪退"说明大概率是这 7 项之一(或者两个阶段都有的某条公共代码路径)。

**第 1 名: Text Processing(192 MiB 单块)**
- 理由 1: 它是全项目**唯一的 192 MiB 连续分配**, 是第二大项(Asset Compression 64 MiB)的 3 倍,
  占一次跑分峰值内存的绝对主导。HarmonyOS 应用对 native 堆有额度, 大块连续分配在小内存机器上
  最容易被"分配失败"或"系统回收/杀进程"; 用户界面自己都写了"可用内存低于 1.5 GB 时大内存负载
  可能被系统回收而异常退出"。
- 理由 2: 分配点在 **NAPI 异步工作线程**里; 14 线程并行时另有 14 个 8 MiB 线程栈 + 14 个 \`std::function\`
  副本, 峰值压力叠加。
- 理由 3: 它是"标定者只改常量"幅度最大的一项(每页 ×16, 页数 ×1.5, 语料 ×24), 与线索描述最吻合。
- 可自查: 崩溃后重开 App 看面包屑(见第 7 节)。若面包屑是 "GB7 单核 · Text Processing" 或
  "GB7 多核 · Text Processing"(它是多核第 4 项), 基本坐实。

**第 2 名: Asset Compression(64 MiB 单块 + 最重的逐像素负载)**
- 理由 1: 标定把纹理从 16 MiB 放大到 64 MiB(×4), 是第二笔大内存; 同时它还是"单核跑一次 4096²
  的 16.7M 个块 × 2 趟"的重负载, 真机在这项上最容易触发降频后的长时间高占用。
- 理由 2: 它的并行按块行分解, 14 线程时 \`partial[1024]\` + 共享纹理, 内存增量小 —— 所以如果崩在它,
  更可能是"内存压力累积"(前一项刚释放 192 MiB, 分配器没还给系统就再要 64 MiB)。

**第 3 名: Photo Editor(36 MB)或 HDR(52.7 MB)**
- 这两项都是"两/三块 11~18 MB 的中等缓冲 + 三趟逐像素流水线", 且都在官方多核 8 项里。
- 它们不像 Text/Asset 那样有单块超大分配, 但**总内存占用在跑分中途叠加**(前一项的释放可能还没归还
  给系统), 是"崩溃点看起来漂移"的典型来源。
- 如果面包屑显示的是"上一项成功、这一项开始就崩", 优先怀疑它们。

并列提醒: **PDF Viewer 多核路径的 worker 池(第 3 节 (a))是我唯一改掉的真 UB**。如果崩溃项是
PDF Viewer, 那这次改动就可能直接修好了它。

---

## 7. 仍需真机确认的部分

1. **最关键的一条: 用 App 自带的崩溃面包屑定位到具体项**
   \`entry/src/main/ets/service/CrashGuard.ets\` 已经在**每一项开跑前把「阶段 + 项名 + 序号」flush 到
   preferences**(\`CRUMB_KEY\`), 崩溃后下次启动在 CrashGuard 页能看到"最后启动的项"(还保留最近 5 条历史
   \`CRUMB_LOG_KEY\`)。**请把那条记录发我**, 这比我任何静态推理都准。注意按它自己的说明:
   "最后启动的项不一定是肇事项, 也可能是上一项留下的大内存"。
2. 若是 Text Processing: 请同时记下**崩溃前那一项的耗时**(结果页每行都有 ms)——
   如果它在 1.8 s 左右正常跑完、下一项才崩, 说明是"释放未归还"型内存压力; 如果它自己就崩在
   192 MiB 分配上, 那就是分配失败/被杀。
3. 若是 Asset Compression: 请确认是否**单核也崩**(两阶段都跑它, 单核只占 1 个输出槽)。
4. 重编后请复跑**同样的两个按钮**, 重点是:
   - PDF Viewer 单核 **与** 多核(改动的 (a) 只影响多核路径)结果是否与改动前一致;
   - Photo Library 单核/多核的 \`images/s\` 与 \`ms\` 是否与改动前一致(改动 (b) 应当零影响);
   - Structure from Motion 的 \`Mpts/s\` 是否与改动前一致(改动 (c) 应当零影响)。
5. 我给的 \`verify_gb7_arrays.py\` / \`verify_gb7_memory.py\` 是静态核算, **不能替代真机**:
   峰值内存表里的 Java/native 堆余量、系统回收阈值、14 线程栈开销都必须在设备上实测
   (可用 App 已有的 MemoryProbe + \`hidumper\`/\`hdc shell\` 采样)。
6. 本机没有 host C++ 工具链(无 MSVC/MinGW 头, 交叉 clang 缺 host libc++ 头), 所以**没有动态验证**;
   如果希望我先在 PC 上把 16 项跑一遍做冒烟测试, 需要允许我装一个 MinGW-w64/libstdc++ 或者用
   WSL —— 这超出本次授权范围, 请明确是否需要。
---

# 附录 A: 第二轮(用户批准的三项决定)执行结果

执行日期: 第二轮。三项决定的前后对比、验证方式与结论如下。

## A.1 【已修】File Compression 的 `h1/h2/h3` 数据竞争

| | 内容 |
|---|---|
| 文件/位置 | `gb7_filecompress.cpp`, `gb7RunFileCompression`(原 260-297 行) |
| 改动前 | `uint64_t h1/h2/h3` 是**普通局部变量**; 12 个任务由动态分块交给 14 个线程执行, 两个线程可能同时对同一个变量读改写 → 数据竞争(UB), 最终 hash 取决于"最后写入者胜", 结果不确定 |
| 改动后 | `std::vector<uint64_t> taskHash(12, 0)`; 任务 k 只写 `taskHash[k]`(k=0/1/2 仍对应原来的 h1/h2/h3 三个 SHA1); k>=3 的任务写"输出长度 ^ 首字节"的 FNV 初值(与原来"被 h1/h2/h3 挡住优化"等价, 仍然只读 O(1) 字节, 不遍历输出); 归约改成**并行区之外**的串行异或, 并保留 `h1/h2/h3` 变量名与语义 |
| 影响评估 | 任务数(12)、每个任务的压缩调用、输出缓冲池、并行分解方式全部未变; sink 只用于"防止结果被优化掉", 不参与 metric/计分 → **对 MB/s 与耗时零影响** |
| 验证 | 交叉编译 `-Wall` exit=0/零告警; 改动是"共享累加 → 私有槽 + 区外归约", 逐任务输出与改造前一致 |

## A.2 【已修】Ray Tracer 并行路径的抖动种子

| | 内容 |
|---|---|
| 文件/位置 | `gb7_batch3.cpp`, `gb7RunRayTracer` 并行分支(原 428-439 行) |
| 改动前 | `rs = 0x9E3779B9 ^ (y * 2654435761u)` —— 同一条扫描线的**左半与右半拿到同一个种子**, 右半行的采样抖动序列与左半行逐位重复(整行镜像), 与串行路径的"一条连续 PRNG 流"不同 |
| 改动后 | 增加 `const uint32_t kHalfSalt = 0x85EBCA6Bu;`, 当 `(i & 1) != 0`(右半行)时 `rs ^= kHalfSalt`, 使一行内的两个半行各自是一条独立、互不重复的抖动序列 |
| 影响评估(已按要求写进代码注释) | **只影响像素值(并行渲染的抖动噪声图案), 不改变每像素的采样次数, 也不改变任何计数 —— metric(`w*h*samples/1e6/秒`)与耗时量级不变。** |
| 验证 | 交叉编译 exit=0/零告警; `samples`/循环结构/并行分解粒度(每行 2 块)均未改动 |

## A.3 【已批准, 但**未采用**】Text Processing 降内存(192 MiB -> ~1 MiB)

**结论: 我把改动回退了, Text Processing 保持原来的 192 MiB 单一连续分配。**
原因很直接: 我没能在"**字节逐位不变**"这个前提下把按页生成做对, 而字节一变就会静默改变
每页的 pageHash 与 pages/s 口径 —— 那是比多占 191 MiB 更严重的问题, 所以宁可如实回退。

### A.3.1 已完成的验证工件(可复现, 留给下一轮直接用)

- `entry/src/main/cpp/verify_gb7_textmem.py` —— 把**改动前**(一次性连续生成)与
  **改动后**(建起始状态表 + 按页生成 1 MiB + 跨页 carry)两条路径都按 C++ 源码逐字重写,
  对 `2/3/5 页 x 4 KiB`、`5 页 x 64 KiB`、`2/3 页 x 1 MiB`、`3 页 x 2 MiB` 逐字节比较,
  并打印两边的 FNV-1a hash 与首个不同字节的偏移。
- `entry/src/main/cpp/gb7_textmem_check.cpp` —— 同样两条路径的 C++ 版本(可交叉编译, 需在设备/有
  libc++ 的机器上运行), 自带 `2/3/4 页 x 4 KiB` 与 `192 页 x 1 MiB` 四种规模的逐字节比较。

### A.3.2 失败的形态与根因(为什么不是"再调一下就行")

- **2 页情形已经逐字节一致**(包括"词恰好跨过页边界"这种最刁钻的场景), 说明"页 0 生成 + 跨页
  carry"的基本模型是对的;
- **>=3 页时, 第 2/3 个页边界处必然错位 1 个字节**。我把这个边界语义按 8 种写法试过
  (carry 只带词尾 / 带词尾+分隔符 / 带"待抽签标记" / 把 pending 与恢复点分开记录 / 窗口式判定 ...),
  每次都能修掉上一个错位点、又在下一个页边界冒出来一个新的 —— 这正是"没有可运行环境"时最危险的状态。
- 根因(已经定位到语义层面): 原实现的页边界语义是**两件独立的事** ——
  1) "某个字节写在哪一页": 原循环先写**整个词**, 再看 `pos` 是否越过页尾; 越过了就 `break`,
     **那个分隔符字节根本不写**; 词的最后一个字节则可能正好落在下一页页首;
  2) "某次 PRNG 抽签算在哪一页": 选词/选分隔符两次抽签都可能发生在"字节还没落盘"的时候。
  要逐位复刻, 必须把 (pending 字节, 它们的逻辑编号, 待抽签的状态, "词已抽/分隔符已抽"标记)
  一起跨页携带, 而其中任一处的口径差 1 就会让后续所有页的抽签流整体错位。
- 本机**无法运行验证**: 无设备、无模拟器, 且没有可用的 host C++ 工具链(无 MSVC/MinGW 头,
  交叉 clang 缺 host libc++ 头), 所以我拿不到"最后一个字节到底属于哪一页"的权威答案。

### A.3.3 下一轮怎么低成本做完(建议)

1. 先在有设备/有 libc++ 的机器上跑 `verify_gb7_textmem.py`(python 即可, 30 秒)或
   `gb7_textmem_check.cpp`, 把最后一个错位点精确到字节;
2. 按 A.3.2 的两条边界语义重写 `genPage`, 并保留我在这一轮已经写好的**运行期自检**
   (对首页/中间页/末页重生成并核对状态链与字节, 不一致就把原因写进 `unit` 字段, 界面可见);
3. 保底方案(零风险, 但收益小于目标): 把语料改成"192 个 1 MiB 块"而不是"单块连续 192 MiB",
   只消除"大块连续分配"这一项风险, 生成逻辑一字不改。

### A.3.4 回退后留在代码里的说明

`gb7_batch2.cpp` 顶部保留了一段 `============ 内存保险(192 MiB -> ~1 MiB): 尝试过, **未采用** ...`
注释, 写明了: 目标、结论、验证工件、失败形态与根因、下一步建议、以及"保底方案"。
同时 `verify_gb7_memory.py` 的峰值内存表里 Text Processing 仍然是 **192.0 / 192.0 MB**,
备注里写明"降内存改造已尝试但未采用, 方案 A 可降到单核 1 MiB / 14 线程 14 MiB"。

## A.4 回退后重跑的验证结果

- 交叉编译(aarch64-linux-ohos, `-std=c++17 -c -O2 -fPIC -pthread -Wall`):
  **10/10 文件 exit=0, 零 error 零 warning**(9 个负载文件 + `gb7_textmem_check.cpp`);
- `python verify_gb7_arrays.py` → **exit 0**(全部 PASS, 无 FAIL / 无 RISK);
- `python verify_gb7_memory.py` → **exit 0**(峰值内存表已按"Text Processing 保持 192 MiB"更新);
- **更新后的峰值内存表与第 4 节完全相同(Text Processing 仍为 192.0 / 192.0 MB)**, 因为 A.3 未落地。

## A.5 本轮改动的最终清单

| 文件 | 改动 | 状态 |
|---|---|---|
| `gb7_filecompress.cpp` | 共享 `h1/h2/h3` → 每任务私有槽 `taskHash[k]` + 区外异或归约 | 已落地, 零影响 |
| `gb7_batch3.cpp` | Ray Tracer 并行种子混入半行标志(仅影响像素值) | 已落地 |
| `gb7_batch3.cpp` | (上一轮)Photo Library 解码图采样前置守卫 | 已落地 |
| `gb7_pdf.cpp` | (上一轮)多核 worker 池空池安全检查 | 已落地 |
| `gb7_sfm.cpp` | (上一轮)特征网格槽位下标的当前特征数上界检查 | 已落地 |
| `gb7_batch2.cpp` | Text Processing 按页降内存 | **已回退**, 保留说明注释 |
| `verify_gb7_textmem.py` / `gb7_textmem_check.cpp` | 字节不变性验证器(两条路径逐字节比较) | 新增, 供下一轮使用 |
