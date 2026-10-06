// CS1 第五批: HTML5 Browser
// 自研小型浏览器渲染管线: HTML 解析 -> CSS 层叠 -> 盒模型/行内排版 -> 帧缓冲绘制。
// 单线程; 无 SIMD/intrinsics/内联汇编; 不依赖浏览器引擎、字体文件或文件系统;
// 所有页面(HTML/CSS 文本)均在内存中生成, 每个 pass 都真实重新解析/重新布局/重新绘制。
#include "gb7.h"
#include "gb7_parallel.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

double nowMsBr()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// ---------------- 视口与各项硬上限(全部用于防越界) ----------------
const int kFbW = 1024;
const int kFbH = 768;
const uint32_t kNoNode = 0xFFFFFFFFu;

const size_t kMaxNodes = 8192;        // DOM 节点上限
const size_t kMaxArena = 1u << 20;    // 文本/类名/id/内联样式字符池: 1MB
const uint32_t kMaxTextNode = 1024;   // 单个文本节点字符上限(超出截断)
const int kMaxDepth = 64;             // 解析/样式/布局递归深度上限
const size_t kMaxRules = 256;         // CSS 规则上限
const int kMaxCompounds = 3;          // 单条选择器复合项(后代组合子)上限
const size_t kMaxBoxes = 65536;       // 显示列表(绘制指令)上限
const int kMaxTagName = 24;           // 标签名缓冲
const int kMaxAttrVal = 160;          // 属性值缓冲
const int kMaxCls = 40;               // class 属性缓冲
const int kMaxId = 32;                // id 属性缓冲
const int kMaxInlineStyle = 200;      // style 属性缓冲
const int kMaxTableRows = 256;        // 单表行数上限
const int kMaxTableCols = 24;         // 单表列数上限

// 长度模式
enum { LEN_AUTO = 0, LEN_PX = 1, LEN_PCT = 2 };
// display
enum { DISP_BLOCK = 0, DISP_INLINE = 1, DISP_NONE = 2 };
// text-align
enum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };
// 显示列表元素类型
enum { BOX_FILL = 0, BOX_TEXT = 1 };

// ---------------- 字符分类/大小写(仅 ASCII) ----------------
inline char lowerCh(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
inline bool isSpaceCh(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
inline bool isNameCh(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}

// ---------------- 内置 5x7 点阵字模(ASCII 32..126, 每字形 7 行, 每行低 5 位为像素) ----------------
static const uint8_t kFont5x7[][7] = {
    {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000}, // 32 space
    {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100}, // 33 !
    {0b01010, 0b01010, 0b01010, 0b00000, 0b00000, 0b00000, 0b00000}, // 34 quote
    {0b01010, 0b01010, 0b11111, 0b01010, 0b11111, 0b01010, 0b01010}, // 35 hash
    {0b00100, 0b01111, 0b10100, 0b01110, 0b00101, 0b11110, 0b00100}, // 36 dollar
    {0b11000, 0b11001, 0b00010, 0b00100, 0b01000, 0b10011, 0b00011}, // 37 percent
    {0b01100, 0b10010, 0b10100, 0b01000, 0b10101, 0b10010, 0b01101}, // 38 amp
    {0b00100, 0b00100, 0b01000, 0b00000, 0b00000, 0b00000, 0b00000}, // 39 apos
    {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010}, // 40 lparen
    {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000}, // 41 rparen
    {0b00000, 0b00100, 0b10101, 0b01110, 0b10101, 0b00100, 0b00000}, // 42 star
    {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000}, // 43 plus
    {0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00100, 0b01000}, // 44 comma
    {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}, // 45 minus
    {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}, // 46 period
    {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000}, // 47 slash
    {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}, // 48 0
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, // 49 1
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}, // 50 2
    {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}, // 51 3
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}, // 52 4
    {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}, // 53 5
    {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}, // 54 6
    {0b11111, 0b10001, 0b00001, 0b00010, 0b00100, 0b00100, 0b00100}, // 55 7
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}, // 56 8
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}, // 57 9
    {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000}, // 58 colon
    {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b00100, 0b01000}, // 59 semicolon
    {0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010}, // 60 lt
    {0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000}, // 61 eq
    {0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000}, // 62 gt
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100}, // 63 question
    {0b01110, 0b10001, 0b00001, 0b01101, 0b10101, 0b10101, 0b01110}, // 64 at
    {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}, // 65 A
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}, // 66 B
    {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}, // 67 C
    {0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100}, // 68 D
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}, // 69 E
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}, // 70 F
    {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}, // 71 G
    {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}, // 72 H
    {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, // 73 I
    {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}, // 74 J
    {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}, // 75 K
    {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}, // 76 L
    {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}, // 77 M
    {0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001}, // 78 N
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}, // 79 O
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}, // 80 P
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}, // 81 Q
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}, // 82 R
    {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}, // 83 S
    {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}, // 84 T
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}, // 85 U
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}, // 86 V
    {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001}, // 87 W
    {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}, // 88 X
    {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}, // 89 Y
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}, // 90 Z
    {0b01110, 0b01000, 0b01000, 0b01000, 0b01000, 0b01000, 0b01110}, // 91 lbracket
    {0b10000, 0b01000, 0b01000, 0b00100, 0b00010, 0b00010, 0b00001}, // 92 backslash
    {0b01110, 0b00010, 0b00010, 0b00010, 0b00010, 0b00010, 0b01110}, // 93 rbracket
    {0b00100, 0b01010, 0b10001, 0b00000, 0b00000, 0b00000, 0b00000}, // 94 caret
    {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111}, // 95 underscore
    {0b01000, 0b00100, 0b00010, 0b00000, 0b00000, 0b00000, 0b00000}, // 96 grave
    {0b00000, 0b00000, 0b01110, 0b00001, 0b01111, 0b10001, 0b01111}, // 97 a
    {0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b10001, 0b11110}, // 98 b
    {0b00000, 0b00000, 0b01111, 0b10000, 0b10000, 0b10000, 0b01111}, // 99 c
    {0b00001, 0b00001, 0b01111, 0b10001, 0b10001, 0b10001, 0b01111}, // 100 d
    {0b00000, 0b00000, 0b01110, 0b10001, 0b11111, 0b10000, 0b01110}, // 101 e
    {0b00110, 0b01001, 0b01000, 0b11100, 0b01000, 0b01000, 0b01000}, // 102 f
    {0b00000, 0b01111, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110}, // 103 g
    {0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b10001, 0b10001}, // 104 h
    {0b00100, 0b00000, 0b01100, 0b00100, 0b00100, 0b00100, 0b01110}, // 105 i
    {0b00010, 0b00000, 0b00110, 0b00010, 0b00010, 0b10010, 0b01100}, // 106 j
    {0b10000, 0b10000, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010}, // 107 k
    {0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, // 108 l
    {0b00000, 0b00000, 0b11010, 0b10101, 0b10101, 0b10101, 0b10101}, // 109 m
    {0b00000, 0b00000, 0b11110, 0b10001, 0b10001, 0b10001, 0b10001}, // 110 n
    {0b00000, 0b00000, 0b01110, 0b10001, 0b10001, 0b10001, 0b01110}, // 111 o
    {0b00000, 0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000}, // 112 p
    {0b00000, 0b01111, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001}, // 113 q
    {0b00000, 0b00000, 0b10110, 0b11001, 0b10000, 0b10000, 0b10000}, // 114 r
    {0b00000, 0b00000, 0b01111, 0b10000, 0b01110, 0b00001, 0b11110}, // 115 s
    {0b01000, 0b01000, 0b11100, 0b01000, 0b01000, 0b01001, 0b00110}, // 116 t
    {0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b10011, 0b01101}, // 117 u
    {0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}, // 118 v
    {0b00000, 0b00000, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010}, // 119 w
    {0b00000, 0b00000, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001}, // 120 x
    {0b00000, 0b10001, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110}, // 121 y
    {0b00000, 0b00000, 0b11111, 0b00010, 0b00100, 0b01000, 0b11111}, // 122 z
    {0b00010, 0b00100, 0b00100, 0b01000, 0b00100, 0b00100, 0b00010}, // 123 lbrace
    {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}, // 124 bar
    {0b01000, 0b00100, 0b00100, 0b00010, 0b00100, 0b00100, 0b01000}, // 125 rbrace
    {0b00000, 0b00000, 0b01000, 0b10101, 0b00010, 0b00000, 0b00000}, // 126 tilde
};
static_assert(sizeof(kFont5x7) / sizeof(kFont5x7[0]) == 95, "5x7 font must cover ASCII 32..126");

// 字符前进宽度表(单位: 1/1000 em), 按字形宽度比例手工构造, 不引入任何字体文件
static const uint16_t kCharAdv[95] = {
    300, 300, 420, 640, 640, 820, 720, 260, 360, 360,   // 32..41
    500, 640, 300, 400, 300, 400,                        // 42..47
    620, 620, 620, 620, 620, 620, 620, 620, 620, 620,    // 48..57  数字
    300, 300, 640, 640, 640, 560, 920,                   // 58..64
    700, 680, 700, 720, 640, 600, 740, 740, 320, 500,    // 65..74  A..J
    680, 580, 880, 740, 760, 660, 760, 680, 640, 620,    // 75..84  K..T
    720, 700, 940, 660, 620, 620,                        // 85..90  U..Z
    360, 400, 360, 640, 600, 400,                        // 91..96
    560, 600, 540, 600, 560, 340, 600, 600, 260, 280,    // 97..106 a..j
    560, 260, 880, 600, 580, 600, 600, 400, 520, 380,    // 107..116 k..t
    600, 540, 800, 540, 560, 500,                        // 117..122 u..z
    400, 300, 400, 640                                  // 123..126
};
static_assert(sizeof(kCharAdv) / sizeof(kCharAdv[0]) == 95, "char width table size");

inline int charAdv1000(unsigned char c)
{
    if (c >= 32 && c < 127) {
        return (int)kCharAdv[c - 32];
    }
    return 500;
}

inline int advForChar(unsigned char c, float fontSize)
{
    int a = (int)((float)charAdv1000(c) * fontSize * 0.001f + 0.5f);
    if (a < 1) {
        a = 1;
    }
    return a;
}

int textAdvance(const char* s, uint32_t n, float fontSize)
{
    int total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        total += advForChar((unsigned char)s[i], fontSize);
        if (total > 1 << 20) { // 病态长词防御
            break;
        }
    }
    return total;
}

// ---------------- 基础几何/颜色 ----------------
struct Rect {
    int x0, y0, x1, y1;
    Rect() : x0(0), y0(0), x1(0), y1(0) {}
    Rect(int a, int b, int c, int d) : x0(a), y0(b), x1(c), y1(d) {}
};

inline bool rectValid(const Rect& r) { return r.x1 > r.x0 && r.y1 > r.y0; }

inline Rect rectIntersect(const Rect& a, const Rect& b)
{
    Rect r;
    r.x0 = a.x0 > b.x0 ? a.x0 : b.x0;
    r.y0 = a.y0 > b.y0 ? a.y0 : b.y0;
    r.x1 = a.x1 < b.x1 ? a.x1 : b.x1;
    r.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
    if (r.x1 < r.x0) { r.x1 = r.x0; }
    if (r.y1 < r.y0) { r.y1 = r.y0; }
    return r;
}

inline uint32_t colAlpha(uint32_t c) { return (c >> 24) & 0xFFu; }

struct Fb {
    uint32_t* px;
    int w, h;
};

inline void fbBlend(Fb& fb, int x, int y, uint32_t c)
{
    uint32_t a = colAlpha(c);
    if (a == 0u) {
        return;
    }
    if (x < 0 || y < 0 || x >= fb.w || y >= fb.h) {
        return;
    }
    uint32_t* p = fb.px + (size_t)y * (size_t)fb.w + (size_t)x;
    if (a == 255u) {
        *p = 0xFF000000u | (c & 0x00FFFFFFu);
        return;
    }
    uint32_t d = *p;
    uint32_t ia = 255u - a;
    uint32_t r = ((((c >> 16) & 0xFFu) * a) + (((d >> 16) & 0xFFu) * ia) + 127u) / 255u;
    uint32_t g = ((((c >> 8) & 0xFFu) * a) + (((d >> 8) & 0xFFu) * ia) + 127u) / 255u;
    uint32_t b = (((c & 0xFFu) * a) + ((d & 0xFFu) * ia) + 127u) / 255u;
    *p = 0xFF000000u | (r << 16) | (g << 8) | b;
}

// 带裁剪的矩形填充(纯 alpha 混合); 所有坐标在内部 clamp 到帧缓冲与裁剪区
void fbFill(Fb& fb, const Rect& clip, int x, int y, int w, int h, uint32_t c)
{
    if (w <= 0 || h <= 0 || colAlpha(c) == 0u) {
        return;
    }
    long long ax0 = x;
    long long ay0 = y;
    long long ax1 = (long long)x + (long long)w;
    long long ay1 = (long long)y + (long long)h;
    if (ax0 < clip.x0) { ax0 = clip.x0; }
    if (ay0 < clip.y0) { ay0 = clip.y0; }
    if (ax1 > clip.x1) { ax1 = clip.x1; }
    if (ay1 > clip.y1) { ay1 = clip.y1; }
    if (ax0 < 0) { ax0 = 0; }
    if (ay0 < 0) { ay0 = 0; }
    if (ax1 > fb.w) { ax1 = fb.w; }
    if (ay1 > fb.h) { ay1 = fb.h; }
    if (ax1 <= ax0 || ay1 <= ay0) {
        return;
    }
    int xx0 = (int)ax0;
    int yy0 = (int)ay0;
    int xx1 = (int)ax1;
    int yy1 = (int)ay1;
    if (colAlpha(c) == 255u) {
        uint32_t col = 0xFF000000u | (c & 0x00FFFFFFu);
        for (int yy = yy0; yy < yy1; ++yy) {
            uint32_t* row = fb.px + (size_t)yy * (size_t)fb.w;
            for (int xx = xx0; xx < xx1; ++xx) {
                row[xx] = col;
            }
        }
    } else {
        for (int yy = yy0; yy < yy1; ++yy) {
            for (int xx = xx0; xx < xx1; ++xx) {
                fbBlend(fb, xx, yy, c);
            }
        }
    }
}

// ---------------- 标签表 ----------------
enum TagId {
    TAG_UNKNOWN = 0,
    TAG_HTML, TAG_HEAD, TAG_BODY, TAG_DIV, TAG_P, TAG_SPAN, TAG_A,
    TAG_H1, TAG_H2, TAG_H3, TAG_H4, TAG_H5, TAG_H6,
    TAG_UL, TAG_OL, TAG_LI,
    TAG_TABLE, TAG_THEAD, TAG_TBODY, TAG_TR, TAG_TD, TAG_TH, TAG_CAPTION,
    TAG_BR, TAG_HR,
    TAG_B, TAG_I, TAG_U, TAG_STRONG, TAG_EM, TAG_SMALL, TAG_BIG,
    TAG_SECTION, TAG_HEADER, TAG_FOOTER, TAG_NAV, TAG_ARTICLE, TAG_ASIDE, TAG_MAIN,
    TAG_PRE, TAG_CODE, TAG_BLOCKQUOTE,
    TAG_STYLE, TAG_SCRIPT, TAG_TITLE, TAG_META, TAG_LINK,
    TAG_FORM, TAG_INPUT, TAG_BUTTON, TAG_LABEL, TAG_IMG,
    TAG_COUNT
};

