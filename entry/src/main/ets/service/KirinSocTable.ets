// ---------------------------------------------------------------------------
// 华为麒麟 (Kirin) SoC 对照表 + 判读算法 —— ArkTS 移植
//
// 数据来源: ref/kirin_socs.json 与 ref/KIRIN_SOCS.md(由 ref/_build_kirin.py 生成, 可复现);
// 算法来源: ref/_build_kirin.py 的 identify() / _match() / soc_class(), 逐行移植
//          (自测向量见 ref/kirin_identify_selftest.py, 移植后的对照见 ref/KIRIN_APP_INTEGRATION.md)。
//
//  必须先读这一条(所有界面文案都受它约束)
//   华为【从来没有】公布过任何一颗麒麟 SoC 的 CPU 核心布局。ref/kirin_socs.json 的 26 条里
//   confidence: official = 0 条 —— 每一个核数、每一个频率、每一个 SMT 结论都是第三方推断
//   (Notebookcheck / 快科技《Huawei手机处理器规格表》/ 极客湾评测转述 / Geekbench 数据库)。
//   因此本文件产出的任何型号都只能用"疑似 / 推断"措辞, 并且必须与"本机实测"分成两行显示。
//   另有 8 个条目(9020A/9020B/9010S/9035/9050/9050 Pro/T93 系列/X90 系列)连一个核心数都没有来源,
//   对它们任何核数都不可能判读 —— 这时只允许显示系统自报的芯片字符串。
//
// 指纹口径(与 _build_kirin.py 完全一致):
//   指纹 = (核数, {频率档 -> 该档核个数}); 两种口径各跑一遍(logical / physical);
//   频率档相对容差 1%(不是 5%): 9000S 小核 1.53GHz 与 9020 小核 1.60GHz 只差 4.6%,
//   5% 容差会把这两颗完全不同的芯片判成同一个。
// ---------------------------------------------------------------------------

// ---- 频率档: ghz = 该档频率(GHz), count = 该档核数 ----
// 两个字段都用 -1 表示"资料里未查到"(不用 null: ArkTS 侧一律按数字处理, 少一类空值分支)。
export interface KirinFreqTier {
  ghz: number;
  count: number;
}

// ---- 一条指纹 ----
export interface KirinSignature {
  soc: string;      // 资料里的 SoC 名(第三方叫法, 非华为官方命名)
  cores: number;    // 该口径下的核数
  smt: boolean;     // 资料是否说它支持超线程
  tiers: KirinFreqTier[];
  src: string;      // 出处 id(与 ref/kirin_socs.json 的 sourceIndex 一一对应)
}

// ---- SoC 元信息(用于平局破除与展示) ----
export interface KirinSocMeta {
  name: string;
  devClass: string;    // phone / tablet / pc / unknown —— 由资料里的已知机型推导
  confidence: string;  // thirdparty = 只有第三方出处; none = 连核心数都没有来源
  cores: number;       // 物理核数; -1 = 未查到
  threads: number;     // 逻辑核数; -1 = 未查到
  smt: number;         // 1 = 是 / 0 = 否 / -1 = 未知
  gpu: string;
}

export const CHIP_GUESS_TAG_THIRD_PARTY: string = '第三方资料推断，非官方';
export const CHIP_GUESS_SOURCE_LEGEND: string =
  '第三方资料（Notebookcheck / 快科技《Huawei手机处理器规格表》/ 极客湾评测转述 / Geekbench 7 数据库）—— 华为从未公布过任何麒麟 SoC 的 CPU 核心布局';

// 资料里 sourceIndex 的 id -> 人读的出处名(只列本文件用到的)。
// 用"数组 + 线性查找"而不是 Record<string,string>: ArkTS 的索引访问会返回非空类型,
// 再判 undefined 容易被静态检查当成恒假分支, 这里干脆避开这类分支。
interface KirinSourceTitle {
  id: string;
  title: string;
}

