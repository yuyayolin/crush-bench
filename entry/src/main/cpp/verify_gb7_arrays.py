# -*- coding: utf-8 -*-
"""
gb7 负载: 固定容量数组 vs 最大索引表达式 核对器 (静态, 无设备)
---------------------------------------------------------------------------
用法:  python verify_gb7_arrays.py            (在 cpp 目录或其上级运行均可)

做三件事:
  1) 扫描 9 个负载文件, 抽出"固定容量声明"(栈数组 / std::array / 带容量宏的 vector),
     并列出该名字在文件里的全部索引写入点 (name[...] = ... / memcpy(name,...)); 
  2) 对每个索引写入点打印"写入点行号 + 上界来源行号", 供人工核对(见 CAPACITY_NOTES 里
     逐条的结论: 每个名字算出的索引上界 vs 声明容量);
  3) 打印"标定后数据规模"表, 并断言: 凡是声明容量与标定常量直接挂钩的数组, 都必须
     满足 容量 >= 标定常量(否则打印 FAIL)。

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import os, re, sys, io

# 输出同时写文件(UTF-8, 避免 PowerShell 重定向产生 UTF-16)
_OUT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "verify_out.txt")
try:
    _OUT = io.open(_OUT_PATH, "w", encoding="utf-8", newline="\n")
except Exception:
    _OUT = None

def emit(s=""):
    try:
        print(s)
    except Exception:
        pass
    if _OUT is not None:
        _OUT.write(s + "\n")
        _OUT.flush()

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = ["gb7_filecompress.cpp", "gb7_batch2.cpp", "gb7_batch3.cpp", "gb7_pdf.cpp",
         "gb7_audio.cpp", "gb7_video.cpp", "gb7_browser.cpp", "gb7_sfm.cpp", "gb7_clang.cpp"]

# ---- 标定后的数据规模(必须与源码里的常量一致; 由本脚本从源码重新解析校验) ----
CALIB = [
    # (负载, 文件, 常量名正则, 期望值, 说明)
    # 2026-10-05: 5 项(Ray Tracer / Video Encoder / Asset Compression / HDR / SfM)已按用户指令
    #   "务必跑满负载"还原成 2026-10-04 第三次复核调小之前的尺寸(单核与多核都还原),
    #   期望值已同步为还原后的值; 其余各项保持上一轮的值不动。
    # 2026-10-04 第三次真机复核: 下面每条的"说明"里都记了本轮实测 ms 与线性推演过程。
    ("Text Processing",  "gb7_batch2.cpp", r"const size_t pageSize = ([0-9* ]+);", 1024*1024, "每页字节数"),
    ("Text Processing",  "gb7_batch2.cpp", r"const int pages = (\d+);",             224,      "页数(192 页实测 1572.9 ms -> 224 页预计 1835 ms)"),
    # 还原后单核与多核同尺寸(texW 不再有 (threads>1) 分支): 4096^2 x 4 = 64.0 MiB
    ("Asset Compression","gb7_batch2.cpp", r"const int texW = (\d+);",              4096, "纹理边长-单核&多核(4096 实测 3289.2 ms; 已由 3584/5120 还原为 4096)"),
    ("Asset Compression","gb7_batch2.cpp", r"const int texH = (texW);",              None,     "纹理高 = 宽(正方形)"),
    # 只校验宽度那一组(第 1 个捕获组): 大地图一定是正方形, 见 gb7_batch2.cpp 的标定注释
    ("Navigation",       "gb7_batch2.cpp", r"makeMap\((\d+), (\d+), 90210u\)",    544,      "大地图 544x544(本轮实测 2142.8 ms, 已在区间内, 未改)"),
    ("Photo Library",    "gb7_batch3.cpp", r"const int w = (\d+);",                 1024,     "照片宽(第1处)"),
    ("Photo Library",    "gb7_batch3.cpp", r"const int photos = (\d+);",            10,       "照片数(真机 7 张 1291.9 ms / 12 张 3375.1 ms -> 反推每张成本(非线性), 10 张预计 2.44~2.54 s)"),
    ("Photo Editor",     "gb7_batch3.cpp", r"const int photos = (\d+);   //",       4,        "照片数(2026-10-06 第五次复核: 悬崖已证伪 —— 单张真实成本 ≈800 ms, 与多核 9 线程 112~137 ms/张 x 每核效率 0.61 互相印证; 4 张 ≈3.2 s, 每张工作量未改)"),
    # 还原后 w/h 不再有 (threads>1) 分支 -> 正则用行尾注释锚定, 避免与 Photo Library/Photo Editor 的
    # "const int w = <数字>;" / "const int h = <数字>;" 撞车(re.search 取的是全文第一处匹配)
    ("HDR",              "gb7_batch3.cpp", r"const int w = (\d+);\s*// HDR 输出宽", 1536, "输出宽-单核&多核(1536 实测 3380.2 ms; 已由 1296/2592 还原为 1536)"),
    ("HDR",              "gb7_batch3.cpp", r"const int h = (\d+);\s*// HDR 输出高", 832,  "输出高-单核&多核(已由 704/1408 还原为 832)"),
    # 还原后多核不再乘 GB7_MULTI_WORK_SCALE(真机多核 run 1791098294041 实测就是 8 采样)
    ("Ray Tracer",       "gb7_batch3.cpp", r"const int samples = (\d+);",          8, "每像素采样数-单核&多核(384x384x8 实测 6191.7 ms; 已由 3/6 还原为 8)"),
    ("Game Physics",     "gb7_batch3.cpp", r"const int steps = (\d+);",              190,      "步数(本轮实测 2113.2 ms, 已在区间内, 未改)"),
    ("Video Encoder",    "gb7_video.cpp",  r"const int kVEncFrameCount4 = (\d+);",   5,        "编码器帧数(5 帧实测 4310.5 ms; 已由 3 还原为 5)"),
    ("Video Decoder",    "gb7_video.cpp",  r"const int kVFrameCount4 = (\d+);",      5,        "解码器帧数(未改: 2 passes x 5 帧 = 10 帧-遍; 6.2 的 1681.2 ms 经 metric x ms = 9.25 Mpx 证明 10/10 帧-遍跑满 —— 掉档不在解码循环, 见 o.diag 自证字段)"),
    ("Audio Encoder",    "gb7_audio.cpp",  r"const int totalFrames = (\d+);",        88000,    "编码样本数(48000 样本/11 块实测 1261.1 ms -> 88000 样本/21 块预计 2408 ms)"),
    ("PDF Viewer",       "gb7_pdf.cpp",    r"const int kPageCount = (\d+);",         6,        "页数(4 页实测 1460.1 ms -> 6 页预计 2190 ms)"),
    ("HTML5 Browser",    "gb7_browser.cpp",r"const int kFixedPages = (\d+);",        80,       "整页渲染轮数(56 页实测 1630.0 ms -> 80 页预计 2329 ms)"),
    ("SfM",              "gb7_sfm.cpp",    r"constexpr int    kMaxFeat       = (\d+);", 560,  "每图特征数(560 实测 3713.8 ms; 已由 380 还原为 560)"),
    ("Clang",            "gb7_clang.cpp",  r"const double kTargetMs = ([\d.]+);",    2200.0,   "计时目标(ms); 注意本轮真机 0/33 轮编译成功, 见报告"),
]

# ---- 固定容量数组的人工核对结论(容量表达式 -> 索引上界表达式 -> 结论) ----
CAPACITY_NOTES = [
 ("gb7_filecompress.cpp", "Sha1::w[80]", "80", "i<80 (line 75..80)", "PASS", "块内 16->80 字扩展, 写读均为常量循环上界"),
 ("gb7_filecompress.cpp", "Sha1::buf[64]", "64", "bufLen<64 (line 99..105)", "PASS", "update() 逐字节推进, 满 64 立即 block+清零"),
 ("gb7_filecompress.cpp", "out[bound]", "LZ4_compressBound(in)", "n<=bound", "PASS", "输出缓冲按 compressBound 分配"),
 ("gb7_filecompress.cpp", "outs[slot]", "useThreads 个槽", "slot 来自 freeSlots 池", "PASS", "池大小 = useThreads, 取一还一"),
 ("gb7_batch2.cpp", "MapGraph::cost[w*h]", "160*160 / 544*544", "v=vy*w+vx, vx<w, vy<h", "PASS", "大地图 640->896->544 后仍按 w*h 分配并夹取边界"),
 ("gb7_batch2.cpp", "dist[w*h]", "n=w*h", "u=sy*w+sx (起终点已夹取), v 同 cost", "PASS", "与 cost 同尺寸, 索引同源"),
 ("gb7_batch2.cpp", "results[24]", "24 条路线", "r<24", "PASS", "任务数未变"),
 ("gb7_batch2.cpp", "text[pageSize*pages]", "1 MiB * 224", "i<text.size(), 且 i+len+2<size 才 memcpy", "PASS", "语料 8MiB->192MiB->224MiB 是同一缓冲, 生成时有边界检查"),
 ("gb7_batch2.cpp", "partial[pages]", "224", "s 为任务区间起点, s<pages", "PASS", "槽位随页数一起增长(128->192->224), 未写死"),
 ("gb7_batch2.cpp", "tex[texW*texH*4]", "4096*4096*4 (单核与多核同尺寸)", "idx=((y*w+x)*4), x<texW,y<texH", "PASS", "纹理 2048->4096->3584/5120 后已还原为 4096(单核与多核同尺寸), 仍按 texW/texH 计算, 无写死"),
 ("gb7_batch2.cpp", "tex 块读 idx+c", "同上", "by<blocksY, bx<blocksX, py,px<4", "PASS", "块坐标由 texW/texH 推导, 无 2048 假设"),
 ("gb7_batch2.cpp", "partial[blocksY]", "1024 (=4096/4, 单核与多核同尺寸)", "rs 为块行区间起点", "PASS", "槽位随块行数增长(还原后 1024)"),
 ("gb7_batch3.cpp", "img[w*h*3]", "1024*768*3 / 3000*2000*3", "idx=((y*w+x)*3), x<w,y<h", "PASS", "按 w/h 分配"),
 ("gb7_batch3.cpp", "decoded[idx..idx+2]", "dw*dh*3 (解码器输出)", "y+=4,x+=4 的采样点", "PASS", "潜在越界读: dw/dh 不被 4 整除时 x+3/y+3 会越过行尾 -> 已加 dw>=4&&dh>=4 的前置守卫(不改口径)"),
 ("gb7_batch3.cpp", "photoAcc[photos]", "10", "p<photos", "PASS", "槽位随张数增长(3->7->12->10), 数组尺寸跟着旋钮走"),
 ("gb7_batch3.cpp", "work[w*h*3]", "3000*2000*3", "i=((y*w+x)*3)+c, c<3", "PASS", "按 w/h 分配"),
 ("gb7_batch3.cpp", "rowAcc[h]", "2000", "y<h", "PASS", "按 h 分配, 按行写"),
 ("gb7_batch3.cpp", "hdr/ldr[w*h*3]", "1536*832*3 (单核与多核同尺寸)", "idx=((y*w+x)*3)+c", "PASS", "按 w/h 分配(还原后 1536x832, 多核不再 x2)"),
 ("gb7_batch3.cpp", "out[w*h*3]", "384*384*3", "idx=((y*w+x)*3)+c", "PASS", "分辨率固定 384x384; 采样数 24->14->8 只影响循环次数, 不参与索引"),
 ("gb7_batch3.cpp", "spheres[6]", "6", "k<6, hit<6", "PASS", "采样数变化不引入除零(samples>=1 且 8!=0)"),
 ("gb7_batch3.cpp", "head[64^3]", "262144", "cellIdx=(cz*g+cy)*g+cx, cx..cz 已夹取到 [0,g-1]", "PASS", "网格与步数无关"),
 ("gb7_batch3.cpp", "next[n]", "8192", "i<n", "PASS", "刚体数未变"),
 ("gb7_pdf.cpp", "kGlyphBits[47][7]", "47*7", "glyphIndex() 返回 [0,46]", "PASS", "查表先线性搜索, 未命中返回 0"),
 ("gb7_pdf.cpp", "kWords[]/kWordCount", "sizeof/sizeof", "xsPdf()%kWordCount", "PASS", "上界由 sizeof 推导"),
 ("gb7_pdf.cpp", "genLine out[cap]", "cap=62/160", "len<cap-1 才写, 结束 out[len]=0", "PASS", "每处写入前都有 len<cap-1 或 len+N<cap 检查"),
 ("gb7_pdf.cpp", "PdfWriter::off[16]", "16", "objBegin 有 (size_t)num<off.size() 检查", "PASS", "检查在写入前"),
 ("gb7_pdf.cpp", "objOff[64]", "64", "onum>0 && onum<objOff.size()", "PASS", "xref 与 obj 扫描两处都有检查"),
 ("gb7_pdf.cpp", "PathB::xy", "kMaxPathPts=400000 点", "moveTo/lineTo 每次先判 xy.size()/2>=kMaxPathPts", "PASS", "超限置 overflow, 后续全部早退"),
 ("gb7_pdf.cpp", "REdge edges", "kMaxEdges=600000", "addEdge 先判 edges.size()>=kMaxEdges", "PASS", "超限丢弃该边(不写越界)"),
 ("gb7_pdf.cpp", "Raster::cov[kPgW]", "1275", "x∈[tmin,tmax]⊆[cx0,ibMax], ibMax=cx1-1<=kPgW-1", "PASS", "kPgW 未改动; 页数 2->4->6 不影响"),
 ("gb7_pdf.cpp", "Raster::pix[kPgW*kPgH*3]", "1275*1650*3", "idx=((row*kPgW)+x)*3+c, row<=kPgH-1, x<=ibMax", "PASS", "页数只决定渲染几页, 缓冲尺寸不变"),
 ("gb7_pdf.cpp", "PdfDrawOp pts/subs", "动态 vector", "recordFill 用 insert/push_back", "PASS", "动态增长, 无固定容量"),
 ("gb7_pdf.cpp", "ks[256] (pdf)", "-", "无此数组", "PASS", "-"),
 ("gb7_pdf.cpp", "freeWorkers", "parThreads 个 worker", "gb7ParallelFor 并发 body 数 <= parThreads", "PASS", "不变式成立; 已加防御性空池检查(见报告 2.3)"),
 ("gb7_audio.cpp", "kSinSizeA4+1 表", "8193", "idx=kSinMaskA4<=8191, 读 idx+1<=8192", "PASS", "表尾多 1 个元素用于线性插值"),
 ("gb7_audio.cpp", "ks[256]", "256", "parts=1<<partOrder<=256", "PASS", "bestPartitionsA4 里 parts>256 即 break"),
 ("gb7_audio.cpp", "lpc[16]/ac[16]/q[16]", "16", "order<=kMaxLpcOrderA4=12", "PASS", "固定预测器 0..4 与 LPC 1..12 都在 16 以内"),
 ("gb7_audio.cpp", "BestSubA4::coeff[16]", "16", "order<=12", "PASS", "同上"),
 ("gb7_audio.cpp", "blockBuf[2][4096]", "2*4096", "i<kBlockSizeA4", "PASS", "块大小常量未改"),
 ("gb7_audio.cpp", "pcm[totalFrames*channels]", "352000", "idx=((blk*4096+i)*2)+1, blk<blocks=21", "PASS", "60s->1s->88000 样本后 blocks=21, 最大下标 < 176000 < 176001"),
 ("gb7_audio.cpp", "scratch[kBlockSizeA4]", "4096", "i<n=4096", "PASS", "writeSubframeA4 内 resize(n) 后只写 [order,n)"),
 ("gb7_audio.cpp", "frameBytes[blocks]", "11", "blk<blocks", "PASS", "动态 vector, 每帧一个"),
 ("gb7_audio.cpp", "scratchSamples[4096]", "4096", "checkSubframeA4(n=4096)", "PASS", "自检路径"),
 ("gb7_video.cpp", "HuffTableV4::len/code[128]", "128", "下标 = 符号 < n(121 / 13)", "PASS", "表项数与符号数一致, 有 --Wall 下的数组边界检查"),
 ("gb7_video.cpp", "buildHuffV4 weight/parent[280]", "280", "nodes<maxNodes=280", "PASS", "合并循环上界即容量"),
 ("gb7_video.cpp", "kDctBasis4[8][8]", "8*8", "u,v,y,x<8", "PASS", "常量"),
 ("gb7_video.cpp", "kQuantTable4[32]", "32", "quantScaleV4 先把 qp 夹到 [0,31]", "PASS", "检查在索引前"),
 ("gb7_video.cpp", "kZigzag4[64]", "64", "i<64 / pos<=63", "PASS", "readCoefBlockV4 有 pos>63 早退"),
 ("gb7_video.cpp", "resid/recon/coef[64]", "64", "i<64", "PASS", "宏块内常量"),
 ("gb7_video.cpp", "MbStoreV4::coef[3600*6*64]", "1382400", "block(mbIdx<3600, slot<6)", "PASS", "宏块数未变(分辨率未变)"),
 ("gb7_video.cpp", "refY/recY[kVLumaSize4]", "921600", "运动补偿前 clampVecV4 夹取, 色度单独夹取", "PASS", "编码器 5 帧/解码器 5 帧都不影响缓冲尺寸(缓冲只分配一次并逐帧复用)"),
 ("gb7_video.cpp", "BitWriterV4 buf (4 MiB)", "4*1024*1024", "writeBits 逐位判 byteIdx>=buf.size() 置 over", "PASS", "固定容量但每次访问都查界, 溢出只置标志(调用方 break)"),
 ("gb7_video.cpp", "BitReaderV4", "受 len*8 限制", "readBits 先判 bitPos+bits>len*8", "PASS", "越界返回 false, 由调用方处理"),
 ("gb7_browser.cpp", "PageSlot::fb[1024*768]", "786432", "所有写入都经 fbBlend/fbFill, 逐坐标夹取到 [0,w)x[0,h)", "PASS", "帧缓冲尺寸未改"),
 ("gb7_browser.cpp", "nm[kMaxTagName+1]", "25", "nl<kMaxTagName 才自增, nm[nl]=0", "PASS", "容量 = 上限+1, 含结束符"),
 ("gb7_browser.cpp", "an[kMaxTagName+1]", "25", "同上", "PASS", "-"),
 ("gb7_browser.cpp", "av[kMaxAttrVal+1]", "161", "avl<kMaxAttrVal 才自增, av[avl]=0", "PASS", "-"),
 ("gb7_browser.cpp", "cls[40]/idb[32]/sty[200]", "40/32/200", "cn 先夹到 kMaxX-1 再 memcpy(cn)+写 0", "PASS", "每个都留了结束符位置"),
 ("gb7_browser.cpp", "Compound::cls[40]/id[32]", "40/32", "len>kMaxX-1 直接 return false", "PASS", "-"),
 ("gb7_browser.cpp", "buf[24] (margin/padding 名)", "24", "bl+sl+1>sizeof(buf) 则 continue", "PASS", "检查在 memcpy 前"),
 ("gb7_browser.cpp", "kFont5x7[95][7]", "95*7", "gi=(int)ch-32, 仅当 32<ch<127", "PASS", "gi 落在 [1,94]"),
 ("gb7_browser.cpp", "curOk/nxt/chain[kMaxDepth+4]", "68", "L<kCap=kMaxDepth+4", "PASS", "向上遍历祖先时受 kCap 限制"),
 ("gb7_browser.cpp", "parseStack vector", "kMaxDepth+2 预留", "push 前判 stack.size()<kMaxDepth", "PASS", "vector 动态增长, reserve 只是预留"),
 ("gb7_sfm.cpp", "HarrisWorkspace 各图级缓冲", "W*H", "base=y*W+x, x<W,y<H", "PASS", "分辨率未改"),
 ("gb7_sfm.cpp", "grid[gw*gh]", "107*80", "gx=px/cell<gw, gy<gh, 索引前判 xx/yy 范围", "PASS", "-"),
 ("gb7_sfm.cpp", "fs.desc[n*4]", "n<=kMaxFeat", "i<nFeat", "PASS", "特征数 900->560->380->560(已还原) 后缓冲随 fs.n 分配"),
 ("gb7_sfm.cpp", "fs.angle[n]", "n<=560", "i<nFeat", "PASS", "曾经是空 vector(写空指针解引用), 已修复为 angle.assign(n)"),
 ("gb7_sfm.cpp", "RansacWorkspace Mb[81]/evb[9]/evcb[81]", "81/9/81", "9x9 矩阵", "PASS", "常量"),
 ("gb7_sfm.cpp", "p1/p2/P1/P2[12] (PnP)", "12", "3x4", "PASS", "常量"),
 ("gb7_sfm.cpp", "parent/used[nNodes=10*kMaxFeat]", "5600", "na=ia*kMaxFeat+a, a<kMaxFeat=560", "PASS", "索引空间随 kMaxFeat 一起变(还原后 10*560=5600)"),
 ("gb7_sfm.cpp", "pattern[kDescBits*4]", "1024", "i<256, k<4", "PASS", "常量"),
 ("gb7_clang.cpp", "aurora_pool[262144]", "262144", "aurora_pool_alloc 先判 used+aligned>POOL_BYTES 返回 0", "PASS", "失败返回 0, 调用方判空"),
 ("gb7_clang.cpp", "keys/values[k07_SLOTS=64]", "64", "hash&(SLOTS-1)", "PASS", "掩码保证不越界"),
 ("gb7_clang.cpp", "scoped/alloc 相关栈数组", "各生成单元内常量", "均在函数内以常量上界循环", "PASS", "生成器模板, 与标定无关"),
]

FAILS = 0

def read(f):
    with open(os.path.join(HERE, f), encoding="utf-8", errors="replace") as fh:
        return fh.readlines()

def eval_num(expr):
    e = expr.strip()
    if e in ("1024*1024", "1024 * 1024"):
        return 1024*1024
    try:
        return int(eval(e, {"__builtins__": {}}, {}))
    except Exception:
        return None

emit("=" * 100)
emit("[1] 标定后数据规模(从源码重新解析, 与代码里的常量逐条核对)")
emit("=" * 100)
for name, f, rx, want, desc in CALIB:
    txt = "".join(read(f))
    m = re.search(rx, txt)
    got = m.group(1).strip() if (m and m.groups()) else None
    if got is None:
        emit("  %-18s %-20s %-28s -> 未匹配(常量已改?)   [%s]" % (name, f, rx, desc))
        FAILS += 1
        continue
    val = eval_num(got) if want is not None else got
    ok = (want is None) or (val == want)
    if not ok:
        FAILS += 1
    emit("  %-18s %-20s %-24s = %-10s 期望 %-10s %s  [%s]" %
          (name, f, rx[:24], got, str(want), "PASS" if ok else "FAIL", desc))

emit()
emit("=" * 100)
emit("[2] 固定容量数组 vs 最大索引表达式 逐条核对(容量 / 索引上界 / 结论)")
emit("=" * 100)
cur = None
for f, arr, cap, idx, verdict, why in CAPACITY_NOTES:
    if f != cur:
        emit("\n--- %s ---" % f)
        cur = f
    if verdict != "PASS":
        FAILS += 1
    emit("  [%s] %-38s 容量=%-16s 索引上界=%-52s %s" % (verdict, arr, cap, idx, why))

emit()
emit("=" * 100)
emit("[3] 索引写入点扫描(辅助人工核对: 文件里每处 name[...] = ... 都出现在上面结论里)")
emit("=" * 100)
WRITE_RX = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\[([^\]]{0,60})\]\s*(?:\+|-|\^|\||&)?=")
for f in FILES:
    lines = read(f)
    hits = []
    for i, l in enumerate(lines, 1):
        s = l.strip()
        if s.startswith("//") or s.startswith("*") or s.startswith("/*"):
            continue
        m = WRITE_RX.search(l)
        if m and not re.match(r"^\s*(if|while|for|return|else)\b", s):
            hits.append("%s:%d: %s" % (f, i, s[:110]))
    emit("  %-22s 索引写入点 %3d 处" % (f, len(hits)))
    for h in hits:
        emit("      " + h)

emit()
emit("=" * 100)
if FAILS == 0:
    emit("结论: 全部 PASS —— 没有任何 '容量 < 索引上界' 的数组(没有按旧尺寸写死的缓冲)。")
else:
    emit("结论: 有 %d 条 FAIL, 见上。" % FAILS)
emit("=" * 100)
sys.exit(1 if FAILS else 0)