static const char* const kTagNames[TAG_COUNT] = {
    "unknown", "html", "head", "body", "div", "p", "span", "a",
    "h1", "h2", "h3", "h4", "h5", "h6",
    "ul", "ol", "li",
    "table", "thead", "tbody", "tr", "td", "th", "caption",
    "br", "hr",
    "b", "i", "u", "strong", "em", "small", "big",
    "section", "header", "footer", "nav", "article", "aside", "main",
    "pre", "code", "blockquote",
    "style", "script", "title", "meta", "link",
    "form", "input", "button", "label", "img"
};
static_assert(sizeof(kTagNames) / sizeof(kTagNames[0]) == (size_t)TAG_COUNT, "tag name table size");

uint8_t tagFromName(const char* name, int len)
{
    if (len <= 0 || len > kMaxTagName) {
        return TAG_UNKNOWN;
    }
    for (int t = 1; t < TAG_COUNT; ++t) {
        const char* n = kTagNames[t];
        int i = 0;
        while (i < len && n[i] != 0 && n[i] == name[i]) {
            ++i;
        }
        if (i == len && n[i] == 0) {
            return (uint8_t)t;
        }
    }
    return TAG_UNKNOWN;
}

inline bool isVoidTag(uint8_t t)
{
    return t == TAG_BR || t == TAG_HR || t == TAG_IMG || t == TAG_INPUT || t == TAG_META || t == TAG_LINK;
}

inline bool isRawTextTag(uint8_t t) { return t == TAG_STYLE || t == TAG_SCRIPT; }

// ---------------- DOM ----------------
struct DomNode {
    uint8_t isText;
    uint8_t tag;
    uint16_t pad0;
    uint32_t parent;
    uint32_t firstChild;
    uint32_t lastChild;
    uint32_t nextSibling;
    uint32_t textOff;
    uint32_t textLen;
    uint32_t styleOff;   // 内联 style 属性
    uint32_t styleLen;
    uint32_t clsOff;     // class 属性
    uint32_t clsLen;
    uint32_t idOff;      // id 属性
    uint32_t idLen;
};

struct Dom {
    std::vector<DomNode> nodes;
    std::vector<char> arena;
    uint32_t root;
    uint32_t layoutRoot;
    uint32_t bodyIdx;
};

// 向字符池追加字符串(截断保护), 返回起始偏移, storedLen 回传实际写入长度
uint32_t arenaAdd(Dom& dom, const char* s, uint32_t n, uint32_t& storedLen)
{
    uint32_t off = (uint32_t)dom.arena.size();
    size_t room = (dom.arena.size() < kMaxArena) ? (kMaxArena - dom.arena.size()) : 0;
    if (room <= 1) {
        storedLen = 0;
        return off;
    }
    size_t take = (size_t)n;
    if (take > room - 1) {
        take = room - 1;
    }
    if (take > 0) {
        dom.arena.insert(dom.arena.end(), s, s + take);
    }
    dom.arena.push_back((char)0);
    storedLen = (uint32_t)take;
    return off;
}

uint32_t addNode(Dom& dom, uint8_t isText, uint8_t tag)
{
    if (dom.nodes.size() >= kMaxNodes) {
        return kNoNode;
    }
    DomNode nd;
    memset(&nd, 0, sizeof(nd));
    nd.isText = isText;
    nd.tag = tag;
    nd.parent = kNoNode;
    nd.firstChild = kNoNode;
    nd.lastChild = kNoNode;
    nd.nextSibling = kNoNode;
    dom.nodes.push_back(nd);
    return (uint32_t)(dom.nodes.size() - 1);
}

void appendChild(Dom& dom, uint32_t parent, uint32_t child)
{
    if (parent == kNoNode || child == kNoNode || parent >= dom.nodes.size() || child >= dom.nodes.size()) {
        return;
    }
    dom.nodes[child].parent = parent;
    if (dom.nodes[parent].lastChild == kNoNode) {
        dom.nodes[parent].firstChild = child;
    } else {
        dom.nodes[dom.nodes[parent].lastChild].nextSibling = child;
    }
    dom.nodes[parent].lastChild = child;
}

// ---------------- HTML 实体解码(仅常用命名实体 + 数字实体) ----------------
inline bool nameEq(const char* p, size_t n, const char* lit)
{
    size_t m = strlen(lit);
    return n == m && memcmp(p, lit, m) == 0;
}

bool decodeEntityAt(const char* s, size_t avail, char& out, size_t& consumed)
{
    if (avail < 3) {
        return false;
    }
    if (s[1] == '#') {
        size_t i = 2;
        uint32_t v = 0;
        int digits = 0;
        if (i < avail && (s[i] == 'x' || s[i] == 'X')) {
            ++i;
            while (i < avail && digits < 6) {
                char c = s[i];
                uint32_t d = 0;
                if (c >= '0' && c <= '9') { d = (uint32_t)(c - '0'); }
                else if (c >= 'a' && c <= 'f') { d = (uint32_t)(c - 'a' + 10); }
                else if (c >= 'A' && c <= 'F') { d = (uint32_t)(c - 'A' + 10); }
                else { break; }
                v = v * 16u + d;
                if (v > 0x10FFFFu) { v = 0x10FFFFu; }
                ++i;
                ++digits;
            }
        } else {
            while (i < avail && digits < 7) {
                char c = s[i];
                if (c < '0' || c > '9') { break; }
                v = v * 10u + (uint32_t)(c - '0');
                if (v > 0x10FFFFu) { v = 0x10FFFFu; }
                ++i;
                ++digits;
            }
        }
        if (digits == 0) {
            return false;
        }
        if (i < avail && s[i] == ';') { ++i; }
        out = (v >= 32u && v < 127u) ? (char)v : '?';
        consumed = i;
        return true;
    }
    struct Ent { const char* nm; char ch; };
    static const Ent kEnts[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'},
        {"apos;", (char)39}, {"nbsp;", ' '}, {"copy;", 'c'}, {"reg;", 'R'},
        {"mdash;", '-'}, {"ndash;", '-'}, {"hellip;", '.'}, {"middot;", '.'},
        {"times;", 'x'}, {"deg;", 'o'}, {"laquo;", '<'}, {"raquo;", '>'},
        {"trade;", 'T'}, {"sect;", 'S'}, {"para;", 'P'}, {"bull;", 'o'},
    };
    for (size_t e = 0; e < sizeof(kEnts) / sizeof(kEnts[0]); ++e) {
        size_t m = strlen(kEnts[e].nm);
        if (avail - 1 >= m && memcmp(s + 1, kEnts[e].nm, m) == 0) {
            out = kEnts[e].ch;
            consumed = 1 + m;
            return true;
        }
    }
    return false;
}

void appendTextNode(Dom& dom, uint32_t parent, const char* s, size_t n)
{
    if (parent == kNoNode || n == 0) {
        return;
    }
    char buf[kMaxTextNode + 1];
    uint32_t out = 0;
    size_t i = 0;
    while (i < n && out < kMaxTextNode) {
        if (s[i] == '&') {
            char ch = 0;
            size_t used = 0;
            if (decodeEntityAt(s + i, n - i, ch, used) && used > 0) {
                buf[out++] = ch;
                i += used;
                continue;
            }
        }
        buf[out++] = s[i++];
    }
    if (out == 0) {
        return;
    }
    uint32_t child = addNode(dom, 1, TAG_UNKNOWN);
    if (child == kNoNode) {
        return; // 节点数达上限: 丢弃该文本节点
    }
    uint32_t stored = 0;
    dom.nodes[child].textOff = arenaAdd(dom, buf, out, stored);
    dom.nodes[child].textLen = stored;
    appendChild(dom, parent, child);
}

void closeOpenElement(std::vector<uint32_t>& stack, uint8_t tag, const Dom& dom)
{
    if (tag == TAG_UNKNOWN || stack.size() <= 1) {
        return;
    }
    for (size_t k = stack.size(); k-- > 1;) {
        if (dom.nodes[stack[k]].tag == tag) {
            stack.resize(k);
            return;
        }
    }
}

// ---------------- HTML 词法/语法分析 -> DOM ----------------
void parseHtml(const std::string& src, Dom& dom, std::string& cssOut)
{
    dom.nodes.clear();
    dom.arena.clear();
    dom.nodes.reserve(kMaxNodes);
    dom.arena.reserve(kMaxArena);
    cssOut.clear();

    uint32_t root = addNode(dom, 0, TAG_HTML); // 合成根, 保证任何输入都有树
    dom.root = root;
    dom.layoutRoot = root;
    dom.bodyIdx = kNoNode;

    std::vector<uint32_t> stack;
    stack.reserve((size_t)kMaxDepth + 2);
    stack.push_back(root);

    const char* s = src.data();
    const size_t n = src.size();
    size_t i = 0;
    while (i < n) {
        if (s[i] != '<') {
            size_t j = i;
            while (j < n && s[j] != '<') { ++j; }
            if (!stack.empty()) {
                appendTextNode(dom, stack.back(), s + i, j - i);
            }
            i = j;
            continue;
        }
        if (i + 4 <= n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
            size_t e = src.find("-->", i + 4);
            i = (e == std::string::npos) ? n : e + 3;
            continue;
        }
        if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) {
            size_t e = src.find('>', i + 1);
            i = (e == std::string::npos) ? n : e + 1;
            continue;
        }
        if (i + 1 < n && s[i + 1] == '/') {
            size_t j = i + 2;
            char nm[kMaxTagName + 1];
            int nl = 0;
            while (j < n && isNameCh(s[j])) {
                if (nl < kMaxTagName) { nm[nl++] = lowerCh(s[j]); }
                ++j;
            }
            nm[nl] = 0;
            size_t e = src.find('>', j);
            i = (e == std::string::npos) ? n : e + 1;
            if (nl > 0) {
                closeOpenElement(stack, tagFromName(nm, nl), dom);
            }
            continue;
        }
        size_t j = i + 1;
        char nm[kMaxTagName + 1];
        int nl = 0;
        while (j < n && isNameCh(s[j])) {
            if (nl < kMaxTagName) { nm[nl++] = lowerCh(s[j]); }
            ++j;
        }
        nm[nl] = 0;
        if (nl == 0) {
            if (!stack.empty()) {
                appendTextNode(dom, stack.back(), s + i, 1); // 孤立 '<'
            }
            ++i;
            continue;
        }
        uint8_t tag = tagFromName(nm, nl);
        char cls[kMaxCls];
        char idb[kMaxId];
        char sty[kMaxInlineStyle];
        uint32_t clsl = 0;
        uint32_t idl = 0;
        uint32_t styl = 0;
        cls[0] = 0;
        idb[0] = 0;
        sty[0] = 0;
        bool selfClose = false;
        for (;;) {
            while (j < n && isSpaceCh(s[j])) { ++j; }
            if (j >= n) { break; }
            if (s[j] == '>') { ++j; break; }
            if (s[j] == '/') {
                if (j + 1 < n && s[j + 1] == '>') { selfClose = true; j += 2; break; }
                ++j;
                continue;
            }
            char an[kMaxTagName + 1];
            int anl = 0;
            while (j < n && !isSpaceCh(s[j]) && s[j] != '=' && s[j] != '>' && s[j] != '/') {
                if (anl < kMaxTagName) { an[anl++] = lowerCh(s[j]); }
                ++j;
            }
            an[anl] = 0;
            while (j < n && isSpaceCh(s[j])) { ++j; }
            char av[kMaxAttrVal + 1];
            uint32_t avl = 0;
            av[0] = 0;
            if (j < n && s[j] == '=') {
                ++j;
                while (j < n && isSpaceCh(s[j])) { ++j; }
                if (j < n && (s[j] == '"' || s[j] == (char)39)) {
                    char q = s[j++];
                    while (j < n && s[j] != q) {
                        if (avl < (uint32_t)kMaxAttrVal) { av[avl++] = s[j]; }
                        ++j;
                    }
                    if (j < n) { ++j; }
                } else {
                    while (j < n && !isSpaceCh(s[j]) && s[j] != '>') {
                        if (avl < (uint32_t)kMaxAttrVal) { av[avl++] = s[j]; }
                        ++j;
                    }
                }
                av[avl] = 0;
            }
            if (anl <= 0) {
                continue;
            }
            if (clsl == 0 && strcmp(an, "class") == 0) {
                uint32_t cn = avl;
                if (cn > (uint32_t)kMaxCls - 1u) { cn = (uint32_t)kMaxCls - 1u; }
                memcpy(cls, av, cn);
                cls[cn] = 0;
                clsl = cn;
            } else if (idl == 0 && strcmp(an, "id") == 0) {
                uint32_t cn = avl;
                if (cn > (uint32_t)kMaxId - 1u) { cn = (uint32_t)kMaxId - 1u; }
                memcpy(idb, av, cn);
                idb[cn] = 0;
                idl = cn;
            } else if (styl == 0 && strcmp(an, "style") == 0) {
                uint32_t cn = avl;
                if (cn > (uint32_t)kMaxInlineStyle - 1u) { cn = (uint32_t)kMaxInlineStyle - 1u; }
                memcpy(sty, av, cn);
                sty[cn] = 0;
                styl = cn;
            }
        }
        uint32_t parent = stack.empty() ? kNoNode : stack.back();
        uint32_t node = addNode(dom, 0, tag);
        if (node != kNoNode) {
            uint32_t stored = 0;
            if (clsl > 0) {
                dom.nodes[node].clsOff = arenaAdd(dom, cls, clsl, stored);
                dom.nodes[node].clsLen = stored;
            }
            if (idl > 0) {
                dom.nodes[node].idOff = arenaAdd(dom, idb, idl, stored);
                dom.nodes[node].idLen = stored;
            }
            if (styl > 0) {
                dom.nodes[node].styleOff = arenaAdd(dom, sty, styl, stored);
                dom.nodes[node].styleLen = stored;
            }
            appendChild(dom, parent, node);
            if (tag == TAG_BODY && dom.bodyIdx == kNoNode) {
                dom.bodyIdx = node;
            }
            if (!selfClose && !isVoidTag(tag) && !isRawTextTag(tag) && (int)stack.size() < kMaxDepth) {
                stack.push_back(node);
            }
        }
        i = j;
        if (isRawTextTag(tag)) {
            size_t k = j;
            size_t endPos = std::string::npos;
            while (k + 1 < n) {
                size_t p = src.find('<', k);
                if (p == std::string::npos || p + 1 >= n) { break; }
                if (src[p + 1] == '/') {
                    size_t q = p + 2;
                    int m = 0;
                    while (q < n && isNameCh(src[q]) && m < nl && lowerCh(src[q]) == nm[m]) {
                        ++q;
                        ++m;
                    }
                    if (m == nl && (q >= n || !isNameCh(src[q]))) {
                        endPos = p;
                        break;
                    }
                }
                k = p + 1;
            }
            if (endPos == std::string::npos) {
                if (tag == TAG_STYLE && cssOut.size() < (1u << 18)) { cssOut.append(src, j, n - j); }
                i = n;
            } else {
                if (tag == TAG_STYLE && cssOut.size() < (1u << 18)) { cssOut.append(src, j, endPos - j); }
                size_t e = src.find('>', endPos);
                i = (e == std::string::npos) ? n : e + 1;
            }
        }
    }

    if (dom.bodyIdx != kNoNode) {
        dom.layoutRoot = dom.bodyIdx;
    } else {
        uint32_t only = kNoNode;
        int cnt = 0;
        for (uint32_t c = dom.nodes[root].firstChild; c != kNoNode; c = dom.nodes[c].nextSibling) {
            if (!dom.nodes[c].isText) {
                ++cnt;
                only = c;
            }
        }
        dom.layoutRoot = (cnt == 1) ? only : root;
    }
}