const SRC_TITLES: KirinSourceTitle[] = [
  { id: 'mydr_gkw', title: '快科技转述极客湾 Mate 80 Pro 评测' },
  { id: 'nbc_9030pro', title: 'Notebookcheck Kirin 9030 Pro' },
  { id: 'nbc_9030', title: 'Notebookcheck Kirin 9030' },
  { id: 'nbc_cn9030', title: 'Notebookcheck 中文站 麒麟9030 Geekbench 首曝' },
  { id: 'cb_9030pro', title: 'ComputerBase Kirin 9030 Pro' },
  { id: 'gb7_sgt', title: 'Geekbench 7 SGT-AL00 (Mate 80 Pro)' },
  { id: 'gb7_cmu', title: 'Geekbench 7 CMU-AL10' },
  { id: 'mydr_mate90', title: '快科技: Mate 90 系列芯片敲定' },
  { id: 'nbc_9020', title: 'Notebookcheck Kirin 9020' },
  { id: 'nbc_9010', title: 'Notebookcheck Kirin 9010' },
  { id: 'nbc_9000w', title: 'Notebookcheck Kirin 9000W' },
  { id: 'nbc_9000', title: 'Notebookcheck Kirin 9000' },
  { id: 'nbc_9000e', title: 'Notebookcheck Kirin 9000E' },
  { id: 'nbc_8020', title: 'Notebookcheck Kirin 8020' },
  { id: 'nbc_t92c', title: 'Notebookcheck Kirin T92C' },
  { id: 'nbc_t92', title: 'Notebookcheck Kirin T92' },
  { id: 'nbc_t82', title: 'Notebookcheck Kirin T82' },
  { id: 'nbc_xe90', title: 'Notebookcheck Kirin XE90' },
  { id: 'ithome_9000s', title: 'IT之家: 麒麟9000S 8核12线程' },
  { id: 'sohu_tianti', title: '搜狐《性能天梯》: 麒麟9000S 1+3+4' },
  { id: 'mydr_tianti', title: '快科技《Huawei手机处理器规格表》' },
  { id: 'mydr_8000a', title: '快科技: 麒麟8000A 首发揭秘' },
  { id: 'mgsm_cmu', title: 'mGSM.pl(未核实)' }
];

export function srcTitle(id: string): string {
  for (const s of SRC_TITLES) {
    if (s.id === id) {
      return s.title;
    }
  }
  return id;
}

// ---------------------------------------------------------------------------
// 逻辑核口径指纹(核数 = 线程数, 如 /proc/cpuinfo / os 层线程数)
// ---------------------------------------------------------------------------
export const LOGICAL_SIGNATURES: KirinSignature[] = [
  { soc: 'Kirin 9030 Pro', cores: 14, smt: true, src: 'mydr_gkw nbc_9030pro cb_9030pro',
    tiers: [{ ghz: 2.750, count: 2 }, { ghz: 2.270, count: 8 }, { ghz: 1.720, count: 4 }] },
  { soc: 'Kirin 9035', cores: 14, smt: true, src: 'mydr_mate90',
    tiers: [{ ghz: 2.850, count: -1 }] },
  { soc: 'Kirin 9030', cores: 12, smt: true, src: 'nbc_9030',
    tiers: [{ ghz: 2.750, count: 2 }, { ghz: 2.270, count: 6 }, { ghz: 1.720, count: 4 }] },
  { soc: 'Kirin 9020', cores: 12, smt: true, src: 'nbc_9020 cb_9030pro',
    tiers: [{ ghz: 2.500, count: 2 }, { ghz: 2.151, count: 6 }, { ghz: 1.600, count: 4 }] },
  { soc: 'Kirin 9010', cores: 12, smt: true, src: 'nbc_9010',
    tiers: [{ ghz: 2.300, count: 2 }, { ghz: 2.180, count: 6 }, { ghz: 1.550, count: 4 }] },
  { soc: 'Kirin 9000W', cores: 12, smt: true, src: 'nbc_9000w',
    tiers: [{ ghz: 2.487, count: 2 }, { ghz: 2.150, count: 6 }, { ghz: 1.530, count: 4 }] },
  { soc: 'Kirin 9000S', cores: 12, smt: true, src: 'ithome_9000s sohu_tianti',
    tiers: [{ ghz: 2.620, count: 2 }, { ghz: 2.150, count: 6 }, { ghz: 1.530, count: 4 }] },
  { soc: 'Kirin 9000 / 9000E', cores: 8, smt: false, src: 'nbc_9000 nbc_9000e',
    tiers: [{ ghz: 3.130, count: 1 }, { ghz: 2.540, count: 3 }, { ghz: 2.050, count: 4 }] },
  { soc: 'Kirin 8000', cores: 8, smt: false, src: 'mydr_tianti mydr_8000a',
    tiers: [{ ghz: 2.400, count: 1 }, { ghz: 2.189, count: 3 }, { ghz: 1.840, count: 4 }] },
  { soc: 'Kirin 8000A', cores: 6, smt: false, src: 'mydr_8000a',
    tiers: [{ ghz: 2.189, count: 3 }, { ghz: 1.840, count: 3 }] },
  { soc: 'Kirin 8020', cores: 12, smt: false, src: 'nbc_8020',
    tiers: [{ ghz: 2.285, count: 2 }, { ghz: 2.050, count: 6 }, { ghz: 1.306, count: 4 }] },
  { soc: 'Kirin T92C', cores: 9, smt: false, src: 'nbc_t92c',
    tiers: [{ ghz: 2.300, count: 2 }, { ghz: 2.000, count: 4 }, { ghz: 1.600, count: 3 }] },
  { soc: 'Kirin T92', cores: 12, smt: false, src: 'nbc_t92',
    tiers: [{ ghz: 2.500, count: -1 }, { ghz: 1.600, count: -1 }] },
  { soc: 'Kirin T82', cores: 12, smt: false, src: 'nbc_t82',
    tiers: [{ ghz: 2.285, count: -1 }, { ghz: 1.306, count: -1 }] }
];