// ---------------- CSS ----------------
enum {
    P_WIDTH = 0, P_HEIGHT,
    P_MG_T, P_MG_R, P_MG_B, P_MG_L,
    P_PAD_T, P_PAD_R, P_PAD_B, P_PAD_L,
    P_BORDER_W, P_BORDER_C,
    P_BG, P_FG, P_FS, P_LH, P_DISP, P_ALIGN, P_WEIGHT,
    P_COUNT
};

struct Decls {
    uint32_t mask;
    float w, h;
    uint8_t wMode, hMode;
    float mg[4];
    float pad[4];
    float borderW;
    uint32_t borderColor;
    uint32_t bg, fg;
    float fontSize;
    float lineHeight;
    uint8_t disp, align, weight;
};

struct Style {
    float w = 0.0f;
    float h = 0.0f;
    float mgT = 0.0f, mgR = 0.0f, mgB = 0.0f, mgL = 0.0f;
    float padT = 0.0f, padR = 0.0f, padB = 0.0f, padL = 0.0f;
    float borderW = 0.0f;
    float fontSize = 16.0f;
    float lineHeight = 1.25f;
    uint8_t wMode = LEN_AUTO;
    uint8_t hMode = LEN_AUTO;
    uint8_t disp = DISP_INLINE;
    uint8_t align = ALIGN_LEFT;
    uint8_t weight = 0;
    uint32_t bg = 0x00000000u;
    uint32_t fg = 0xFF1A1A1Au;
    uint32_t borderColor = 0xFF808080u;
};

inline float clampf(float v, float lo, float hi)
{
    if (!(v > lo)) { return lo; } // 同时挡住 NaN
    if (v > hi) { return hi; }
    return v;
}

bool parseNumber(const char* s, size_t n, size_t& i, float& out)
{
    bool neg = false;
    if (i < n && (s[i] == '-' || s[i] == '+')) {
        neg = (s[i] == '-');
        ++i;
    }
    float v = 0.0f;
    int digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9' && digits < 7) {
        v = v * 10.0f + (float)(s[i] - '0');
        ++i;
        ++digits;
    }
    if (i < n && s[i] == '.') {
        ++i;
        float f = 0.1f;
        int fd = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && fd < 5) {
            v += (float)(s[i] - '0') * f;
            f *= 0.1f;
            ++i;
            ++fd;
            ++digits;
        }
    }
    if (digits == 0) {
        return false;
    }
    out = neg ? -v : v;
    return true;
}

bool parseLength(const char* s, size_t n, float& val, uint8_t& mode)
{
    size_t i = 0;
    float v = 0.0f;
    if (!parseNumber(s, n, i, v)) {
        return false;
    }
    while (i < n && isSpaceCh(s[i])) { ++i; }
    uint8_t m = LEN_PX;
    if (i < n && s[i] == '%') {
        m = LEN_PCT;
    }
    val = clampf(v, -4096.0f, 8192.0f);
    mode = m;
    return true;
}

struct NamedColor {
    const char* nm;
    uint32_t rgb;
};

static const NamedColor kNamedColors[] = {
    {"black", 0x000000u}, {"white", 0xFFFFFFu}, {"red", 0xFF0000u}, {"green", 0x008000u},
    {"lime", 0x00FF00u}, {"blue", 0x0000FFu}, {"yellow", 0xFFFF00u}, {"orange", 0xFFA500u},
    {"purple", 0x800080u}, {"gray", 0x808080u}, {"grey", 0x808080u}, {"silver", 0xC0C0C0u},
    {"navy", 0x000080u}, {"teal", 0x008080u}, {"maroon", 0x800000u}, {"olive", 0x808000u},
    {"aqua", 0x00FFFFu}, {"fuchsia", 0xFF00FFu}, {"darkgray", 0xA9A9A9u}, {"lightgray", 0xD3D3D3u},
    {"whitesmoke", 0xF5F5F5u}, {"steelblue", 0x4682B4u}, {"crimson", 0xDC143Cu}, {"gold", 0xFFD700u},
    {"transparent", 0x000000u},
};

bool parseColor(const char* s, size_t n, uint32_t& out)
{
    size_t i = 0;
    while (i < n && isSpaceCh(s[i])) { ++i; }
    size_t e = n;
    while (e > i && isSpaceCh(s[e - 1])) { --e; }
    if (i >= e) {
        return false;
    }
    if (s[i] == '#') {
        ++i;
        uint32_t v = 0;
        int digits = 0;
        while (i < e && digits < 8) {
            char c = s[i];
            uint32_t d = 0;
            if (c >= '0' && c <= '9') { d = (uint32_t)(c - '0'); }
            else if (c >= 'a' && c <= 'f') { d = (uint32_t)(c - 'a' + 10); }
            else if (c >= 'A' && c <= 'F') { d = (uint32_t)(c - 'A' + 10); }
            else { break; }
            v = (v << 4) | d;
            ++i;
            ++digits;
        }
        if (digits == 3) {
            uint32_t r = (v >> 8) & 0xFu;
            uint32_t g = (v >> 4) & 0xFu;
            uint32_t b = v & 0xFu;
            out = 0xFF000000u | ((r * 17u) << 16) | ((g * 17u) << 8) | (b * 17u);
            return true;
        }
        if (digits == 6) {
            out = 0xFF000000u | (v & 0xFFFFFFu);
            return true;
        }
        if (digits == 8) {
            out = ((v & 0xFFu) << 24) | ((v >> 8) & 0xFFFFFFu);
            return true;
        }
        return false;
    }
    if ((s[i] == 'r' || s[i] == 'R') && i + 3 < e) {
        size_t p = i + 3;
        if (p < e && (s[p] == 'a' || s[p] == 'A')) { ++p; }
        if (p < e && s[p] == '(') {
            ++p;
            float comp[4];
            bool pct[4];
            int cnt = 0;
            while (p < e && s[p] != ')' && cnt < 4) {
                while (p < e && (isSpaceCh(s[p]) || s[p] == ',')) { ++p; }
                if (p >= e || s[p] == ')') { break; }
                float v = 0.0f;
                if (!parseNumber(s, e, p, v)) { break; }
                bool isPct = (p < e && s[p] == '%');
                if (isPct) { ++p; }
                comp[cnt] = v;
                pct[cnt] = isPct;
                ++cnt;
            }
            if (cnt < 3) {
                return false;
            }
            uint32_t r = (uint32_t)clampf(pct[0] ? comp[0] * 2.55f : comp[0], 0.0f, 255.0f);
            uint32_t g = (uint32_t)clampf(pct[1] ? comp[1] * 2.55f : comp[1], 0.0f, 255.0f);
            uint32_t b = (uint32_t)clampf(pct[2] ? comp[2] * 2.55f : comp[2], 0.0f, 255.0f);
            uint32_t a = 255u;
            if (cnt >= 4) {
                if (pct[3]) {
                    a = (uint32_t)clampf(comp[3] * 2.55f, 0.0f, 255.0f);
                } else if (comp[3] <= 1.0f) {
                    a = (uint32_t)clampf(comp[3] * 255.0f, 0.0f, 255.0f);
                } else {
                    a = 255u;
                }
            }
            out = (a << 24) | (r << 16) | (g << 8) | b;
            return true;
        }
    }
    char nm[20];
    size_t ln = 0;
    for (size_t k = i; k < e && ln < sizeof(nm) - 1; ++k) {
        nm[ln++] = lowerCh(s[k]);
    }
    nm[ln] = 0;
    for (size_t k = 0; k < sizeof(kNamedColors) / sizeof(kNamedColors[0]); ++k) {
        if (strcmp(nm, kNamedColors[k].nm) == 0) {
            if (strcmp(nm, "transparent") == 0) {
                out = 0u;
            } else {
                out = 0xFF000000u | (kNamedColors[k].rgb & 0xFFFFFFu);
            }
            return true;
        }
    }
    return false;
}

bool valueHasWord(const char* v, size_t n, const char* word)
{
    size_t m = strlen(word);
    if (m == 0 || n < m) {
        return false;
    }
    for (size_t i = 0; i + m <= n; ++i) {
        size_t k = 0;
        while (k < m && lowerCh(v[i + k]) == word[k]) { ++k; }
        if (k == m) {
            return true;
        }
    }
    return false;
}

bool parseShorthand4(const char* v, size_t n, float out[4])
{
    float vals[4];
    int cnt = 0;
    size_t i = 0;
    while (i < n && cnt < 4) {
        while (i < n && isSpaceCh(v[i])) { ++i; }
        if (i >= n) { break; }
        size_t j = i;
        while (j < n && !isSpaceCh(v[j])) { ++j; }
        float f = 0.0f;
        uint8_t m = LEN_PX;
        if (parseLength(v + i, j - i, f, m)) {
            vals[cnt++] = f;
        }
        i = j;
    }
    if (cnt == 0) {
        return false;
    }
    float t, r, b, l;
    if (cnt == 1) { t = r = b = l = vals[0]; }
    else if (cnt == 2) { t = b = vals[0]; r = l = vals[1]; }
    else if (cnt == 3) { t = vals[0]; r = l = vals[1]; b = vals[2]; }
    else { t = vals[0]; r = vals[1]; b = vals[2]; l = vals[3]; }
    out[0] = t;
    out[1] = r;
    out[2] = b;
    out[3] = l;
    return true;
}

void parseBorderShorthand(const char* val, size_t vl, Decls& d)
{
    size_t i = 0;
    bool gotW = false;
    while (i < vl) {
        while (i < vl && isSpaceCh(val[i])) { ++i; }
        if (i >= vl) { break; }
        size_t j = i;
        while (j < vl && !isSpaceCh(val[j])) { ++j; }
        if (!gotW) {
            float v = 0.0f;
            uint8_t m = LEN_PX;
            if (parseLength(val + i, j - i, v, m)) {
                d.borderW = v;
                d.mask |= 1u << P_BORDER_W;
                gotW = true;
                i = j;
                continue;
            }
        }
        uint32_t c = 0u;
        if (parseColor(val + i, j - i, c)) {
            d.borderColor = c;
            d.mask |= 1u << P_BORDER_C;
        }
        i = j;
    }
}

void applyProperty(const char* name, size_t nl, const char* val, size_t vl, Decls& d)
{
    if (nameEq(name, nl, "width")) {
        float v = 0.0f;
        uint8_t m = LEN_PX;
        if (parseLength(val, vl, v, m)) { d.w = v; d.wMode = m; d.mask |= 1u << P_WIDTH; }
        return;
    }
    if (nameEq(name, nl, "height")) {
        float v = 0.0f;
        uint8_t m = LEN_PX;
        if (parseLength(val, vl, v, m)) { d.h = v; d.hMode = m; d.mask |= 1u << P_HEIGHT; }
        return;
    }
    if (nameEq(name, nl, "margin") || nameEq(name, nl, "padding")) {
        float o[4];
        if (parseShorthand4(val, vl, o)) {
            bool isPad = nameEq(name, nl, "padding");
            for (int k = 0; k < 4; ++k) {
                if (isPad) { d.pad[k] = o[k]; }
                else { d.mg[k] = o[k]; }
                d.mask |= 1u << ((isPad ? P_PAD_T : P_MG_T) + k);
            }
        }
        return;
    }
    {
        static const char* const kSide[4] = {"-top", "-right", "-bottom", "-left"};
        for (int pass = 0; pass < 2; ++pass) {
            const char* base = (pass == 0) ? "margin" : "padding";
            size_t bl = strlen(base);
            for (int k = 0; k < 4; ++k) {
                char buf[24];
                size_t sl = strlen(kSide[k]);
                if (bl + sl + 1 > sizeof(buf)) { continue; }
                memcpy(buf, base, bl);
                memcpy(buf + bl, kSide[k], sl);
                buf[bl + sl] = 0;
                if (nameEq(name, nl, buf)) {
                    float v = 0.0f;
                    uint8_t m = LEN_PX;
                    if (parseLength(val, vl, v, m)) {
                        if (pass == 0) { d.mg[k] = v; d.mask |= 1u << (P_MG_T + k); }
                        else { d.pad[k] = v; d.mask |= 1u << (P_PAD_T + k); }
                    }
                    return;
                }
            }
        }
    }
    if (nameEq(name, nl, "border-width")) {
        float v = 0.0f;
        uint8_t m = LEN_PX;
        if (parseLength(val, vl, v, m)) { d.borderW = v; d.mask |= 1u << P_BORDER_W; }
        return;
    }
    if (nameEq(name, nl, "border-color")) {
        uint32_t c = 0u;
        if (parseColor(val, vl, c)) { d.borderColor = c; d.mask |= 1u << P_BORDER_C; }
        return;
    }
    if (nameEq(name, nl, "border") || nameEq(name, nl, "border-top") || nameEq(name, nl, "border-bottom")
        || nameEq(name, nl, "border-left") || nameEq(name, nl, "border-right")) {
        parseBorderShorthand(val, vl, d);
        return;
    }
    if (nameEq(name, nl, "background-color") || nameEq(name, nl, "background")) {
        uint32_t c = 0u;
        if (parseColor(val, vl, c)) { d.bg = c; d.mask |= 1u << P_BG; }
        return;
    }
    if (nameEq(name, nl, "color")) {
        uint32_t c = 0u;
        if (parseColor(val, vl, c)) { d.fg = c; d.mask |= 1u << P_FG; }
        return;
    }
    if (nameEq(name, nl, "font-size")) {
        float v = 0.0f;
        uint8_t m = LEN_PX;
        if (parseLength(val, vl, v, m) && m == LEN_PX) { d.fontSize = v; d.mask |= 1u << P_FS; }
        return;
    }
    if (nameEq(name, nl, "line-height")) {
        float v = 0.0f;
        uint8_t m = LEN_PX;
        if (parseLength(val, vl, v, m)) {
            float lh = (m == LEN_PCT) ? (v / 100.0f) : ((v > 3.2f) ? (v / 16.0f) : v);
            if (lh >= 0.8f && lh <= 3.0f) {
                d.lineHeight = lh;
                d.mask |= 1u << P_LH;
            }
        }
        return;
    }
    if (nameEq(name, nl, "display")) {
        if (valueHasWord(val, vl, "none")) { d.disp = DISP_NONE; }
        else if (valueHasWord(val, vl, "block") || valueHasWord(val, vl, "flex")) { d.disp = DISP_BLOCK; }
        else if (valueHasWord(val, vl, "inline")) { d.disp = DISP_INLINE; }
        else { return; }
        d.mask |= 1u << P_DISP;
        return;
    }
    if (nameEq(name, nl, "text-align")) {
        if (valueHasWord(val, vl, "center")) { d.align = ALIGN_CENTER; }
        else if (valueHasWord(val, vl, "right")) { d.align = ALIGN_RIGHT; }
        else if (valueHasWord(val, vl, "left")) { d.align = ALIGN_LEFT; }
        else { return; }
        d.mask |= 1u << P_ALIGN;
        return;
    }
    if (nameEq(name, nl, "font-weight")) {
        if (valueHasWord(val, vl, "bold") || valueHasWord(val, vl, "700") || valueHasWord(val, vl, "800") || valueHasWord(val, vl, "900")) {
            d.weight = 1;
        } else {
            d.weight = 0;
        }
        d.mask |= 1u << P_WEIGHT;
        return;
    }
}

void parseDeclarationList(const char* s, size_t n, Decls& d)
{
    size_t i = 0;
    while (i < n) {
        while (i < n && (isSpaceCh(s[i]) || s[i] == ';')) { ++i; }
        if (i >= n) { break; }
        size_t ns = i;
        while (i < n && s[i] != ':' && s[i] != ';' && s[i] != '}') { ++i; }
        size_t ne = i;
        while (ne > ns && isSpaceCh(s[ne - 1])) { --ne; }
        if (i < n && s[i] == ':') {
            ++i;
        } else {
            while (i < n && s[i] != ';') { ++i; }
            continue;
        }
        size_t vs = i;
        while (i < n && s[i] != ';' && s[i] != '}') { ++i; }
        size_t ve = i;
        while (ve > vs && isSpaceCh(s[ve - 1])) { --ve; }
        while (vs < ve && isSpaceCh(s[vs])) { ++vs; }
        if (ne > ns && ve > vs) {
            applyProperty(s + ns, ne - ns, s + vs, ve - vs, d);
        }
    }
}

struct Compound {
    uint8_t hasTag;
    uint8_t hasCls;
    uint8_t hasId;
    uint8_t tag;
    char cls[kMaxCls];
    char id[kMaxId];
};

struct Rule {
    Compound cmp[kMaxCompounds];
    int nComp;
    uint32_t spec;
    Decls d;
};

bool parseOneSelector(const char* s, size_t n, Rule& r)
{
    r.nComp = 0;
    r.spec = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && isSpaceCh(s[i])) { ++i; }
        if (i >= n) { break; }
        if (r.nComp >= kMaxCompounds) {
            return false; // 选择器过于复杂: 丢弃
        }
        Compound& c = r.cmp[r.nComp];
        c.hasTag = 0;
        c.hasCls = 0;
        c.hasId = 0;
        c.tag = TAG_UNKNOWN;
        c.cls[0] = 0;
        c.id[0] = 0;
        bool any = false;
        while (i < n && !isSpaceCh(s[i])) {
            char k = s[i];
            if (k == '*') {
                ++i;
                any = true;
                continue;
            }
            if (k == '.' || k == '#') {
                ++i;
                size_t j = i;
                while (j < n && !isSpaceCh(s[j]) && s[j] != '.' && s[j] != '#' && s[j] != ':') { ++j; }
                size_t len = j - i;
                if (k == '.') {
                    if (len == 0 || len > (size_t)kMaxCls - 1u) { return false; }
                    memcpy(c.cls, s + i, len);
                    c.cls[len] = 0;
                    c.hasCls = 1;
                } else {
                    if (len == 0 || len > (size_t)kMaxId - 1u) { return false; }
                    memcpy(c.id, s + i, len);
                    c.id[len] = 0;
                    c.hasId = 1;
                }
                any = true;
                i = j;
                continue;
            }
            if (k == ':' || k == '[' || k == '>' || k == '+' || k == '~') {
                return false; // 伪类/属性选择器/子组合子/兄弟组合子不支持
            }
            size_t j = i;
            while (j < n && !isSpaceCh(s[j]) && s[j] != '.' && s[j] != '#' && s[j] != ':' && s[j] != '[') { ++j; }
            char nm[kMaxTagName + 1];
            int ln = 0;
            for (size_t q = i; q < j && ln < kMaxTagName; ++q) {
                nm[ln++] = lowerCh(s[q]);
            }
            nm[ln] = 0;
            uint8_t t = tagFromName(nm, ln);
            if (t == TAG_UNKNOWN) {
                return false;
            }
            c.tag = t;
            c.hasTag = 1;
            any = true;
            i = j;
        }
        if (!any) {
            return false;
        }
        if (c.hasId) { r.spec += 100u; }
        if (c.hasCls) { r.spec += 10u; }
        if (c.hasTag) { r.spec += 1u; }
        ++r.nComp;
    }
    return r.nComp > 0;
}

void parseStyleSheet(const char* s, size_t n, std::vector<Rule>& rules)
{
    size_t i = 0;
    while (i < n) {
        while (i < n && isSpaceCh(s[i])) { ++i; }
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            size_t e = i + 2;
            while (e + 1 < n && !(s[e] == '*' && s[e + 1] == '/')) { ++e; }
            i = (e + 1 < n) ? e + 2 : n;
            continue;
        }
        if (i >= n) { break; }
        if (s[i] == '}') { ++i; continue; }
        if (s[i] == '@') {
            while (i < n && s[i] != '{' && s[i] != ';') { ++i; }
            if (i < n && s[i] == '{') {
                int depth = 1;
                ++i;
                while (i < n && depth > 0) {
                    if (s[i] == '{') { ++depth; }
                    else if (s[i] == '}') { --depth; }
                    ++i;
                }
            } else if (i < n) {
                ++i;
            }
            continue;
        }
        size_t sels = i;
        while (i < n && s[i] != '{' && s[i] != '}') { ++i; }
        size_t sele = i;
        if (i >= n || s[i] != '{') {
            if (i < n) { ++i; }
            continue;
        }
        ++i;
        size_t ds = i;
        while (i < n && s[i] != '}') { ++i; }
        size_t de = i;
        if (i < n) { ++i; }
        if (de <= ds) { continue; }
        Decls d;
        memset(&d, 0, sizeof(d));
        parseDeclarationList(s + ds, de - ds, d);
        if (d.mask == 0u) { continue; }
        size_t a = sels;
        while (a < sele) {
            size_t b = a;
            while (b < sele && s[b] != ',') { ++b; }
            if (b > a && rules.size() < kMaxRules) {
                Rule r;
                memset(&r, 0, sizeof(r));
                if (parseOneSelector(s + a, b - a, r)) {
                    r.d = d;
                    rules.push_back(r);
                }
            }
            a = (b < sele) ? b + 1 : sele;
        }
    }
}

// ---------------- 选择器匹配 ----------------
bool classListHas(const Dom& dom, const DomNode& nd, const char* want)
{
    if (nd.clsLen == 0) {
        return false;
    }
    size_t wl = strlen(want);
    if (wl == 0) {
        return false;
    }
    const char* s = dom.arena.data() + nd.clsOff;
    uint32_t n = nd.clsLen;
    uint32_t i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) { ++i; }
        uint32_t j = i;
        while (j < n && s[j] != ' ' && s[j] != '\t' && s[j] != '\n' && s[j] != '\r') { ++j; }
        if (j > i && (size_t)(j - i) == wl && memcmp(s + i, want, wl) == 0) {
            return true;
        }
        i = j;
    }
    return false;
}

bool compoundMatches(const Dom& dom, uint32_t node, const Compound& c)
{
    if (node >= dom.nodes.size()) {
        return false;
    }
    const DomNode& nd = dom.nodes[node];
    if (nd.isText) {
        return false;
    }
    if (c.hasTag && nd.tag != c.tag) {
        return false;
    }
    if (c.hasId) {
        size_t wl = strlen(c.id);
        if (nd.idLen == 0 || (size_t)nd.idLen != wl) {
            return false;
        }
        if (memcmp(dom.arena.data() + nd.idOff, c.id, wl) != 0) {
            return false;
        }
    }
    if (c.hasCls && !classListHas(dom, nd, c.cls)) {
        return false;
    }
    return true;
}

bool selectorMatches(const Dom& dom, uint32_t node, const Rule& r, uint32_t* chainBuf)
{
    const int kCap = kMaxDepth + 4;
    int L = 0;
    uint32_t cur = node;
    while (cur != kNoNode && L < kCap) {
        chainBuf[L++] = cur;
        cur = dom.nodes[cur].parent;
    }
    if (L == 0) {
        return false;
    }
    bool curOk[kMaxDepth + 4];
    for (int j = 0; j < L; ++j) {
        curOk[j] = compoundMatches(dom, chainBuf[j], r.cmp[r.nComp - 1]);
    }
    for (int ci = r.nComp - 2; ci >= 0; --ci) {
        bool nxt[kMaxDepth + 4];
        bool any = false;
        for (int j = L - 1; j >= 0; --j) {
            nxt[j] = any;
            if (curOk[j]) { any = true; }
        }
        for (int j = 0; j < L; ++j) {
            curOk[j] = nxt[j] && compoundMatches(dom, chainBuf[j], r.cmp[ci]);
        }
    }
    return curOk[0];
}

// ---------------- 层叠: UA 默认 -> 作者规则 -> 内联样式 ----------------
void uaDefaults(uint8_t tag, const Style& parent, Style& s)
{
    s.w = 0.0f;
    s.h = 0.0f;
    s.wMode = LEN_AUTO;
    s.hMode = LEN_AUTO;
    s.mgT = 0.0f;
    s.mgR = 0.0f;
    s.mgB = 0.0f;
    s.mgL = 0.0f;
    s.padT = 0.0f;
    s.padR = 0.0f;
    s.padB = 0.0f;
    s.padL = 0.0f;
    s.borderW = 0.0f;
    s.borderColor = 0xFF808080u;
    s.bg = 0x00000000u;
    s.fg = parent.fg;
    s.fontSize = parent.fontSize;
    s.lineHeight = parent.lineHeight;
    s.align = parent.align;
    s.weight = 0;
    s.disp = DISP_INLINE;
    float fs = parent.fontSize;
    switch (tag) {
    case TAG_HTML:
    case TAG_BODY:
        s.disp = DISP_BLOCK;
        s.mgT = 8.0f; s.mgR = 8.0f; s.mgB = 8.0f; s.mgL = 8.0f;
        break;
    case TAG_DIV:
    case TAG_SECTION:
    case TAG_HEADER:
    case TAG_FOOTER:
    case TAG_NAV:
    case TAG_ARTICLE:
    case TAG_ASIDE:
    case TAG_MAIN:
    case TAG_FORM:
    case TAG_THEAD:
    case TAG_TBODY:
    case TAG_TR:
        s.disp = DISP_BLOCK;
        break;
    case TAG_P:
        s.disp = DISP_BLOCK;
        s.mgT = fs * 0.9f;
        s.mgB = fs * 0.9f;
        break;
    case TAG_BLOCKQUOTE:
        s.disp = DISP_BLOCK;
        s.mgT = 10.0f; s.mgB = 10.0f; s.mgL = 24.0f;
        break;
    case TAG_H1: s.disp = DISP_BLOCK; s.fontSize = 32.0f; s.weight = 1; s.mgT = 14.0f; s.mgB = 10.0f; break;
    case TAG_H2: s.disp = DISP_BLOCK; s.fontSize = 24.0f; s.weight = 1; s.mgT = 12.0f; s.mgB = 8.0f; break;
    case TAG_H3: s.disp = DISP_BLOCK; s.fontSize = 19.0f; s.weight = 1; s.mgT = 10.0f; s.mgB = 6.0f; break;
    case TAG_H4: s.disp = DISP_BLOCK; s.fontSize = 16.0f; s.weight = 1; s.mgT = 8.0f; s.mgB = 6.0f; break;
    case TAG_H5: s.disp = DISP_BLOCK; s.fontSize = 14.0f; s.weight = 1; s.mgT = 6.0f; s.mgB = 4.0f; break;
    case TAG_H6: s.disp = DISP_BLOCK; s.fontSize = 12.0f; s.weight = 1; s.mgT = 6.0f; s.mgB = 4.0f; break;
    case TAG_UL:
    case TAG_OL:
        s.disp = DISP_BLOCK;
        s.mgT = 10.0f; s.mgB = 10.0f; s.padL = 30.0f;
        break;
    case TAG_LI:
        s.disp = DISP_BLOCK;
        s.mgT = 2.0f; s.mgB = 2.0f;
        break;
    case TAG_TABLE:
        s.disp = DISP_BLOCK;
        s.mgT = 8.0f; s.mgB = 8.0f;
        break;
    case TAG_CAPTION:
        s.disp = DISP_BLOCK;
        s.fontSize = clampf(fs * 1.05f, 6.0f, 96.0f);
        s.weight = 1;
        break;
    case TAG_TD:
    case TAG_TH:
        s.disp = DISP_BLOCK;
        s.padT = 4.0f; s.padR = 4.0f; s.padB = 4.0f; s.padL = 4.0f;
        break;
    case TAG_HR:
        s.disp = DISP_BLOCK;
        s.hMode = LEN_PX;
        s.h = 2.0f;
        s.bg = 0xFF9AA3B0u;
        s.mgT = 10.0f; s.mgB = 10.0f;
        break;
    case TAG_PRE:
        s.disp = DISP_BLOCK;
        s.fontSize = clampf(fs * 0.9f, 6.0f, 96.0f);
        s.padT = 8.0f; s.padR = 10.0f; s.padB = 8.0f; s.padL = 10.0f;
        s.bg = 0xFFF4F4F6u;
        break;
    case TAG_CODE:
        s.fontSize = clampf(fs * 0.92f, 6.0f, 96.0f);
        break;
    case TAG_B:
    case TAG_STRONG:
        s.weight = 1;
        break;
    case TAG_SMALL:
        s.fontSize = clampf(fs * 0.85f, 6.0f, 96.0f);
        break;
    case TAG_BIG:
        s.fontSize = clampf(fs * 1.25f, 6.0f, 48.0f);
        break;
    case TAG_A:
        s.fg = 0xFF1155CCu;
        break;
    case TAG_BUTTON:
        s.borderW = 1.0f;
        s.padT = 4.0f; s.padR = 8.0f; s.padB = 4.0f; s.padL = 8.0f;
        s.bg = 0xFFE8ECF2u;
        break;
    case TAG_INPUT:
        s.wMode = LEN_PX; s.w = 140.0f;
        s.hMode = LEN_PX; s.h = 22.0f;
        s.borderW = 1.0f;
        s.bg = 0xFFFFFFFFu;
        break;
    case TAG_IMG:
        s.wMode = LEN_PX; s.w = 64.0f;
        s.hMode = LEN_PX; s.h = 64.0f;
        s.bg = 0xFFCCCCCCu;
        break;
    case TAG_HEAD:
    case TAG_TITLE:
    case TAG_META:
    case TAG_LINK:
    case TAG_STYLE:
    case TAG_SCRIPT:
        s.disp = DISP_NONE;
        break;
    default:
        break;
    }
}