// ---------------------------------------------------------------------------
// 物理核口径指纹(核数 = 物理核数, 如 Geekbench 的 "1 Processor, N Cores")
// 由 ref/_build_kirin.py 的 build_physical_signatures() 从 SOCS 的 clusters 自动推导
// ---------------------------------------------------------------------------
export const PHYSICAL_SIGNATURES: KirinSignature[] = [
  { soc: 'Kirin 9050 Pro', cores: 9, smt: true, src: 'gb7_cmu mydr_mate90',
    tiers: [{ ghz: 3.100, count: 1 }, { ghz: 2.700, count: 2 }, { ghz: 2.200, count: 4 }, { ghz: 1.750, count: 2 }] },
  { soc: 'Kirin 9035', cores: 9, smt: true, src: 'mydr_mate90',
    tiers: [{ ghz: 2.850, count: 1 }, { ghz: -1, count: 4 }, { ghz: -1, count: 4 }] },
  { soc: 'Kirin 9030 Pro', cores: 9, smt: true, src: 'mydr_gkw nbc_9030pro cb_9030pro gb7_sgt',
    tiers: [{ ghz: 2.750, count: 1 }, { ghz: 2.270, count: 4 }, { ghz: 1.720, count: 4 }] },
  { soc: 'Kirin 9030', cores: 8, smt: true, src: 'nbc_9030 nbc_cn9030',
    tiers: [{ ghz: 2.750, count: 1 }, { ghz: 2.270, count: 3 }, { ghz: 1.720, count: 4 }] },
  { soc: 'Kirin 9030S', cores: 8, smt: false, src: 'mydr_tianti',
    tiers: [{ ghz: 2.690, count: 1 }, { ghz: -1, count: 3 }, { ghz: -1, count: 4 }] },
  { soc: 'Kirin 9020', cores: 8, smt: true, src: 'nbc_9020 cb_9030pro',
    tiers: [{ ghz: 2.500, count: 1 }, { ghz: 2.151, count: 3 }, { ghz: 1.600, count: 4 }] },
  { soc: 'Kirin 9010', cores: 8, smt: true, src: 'nbc_9010 mydr_tianti',
    tiers: [{ ghz: 2.300, count: 1 }, { ghz: 2.180, count: 3 }, { ghz: 1.550, count: 4 }] },
  { soc: 'Kirin 9000W', cores: 8, smt: true, src: 'nbc_9000w',
    tiers: [{ ghz: 2.487, count: 1 }, { ghz: 2.150, count: 3 }, { ghz: 1.530, count: 4 }] },
  { soc: 'Kirin 9000S', cores: 8, smt: true, src: 'ithome_9000s sohu_tianti',
    tiers: [{ ghz: 2.620, count: 1 }, { ghz: 2.150, count: 3 }, { ghz: 1.530, count: 4 }] },
  { soc: 'Kirin 9000E', cores: 8, smt: false, src: 'nbc_9000e',
    tiers: [{ ghz: 3.130, count: 1 }, { ghz: 2.540, count: 3 }, { ghz: 2.050, count: 4 }] },
  { soc: 'Kirin 9000', cores: 8, smt: false, src: 'nbc_9000',
    tiers: [{ ghz: 3.130, count: 1 }, { ghz: 2.540, count: 3 }, { ghz: 2.050, count: 4 }] },
  { soc: 'Kirin 8020', cores: 12, smt: false, src: 'nbc_8020 mydr_tianti',
    tiers: [{ ghz: 2.285, count: 2 }, { ghz: 2.050, count: 6 }, { ghz: 1.306, count: 4 }] },
  { soc: 'Kirin 8000', cores: 8, smt: false, src: 'mydr_tianti mydr_8000a',
    tiers: [{ ghz: 2.400, count: 1 }, { ghz: 2.189, count: 3 }, { ghz: 1.840, count: 4 }] },
  { soc: 'Kirin 8000A', cores: 6, smt: false, src: 'mydr_8000a',
    tiers: [{ ghz: 2.189, count: 3 }, { ghz: 1.840, count: 3 }] },
  { soc: 'Kirin XE90', cores: 9, smt: true, src: 'nbc_xe90',
    tiers: [{ ghz: 2.750, count: 1 }, { ghz: 2.270, count: 4 }, { ghz: 1.720, count: 4 }] }
];

// ---------------------------------------------------------------------------
// SoC 元信息(26 条; cores/threads/smt 的 -1 一律表示"资料里未查到", 不是 0)
//
// devClass 由资料里的已知机型推导(phone / tablet / pc)。 资料里的 soc_class() 会把
// Kirin XE90 归成 phone, 因为它的机型条目写的是"华为鸿蒙笔记本"而没写 MateBook ——
// 这里按资料正文("XE90 = 9030 Pro 的笔记本版")手工订正为 pc, 并在文档里写明。
// ---------------------------------------------------------------------------
const SOC_META_RAW: string[] = [
  'Kirin 9050 Pro|phone|thirdparty|9|-1|1|Maleoon 955',
  'Kirin 9050|phone|none|-1|-1|-1|未查到',
  'Kirin 9035|phone|thirdparty|9|14|1|Maleoon 935',
  'Kirin 9030 Pro|phone|thirdparty|9|14|1|Maleoon 935',
  'Kirin 9030|phone|thirdparty|8|12|1|Maleoon 935',
  'Kirin 9030S|phone|thirdparty|8|-1|-1|Maleoon 935F',
  'Kirin 9020|phone|thirdparty|8|12|1|Maleoon 920',
  'Kirin 9020A|phone|none|-1|-1|-1|未查到',
  'Kirin 9020B|phone|none|-1|-1|-1|未查到',
  'Kirin 9010S|phone|none|-1|-1|-1|未查到',
  'Kirin 9010|phone|thirdparty|8|12|1|Maleoon 910',
  'Kirin 9000W|tablet|thirdparty|8|12|1|Maleoon 910',
  'Kirin 9000S|phone|thirdparty|8|12|1|Maleoon 910',
  'Kirin 9000E|phone|thirdparty|8|8|0|Mali-G78 MP22',
  'Kirin 9000|phone|thirdparty|8|8|0|Mali-G78 MP24',
  'Kirin 8020|phone|thirdparty|12|12|0|Maleoon 920c / 920(来源不一致)',
  'Kirin 8000|phone|thirdparty|8|8|0|Mali-G610 MP4',
  'Kirin 8000A|phone|thirdparty|6|6|0|Mali-G610',
  'Kirin T93 Pro|tablet|none|-1|-1|-1|未查到',
  'Kirin T93|tablet|none|-1|-1|-1|未查到',
  'Kirin T93B / T93C|tablet|none|-1|-1|-1|未查到',
  'Kirin T92 / T92C|tablet|thirdparty|12|12|0|Maleoon 920',
  'Kirin T82 / T82B|tablet|thirdparty|12|12|0|Maleoon 920c',
  'Kirin X90 / X90A|tablet|none|-1|-1|-1|未查到',
  'Kirin X90 Plus|pc|none|-1|-1|-1|未查到',
  'Kirin XE90|pc|thirdparty|9|14|1|Maleoon 935'
];

function toInt(s: string): number {
  return parseInt(s, 10);
}

export const SOC_META: KirinSocMeta[] = SOC_META_RAW.map((line: string): KirinSocMeta => {
  const f: string[] = line.split('|');
  return {
    name: f[0], devClass: f[1], confidence: f[2],
    cores: toInt(f[3]), threads: toInt(f[4]), smt: toInt(f[5]), gpu: f[6]
  };
});

// 按资料里的已知机型归类(与 _build_kirin.py 的 soc_class 同义; 查不到按 unknown)
export function socClass(name: string): string {
  for (const m of SOC_META) {
    if (m.name === name || name.startsWith(m.name) || m.name.startsWith(name)) {
      return m.devClass;
    }
  }
  return 'unknown';
}

export function socMetaOf(name: string): KirinSocMeta | null {
  for (const m of SOC_META) {
    if (m.name === name) {
      return m;
    }
  }
  return null;
}

// 资料里明确写出来的"指纹相同、分不开"的组合(判读文案要指名道姓地说为什么分不开)
const KNOWN_TIE_NOTE: string[] = [
  'Kirin 9030 Pro|Kirin XE90|两者指纹完全相同(9 物理核 + 2750/2270/1720): 资料里 XE90 被写成 9030 Pro 的笔记本版, CPU 侧没有任何差异 —— 只能用设备形态(手机 / 平板 / PC)破除',
  'Kirin 9000|Kirin 9000E|两者只差 GPU 核心数(Mali-G78 MP24 vs MP22), CPU 核数与频率完全相同 —— 只靠 CPU 指纹无法区分',
  'Kirin 9000|Kirin 9000 / 9000E|同上: 9000 与 9000E 的 CPU 侧完全一样',
  'Kirin 9000E|Kirin 9000 / 9000E|同上: 9000 与 9000E 的 CPU 侧完全一样',
  'Kirin T92|Kirin T92 / T92C|资料把 T92 与 T92C 合写在一条里, 只有核数(12 vs 9)可区分',
  'Kirin T92C|Kirin T92 / T92C|同上',
  'Kirin T82|Kirin T82 / T82B|资料把 T82 与 T82B 合写在一条里, 频率未分簇',
  'Kirin XE90|Kirin 9030 Pro|两者指纹完全相同(9 物理核 + 2750/2270/1720), 资料里 XE90 是 9030 Pro 的笔记本版 —— 只能用设备形态破除'
];