void clampStyle(Style& s)
{
    s.fontSize = clampf(s.fontSize, 6.0f, 96.0f);
    s.lineHeight = clampf(s.lineHeight, 0.8f, 3.0f);
    s.mgT = clampf(s.mgT, 0.0f, 256.0f);
    s.mgR = clampf(s.mgR, 0.0f, 256.0f);
    s.mgB = clampf(s.mgB, 0.0f, 256.0f);
    s.mgL = clampf(s.mgL, 0.0f, 256.0f);
    s.padT = clampf(s.padT, 0.0f, 256.0f);
    s.padR = clampf(s.padR, 0.0f, 256.0f);
    s.padB = clampf(s.padB, 0.0f, 256.0f);
    s.padL = clampf(s.padL, 0.0f, 256.0f);
    s.borderW = clampf(s.borderW, 0.0f, 32.0f);
    s.w = clampf(s.w, 0.0f, 8192.0f);
    s.h = clampf(s.h, 0.0f, 8192.0f);
}

void applyDecls(Style& s, const Decls& d, uint32_t spec, uint32_t* best)
{
    if ((d.mask & (1u << P_WIDTH)) && spec >= best[P_WIDTH]) { best[P_WIDTH] = spec; s.w = d.w; s.wMode = d.wMode; }
    if ((d.mask & (1u << P_HEIGHT)) && spec >= best[P_HEIGHT]) { best[P_HEIGHT] = spec; s.h = d.h; s.hMode = d.hMode; }
    if (d.mask & (1u << P_MG_T)) { if (spec >= best[P_MG_T]) { best[P_MG_T] = spec; s.mgT = d.mg[0]; } }
    if (d.mask & (1u << P_MG_R)) { if (spec >= best[P_MG_R]) { best[P_MG_R] = spec; s.mgR = d.mg[1]; } }
    if (d.mask & (1u << P_MG_B)) { if (spec >= best[P_MG_B]) { best[P_MG_B] = spec; s.mgB = d.mg[2]; } }
    if (d.mask & (1u << P_MG_L)) { if (spec >= best[P_MG_L]) { best[P_MG_L] = spec; s.mgL = d.mg[3]; } }
    if (d.mask & (1u << P_PAD_T)) { if (spec >= best[P_PAD_T]) { best[P_PAD_T] = spec; s.padT = d.pad[0]; } }
    if (d.mask & (1u << P_PAD_R)) { if (spec >= best[P_PAD_R]) { best[P_PAD_R] = spec; s.padR = d.pad[1]; } }
    if (d.mask & (1u << P_PAD_B)) { if (spec >= best[P_PAD_B]) { best[P_PAD_B] = spec; s.padB = d.pad[2]; } }
    if (d.mask & (1u << P_PAD_L)) { if (spec >= best[P_PAD_L]) { best[P_PAD_L] = spec; s.padL = d.pad[3]; } }
    if (d.mask & (1u << P_BORDER_W)) { if (spec >= best[P_BORDER_W]) { best[P_BORDER_W] = spec; s.borderW = d.borderW; } }
    if (d.mask & (1u << P_BORDER_C)) { if (spec >= best[P_BORDER_C]) { best[P_BORDER_C] = spec; s.borderColor = d.borderColor; } }
    if (d.mask & (1u << P_BG)) { if (spec >= best[P_BG]) { best[P_BG] = spec; s.bg = d.bg; } }
    if (d.mask & (1u << P_FG)) { if (spec >= best[P_FG]) { best[P_FG] = spec; s.fg = d.fg; } }
    if (d.mask & (1u << P_FS)) { if (spec >= best[P_FS]) { best[P_FS] = spec; s.fontSize = d.fontSize; } }
    if (d.mask & (1u << P_LH)) { if (spec >= best[P_LH]) { best[P_LH] = spec; s.lineHeight = d.lineHeight; } }
    if (d.mask & (1u << P_DISP)) { if (spec >= best[P_DISP]) { best[P_DISP] = spec; s.disp = d.disp; } }
    if (d.mask & (1u << P_ALIGN)) { if (spec >= best[P_ALIGN]) { best[P_ALIGN] = spec; s.align = d.align; } }
    if (d.mask & (1u << P_WEIGHT)) { if (spec >= best[P_WEIGHT]) { best[P_WEIGHT] = spec; s.weight = d.weight; } }
}

void resolveStyles(const Dom& dom, const std::vector<Rule>& rules, std::vector<Style>& styles)
{
    Style rootStyle;
    rootStyle.fontSize = 16.0f;
    rootStyle.lineHeight = 1.25f;
    rootStyle.fg = 0xFF1A1A1Au;
    rootStyle.disp = DISP_BLOCK;
    styles.assign(dom.nodes.size(), rootStyle);
    if (dom.nodes.empty()) {
        return;
    }
    uint32_t chain[kMaxDepth + 4];
    std::vector<uint32_t> stack;
    stack.reserve(1024);
    stack.push_back(dom.root);
    while (!stack.empty()) {
        uint32_t n = stack.back();
        stack.pop_back();
        if (n == kNoNode || n >= dom.nodes.size()) {
            continue;
        }
        const DomNode& nd = dom.nodes[n];
        if (nd.isText) {
            continue;
        }
        Style parentStyle = rootStyle;
        if (nd.parent != kNoNode && nd.parent < styles.size()) {
            parentStyle = styles[nd.parent];
        }
        Style s;
        uaDefaults(nd.tag, parentStyle, s);
        uint32_t best[P_COUNT];
        for (int k = 0; k < P_COUNT; ++k) {
            best[k] = 0u;
        }
        for (size_t ri = 0; ri < rules.size(); ++ri) {
            const Rule& r = rules[ri];
            if (r.nComp <= 0) {
                continue;
            }
            if (!compoundMatches(dom, n, r.cmp[r.nComp - 1])) {
                continue;
            }
            if (r.nComp > 1 && !selectorMatches(dom, n, r, chain)) {
                continue;
            }
            applyDecls(s, r.d, r.spec, best);
        }
        if (nd.styleLen > 0) {
            Decls d;
            memset(&d, 0, sizeof(d));
            parseDeclarationList(dom.arena.data() + nd.styleOff, nd.styleLen, d);
            applyDecls(s, d, 1000u, best);
        }
        clampStyle(s);
        styles[n] = s;
        for (uint32_t c = nd.firstChild; c != kNoNode; c = dom.nodes[c].nextSibling) {
            if (c >= dom.nodes.size()) {
                break;
            }
            if (!dom.nodes[c].isText) {
                stack.push_back(c);
            } else if (c < styles.size()) {
                styles[c] = s; // 文本节点继承父元素样式
            }
        }
    }
}

// ---------------- 布局: 显示列表 ----------------
struct Box {
    int x, y, w, h;
    int cx0, cy0, cx1, cy1; // 裁剪矩形(半开区间)
    uint32_t color;
    uint32_t textOff;
    uint32_t textLen;
    float fontSize;
    uint8_t kind;
    uint8_t pad0, pad1, pad2;
};

struct Layout {
    const Dom* dom;
    const std::vector<Style>* styles;
    std::vector<Box>* boxes;
    uint32_t dropped;
};

uint32_t emitFill(Layout& L, int x, int y, int w, int h, uint32_t color, const Rect& clip)
{
    if (L.boxes->size() >= kMaxBoxes) {
        L.dropped++;
        return kNoNode;
    }
    Box b;
    b.x = x; b.y = y; b.w = w; b.h = h;
    b.cx0 = clip.x0; b.cy0 = clip.y0; b.cx1 = clip.x1; b.cy1 = clip.y1;
    b.color = color;
    b.textOff = 0; b.textLen = 0; b.fontSize = 0.0f;
    b.kind = BOX_FILL;
    b.pad0 = 0; b.pad1 = 0; b.pad2 = 0;
    L.boxes->push_back(b);
    return (uint32_t)(L.boxes->size() - 1);
}

uint32_t emitText(Layout& L, int x, int y, int w, int h, uint32_t color, uint32_t off, uint32_t len, float fs, const Rect& clip)
{
    if (L.boxes->size() >= kMaxBoxes) {
        L.dropped++;
        return kNoNode;
    }
    Box b;
    b.x = x; b.y = y; b.w = w; b.h = h;
    b.cx0 = clip.x0; b.cy0 = clip.y0; b.cx1 = clip.x1; b.cy1 = clip.y1;
    b.color = color;
    b.textOff = off; b.textLen = len; b.fontSize = fs;
    b.kind = BOX_TEXT;
    b.pad0 = 0; b.pad1 = 0; b.pad2 = 0;
    L.boxes->push_back(b);
    return (uint32_t)(L.boxes->size() - 1);
}

int resolveLen(uint8_t mode, float v, int avail)
{
    if (mode == LEN_PX) {
        return (int)(v + 0.5f);
    }
    if (mode == LEN_PCT) {
        return (int)((float)avail * v * 0.01f + 0.5f);
    }
    return -1; // auto
}

// ---------------- 行内排版 ----------------
struct InlineItem {
    uint32_t off, len;
    int relX, adv;
    float fontSize;
    uint32_t color, bg;
};

struct LineState {
    std::vector<InlineItem> items;
    int penX, pendingSpace;
    float maxFs, lh;
    uint8_t align;
    int contentX, curY, contentW;
    Rect clip;
};

void lineFlush(Layout& L, LineState& ls)
{
    if (ls.items.empty()) {
        ls.penX = 0;
        ls.pendingSpace = 0;
        return;
    }
    int lineH = (int)(ls.maxFs * ls.lh + 0.5f);
    if (lineH < 6) { lineH = 6; }
    if (lineH > 160) { lineH = 160; }
    int offset = 0;
    if (ls.align == ALIGN_CENTER) { offset = (ls.contentW - ls.penX) / 2; }
    else if (ls.align == ALIGN_RIGHT) { offset = ls.contentW - ls.penX; }
    if (offset < 0) { offset = 0; }
    for (size_t i = 0; i < ls.items.size(); ++i) {
        const InlineItem& it = ls.items[i];
        int ix = ls.contentX + offset + it.relX;
        if (colAlpha(it.bg) != 0u) {
            emitFill(L, ix - 1, ls.curY, it.adv + 2, lineH, it.bg, ls.clip);
        }
        emitText(L, ix, ls.curY, it.adv, lineH, it.color, it.off, it.len, it.fontSize, ls.clip);
    }
    ls.curY += lineH;
    ls.items.clear();
    ls.penX = 0;
    ls.pendingSpace = 0;
    ls.maxFs = 0.0f;
}

void linePlaceWord(Layout& L, LineState& ls, uint32_t off, uint32_t len, float fs, uint32_t color, uint32_t bg)
{
    const char* s = L.dom->arena.data() + off;
    int adv = textAdvance(s, len, fs);
    if (adv <= 0) {
        return;
    }
    int startX = ls.items.empty() ? 0 : (ls.penX + ls.pendingSpace);
    if (!ls.items.empty() && startX + adv > ls.contentW) {
        lineFlush(L, ls);
        startX = 0;
    }
    InlineItem it;
    it.off = off;
    it.len = len;
    it.relX = startX;
    it.adv = adv;
    it.fontSize = fs;
    it.color = color;
    it.bg = bg;
    ls.items.push_back(it);
    ls.penX = startX + adv;
    ls.pendingSpace = 0;
    if (fs > ls.maxFs) { ls.maxFs = fs; }
}

void lineBreak(Layout& L, LineState& ls, float baseFs)
{
    if (ls.items.empty()) {
        int lh = (int)(baseFs * ls.lh + 0.5f);
        if (lh < 6) { lh = 6; }
        if (lh > 160) { lh = 160; }
        ls.curY += lh;
        return;
    }
    lineFlush(L, ls);
}

void feedText(Layout& L, LineState& ls, const DomNode& tn, float fs, uint32_t color, uint32_t bg)
{
    if (tn.textLen == 0) {
        return;
    }
    const char* s = L.dom->arena.data() + tn.textOff;
    uint32_t n = tn.textLen;
    if ((size_t)tn.textOff + (size_t)n > L.dom->arena.size()) {
        n = (uint32_t)(L.dom->arena.size() - tn.textOff);
    }
    uint32_t i = 0;
    while (i < n) {
        if (isSpaceCh(s[i])) {
            while (i < n && isSpaceCh(s[i])) { ++i; }
            if (!ls.items.empty()) {
                ls.pendingSpace = advForChar(' ', fs);
            }
            continue;
        }
        uint32_t j = i;
        while (j < n && !isSpaceCh(s[j])) { ++j; }
        linePlaceWord(L, ls, tn.textOff + i, j - i, fs, color, bg);
        i = j;
    }
}

void collectInline(Layout& L, LineState& ls, uint32_t node, float fs, uint32_t color, uint32_t bg, int depth)
{
    if (depth > kMaxDepth || node >= L.dom->nodes.size()) {
        return;
    }
    const Dom& dom = *L.dom;
    for (uint32_t c = dom.nodes[node].firstChild; c != kNoNode; c = dom.nodes[c].nextSibling) {
        if (c >= dom.nodes.size()) {
            break;
        }
        const DomNode& cn = dom.nodes[c];
        if (cn.isText) {
            feedText(L, ls, cn, fs, color, bg);
            continue;
        }
        if (cn.tag == TAG_STYLE || cn.tag == TAG_SCRIPT || cn.tag == TAG_TITLE || cn.tag == TAG_META || cn.tag == TAG_LINK) {
            continue;
        }
        const Style& cs = (*L.styles)[c];
        if (cs.disp == DISP_NONE) {
            continue;
        }
        if (cn.tag == TAG_BR || cn.tag == TAG_HR) {
            lineBreak(L, ls, fs);
            continue;
        }
        if (cs.disp == DISP_BLOCK) {
            lineBreak(L, ls, fs); // 行内元素里嵌块级: 简化为强制换行
        }
        uint32_t cbg = (colAlpha(cs.bg) != 0u) ? cs.bg : bg;
        collectInline(L, ls, c, cs.fontSize, cs.fg, cbg, depth + 1);
    }
}

int layoutBlock(Layout& L, uint32_t node, int parentContentX, int y, int availW, const Rect& clip, int depth);