export function tieNoteFor(a: string, b: string): string {
  for (const line of KNOWN_TIE_NOTE) {
    const f: string[] = line.split('|');
    if ((f[0] === a && f[1] === b) || (f[0] === b && f[1] === a)) {
      return f[2];
    }
  }
  return '';
}

// ---------------------------------------------------------------------------
// 判读结果
// ---------------------------------------------------------------------------
export interface KirinCandidate {
  name: string;
  score: number;
  smt: boolean;
  devClass: string;
  modes: string[];     // 命中的口径: logical / physical
  why: string[];       // 逐条判据(资料原文口径)
  src: string;
}

export interface KirinIdentifyResult {
  inputCores: number;
  freqTiersGhz: number[];
  tierHistogram: string;   // 形如 "2750MHz×2 / 2270MHz×8 / 1720MHz×4"
  candidates: KirinCandidate[];
  warnings: string[];
  dataSufficient: boolean;
  structuralHint: string;
}

// 频率按 3 位小数归整 —— 与 Python 的 round(f, 3) 同义。
// (本 App 的频率一律来自 kHz 整数, 3 位小数是精确表示, 不存在半分位舍入分歧。)
function roundGhz(f: number): number {
  return Math.round(f * 1000) / 1000;
}

// 把 GHz 列表聚成档 —— 与 _build_kirin.py 的 identify() 完全一致:
//   档 = round(f, 3) 的精确去重(这里不加任何容差; 容差只用在 _match 里找"第一个命中的档"),
//   档的插入顺序 = freqs 的遍历顺序(不排序), 因为 _match 取的是"第一个命中的档"。
function buildTierMap(freqs: number[]): number[] {
  const out: number[] = [];
  for (const f of freqs) {
    if (!(f > 0)) {
      continue;
    }
    const r: number = roundGhz(f);
    let found: boolean = false;
    for (const t of out) {
      if (t === r) {
        found = true;
        break;
      }
    }
    if (!found) {
      out.push(r);
    }
  }
  return out;
}

// 落在某个档(精确键相等)上的核数 —— 对应 Python 的 tiers[hit[0]]
function countInTier(freqs: number[], tier: number): number {
  let n: number = 0;
  for (const f of freqs) {
    if (!(f > 0)) {
      continue;
    }
    if (roundGhz(f) === tier) {
      n++;
    }
  }
  return n;
}

// 与 _build_kirin.py 的 _match() 逐行同源
function matchSignatures(sigs: KirinSignature[], n: number, freqs: number[],
                         tiers: number[], tol: number, mode: string): KirinCandidate[] {
  const res: KirinCandidate[] = [];
  for (const sig of sigs) {
    if (sig.cores !== n) {
      continue;
    }
    let score: number = 2;
    const why: string[] = ['核数完全一致(' + n.toString() + ')'];
    for (const t of sig.tiers) {
      if (t.ghz < 0) {
        continue;
      }
      const mhz: number = Math.round(t.ghz * 1000);
      let hit: number = -1;
      for (const cand of tiers) {
        if (Math.abs(cand - t.ghz) <= t.ghz * tol) {
          hit = cand;
          break;
        }
      }
      if (hit < 0) {
        score -= 1;
        why.push(mhz.toString() + 'MHz 档未出现');
        continue;
      }
      score += 1;
      const got: number = countInTier(freqs, hit);
      if (t.count < 0) {
        why.push(mhz.toString() + 'MHz 档命中(该档核数未查到)');
      } else if (got === t.count) {
        score += 1;
        why.push(mhz.toString() + 'MHz×' + t.count.toString() + ' 完全吻合');
      } else {
        score -= 1;
        why.push(mhz.toString() + 'MHz 档有 ' + got.toString() + ' 个核, 表中为 ' +
          t.count.toString() + ' 个(不符)');
      }
    }
    res.push({
      name: sig.soc, score: score, smt: sig.smt, devClass: socClass(sig.soc),
      modes: [mode], why: why, src: sig.src
    });
  }
  return res;
}