int layoutContent(Layout& L, uint32_t node, int contentX, int contentY, int contentW, const Rect& clip, int depth)
{
    const Dom& dom = *L.dom;
    const Style& s = (*L.styles)[node];
    LineState ls;
    ls.items.reserve(64);
    ls.penX = 0;
    ls.pendingSpace = 0;
    ls.maxFs = 0.0f;
    ls.align = s.align;
    ls.lh = clampf(s.lineHeight, 0.8f, 3.0f);
    ls.contentX = contentX;
    ls.contentW = contentW;
    ls.curY = contentY;
    ls.clip = clip;

    for (uint32_t c = dom.nodes[node].firstChild; c != kNoNode; c = dom.nodes[c].nextSibling) {
        if (c >= dom.nodes.size()) {
            break;
        }
        const DomNode& cn = dom.nodes[c];
        if (cn.isText) {
            feedText(L, ls, cn, s.fontSize, s.fg, 0u);
            continue;
        }
        if (cn.tag == TAG_STYLE || cn.tag == TAG_SCRIPT || cn.tag == TAG_TITLE || cn.tag == TAG_META || cn.tag == TAG_LINK) {
            continue;
        }
        const Style& cs = (*L.styles)[c];
        if (cs.disp == DISP_NONE) {
            continue;
        }
        if (cn.tag == TAG_BR) {
            lineBreak(L, ls, s.fontSize);
            continue;
        }
        if (cs.disp == DISP_BLOCK) {
            lineFlush(L, ls);
            ls.curY += layoutBlock(L, c, contentX, ls.curY, contentW, clip, depth + 1);
            continue;
        }
        collectInline(L, ls, c, cs.fontSize, cs.fg, colAlpha(cs.bg) != 0u ? cs.bg : 0u, depth + 1);
    }
    lineFlush(L, ls);
    int h = ls.curY - contentY;
    if (h < 0) { h = 0; }
    return h;
}

// 表格: 统一列宽网格(均分可用宽度), 行高 = 该行各单元格最大高度
int layoutTable(Layout& L, uint32_t tableNode, int contentX, int contentY, int contentW, const Rect& clip, int depth)
{
    const Dom& dom = *L.dom;
    int curY = contentY;
    for (uint32_t c = dom.nodes[tableNode].firstChild; c != kNoNode; c = dom.nodes[c].nextSibling) {
        if (c >= dom.nodes.size()) { break; }
        if (!dom.nodes[c].isText && dom.nodes[c].tag == TAG_CAPTION) {
            curY += layoutBlock(L, c, contentX, curY, contentW, clip, depth + 1);
        }
    }
    std::vector<uint32_t> rows;
    rows.reserve(32);
    for (uint32_t c = dom.nodes[tableNode].firstChild; c != kNoNode; c = dom.nodes[c].nextSibling) {
        if (c >= dom.nodes.size()) { break; }
        if (dom.nodes[c].isText) { continue; }
        uint8_t t = dom.nodes[c].tag;
        if (t == TAG_TR) {
            rows.push_back(c);
        } else if (t == TAG_THEAD || t == TAG_TBODY) {
            for (uint32_t g = dom.nodes[c].firstChild; g != kNoNode; g = dom.nodes[g].nextSibling) {
                if (g >= dom.nodes.size()) { break; }
                if (!dom.nodes[g].isText && dom.nodes[g].tag == TAG_TR) {
                    rows.push_back(g);
                }
            }
        }
        if (rows.size() >= (size_t)kMaxTableRows) { break; }
    }
    if (rows.empty()) {
        int h0 = curY - contentY;
        return h0 < 0 ? 0 : h0;
    }
    int ncols = 0;
    for (size_t r = 0; r < rows.size(); ++r) {
        int cnt = 0;
        for (uint32_t c = dom.nodes[rows[r]].firstChild; c != kNoNode; c = dom.nodes[c].nextSibling) {
            if (c >= dom.nodes.size()) { break; }
            if (dom.nodes[c].isText) { continue; }
            uint8_t t = dom.nodes[c].tag;
            if (t == TAG_TD || t == TAG_TH) { ++cnt; }
            if (cnt >= kMaxTableCols) { break; }
        }
        if (cnt > ncols) { ncols = cnt; }
    }
    if (ncols <= 0) {
        int h0 = curY - contentY;
        return h0 < 0 ? 0 : h0;
    }
    int colW = contentW / ncols;
    if (colW < 0) { colW = 0; }
    for (size_t r = 0; r < rows.size(); ++r) {
        uint32_t row = rows[r];
        const Style& rs = (*L.styles)[row];
        uint32_t bgIdx = kNoNode;
        if (colAlpha(rs.bg) != 0u) {
            bgIdx = emitFill(L, contentX, curY, contentW, 0, rs.bg, clip);
        }
        int rowH = 0;
        int k = 0;
        for (uint32_t c = dom.nodes[row].firstChild; c != kNoNode && k < ncols; c = dom.nodes[c].nextSibling) {
            if (c >= dom.nodes.size()) { break; }
            if (dom.nodes[c].isText) { continue; }
            uint8_t t = dom.nodes[c].tag;
            if (t != TAG_TD && t != TAG_TH) { continue; }
            int cw = (k == ncols - 1) ? (contentW - colW * (ncols - 1)) : colW;
            if (cw < 0) { cw = 0; }
            int h = layoutBlock(L, c, contentX + k * colW, curY, cw, clip, depth + 1);
            if (h > rowH) { rowH = h; }
            ++k;
        }
        if (bgIdx != kNoNode) {
            (*L.boxes)[bgIdx].h = rowH;
        }
        curY += rowH;
        if (curY - contentY > 1000000) { break; } // 病态内容防御
    }
    int h0 = curY - contentY;
    return h0 < 0 ? 0 : h0;
}

int layoutBlock(Layout& L, uint32_t node, int parentContentX, int y, int availW, const Rect& clip, int depth)
{
    if (node == kNoNode || node >= L.dom->nodes.size() || depth > kMaxDepth) {
        return 0;
    }
    const Style& s = (*L.styles)[node];
    if (s.disp == DISP_NONE) {
        return 0;
    }
    const DomNode& nd = L.dom->nodes[node];
    int bw = (int)(s.borderW + 0.5f);
    int padL = (int)(s.padL + 0.5f);
    int padR = (int)(s.padR + 0.5f);
    int padT = (int)(s.padT + 0.5f);
    int padB = (int)(s.padB + 0.5f);
    int mgT = (int)(s.mgT + 0.5f);
    int mgB = (int)(s.mgB + 0.5f);
    int mgL = (int)(s.mgL + 0.5f);
    int mgR = (int)(s.mgR + 0.5f);

    int contentW = resolveLen(s.wMode, s.w, availW);
    if (contentW < 0) {
        contentW = availW - mgL - mgR - padL - padR - 2 * bw;
        if (contentW < 0) { contentW = 0; }
    }
    int fixedH = resolveLen(s.hMode, s.h, availW);
    if (fixedH < 0) { fixedH = -1; }

    int borderX = parentContentX + mgL;
    int borderY = y + mgT;
    int contentX = borderX + bw + padL;
    int contentY = borderY + bw + padT;
    int borderBoxW = contentW + padL + padR + 2 * bw;

    // 子元素裁剪区 = 本盒的 padding box(CSS 的 overflow 裁剪边界), 与祖先裁剪区求交。
    // 这样后代不会画出父盒之外, 同时允许内容使用内边距区域(例如列表项目符号)。
    Rect inner = clip;
    inner.x0 = borderX + bw;
    inner.y0 = borderY + bw;
    inner.x1 = borderX + borderBoxW - bw;
    if (fixedH >= 0) {
        inner.y1 = contentY + fixedH + padB;
    }
    Rect childClip = rectIntersect(clip, inner);

    uint32_t bgIdx = kNoNode;
    if (colAlpha(s.bg) != 0u && borderBoxW > 0) {
        bgIdx = emitFill(L, borderX, borderY, borderBoxW, 0, s.bg, clip);
    }

    int contentH;
    if (nd.tag == TAG_TABLE) {
        contentH = layoutTable(L, node, contentX, contentY, contentW, childClip, depth + 1);
    } else {
        contentH = layoutContent(L, node, contentX, contentY, contentW, childClip, depth + 1);
    }
    int contentBoxH = (fixedH >= 0) ? fixedH : contentH;
    if (contentBoxH < 0) { contentBoxH = 0; }
    int borderBoxH = contentBoxH + padT + padB + 2 * bw;
    if (bgIdx != kNoNode) {
        (*L.boxes)[bgIdx].h = borderBoxH; // 背景位于全部子元素之前, 高度事后回填
    }
    if (bw > 0 && borderBoxW > 0 && borderBoxH > 0) {
        emitFill(L, borderX, borderY, borderBoxW, bw, s.borderColor, clip);
        emitFill(L, borderX, borderY + borderBoxH - bw, borderBoxW, bw, s.borderColor, clip);
        emitFill(L, borderX, borderY + bw, bw, borderBoxH - 2 * bw, s.borderColor, clip);
        emitFill(L, borderX + borderBoxW - bw, borderY + bw, bw, borderBoxH - 2 * bw, s.borderColor, clip);
    }
    if (nd.tag == TAG_LI) {
        emitFill(L, borderX - 10, contentY + 4, 5, 5, 0xFF556070u, clip); // 列表项目符号
    }
    int outerH = mgT + borderBoxH + mgB;
    if (outerH < 0) { outerH = 0; }
    return outerH;
}

// ---------------- 绘制 ----------------
void drawTextRun(Fb& fb, const Rect& clip, const char* t, uint32_t n, int x, int y, int lineH, float fontSize, uint32_t color)
{
    int sy = (int)(fontSize * 0.125f + 0.5f);
    if (sy < 1) { sy = 1; }
    if (sy > 8) { sy = 8; }
    int glyphH = 7 * sy;
    int top = y + (lineH - glyphH) / 2;
    if (top < y) { top = y; }
    int penX = x;
    for (uint32_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)t[i];
        int adv = advForChar(ch, fontSize);
        if (ch > 32 && ch < 127) {
            if (penX >= clip.x1) {
                break;
            }
            if (penX + adv > clip.x0) {
                int gi = (int)ch - 32;
                int sx = adv / 5;
                if (sx < 1) { sx = 1; }
                if (sx > 8) { sx = 8; }
                for (int row = 0; row < 7; ++row) {
                    uint8_t bits = kFont5x7[gi][row];
                    if (bits == 0) { continue; }
                    for (int col = 0; col < 5; ++col) {
                        if ((bits >> (4 - col)) & 1u) {
                            fbFill(fb, clip, penX + col * sx, top + row * sy, sx, sy, color);
                        }
                    }
                }
            }
        }
        penX += adv;
    }
}

void paintBoxes(Fb& fb, const std::vector<Box>& boxes, const Dom& dom)
{
    const size_t arenaSize = dom.arena.size();
    const char* arena = dom.arena.data();
    for (size_t i = 0; i < boxes.size(); ++i) {
        const Box& b = boxes[i];
        Rect clip(b.cx0, b.cy0, b.cx1, b.cy1);
        if (!rectValid(clip)) {
            continue;
        }
        if (b.kind == BOX_FILL) {
            fbFill(fb, clip, b.x, b.y, b.w, b.h, b.color);
            continue;
        }
        if (b.textLen == 0 || b.textOff >= arenaSize) {
            continue;
        }
        uint32_t n = b.textLen;
        if ((size_t)b.textOff + (size_t)n > arenaSize) {
            n = (uint32_t)(arenaSize - b.textOff);
        }
        drawTextRun(fb, clip, arena + b.textOff, n, b.x, b.y, b.h, b.fontSize, b.color);
    }
}

uint64_t countNonCanvasPixels(const Fb& fb, uint32_t canvas)
{
    uint64_t non = 0;
    const size_t total = (size_t)fb.w * (size_t)fb.h;
    for (size_t i = 0; i < total; ++i) {
        if (fb.px[i] != canvas) {
            ++non;
        }
    }
    return non;
}

// ---------------- 网页生成(HTML/CSS 全部在内存中拼字符串) ----------------
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int range(int n)
    {
        if (n <= 0) { return 0; }
        return (int)(next() % (uint32_t)n);
    }
};

static const char* const kWords[] = {
    "render", "layout", "engine", "pipeline", "cascade", "selector", "viewport", "fragment",
    "texture", "compositor", "glyph", "baseline", "margin", "padding", "border", "shadow",
    "gradient", "palette", "contrast", "opacity", "timeline", "latency", "throughput", "benchmark",
    "workload", "profile", "sampler", "raster", "vector", "spline", "cache", "buffer",
    "stream", "packet", "module", "kernel", "scheduler", "quantize", "entropy", "checksum",
    "payload", "request", "response", "channel", "cursor", "session", "token", "parser",
    "syntax", "semantic", "document", "element", "attribute", "surface", "shader", "cluster",
};
const int kWordCount = (int)(sizeof(kWords) / sizeof(kWords[0]));

void genWords(std::string& out, Rng& rng, int n)
{
    for (int i = 0; i < n; ++i) {
        const char* w = kWords[rng.range(kWordCount)];
        if (i == 0) {
            out += (char)(w[0] - 32);
            out += (w + 1);
        } else {
            out += ' ';
            out += w;
        }
    }
}

void genSentence(std::string& out, Rng& rng, int minWords, int maxWords)
{
    if (maxWords < minWords) { maxWords = minWords; }
    int n = minWords + rng.range(maxWords - minWords + 1);
    if (n < 1) { n = 1; }
    genWords(out, rng, n);
    out += '.';
}

void genParagraph(std::string& out, Rng& rng, int sentences)
{
    for (int i = 0; i < sentences; ++i) {
        if (i > 0) { out += ' '; }
        genSentence(out, rng, 6, 14);
    }
}

void genTitle(std::string& out, Rng& rng, int minWords, int maxWords)
{
    genWords(out, rng, minWords + rng.range(maxWords - minWords + 1));
}

// 站点样式表: 手写基础规则 + 生成的类规则(全部以 CSS 文本形式给出, 由渲染管线真实解析)
void appendBaseCss(std::string& css, Rng& rng)
{
    css += "body{margin:8px;padding:0;background-color:#f2f3f6;color:#222222;font-size:16px;line-height:1.35;}";
    css += "h1{font-size:32px;margin-top:10px;margin-bottom:12px;color:#10224a;}";
    css += "h2{font-size:24px;margin-top:12px;margin-bottom:8px;color:#16325c;}";
    css += "h3{font-size:19px;margin-top:10px;margin-bottom:6px;color:#204060;}";
    css += "p{margin-top:0;margin-bottom:9px;color:#333333;}";
    css += "a{color:#1155cc;}";
    css += ".hl{background-color:rgba(255,220,80,0.62);padding:1px;color:#402800;}";
    css += ".muted{color:rgba(90,100,120,0.85);font-size:13px;}";
    css += ".lead{font-size:18px;color:#1b2a41;line-height:1.5;}";
    css += ".header-bar{background-color:#12315e;color:#ffffff;padding:12px;border:2px solid #0a1f3c;}";
    css += ".header-bar h1{color:#ffffff;margin-top:0;margin-bottom:6px;}";
    css += ".subtitle{font-size:14px;color:#bcd0ea;}";
    css += ".nav{background-color:#1d4e89;padding:6px;margin-bottom:10px;}";
    css += ".nav-item{color:#dce8f7;margin-right:12px;font-size:14px;}";
    css += ".content{padding:4px;}";
    css += ".article{background-color:#ffffff;padding:10px;border:1px solid #ccd3de;}";
    css += ".sidebar{background-color:#eef1f6;padding:8px;border:1px solid #c6cede;}";
    css += ".panel{background-color:#ffffff;border:1px solid #d0d6e0;padding:6px;margin-bottom:8px;}";
    css += ".panel-title{font-size:18px;color:#0b3d91;margin-bottom:6px;}";
    css += ".card{background-color:#ffffff;border:1px solid #d8dee8;padding:6px;margin-bottom:6px;}";
    css += ".card-title{font-size:16px;color:#14315c;margin-bottom:4px;}";
    css += ".quote{background-color:#f6f7f9;border:1px solid #dde2ea;padding:6px;color:#40495a;}";
    css += ".kpi{font-size:22px;color:#0f5c3a;margin-bottom:4px;}";
    css += "ul{margin-top:6px;margin-bottom:6px;padding-left:26px;}";
    css += "ol{margin-top:6px;margin-bottom:6px;padding-left:30px;}";
    css += "li{margin-bottom:3px;}";
    css += ".footer{background-color:#20242c;color:#c8cfda;padding:10px;margin-top:12px;}";
    css += ".footer p{color:#c8cfda;}";
    css += "table.data{border:1px solid #b9c2d0;margin-top:6px;margin-bottom:10px;}";
    css += "td{border:1px solid #cfd6e2;font-size:14px;color:#2c333f;}";
    css += "th{border:1px solid #b9c2d0;background-color:#dde5f0;font-size:14px;color:#16325c;}";
    css += "tr.alt{background-color:#f0f4fa;}";
    css += "td.num{text-align:right;color:#123a6b;}";
    css += "caption{font-size:18px;color:#14315c;margin-bottom:6px;}";
    css += ".center{text-align:center;}";
    css += ".right{text-align:right;}";
    css += "#page-root{padding:4px;}";
    css += "#main .content p{line-height:1.42;}";
    css += "div.panel div.panel-title{color:#0b3d91;}";
    css += "div.card div.card-title{color:#1c3f6e;}";
    css += ".bar-track{background-color:#dde3ec;padding:2px;margin-bottom:4px;}";
    css += ".badge{font-size:12px;padding:2px;margin-right:4px;}";
    css += ".toc{background-color:#ffffff;border:1px solid #d5dbe5;padding:8px;}";
    css += ".toc-item{font-size:14px;color:#20456f;margin-bottom:3px;}";
    css += ".code-block{background-color:#f4f4f6;padding:8px;font-size:14px;color:#233043;border:1px solid #dfe3ea;}";
    css += ".stat{font-size:15px;color:#1b4a7a;margin-bottom:4px;}";
    char buf[256];
    for (int i = 0; i < 40; ++i) {
        int r = 30 + rng.range(190);
        int g = 30 + rng.range(190);
        int b = 30 + rng.range(190);
        int ap = 25 + rng.range(60);
        snprintf(buf, sizeof(buf),
                 ".badge-%d{background-color:rgba(%d,%d,%d,0.%02d);color:#ffffff;font-size:12px;padding:2px;margin-right:4px;}",
                 i, r, g, b, ap);
        css += buf;
    }
    for (int i = 0; i < 30; ++i) {
        snprintf(buf, sizeof(buf), ".bar-%d{background-color:#3b6fb6;height:%dpx;margin-bottom:2px;}", i, 6 + (i % 11));
        css += buf;
    }
    for (int i = 0; i < 24; ++i) {
        snprintf(buf, sizeof(buf), ".panel-%d{background-color:#ffffff;border:1px solid #d0d6e0;padding:%dpx;margin-bottom:%dpx;}",
                 i, 4 + (i % 5), 3 + (i % 7));
        css += buf;
    }
    for (int i = 0; i < 20; ++i) {
        int r = 190 + rng.range(60);
        int g = 195 + rng.range(55);
        int b = 200 + rng.range(50);
        snprintf(buf, sizeof(buf), ".row-%d{background-color:rgba(%d,%d,%d,0.%02d);}", i, r, g, b, 25 + rng.range(60));
        css += buf;
    }
}

void appendTable(std::string& h, Rng& rng, int rows, int cols, bool header)
{
    h += "<table class=\"data\">";
    if (header) {
        h += "<tr>";
        for (int c = 0; c < cols; ++c) {
            h += "<th>Column ";
            h += (char)('A' + (c % 26));
            h += "</th>";
        }
        h += "</tr>";
    }
    for (int r = 0; r < rows; ++r) {
        h += (r % 2) ? "<tr class=\"alt\">" : "<tr>";
        for (int c = 0; c < cols; ++c) {
            h += (c == cols - 1) ? "<td class=\"num\">" : "<td>";
            genSentence(h, rng, 2, 5);
            h += "</td>";
        }
        h += "</tr>";
    }
    h += "</table>";
}

// 页面 1: 新闻/博客(段落 + 内联元素 + 侧栏列表 + 条形图 + 数据表)
std::string makeNewsPage(uint32_t seed)
{
    Rng rng(seed * 2654435761u + 11u);
    std::string h;
    h.reserve(240000);
    h += "<!DOCTYPE html><html><head><title>Aurora Daily &amp; Reports</title>";
    h += "<meta charset=\"utf-8\"><style>";
    appendBaseCss(h, rng);
    h += "#page-root .sidebar .panel{padding:8px;}";
    h += ".article p{color:#333333;}";
    h += "</style></head><body><div id=\"page-root\" class=\"page\">";
    h += "<div class=\"header-bar\"><h1>Aurora Daily &amp; Reports</h1>";
    h += "<div class=\"subtitle\">Independent coverage of compute, science &amp; industry</div></div>";
    h += "<div class=\"nav\">";
    for (int i = 0; i < 12; ++i) {
        h += "<span class=\"nav-item\">";
        genTitle(h, rng, 1, 2);
        h += "</span>";
    }
    h += "</div><div class=\"content\"><div class=\"article\">";
    h += "<h2>";
    genTitle(h, rng, 5, 9);
    h += "</h2><p class=\"lead\">";
    genSentence(h, rng, 12, 18);
    h += "</p>";
    for (int i = 0; i < 46; ++i) {
        h += "<p>";
        genSentence(h, rng, 8, 16);
        h += " <b>";
        genSentence(h, rng, 3, 6);
        h += "</b> ";
        genSentence(h, rng, 6, 12);
        h += " <span class=\"hl\">";
        genSentence(h, rng, 3, 7);
        h += "</span> ";
        genSentence(h, rng, 5, 11);
        if ((i % 3) == 0) {
            h += " <i class=\"muted\">";
            genSentence(h, rng, 4, 8);
            h += "</i>";
        }
        if ((i % 11) == 5) {
            h += " <a href=\"#\">";
            genSentence(h, rng, 2, 4);
            h += "</a>";
        }
        h += "</p>";
        if ((i % 9) == 4) {
            h += "<blockquote>";
            genParagraph(h, rng, 2);
            h += "</blockquote>";
        }
    }
    h += "<h3>";
    genTitle(h, rng, 4, 7);
    h += "</h3>";
    appendTable(h, rng, 14, 5, true);
    h += "</div><div class=\"sidebar\">";
    for (int p = 0; p < 3; ++p) {
        h += "<div class=\"panel\"><div class=\"panel-title\">";
        genTitle(h, rng, 3, 5);
        h += "</div><ul>";
        for (int i = 0; i < 42; ++i) {
            h += "<li>";
            genSentence(h, rng, 4, 9);
            h += "</li>";
        }
        h += "</ul></div>";
    }
    h += "<div class=\"panel\"><div class=\"panel-title\">Trend</div><div class=\"bar-track\">";
    for (int i = 0; i < 40; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "<div class=\"bar-%d\"></div>", i % 30);
        h += buf;
    }
    h += "</div></div></div></div>";
    h += "<div class=\"footer\"><p class=\"muted\">";
    genSentence(h, rng, 10, 16);
    h += "</p></div></div></body></html>";
    return h;
}

// 页面 2: 数据报表(4 张宽表, 百分比宽度 + 斑马纹 + 右对齐数字列)
std::string makeTablePage(uint32_t seed)
{
    Rng rng(seed * 40503u + 7u);
    std::string h;
    h.reserve(240000);
    h += "<!DOCTYPE html><html><head><title>Aurora Data Tables</title><style>";
    appendBaseCss(h, rng);
    h += "table.data{width:96%;}";
    h += ".section-title{font-size:20px;color:#1b3a63;margin-top:14px;margin-bottom:6px;}";
    h += "</style></head><body><div id=\"page-root\">";
    h += "<div class=\"header-bar\"><h1>Aurora Data Tables</h1>";
    h += "<div class=\"subtitle\">Quarterly aggregates &amp; variance reports</div></div>";
    for (int t = 0; t < 4; ++t) {
        h += "<div class=\"section-title\">";
        genTitle(h, rng, 4, 7);
        h += "</div><table class=\"data\"><caption>";
        genSentence(h, rng, 3, 6);
        h += "</caption><tr>";
        for (int c = 0; c < 6; ++c) {
            h += "<th>";
            genTitle(h, rng, 1, 3);
            h += "</th>";
        }
        h += "</tr>";
        for (int r = 0; r < 22; ++r) {
            h += (r % 2) ? "<tr class=\"alt\">" : "<tr>";
            for (int c = 0; c < 6; ++c) {
                h += (c == 5) ? "<td class=\"num\">" : "<td>";
                genSentence(h, rng, 2, 4);
                h += "</td>";
            }
            h += "</tr>";
        }
        h += "</table>";
    }
    h += "<div class=\"footer\"><p class=\"muted\">";
    genParagraph(h, rng, 3);
    h += "</p></div></div></body></html>";
    return h;
}

// 页面 3: 长列表(徽章 + 文本 + 嵌套列表) 与一张汇总表
std::string makeListPage(uint32_t seed)
{
    Rng rng(seed * 2246822519u + 3u);
    std::string h;
    h.reserve(240000);
    h += "<!DOCTYPE html><html><head><title>Aurora Inventory Lists</title><style>";
    appendBaseCss(h, rng);
    h += ".inventory{background-color:#ffffff;border:1px solid #ccd3de;padding:10px;}";
    h += "ul li{line-height:1.3;}";
    h += "</style></head><body><div id=\"page-root\">";
    h += "<div class=\"header-bar\"><h1>Aurora Inventory</h1>";
    h += "<div class=\"subtitle\">Long list rendering benchmark &amp; nested structures</div></div>";
    h += "<div class=\"inventory\"><ul>";
    for (int i = 0; i < 220; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "<span class=\"badge-%d\">", i % 40);
        h += "<li>";
        h += buf;
        h += (char)('A' + (i % 26));
        h += (char)('0' + (i % 10));
        h += "</span> ";
        genSentence(h, rng, 5, 10);
        if ((i % 7) == 0) {
            h += " <span class=\"muted\">";
            genSentence(h, rng, 3, 6);
            h += "</span>";
        }
        h += "</li>";
    }
    h += "</ul></div>";
    h += "<div class=\"panel\"><div class=\"panel-title\">Nested groups</div><ul>";
    for (int i = 0; i < 60; ++i) {
        h += "<li>";
        genSentence(h, rng, 3, 6);
        h += "<ul><li>";
        genSentence(h, rng, 2, 5);
        h += "</li><li>";
        genSentence(h, rng, 2, 5);
        h += "</li></ul></li>";
    }
    h += "</ul></div>";
    h += "<div class=\"panel\"><div class=\"panel-title\">Summary</div>";
    appendTable(h, rng, 18, 4, true);
    h += "</div>";
    h += "<div class=\"footer\"><p class=\"muted\">";
    genSentence(h, rng, 10, 16);
    h += "</p></div></div></body></html>";
    return h;
}

// 页面 4: 仪表盘(深嵌套卡片 + 指标 + 条形图)
std::string makeDashboardPage(uint32_t seed)
{
    Rng rng(seed * 1103515245u + 17u);
    std::string h;
    h.reserve(240000);
    h += "<!DOCTYPE html><html><head><title>Aurora Dashboard</title><style>";
    appendBaseCss(h, rng);
    h += ".dash{background-color:#e9edf3;padding:6px;}";
    h += ".dash .panel-3{padding:8px;}";
    h += "</style></head><body><div id=\"page-root\">";
    h += "<div class=\"header-bar\"><h1>Aurora Dashboard</h1>";
    h += "<div class=\"subtitle\">Live metrics rendered from a synthetic document</div></div>";
    h += "<div class=\"dash\">";
    for (int p = 0; p < 36; ++p) {
        char buf[96];
        snprintf(buf, sizeof(buf), "<div class=\"panel-%d\"><div class=\"card-title\">", p % 24);
        h += buf;
        genTitle(h, rng, 3, 6);
        h += "</div><div class=\"kpi\">";
        genTitle(h, rng, 1, 2);
        h += "</div>";
        for (int q = 0; q < 3; ++q) {
            h += "<p>";
            genParagraph(h, rng, 2);
            h += "</p>";
        }
        h += "<div class=\"bar-track\">";
        for (int b = 0; b < 12; ++b) {
            snprintf(buf, sizeof(buf), "<div class=\"bar-%d\"></div>", (p + b) % 30);
            h += buf;
        }
        h += "</div>";
        if ((p % 4) == 0) {
            h += "<div class=\"card\"><div class=\"card-title\">Details</div><div class=\"card\">";
            h += "<div class=\"quote\">";
            genParagraph(h, rng, 2);
            h += "</div></div></div>";
        }
        if ((p % 6) == 3) {
            h += "<div class=\"stat\">";
            genSentence(h, rng, 5, 9);
            h += "</div>";
        }
        h += "</div>";
    }
    h += "</div><div class=\"footer\"><p class=\"muted\">";
    genParagraph(h, rng, 3);
    h += "</p></div></div></body></html>";
    return h;
}

// 页面 5: 技术文档(标题层级 + 目录 + 代码块 + 段落 + 多张表)
std::string makeDocsPage(uint32_t seed)
{
    Rng rng(seed * 69069u + 23u);
    std::string h;
    h.reserve(240000);
    h += "<!DOCTYPE html><html><head><title>Aurora Engine Manual</title><style>";
    appendBaseCss(h, rng);
    h += ".doc{background-color:#ffffff;padding:12px;border:1px solid #ccd3de;}";
    h += ".doc h2{color:#123156;}";
    h += ".toc-title{font-size:18px;color:#123156;margin-bottom:6px;}";
    h += "</style></head><body><div id=\"page-root\">";
    h += "<div class=\"header-bar\"><h1>Aurora Engine Manual</h1>";
    h += "<div class=\"subtitle\">Reference for the rendering pipeline &amp; document model</div></div>";
    h += "<div class=\"toc\"><div class=\"toc-title\">Contents</div>";
    for (int i = 0; i < 60; ++i) {
        h += "<div class=\"toc-item\">";
        genTitle(h, rng, 2, 5);
        h += "</div>";
    }
    h += "</div><div class=\"doc\">";
    for (int s = 0; s < 10; ++s) {
        char hb[16];
        snprintf(hb, sizeof(hb), "<h%d>", 2 + (s % 2));
        h += hb;
        genTitle(h, rng, 3, 7);
        h += (s % 2) ? "</h3>" : "</h2>";
        for (int i = 0; i < 7; ++i) {
            h += "<p>";
            genSentence(h, rng, 8, 15);
            h += " <code>";
            genTitle(h, rng, 1, 2);
            h += "</code> ";
            genSentence(h, rng, 6, 12);
            h += "</p>";
        }
        h += "<div class=\"code-block\">";
        genSentence(h, rng, 10, 18);
        h += "</div>";
        if ((s % 2) == 0) {
            appendTable(h, rng, 10, 4, true);
        }
        if ((s % 3) == 1) {
            h += "<pre>";
            genParagraph(h, rng, 3);
            h += "</pre>";
        }
    }
    h += "</div><div class=\"footer\"><p class=\"muted\">";
    genParagraph(h, rng, 3);
    h += "</p></div></div></body></html>";
    return h;
}