function structuralHintOf(n: number): string {
  if (n === 6) {
    return '6 个核：目前唯一 6 核麒麟是 Kirin 8000A（畅享 90 / 畅享 70X）。';
  }
  if (n === 8) {
    return '8 个核：可能是无 SMT 的 9000/9000E/8000（8 物理核），也可能是 SMT 芯片只报了 8 个物理核' +
      '（9000S/9000W/9010/9020/9030 的物理核数同样是 8）。靠频率档区分。';
  }
  if (n === 9) {
    return '9 个核：9030 Pro / 9035 / 9050 Pro / XE90 的物理核数是 9（SMT 未生效时报 9），T92C 也是 9 个真物理核。';
  }
  if (n === 12) {
    return '12 个核：可能是 SMT 芯片的逻辑核（9000S/9000W/9010/9020/9030），也可能是本身就 12 个物理核的 8020 / T92 / T82。频率档是唯一区分手段。';
  }
  if (n === 14) {
    return '14 个核：目前只有 Kirin 9030 Pro / XE90 / 9035 是 9 物理核 14 线程，因此 14 个核 ≈ 9 物理核的 SMT 麒麟。';
  }
  return '';
}

// ---------------------------------------------------------------------------
// identify(): 与 ref/_build_kirin.py 的 identify(n, freqs, tol) 逐行同源
//   输入 n     = 本机实测的核数(逻辑核口径: os 层线程数 / /proc/cpuinfo)
//   输入 freqs = 本机实测的每个核最高频率(GHz; 读不到的核直接不放进来)
//   输出       = 候选列表 + 是否足以定论(dataSufficient) + 结构提示
//   定论判据(资料原文): 最高分 >= 4 【且】 最高分 - 次名分 >= 2
// ---------------------------------------------------------------------------
export function identifyKirin(n: number, freqsGhz: number[], tol: number = 0.01): KirinIdentifyResult {
  const tiers: number[] = buildTierMap(freqsGhz);
  const sorted: number[] = tiers.slice(0);
  sorted.sort((a: number, b: number): number => b - a);
  let hist: string = '';
  for (const t of sorted) {
    if (hist.length > 0) {
      hist += ' / ';
    }
    hist += Math.round(t * 1000).toString() + 'MHz×' + countInTier(freqsGhz, t).toString();
  }
  if (hist.length === 0) {
    hist = '（一个频率档都没读到）';
  }

  let cands: KirinCandidate[] =
    matchSignatures(LOGICAL_SIGNATURES, n, freqsGhz, tiers, tol, 'logical')
      .concat(matchSignatures(PHYSICAL_SIGNATURES, n, freqsGhz, tiers, tol, 'physical'));
  cands = cands.filter((c: KirinCandidate): boolean => c.score > 0);

  // 同名候选合并(无 SMT 的芯片在两种口径下指纹相同, 否则会自己和自己打平)
  const merged: KirinCandidate[] = [];
  for (const c of cands) {
    let hit: KirinCandidate | null = null;
    for (const m of merged) {
      if (m.name === c.name) {
        hit = m;
        break;
      }
    }
    if (hit === null) {
      merged.push({ name: c.name, score: c.score, smt: c.smt, devClass: c.devClass,
        modes: c.modes.slice(0), why: c.why.slice(0), src: c.src });
    } else {
      if (hit.modes.indexOf(c.modes[0]) < 0) {
        hit.modes.push(c.modes[0]);
      }
      if (c.score > hit.score) {
        hit.score = c.score;
      }
      if (c.why.length > hit.why.length) {
        hit.why = c.why.slice(0);
      }
    }
  }
  merged.sort((a: KirinCandidate, b: KirinCandidate): number => b.score - a.score);

  const out: KirinIdentifyResult = {
    inputCores: n,
    freqTiersGhz: sorted,
    tierHistogram: hist,
    candidates: merged.slice(0, 6),
    warnings: [],
    dataSufficient: false,
    structuralHint: structuralHintOf(n)
  };

  for (const c of merged) {
    if (c.modes.length >= 2) {
      c.why.push('该芯片无 SMT, logical 与 physical 两种口径指纹相同, 故两路都命中');
    }
  }

  if (merged.length === 0) {
    out.warnings.push('没有任何条目的核数等于 ' + n.toString() + '。');
    out.dataSufficient = false;
    return out;
  }
  const top: KirinCandidate = merged[0];
  const second: number = merged.length > 1 ? merged[1].score : -99;
  out.dataSufficient = (top.score >= 4 && (top.score - second) >= 2);
  if (!out.dataSufficient) {
    if (top.score < 4) {
      out.warnings.push('最高分只有 ' + top.score.toString() +
        '（<4）：表内该芯片的频率/核数本身标注为未查到或来源冲突，不足以定论。');
    } else {
      out.warnings.push('最高分 ' + top.score.toString() + '，但与次名 ' + merged[1].name +
        '(' + second.toString() + ') 差距 ' + (top.score - second).toString() +
        ' < 2：两个候选无法区分。');
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// 一次性把 native 的探测结果翻成"本机实测 + 第三方推断"两行文本
// ---------------------------------------------------------------------------
export interface ChipFactsInput {
  logical: number;         // 逻辑核数(实测)
  physical: number;        // 物理核数(实测; 未知时 = logical)
  smtPossible: boolean;
  topoKnown: boolean;
  freqsGhz: number[];      // 每个读到了的核的最高频率(GHz)
  readCores: number;       // 读到了频率的核数
  scannedCores: number;    // 扫了几个核号
  perCore: string;         // native 给的逐核原始读数一行
  verdict: string;         // native 给的"漏核/截断"判定
  missedFaster: boolean;
  tableCores: number;
  tableMaxKhz: number;
  probeMaxKhz: number;
  deviceType: string;      // deviceInfo.deviceType: phone / tablet / 2in1 ...
  midrParts: number[];     // /proc/cpuinfo 的 CPU part 十进制值(读不到则为空数组)
  cpuinfoNote: string;     // /proc/cpuinfo 读取情况(读到 / errno)
}

export interface ChipJudgment {
  measured: string;   // 第一行: 本机实测(设备自己说的话)
  infer: string;      // 第二行: 第三方资料的型号推断(疑似/推断措辞)
  perCore: string;    // 第三行: 逐核原始读数(原样透传 native)
  dataSufficient: boolean;
  topName: string;
}

// 资料里明确标注过的 MIDR part 号(只有 9030 家族有: 3334 / 3335 = 0xd06 / 0xd07)
const MIDR_PART_9030_FAMILY: number[] = [3334, 3335];

function fmtMhz(khz: number): string {
  if (khz <= 0) {
    return '未知';
  }
  return Math.round(khz / 1000).toString() + 'MHz';
}

// 第一行: 本机实测(逻辑核 / 物理核 / 频率档 / 逐核读数完整性)
export function renderMeasuredLine(f: ChipFactsInput): string {
  let s: string = '芯片与频率（本机实测，设备自报）：';
  s += f.logical.toString() + ' 逻辑核 / ' + f.physical.toString() + ' 物理核';
  if (!f.topoKnown) {
    s += '（拓扑未知, 已按 1:1 处理）';
  } else if (f.smtPossible) {
    s += '（检测到 SMT）';
  } else {
    s += '（无 SMT）';
  }
  const tiers: number[] = buildTierMap(f.freqsGhz);
  const sorted: number[] = tiers.slice(0);
  sorted.sort((a: number, b: number): number => b - a);
  let hist: string = '';
  for (const t of sorted) {
    if (hist.length > 0) {
      hist += ' / ';
    }
    hist += Math.round(t * 1000).toString() + 'MHz×' + countInTier(f.freqsGhz, t).toString();
  }
  s += ' · 逐核 cpuinfo_max_freq 档位 ' + (hist.length > 0 ? hist : '（一个都没读到）');
  if (f.verdict.length > 0) {
    // native 的判定文本: 里面已经写了"绑核用频率表 vs 逐核探测"的对比与 errno, 直接原样透传
    s += ' · ' + f.verdict;
  } else {
    s += ' · ' + f.readCores.toString() + '/' + f.scannedCores.toString() + ' 个核读到';
    if (f.tableCores !== f.readCores || f.missedFaster) {
      s += '；绑核用频率表 ' + f.tableCores.toString() + ' 核(最高 ' + fmtMhz(f.tableMaxKhz) +
        ') vs 逐核探测最高 ' + fmtMhz(f.probeMaxKhz);
      if (f.missedFaster) {
        s += ' 绑核路径漏掉了更高频的核, 大核簇可能建错档';
      }
    }
  }
  return s;
}

// 第二行: 第三方资料的型号推断(措辞必须是"疑似/推断", 且点名出处等级)
export function renderInferLine(f: ChipFactsInput): ChipJudgment {
  const r: KirinIdentifyResult = identifyKirin(f.logical, f.freqsGhz);
  const measured: string = renderMeasuredLine(f);
  const parts: string[] = [];
  let dataSufficient: boolean = r.dataSufficient;
  let topName: string = r.candidates.length > 0 ? r.candidates[0].name : '';

  const midrKnown: boolean = f.midrParts.length > 0;
  let midrNote: string = '';
  if (midrKnown) {
    let inFamily: boolean = false;
    for (const p of f.midrParts) {
      if (MIDR_PART_9030_FAMILY.indexOf(p) >= 0) {
        inFamily = true;
      }
    }
    midrNote = ' · MIDR CPU part=' + f.midrParts.join('/') +
      (inFamily ? '（资料只标注过 9030 家族是 3334/3335, 本机落在这个范围内）'
                : '（资料里没有这个 part 号的记录, 不能据此支持或否定任何型号）');
  }

  if (f.freqsGhz.length === 0) {
    // 连一个频率都没读到: 这时只有核数是事实, 频率指纹完全用不上, 必须明说
    parts.push('数据不足，无法判读 —— 一个核的 cpufreq/cpuinfo_max_freq 都没读到（逐核读数见下一行）');
    if (r.structuralHint.length > 0) {
      parts.push('只能按核数给线索：' + r.structuralHint);
    }
    dataSufficient = false;
  } else if (r.candidates.length === 0) {
    parts.push('数据不足，无法定型号 —— 表内没有任何条目的核数是 ' + f.logical.toString() +
      '（资料共 26 条, 其中 8 条连核心数都没有来源）');
    dataSufficient = false;
  } else if (r.dataSufficient) {
    const top: KirinCandidate = r.candidates[0];
    const meta: KirinSocMeta | null = socMetaOf(top.name);
    let core: string = '';
    if (meta !== null && meta.cores > 0) {
      core = meta.cores.toString() + ' 物理核';
      if (meta.threads > 0) {
        core += ' / ' + meta.threads.toString() + ' 逻辑核';
      }
      if (meta.smt === 1) {
        core += ', SMT';
      }
    }
    parts.push('疑似 ' + top.name + (core.length > 0 ? '（' + core + '）' : ''));
    parts.push('依据 ' + f.logical.toString() + ' 逻辑核 + 频率档 ' + r.freqTiersGhz.map(
      (g: number): string => Math.round(g * 1000).toString()).join('/') +
      ' 且各档核数 ' + r.tierHistogram + '（得分 ' + top.score.toString() + '）');
    parts.push('出处 ' + top.src.split(' ').map((id: string): string => srcTitle(id)).join(' / '));
  } else {
    const top: KirinCandidate = r.candidates[0];
    if (top.score < 4) {
      // "分不够": 表内该芯片自身标注未查到或来源冲突
      let structural: string = r.structuralHint;
      if (f.logical === 14) {
        structural = '能确定的只有「9 物理核 + SMT（Prime 与中核开超线程）的 14 逻辑核麒麟平台」' +
          '（资料: 逻辑核 = 物理核 + Prime核数 + 中核数)';
      }
      parts.push('数据不足，无法定型号 —— ' + structural);
      parts.push('最高分只有 ' + top.score.toString() + '（<4）：表内 ' + top.name +
        ' 的中/小核频率未查到（或来源冲突），实测频率档 ' + r.tierHistogram +
        ' 与它已查到的档位对不上');
      if (f.missedFaster) {
        parts.push('本机绑核路径确实漏读了更高频的核(' + fmtMhz(f.probeMaxKhz) + ' > 表内 ' +
          fmtMhz(f.tableMaxKhz) + ')，频率档本身不完整 —— 先修读取口径再套指纹表');
      } else {
        parts.push('本机逐核读取没有漏核，所以这组档位就是设备真实报出来的值：' +
          '与 9030 Pro 的 2750/2270/1720 不吻合，属于资料不足或设备非该型号，不做进一步推断');
      }
    } else {
      // "两个候选分不开"
      const a: string = r.candidates[0].name;
      const b: string = r.candidates[1].name;
      const note: string = tieNoteFor(a, b);
      parts.push('无法区分 —— 候选 ' + a + ' 与 ' + b + ' 的最高分相同（' +
        r.candidates[0].score.toString() + '），差距 < 2');
      if (note.length > 0) {
        parts.push(note);
      }
      const dt: string = (f.deviceType.length > 0 ? f.deviceType : '未知');
      const ca: string = socClass(a);
      const cb: string = socClass(b);
      parts.push('需用设备形态破除：本机 deviceType=' + dt + '；资料里 ' + a + ' 的已知机型类别=' +
        ca + '、' + b + ' 的已知机型类别=' + cb);
      dataSufficient = false;
    }
  }

  let infer: string = '芯片判读（' + CHIP_GUESS_TAG_THIRD_PARTY + '）：' + parts.join(' · ');
  if (midrNote.length > 0) {
    infer += midrNote;
  }
  if (f.cpuinfoNote.length > 0) {
    infer += ' · ' + f.cpuinfoNote;
  }
  if (f.missedFaster) {
    // 只在"绑核路径确实漏读了更高频的核"时加这句: 这时上面的档位取自逐核探测(而不是绑核表),
    // 型号推断与绑核行为是两件事, 必须分开说清楚, 免得把"绑错簇"误读成"型号判错了"。
    infer += ' · 本机绑核用的频率表比逐核探测少读到了核(见上一行的判定), 快簇可能建在错误档上 —— ' +
      '上面的型号推断用的是逐核探测的档位, 与绑核行为是两件事; 建议先修读取口径再下结论';
  }
  infer += ' · ' + CHIP_GUESS_SOURCE_LEGEND;

  return {
    measured: measured,
    infer: infer,
    perCore: f.perCore,
    dataSufficient: dataSufficient,
    topName: topName
  };
}