// 把 tasks 个互相独立的任务交给 gb7ParallelFor。
// gb7ParallelFor 的块粒度固定为 64: 直接以任务数作索引规模时, 少量任务(例如一页一个)
// 会全部落到单线程上。这里把索引空间放大 64 倍, 因块起止点始终是 64 的整数倍,
// 每个线程收到的区间都对齐到整任务边界 -> 任务级动态负载均衡。
// threads <= 1 时 gb7ParallelFor 直接以 body(0, tasks) 调用, 与串行循环一致。
// ---------------------------------------------------------------------------
// 站点外壳(2026-10-06 新增): 顶部导航 + 侧栏面板 + 页脚链接区
// ---------------------------------------------------------------------------
// 官方定义(ref/geekbench7-cpu-workloads.txt 第 35-40 行, 逐字):
//   "The HTML5 Browser workload renders web pages using a web browser. ... This workload
//    uses a headless browser to open, parse, lay out, and render web content from **eight
//    web pages modeled on popular websites** (e.g., Ars Technica, Instagram, and Wikipedia)."
// 官方真值(麒麟 9030 Pro / Mate 80 Pro Max, CS1 单核): **23.0 pages/s**。
// 我方同芯片真机: 32.17 ~ 34.36 pages/s -> 偏快 **1.40 ~ 1.49x**。
//
// 【差在哪】官方那 8 页是真实站点的内容模型: 每页都有顶部导航、侧栏、页脚链接区这些
// "站点外壳", DOM 节点数与文本量远高于本实现这 5 个合成页。本实现的 5 个页面**一个外壳
// 都没有**, 从 header-bar 直接进正文 —— 所以"我们的一页"比官方的一页轻。
// 本次按真实站点的公共骨架补齐三段(全部用已有的标签与 CSS 类, 由现有管线真解析/真排版/
// 真绘制, 没有一条是空转, 也没有新增任何"重复渲染同一页"的趟数):
//   * 顶部导航: 12 个 .nav-item(每个带一段生成的标题文本)
//   * 侧栏    : 14 个 .panel-N 面板, 每个 = panel-title + 一段 2 句的正文
//   * 页脚    : 8 条 .muted 句子
// 约 +60 个 DOM 节点 / +2.2 KB 文本 per page。
//
// 【预计新 metric】当前每页 2456.7 ms / 80 页 = 30.7 ms/页(真机 run 1791123841066-39691,
// metric 32.56)。新增量按"节点/文本量占比"估为 **+8% ~ +17%**:
//   -> 每页 33.2 ~ 35.9 ms, 80 页 **2.66 ~ 2.87 s**(仍在 1.5~3.0 s 窗口内, 留 4%~11% 余量)
//   -> 预计 **27.9 ~ 30.2 pages/s(改前 32.2~34.4), 对官方 23.0 约 1.21 ~ 1.31x**
//      (改前 1.40~1.49x, 已贴着 0.7~1.4 区间的上沿)。
//   如果真机实测超出 3.0 s: 唯一能用的线性旋钮 kFixedPages(80)是
//   verify_gb7_arrays.py 的断言值、不能改; 只能把上面三段再收窄(先砍侧栏面板数)。
//   口径不变: pages/s = 完成的整页渲染轮数 / 秒; k / conv / 单位文字 / 计分公式一个字未动。
// ---------------------------------------------------------------------------
void insertSiteChrome(std::string& h, uint32_t seed)
{
    Rng rng(seed * 2654435761u + 17u);
    std::string chunk;
    chunk.reserve(8192);
    char buf[96];
    chunk += "<div class=\"nav\">";
    for (int i = 0; i < 12; ++i) {
        chunk += "<span class=\"nav-item\">";
        genTitle(chunk, rng, 1, 3);
        chunk += "</span>";
    }
    chunk += "</div>";
    chunk += "<div class=\"sidebar\">";
    for (int i = 0; i < 14; ++i) {
        snprintf(buf, sizeof(buf), "<div class=\"panel-%d\"><div class=\"panel-title\">", i % 24);
        chunk += buf;
        genTitle(chunk, rng, 2, 5);
        chunk += "</div><p class=\"muted\">";
        genParagraph(chunk, rng, 2);
        chunk += "</p></div>";
    }
    chunk += "</div>";
    chunk += "<div class=\"footer\">";
    for (int i = 0; i < 8; ++i) {
        chunk += "<p class=\"muted\">";
        genSentence(chunk, rng, 4, 9);
        chunk += "</p>";
    }
    chunk += "</div>";
    const size_t at = h.rfind("</body>");
    if (at == std::string::npos) {
        h += chunk;
        return;
    }
    h.insert(at, chunk);
}

inline void gb7BrParTasks(int threads, long long tasks, const std::function<void(long long, long long)>& body)
{
    if (tasks <= 0) {
        return;
    }
    gb7ParallelFor(threads, tasks * 64, [&body](long long s, long long e) {
        body(s / 64, e / 64);
    });
}

} // namespace

Gb7Outcome gb7RunHtml5Browser(int threads)
{
    if (threads < 1) { threads = 1; }
    Gb7Outcome o;
    o.name = "HTML5 Browser";
    o.section = "Productivity";
    o.unit = "pages/s";
    o.score = 0.0;

    // 5 个结构完整的网页(HTML 文本 + <style> 样式表全部在内存中生成)
    std::vector<std::string> pages;
    pages.reserve(5);
    pages.push_back(makeNewsPage(1u));
    pages.push_back(makeTablePage(2u));
    pages.push_back(makeListPage(3u));
    pages.push_back(makeDashboardPage(4u));
    pages.push_back(makeDocsPage(5u));
    // 站点外壳: 见上方 insertSiteChrome 的说明(官方那 8 页是真实站点, 每页都有导航/侧栏/页脚)
    for (size_t i = 0; i < pages.size(); ++i) {
        insertSiteChrome(pages[i], (uint32_t)(i + 1));
    }
    const int pageCount = (int)pages.size();

    const Rect pageRect(0, 0, kFbW, kFbH);

    // 每个并行工作槽持有自己的一整套渲染状态: DOM / CSS / 层叠样式 / 显示列表 +
    // 独立帧缓冲(1024x768x4 = 3MB)。页面之间没有任何共享可写数据, 因此可以整页并行。
    struct PageSlot {
        std::vector<uint32_t> fb;
        Fb view;
        Dom dom;
        std::string css;
        std::vector<Rule> rules;
        std::vector<Style> styles;
        std::vector<Box> boxes;
    };
    int useThreads = threads;
    if (useThreads > 8) { useThreads = 8; } // 每槽 3MB 帧缓冲, 限制内存增量
    std::vector<PageSlot> slots((size_t)useThreads);
    for (int t = 0; t < useThreads; ++t) {
        PageSlot& S = slots[(size_t)t];
        S.fb.assign((size_t)kFbW * (size_t)kFbH, 0xFFFFFFFFu);
        S.view.px = S.fb.data();
        S.view.w = kFbW;
        S.view.h = kFbH;
        S.rules.reserve(kMaxRules);
        S.boxes.reserve(16384);
    }

    uint64_t acc = 0;
    uint64_t pixels = 0;   // 累计非背景像素(每页统计)
    int rendered = 0;
    double pageMsAcc = 0.0; // 累计单页渲染耗时
    // ======================== 工作量标定(首次真机复核后) ========================
    // metric 口径(明确写死, 便于人工核对): pages/s = 实际完成的整页渲染轮数 / 秒数。
    //   一轮 = 该页完整走一遍 parseHtml -> parseStyleSheet -> resolveStyles -> 清屏 ->
    //   layoutBlock -> paintBoxes -> 像素统计, 与 GB7 的"渲染一页"同义。
    // 调整前: 旧实现是"1500 ms 预算内尽量多渲染页"(while + budgetMs 判据), 于是 pages/s
    //   直接取决于机器快慢 —— 它对不同机型的可比性依赖于"预算本身固定", 语义上不是
    //   固定工作量, 真机实测也就落在 ~32 pages/s(0.9~1.5 s 区间边缘, 且有测试开销波动)。
    // 调整: 改成固定页数 kFixedPages = 56。工作量对所有设备完全相同, pages/s 与
    //   GB7 的同义(固定份工作 / 耗时); 同时保留一个只做"兜底保护"的墙钟上限
    //   kMaxWallMs = 4500 ms —— 它不是工作量定义, 而是防止极慢设备把页面卡住不返回
    //   (触发时 pages/s 会因分子分母同倍缩小而保持正确, 不会虚增)。
    //   -> 本机预计 56 / ~2.1 s ≈ 26.7 pages/s, 对 GB7 参考 23.0 pages/s 约 1.16x
    //      (调整前约 1.41x), 两者都在 0.5~2x 区间内。
    // 页数 56 -> **80**(2026-10-04 第三次真机复核):
    //   本轮真机实测(CS1 单核阶段第 11 项, runlog.jsonl run 1791098115489-82335):
    //     56 页, o.ms = **1630.0 ms**, metric 34.36 pages/s(自洽: 56 / 1.630 = 34.36 ✓)。
    //   单页成本 = 1630.0 / 56 = 29.11 ms/页(每页都是完整的
    //     parseHtml -> parseStyleSheet -> resolveStyles -> 清屏 -> layoutBlock ->
    //     paintBoxes -> 像素统计, 页与页之间没有任何缓存 —— 同一份 5 个页面轮流
    //     重跑全管线, 所以第 57 页和第 1 页做的工作完全相同, 总耗时与页数严格成正比)。
    //   80 x 29.11 ms = **2329 ms**, 落在 1.5~3.0 s 区间中部。
    //   加的是真实的整页渲染轮数(更多次 HTML 解析/CSS 层叠/盒模型排版/像素绘制),
    //   不是"把同一页数成多页": metric 的分子就是实际完成的整页轮数。
    //   吞吐不变: pages/s = 页数 / 秒 -> 仍 ≈ 34.4 pages/s。
    //   墙钟上限: 2329 ms 远低于下面的 4500 ms 兜底值, 正常不会被触发。
    const int kFixedPages = 80;
    const double kMaxWallMs = 4500.0; // 仅兜底, 不参与工作量定义
    const int maxPasses = kFixedPages;

    // 单页渲染: 管线与改造前逐行一致(解析 -> CSS -> 层叠 -> 清屏 -> 布局 -> 绘制 -> 统计),
    // 只是把共享状态换成该工作槽自己的状态; 统计结果写到调用方给的本槽输出槽位。
    auto renderOnePage = [&](PageSlot& S, int pageIndex, uint64_t& outPixels, uint64_t& outAcc, double& outMs) {
        double pageT0 = nowMsBr();
        const std::string& src = pages[(size_t)(pageIndex % pageCount)];

        parseHtml(src, S.dom, S.css);                        // 1. 重新解析 HTML -> DOM(无缓存)
        S.rules.clear();
        parseStyleSheet(S.css.data(), S.css.size(), S.rules); // 2. 重新解析 CSS 规则
        resolveStyles(S.dom, S.rules, S.styles);              // 3. 重新做层叠/继承

        uint32_t canvas = 0xFFFFFFFFu;                        // 背景色由 body 传播到画布
        if (S.dom.layoutRoot != kNoNode && S.dom.layoutRoot < S.styles.size()) {
            uint32_t bg = S.styles[S.dom.layoutRoot].bg;
            if (colAlpha(bg) != 0u) {
                canvas = 0xFF000000u | (bg & 0x00FFFFFFu);
            }
        }
        fbFill(S.view, pageRect, 0, 0, kFbW, kFbH, canvas);   // 4. 每页清空帧缓冲

        S.boxes.clear();
        Layout L;
        L.dom = &S.dom;
        L.styles = &S.styles;
        L.boxes = &S.boxes;
        L.dropped = 0;
        int pageHeight = layoutBlock(L, S.dom.layoutRoot, 0, 0, kFbW, pageRect, 0); // 5. 盒模型/行内排版
        paintBoxes(S.view, S.boxes, S.dom);                   // 6. 绘制(alpha 混合 + 裁剪)

        outPixels = countNonCanvasPixels(S.view, canvas);      // 每页像素统计
        outMs = nowMsBr() - pageT0;                            // 每页渲染耗时
        outAcc = outPixels + (uint64_t)pageHeight + (uint64_t)S.boxes.size()
               + (uint64_t)S.dom.nodes.size() + (uint64_t)L.dropped;
    };

    std::vector<uint64_t> slotPixels((size_t)useThreads, 0);
    std::vector<uint64_t> slotAcc((size_t)useThreads, 0);
    std::vector<double> slotMs((size_t)useThreads, 0.0);

    std::clock_t cpu0 = std::clock();
    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    double t0 = nowMsBr();
    while (rendered < maxPasses) {
        if (nowMsBr() - t0 >= kMaxWallMs) {
            break;   // 见上方"工作量标定": 兜底保护, 不是工作量定义
        }
        // 每批 useThreads 页: 第 k 页用第 k 个工作槽、渲染页号 rendered+k,
        // 因此 pages[pageIndex % pageCount] 的顺序与串行完全一致。
        long long batch = useThreads;
        if (batch > (long long)(maxPasses - rendered)) {
            batch = (long long)(maxPasses - rendered);
        }
        const int baseIndex = rendered;
        gb7BrParTasks((int)batch, batch, [&](long long s, long long e) {
            for (long long k = s; k < e; ++k) {
                renderOnePage(slots[(size_t)k], baseIndex + (int)k,
                              slotPixels[(size_t)k], slotAcc[(size_t)k], slotMs[(size_t)k]);
            }
        });
        // 归约在并行区外串行做(整数和/浮点和的合并顺序与原实现一致)
        for (long long k = 0; k < batch; ++k) {
            pixels += slotPixels[(size_t)k];
            acc += slotAcc[(size_t)k];
            pageMsAcc += slotMs[(size_t)k];
        }
        rendered += (int)batch;
    }
    double t1 = nowMsBr();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)
    double wallMs = t1 - t0;
    double cpuMs = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
    // 汇总校验值, 防止渲染结果被优化掉
    volatile uint64_t sink = acc + pixels + (uint64_t)(pageMsAcc * 1000.0) + (uint64_t)rendered;
    (void)sink;

    if (rendered < 1) {
        rendered = 1;
    }
    double seconds = wallMs / 1000.0;
    if (!(seconds > 0.0)) {
        seconds = 0.000001;
    }
    double pps = (double)rendered / seconds;
    char buf[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf, sizeof(buf), "%.4g", pps);
    o.ms = wallMs;
    o.metric = buf;
    o.parallelism = gb7Parallelism(cpuMs, wallMs);
    return o;
}
