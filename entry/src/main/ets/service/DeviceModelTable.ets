// --------------------------------------------------------------------------
// 由 ref/device_models.json 生成, 共 110 条 —— 请勿手改本文件。
//
//   生成脚本: ref/gen_device_table.py（在仓库根目录执行 python ref/gen_device_table.py）
//   数据版本: schemaVersion=1, generatedAt=2026-10-04
//   数据报告: ref/DEVICE_MODELS.md（含存疑表 / 前缀冲突清单 / 后缀规则 / 覆盖率自评）
//
//   条目统计: 主型号码 110 条（+ variants 备用型号码 45 个精确匹配键，合计 155 个键）
//             形态: bar 71 / foldable 22 / tablet 17
//             品牌: HONOR 4 / HUAWEI 86 / OPPO 3 / OnePlus 4 / POCO 2 / Redmi 1 / Samsung 3 / Xiaomi 4 / iQOO (vivo) 1 / vivo 2
//             含屏幕参数 51 条, 表内无屏幕参数 59 条（原样留空, 运行时不瞎填）
//             前缀 72 个: 唯一可用 53 个, 冲突不可用 19 个（见 PREFIX_CONFLICT）
//
//   匹配规则(与 ref/DEVICE_MODELS.md 第 6 节一致, 实现在 DeviceProfile.ets):
//     1) 完整型号码精确匹配为准;
//     2) 三星(Build.MODEL 常带颜色/容量后缀, 如 SM-S948BZVCXME)先去掉尾部后缀再精确匹配,
//        规则 = 表内键是该值的开头且下一字符是字母或已结束; 只对长度>=8 且 SM- 开头的值启用;
//     3) 前缀匹配只在"该前缀在本表内唯一对应一个传播名"时才允许;
//     4) 前缀冲突(PREFIX_CONFLICT, 含 SM-)一律不用, 直接显示型号码原文;
//     5) 品牌判定只用 deviceInfo.brand, 不用型号码前缀/形态推断（荣耀与华为同源命名,
//        小米/POCO 是 8 位数字码, vivo 是 V####A, OPPO 有 P??110/CPH#### 两套）。
// --------------------------------------------------------------------------

/**
 * 表内一条机型的规格(屏幕参数可能为空: 官方参数页已下架, 不拿第三方数字填充)。
 */
export interface DeviceSpec {
  name: string;            // 传播名
  brand: string;           // 表内品牌(仅备注用; 运行时判品牌只用 deviceInfo.brand)
  formFactor: string;      // bar | foldable | tablet
  innerScreenInch: number; // 内屏/主屏英寸, -1 = 表内无
  outerScreenInch: number; // 外屏/副屏英寸, -1 = 表内无
  resolution: string;      // 分辨率原文, 空串 = 表内无
  source: string;          // 屏幕/形态参数来源 URL, 空串 = 表内无
  os: string;              // 出厂/可升级 HarmonyOS 版本, 空串 = 表内无
  note: string;            // 备注与不确定性说明
}

/**
 * 型号码 -> 传播名(精确匹配键)。
 * 键 = 主型号码 + variants 里的备用型号码(如 ALN-AL80 / HOP-AL10 / TET-AL00)。
 */
export const MODEL_ALIAS: Record<string, string> = {
  'ADA-AL00': 'HUAWEI nova 12 Pro',
  'ADA-AL00U': 'HUAWEI nova 12 Ultra',
  'ADA-AL10U': 'HUAWEI nova 12 Ultra 星耀版',
  'ADY-AL00': 'HUAWEI Pura 70',
  'ADY-AL10': 'HUAWEI Pura 70 北斗卫星消息版',
  'ALN-AL00': 'HUAWEI Mate 60 Pro',  // 备用型号码: ALN-AL80
  'ALN-AL80': 'HUAWEI Mate 60 Pro',
  'ALN-AL10': 'HUAWEI Mate 60 Pro+',
  'ALT-AL00': 'HUAWEI Mate X3',
  'ALT-AL10': 'HUAWEI Mate X5',
  'BAL-AL00': 'HUAWEI P50 Pocket',
  'BAL-AL80': 'HUAWEI P50 Pocket（另一型号码）',
  'BKY-W00': 'HUAWEI MatePad Air 12 英寸 2024',  // 备用型号码: BKY-W10, BKY-W20, BKY-W21
  'BKY-W10': 'HUAWEI MatePad Air 12 英寸 2024',
  'BKY-W20': 'HUAWEI MatePad Air 12 英寸 2024',
  'BKY-W21': 'HUAWEI MatePad Air 12 英寸 2024',
  'BLK-AL00': 'HUAWEI nova 12',
  'BLK-AL80': 'HUAWEI nova 13',
  'BRA-AL00': 'HUAWEI Mate 60',
  'BRE-AL80a': '华为畅享 70X 尊享版',
  'CAS-AL50': 'HUAWEI nova 16 SE',
  'CHZ-AL00': '华为畅享 90 Pro Max',
  'CLS-AL00': 'HUAWEI Mate 70',  // 备用型号码: CLS-AL30
  'CLS-AL30': 'HUAWEI Mate 70',
  'CRS-AL00': 'HUAWEI nova 16 Pro',
  'DAL-W00': 'HUAWEI MatePad Pro 12 英寸',
  'DAL-W20': 'HUAWEI MatePad Pro 12 英寸 悦享款',
  'DEL-AL10': 'HUAWEI Mate X7',  // 备用型号码: DEL-AL20
  'DEL-AL20': 'HUAWEI Mate X7',
  'EMA-AL00U': 'HUAWEI nova 16',
  'GEM-W00': 'HUAWEI MatePad Pro Max',  // 备用型号码: GEM-W10
  'GEM-W10': 'HUAWEI MatePad Pro Max',
  'GEM-W40': 'HUAWEI MatePad Pro Max 悦享款',
  'GRL-AL10': 'HUAWEI Mate XT 非凡大师',  // 备用型号码: GRL-AL10D
  'GRL-AL10D': 'HUAWEI Mate XT 非凡大师',
  'GRL-AL20': 'HUAWEI Mate XTs 非凡大师',
  'HBN-AL00': 'HUAWEI Pura 70 Pro',
  'HBN-AL10': 'HUAWEI Pura 70 Pro+',  // 备用型号码: HBN-AL80
  'HBN-AL80': 'HUAWEI Pura 70 Pro+',
  'HBP-AL00': 'HUAWEI Pura 70 Ultra',
  'HED-AL00': 'HUAWEI Pura 80',
  'HIP-AL00': 'HUAWEI nova 16 Ultra',
  'HOP-AL00': 'HUAWEI Pura X Max',  // 备用型号码: HOP-AL10
  'HOP-AL10': 'HUAWEI Pura X Max',
  'ICL-AL10': 'HUAWEI Mate X6',  // 备用型号码: ICL-AL20
  'ICL-AL20': 'HUAWEI Mate X6',
  'JUY-AL50': '华为畅享 90 Plus',
  'JUY-AL80': '华为畅享 90',
  'KEE-AL00': 'HUAWEI Pura 90',
  'KLE-AL00U': 'HUAWEI nova 15 Pro',
  'LAP-AL10': 'HUAWEI Mate XT 2 非凡大师',
  'LEM-AL00': 'HUAWEI Pocket 2',
  'LEM-AL50': 'HUAWEI Pocket 2 优享版',
  'LMR-AL00': 'HUAWEI Pura 80 Pro',
  'LMR-AL10': 'HUAWEI Pura 80 Pro+',
  'LMU-AL00': 'HUAWEI Pura 80 Ultra',
  'LNA-AL00': 'HUAWEI P60',
  'LRT-W00': 'HUAWEI MatePad Air 12 英寸 2025',  // 备用型号码: LRT-W20, LRT-W30
  'LRT-W20': 'HUAWEI MatePad Air 12 英寸 2025',
  'LRT-W30': 'HUAWEI MatePad Air 12 英寸 2025',
  'MIA-AL00': 'HUAWEI nova 14 Pro',
  'MIS-AL00': 'HUAWEI nova 13 Pro',
  'MLN-AL00': 'HUAWEI Pura 90 Pro',
  'MLR-AL00': 'HUAWEI MatePad Mini 悦读版',
  'MLR-AL10': 'HUAWEI MatePad Mini',
  'MNA-AL00': 'HUAWEI P60 Pro',
  'MRDI-W00': 'HUAWEI MatePad Pro 12.2 英寸 2025',  // 备用型号码: MRDI-W10, MRDI-W20
  'MRDI-W10': 'HUAWEI MatePad Pro 12.2 英寸 2025',
  'MRDI-W20': 'HUAWEI MatePad Pro 12.2 英寸 2025',
  'MRO-AL10': 'HUAWEI MatePad Pro 12.2 英寸 2024',  // 备用型号码: MRO-W00, MRO-W10
  'MRO-W00': 'HUAWEI MatePad Pro 12.2 英寸 2024',
  'MRO-W10': 'HUAWEI MatePad Pro 12.2 英寸 2024',
  'MRT-AL10': 'HUAWEI nova 14 Ultra',
  'PAL-AL00': 'HUAWEI Mate Xs 2',  // 备用型号码: PAL-AL10
  'PAL-AL10': 'HUAWEI Mate Xs 2',
  'PCE-AL30': 'HUAWEI MatePad Pro 13.2 英寸 2023',  // 备用型号码: PCE-W30, PCE-W40, PCE-AL40
  'PCE-W30': 'HUAWEI MatePad Pro 13.2 英寸 2023',
  'PCE-W40': 'HUAWEI MatePad Pro 13.2 英寸 2023',
  'PCE-AL40': 'HUAWEI MatePad Pro 13.2 英寸 2023',
  'PKN-AL00': 'HUAWEI nova 16z',
  'PLA-AL10': 'HUAWEI Mate 70 Pro+',
  'PLR-AL00': 'HUAWEI Mate 70 Pro',  // 备用型号码: PLR-AL30
  'PLR-AL30': 'HUAWEI Mate 70 Pro',
  'PLR-AL50': 'HUAWEI Mate 70 Pro 优享版',  // 备用型号码: PLR-AL80
  'PLR-AL80': 'HUAWEI Mate 70 Pro 优享版',
  'PLU-AL10': 'HUAWEI Mate 70 RS 非凡大师',
  'PSD-AL00': 'HUAWEI nova Flip',
  'PSD-AL80': 'HUAWEI nova Flip S',
  'PSN-AL00': 'HUAWEI nova 15',
  'QXS-W00': 'HUAWEI MatePad Edge',  // 备用型号码: QXS-W10, QXS-W20
  'QXS-W10': 'HUAWEI MatePad Edge',
  'QXS-W20': 'HUAWEI MatePad Edge',
  'SCA-AL00': 'HUAWEI Pura 90 Pro Max',
  'SGT-AL00': 'HUAWEI Mate 80 Pro',  // 备用型号码: SGT-AL50
  'SGT-AL50': 'HUAWEI Mate 80 Pro',
  'SGT-AL10': 'HUAWEI Mate 80 Pro Max',
  'SGU-AL10': 'HUAWEI Mate 80 RS 非凡大师',
  'SLG-W00': 'HUAWEI MatePad 11.5 S 2025 款',  // 备用型号码: SLG-W30, SLG-W60, SLG-W50
  'SLG-W30': 'HUAWEI MatePad 11.5 S 2025 款',
  'SLG-W60': 'HUAWEI MatePad 11.5 S 2025 款',
  'SLG-W50': 'HUAWEI MatePad 11.5 S 2025 款',
  'SLY-AL00': 'HUAWEI nova 15 Ultra',
  'SUP-AL90': 'HUAWEI Mate 70 Air',  // 备用型号码: SUP-AL90a
  'SUP-AL90a': 'HUAWEI Mate 70 Air',
  'TAH-AN00': 'HUAWEI Mate X',
  'TAH-AN00m': 'HUAWEI Mate Xs',
  'TET-AN00': 'HUAWEI Mate X2',  // 备用型号码: TET-AN10, TET-AN50, TET-AL00
  'TET-AN10': 'HUAWEI Mate X2',
  'TET-AN50': 'HUAWEI Mate X2',
  'TET-AL00': 'HUAWEI Mate X2',
  'TGR-W00': 'HUAWEI MatePad 11.5 S',  // 备用型号码: TGR-W00G, TGR-W10, TGR-W10G, TGR-W20, TGR-W40
  'TGR-W00G': 'HUAWEI MatePad 11.5 S',
  'TGR-W10': 'HUAWEI MatePad 11.5 S',
  'TGR-W10G': 'HUAWEI MatePad 11.5 S',
  'TGR-W20': 'HUAWEI MatePad 11.5 S',
  'TGR-W40': 'HUAWEI MatePad 11.5 S',
  'TLR-AL00': 'HUAWEI nova 14',
  'TXZ-W10': 'HUAWEI MatePad 11.5 2026 款',  // 备用型号码: TXZ-W20, TXZ-AL10
  'TXZ-W20': 'HUAWEI MatePad 11.5 2026 款',
  'TXZ-AL10': 'HUAWEI MatePad 11.5 2026 款',
  'TYR-AL00': 'HUAWEI nova 14 活力版',
  'VDE-AL00': 'HUAWEI Pura X',  // 备用型号码: VDE-AL10
  'VDE-AL10': 'HUAWEI Pura X',
  'VOL-AL00': 'HUAWEI Pura X View',
  'VYG-AL00': 'HUAWEI Mate 80',  // 备用型号码: VYG-AL30
  'VYG-AL30': 'HUAWEI Mate 80',
  'WEB-AL00': 'HUAWEI MatePad Pro 13.2 英寸 2025',  // 备用型号码: WEB-AL10, WEB-W00, WEB-W10
  'WEB-AL10': 'HUAWEI MatePad Pro 13.2 英寸 2025',
  'WEB-W00': 'HUAWEI MatePad Pro 13.2 英寸 2025',
  'WEB-W10': 'HUAWEI MatePad Pro 13.2 英寸 2025',
  'XYAO-W00': 'HUAWEI MatePad Pro 11 英寸 2024',
  'BVL-N49': 'HONOR Magic6 Pro',
  'FCP-N49': 'HONOR Magic V3',
  'MAG-AN00': 'HONOR 90 GT',
  'MBH-N49': 'HONOR Magic V5',
  'PHY110': 'OPPO Find X7 Ultra',
  'PKB110': 'OPPO Find X8',
  'PKJ110': 'OPPO Find X8 Ultra',
  'CPH2581': 'OnePlus 12（国际版）',
  'PJD110': 'OnePlus 12（国行）',
  'PJZ110': 'OnePlus 13',
  'PKX110': 'OnePlus 13T',
  '24069PC21G': 'POCO F6',
  '25053PC47G': 'POCO F7',
  '2312DRA50C': 'Redmi Note 13 Pro 5G（国行名 Redmi Note 13 Pro）',
  'SM-F966B': 'Samsung Galaxy Z Fold7',
  'SM-S928B': 'Samsung Galaxy S24 Ultra',
  'SM-S948B': 'Samsung Galaxy S26 Ultra',
  '23127PN0CC': 'Xiaomi 14',
  '2410DPN6CC': 'Xiaomi 15 Pro',
  '24129PN74G': 'Xiaomi 15',
  '25010PN30G': 'Xiaomi 15 Ultra',
  'V2408A': 'iQOO 13',
  'V2309A': 'vivo X100',
  'V2324A': 'vivo X100 Pro',
};

/**
 * 型号码 -> 规格(主型号码为键; 备用型号码用 MODEL_SPEC_ALIAS 转成主型号码再查)。
 * 屏幕参数为空时: innerScreenInch / outerScreenInch = -1, resolution / source / os = 空串。
 */
export const MODEL_SPEC: Record<string, DeviceSpec> = {
  'ADA-AL00': {
    name: 'HUAWEI nova 12 Pro',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到（已下架）'
  },
  'ADA-AL00U': {
    name: 'HUAWEI nova 12 Ultra',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到（已下架）'
  },
  'ADA-AL10U': {
    name: 'HUAWEI nova 12 Ultra 星耀版',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到（已下架）'
  },
  'ADY-AL00': {
    name: 'HUAWEI Pura 70',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页已下架（中文站）；屏幕参数留空'
  },
  'ADY-AL10': {
    name: 'HUAWEI Pura 70 北斗卫星消息版',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页已下架'
  },
  'ALN-AL00': {
    name: 'HUAWEI Mate 60 Pro',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: 'ALN-AL00/ALN-AL80 同为 Mate 60 Pro；屏幕参数留空'
  },
  'ALN-AL10': {
    name: 'HUAWEI Mate 60 Pro+',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '同一型号码 ALN-AL10 在华为官方页同时标注 Mate 60 Pro+ 与 Mate 60 RS 非凡大师'
  },
  'ALT-AL00': {
    name: 'HUAWEI Mate X3',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: 7.85,
    outerScreenInch: 6.4,
    resolution: '内屏 2496 × 2224 / 外屏 2504 × 1080',
    source: 'https://consumer.huawei.com/bh/phones/mate-x3/specs/',
    os: '',
    note: ''
  },
  'ALT-AL10': {
    name: 'HUAWEI Mate X5',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页已下架；典藏版与标准版同为 ALT-AL10（官方口径）'
  },
  'BAL-AL00': {
    name: 'HUAWEI P50 Pocket',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方产品重要信息页列出 BAL-AL00 / BAL-AL80'
  },
  'BAL-AL80': {
    name: 'HUAWEI P50 Pocket（另一型号码）',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '与 BAL-AL00 同页列出；对应具体版本（如艺术定制版）未确认'
  },
  'BKY-W00': {
    name: 'HUAWEI MatePad Air 12 英寸 2024',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板；官方参数页未取到'
  },
  'BLK-AL00': {
    name: 'HUAWEI nova 12',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到（已下架）'
  },
  'BLK-AL80': {
    name: 'HUAWEI nova 13',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到（已下架）'
  },
  'BRA-AL00': {
    name: 'HUAWEI Mate 60',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '屏幕参数官方页已下架，留空'
  },
  'BRE-AL80a': {
    name: '华为畅享 70X 尊享版',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'CAS-AL50': {
    name: 'HUAWEI nova 16 SE',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'CHZ-AL00': {
    name: '华为畅享 90 Pro Max',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'CLS-AL00': {
    name: 'HUAWEI Mate 70',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2688 × 1216 像素（6.7 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/mate70/specs/',
    os: 'HarmonyOS 4.3 / 5.0',
    note: ''
  },
  'CRS-AL00': {
    name: 'HUAWEI nova 16 Pro',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'DAL-W00': {
    name: 'HUAWEI MatePad Pro 12 英寸',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板；官方参数页未取到'
  },
  'DAL-W20': {
    name: 'HUAWEI MatePad Pro 12 英寸 悦享款',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板'
  },
  'DEL-AL10': {
    name: 'HUAWEI Mate X7',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: 8,
    outerScreenInch: 6.49,
    resolution: '内屏 2416 × 2210 / 外屏 2444 × 1080',
    source: 'https://consumer.huawei.com/cn/phones/mate-x7/specs/',
    os: 'HarmonyOS 6.0',
    note: ''
  },
  'EMA-AL00U': {
    name: 'HUAWEI nova 16',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'GEM-W00': {
    name: 'HUAWEI MatePad Pro Max',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板'
  },
  'GEM-W40': {
    name: 'HUAWEI MatePad Pro Max 悦享款',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板'
  },
  'GRL-AL10': {
    name: 'HUAWEI Mate XT 非凡大师',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: 10.2,
    outerScreenInch: 6.4,
    resolution: '单屏 2232 × 1008 / 双屏 2232 × 2048 / 三屏 2232 × 3184',
    source: 'https://consumer.huawei.com/by/phones/mate-xt-ultimate-design/specs/',
    os: '',
    note: '三折叠：单屏 6.4 / 双屏 7.9 / 三屏 10.2 英寸（官方 Belarus 页）'
  },
  'GRL-AL20': {
    name: 'HUAWEI Mate XTs 非凡大师',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '三折叠；官方参数页未取到'
  },
  'HBN-AL00': {
    name: 'HUAWEI Pura 70 Pro',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页已下架'
  },
  'HBN-AL10': {
    name: 'HUAWEI Pura 70 Pro+',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页已下架'
  },
  'HBP-AL00': {
    name: 'HUAWEI Pura 70 Ultra',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '2844 × 1260 像素（6.8 英寸）',
    source: 'https://consumer.huawei.com/pt/phones/pura70-ultra/specs/',
    os: '',
    note: '官方葡萄牙页：屏幕 6.8 英寸（官方脚注 6.77 英寸）'
  },
  'HED-AL00': {
    name: 'HUAWEI Pura 80',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2760 × 1256 像素（6.58 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/pura80/specs/',
    os: 'HarmonyOS 5.1',
    note: ''
  },
  'HIP-AL00': {
    name: 'HUAWEI nova 16 Ultra',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'HOP-AL00': {
    name: 'HUAWEI Pura X Max',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: 7.7,
    outerScreenInch: 5.4,
    resolution: '内屏 2584 × 1828 / 外屏 1848 × 1264',
    source: 'https://consumer.huawei.com/cn/phones/pura-x-max/specs/',
    os: 'HarmonyOS 7',
    note: '本次目标机型；官方同时给出折叠态 85×120 mm、展开态 166.5×120 mm、重约 229 g'
  },
  'ICL-AL10': {
    name: 'HUAWEI Mate X6',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: 7.93,
    outerScreenInch: 6.45,
    resolution: '内屏 2440 × 2240 / 外屏 2440 × 1080',
    source: 'https://consumer.huawei.com/cn/phones/mate-x6/specs/',
    os: 'HarmonyOS 4.3 / 5.0',
    note: ''
  },
  'JUY-AL50': {
    name: '华为畅享 90 Plus',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'JUY-AL80': {
    name: '华为畅享 90',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'KEE-AL00': {
    name: 'HUAWEI Pura 90',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官网参数页未取到'
  },
  'KLE-AL00U': {
    name: 'HUAWEI nova 15 Pro',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'LAP-AL10': {
    name: 'HUAWEI Mate XT 2 非凡大师',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '三折叠；官方参数页未取到'
  },
  'LEM-AL00': {
    name: 'HUAWEI Pocket 2',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '小折叠；官方参数页未取到'
  },
  'LEM-AL50': {
    name: 'HUAWEI Pocket 2 优享版',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: 6.94,
    outerScreenInch: 1.15,
    resolution: '内屏 FHD+ 2690 × 1136 / 外屏 340 × 340',
    source: 'https://consumer.huawei.com/cn/phones/pocket-2-youxiangban/specs/',
    os: '',
    note: '小折叠；UFG 玻纤版等子型号未细分'
  },
  'LMR-AL00': {
    name: 'HUAWEI Pura 80 Pro',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2848 × 1276 像素（6.78 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/pura80-pro/specs/',
    os: 'HarmonyOS 5.1',
    note: ''
  },
  'LMR-AL10': {
    name: 'HUAWEI Pura 80 Pro+',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2848 × 1276 像素（6.78 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/pura80-pro/specs/',
    os: '',
    note: '华为官方支持页明确 Pro+=LMR-AL10、Ultra=LMU-AL00；开发者文档误写 Ultra 为 LMU-AL10（见存疑表）'
  },
  'LMU-AL00': {
    name: 'HUAWEI Pura 80 Ultra',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2848 × 1276 像素（6.78 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/pura80-ultra/specs/',
    os: '',
    note: 'Ultra 型号码以华为官方支持页 zh-cn16048049 为准（LMU-AL00）'
  },
  'LNA-AL00': {
    name: 'HUAWEI P60',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '参数未取到'
  },
  'LRT-W00': {
    name: 'HUAWEI MatePad Air 12 英寸 2025',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板；官方参数页未取到'
  },
  'MIA-AL00': {
    name: 'HUAWEI nova 14 Pro',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2776 × 1224 像素',
    source: 'https://consumer.huawei.com/cn/phones/nova14-pro/specs/',
    os: 'HarmonyOS 5.0',
    note: ''
  },
  'MIS-AL00': {
    name: 'HUAWEI nova 13 Pro',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到（已下架）'
  },
  'MLN-AL00': {
    name: 'HUAWEI Pura 90 Pro',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官网参数页未取到'
  },
  'MLR-AL00': {
    name: 'HUAWEI MatePad Mini 悦读版',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: 8.8,
    outerScreenInch: -1,
    resolution: '2560 × 1600',
    source: 'https://consumer.huawei.com/cn/tablets/matepad-mini/specs/',
    os: '',
    note: '官方页列 MLR-AL10E/F/G/HP/KP 等子型号'
  },
  'MLR-AL10': {
    name: 'HUAWEI MatePad Mini',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: 8.8,
    outerScreenInch: -1,
    resolution: '2560 × 1600',
    source: 'https://consumer.huawei.com/cn/tablets/matepad-mini/specs/',
    os: '',
    note: ''
  },
  'MNA-AL00': {
    name: 'HUAWEI P60 Pro',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '参数未取到'
  },
  'MRDI-W00': {
    name: 'HUAWEI MatePad Pro 12.2 英寸 2025',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板'
  },
  'MRO-AL10': {
    name: 'HUAWEI MatePad Pro 12.2 英寸 2024',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板'
  },
  'MRT-AL10': {
    name: 'HUAWEI nova 14 Ultra',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2860 × 1272 像素',
    source: 'https://consumer.huawei.com/cn/phones/nova14-ultra/specs/',
    os: 'HarmonyOS 5.0',
    note: ''
  },
  'PAL-AL00': {
    name: 'HUAWEI Mate Xs 2',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页已下架'
  },
  'PCE-AL30': {
    name: 'HUAWEI MatePad Pro 13.2 英寸 2023',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板'
  },
  'PKN-AL00': {
    name: 'HUAWEI nova 16z',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'PLA-AL10': {
    name: 'HUAWEI Mate 70 Pro+',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2832 × 1316 像素（6.9 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/mate70-pro-plus/specs/',
    os: 'HarmonyOS 4.3 / 5.0',
    note: ''
  },
  'PLR-AL00': {
    name: 'HUAWEI Mate 70 Pro',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2832 × 1316 像素（6.9 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/mate70-pro/specs/',
    os: 'HarmonyOS 4.3 / 5.0',
    note: ''
  },
  'PLR-AL50': {
    name: 'HUAWEI Mate 70 Pro 优享版',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: 'https://consumer.huawei.com/cn/phones/mate70-pro/specs/',
    os: '',
    note: '优享版无独立官方参数页；PLR-AL80 见零售商参数页（非官方）'
  },
  'PLU-AL10': {
    name: 'HUAWEI Mate 70 RS 非凡大师',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '无独立官方参数页'
  },
  'PSD-AL00': {
    name: 'HUAWEI nova Flip',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '小折叠；官方参数页未取到'
  },
  'PSD-AL80': {
    name: 'HUAWEI nova Flip S',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '小折叠；官方参数页未取到'
  },
  'PSN-AL00': {
    name: 'HUAWEI nova 15',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'QXS-W00': {
    name: 'HUAWEI MatePad Edge',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板；社区称 14.2 英寸（未获官方确认）'
  },
  'SCA-AL00': {
    name: 'HUAWEI Pura 90 Pro Max',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官网参数页未取到'
  },
  'SGT-AL00': {
    name: 'HUAWEI Mate 80 Pro',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2832 × 1280 像素（6.75 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/mate80-pro/specs/',
    os: 'HarmonyOS 6.0',
    note: 'SGT-AL50 = 麒麟 9030 版，SGT-AL00 = 麒麟 9030 Pro 版（社区）'
  },
  'SGT-AL10': {
    name: 'HUAWEI Mate 80 Pro Max',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2848 × 1320 像素（6.9 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/mate80-pro-max/specs/',
    os: 'HarmonyOS 6.0',
    note: ''
  },
  'SGU-AL10': {
    name: 'HUAWEI Mate 80 RS 非凡大师',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '无独立官方参数页，屏幕参数留空'
  },
  'SLG-W00': {
    name: 'HUAWEI MatePad 11.5 S 2025 款',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: 11.5,
    outerScreenInch: -1,
    resolution: '2800 × 1840',
    source: 'https://consumer.huawei.com/cn/tablets/matepad-11-5-s-2025/specs/',
    os: '',
    note: ''
  },
  'SLY-AL00': {
    name: 'HUAWEI nova 15 Ultra',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'SUP-AL90': {
    name: 'HUAWEI Mate 70 Air',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2760 × 1320 像素（7 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/mate70-air/specs/',
    os: 'HarmonyOS 5.1',
    note: ''
  },
  'TAH-AN00': {
    name: 'HUAWEI Mate X',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页已下架。社区常引 8.0 英寸内屏/2480×2200，但未获官方页面确认，故留空'
  },
  'TAH-AN00m': {
    name: 'HUAWEI Mate Xs',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页已下架'
  },
  'TET-AN00': {
    name: 'HUAWEI Mate X2',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页已下架'
  },
  'TGR-W00': {
    name: 'HUAWEI MatePad 11.5 S',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板；官方参数页未取到'
  },
  'TLR-AL00': {
    name: 'HUAWEI nova 14',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2412 × 1084 像素（6.7 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/nova14/specs/',
    os: 'HarmonyOS 5.0',
    note: ''
  },
  'TXZ-W10': {
    name: 'HUAWEI MatePad 11.5 2026 款',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: 11.5,
    outerScreenInch: -1,
    resolution: '2456 × 1600',
    source: 'https://consumer.huawei.com/cn/tablets/matepad-11-5-2026/specs/',
    os: '',
    note: ''
  },
  'TYR-AL00': {
    name: 'HUAWEI nova 14 活力版',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '官方参数页未取到'
  },
  'VDE-AL00': {
    name: 'HUAWEI Pura X',
    brand: 'HUAWEI',
    formFactor: 'foldable',
    innerScreenInch: 6.3,
    outerScreenInch: 3.5,
    resolution: '内屏 2120 × 1320 / 外屏 980 × 980',
    source: 'https://consumer.huawei.com/cn/phones/pura-x/specs/',
    os: 'HarmonyOS 5.0.1',
    note: ''
  },
  'VOL-AL00': {
    name: 'HUAWEI Pura X View',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2232 × 1320 像素（6.39 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/pura-x-view/specs/',
    os: 'HarmonyOS 7.0',
    note: '官方定位『阔直板』，非折叠屏'
  },
  'VYG-AL00': {
    name: 'HUAWEI Mate 80',
    brand: 'HUAWEI',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: 'FHD+ 2832 × 1280 像素（6.75 英寸）',
    source: 'https://consumer.huawei.com/cn/phones/mate80/specs/',
    os: 'HarmonyOS 6.0',
    note: ''
  },
  'WEB-AL00': {
    name: 'HUAWEI MatePad Pro 13.2 英寸 2025',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板；官方参数页未取到'
  },
  'XYAO-W00': {
    name: 'HUAWEI MatePad Pro 11 英寸 2024',
    brand: 'HUAWEI',
    formFactor: 'tablet',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '',
    source: '',
    os: '',
    note: '平板'
  },
  'BVL-N49': {
    name: 'HONOR Magic6 Pro',
    brand: 'HONOR',
    formFactor: 'bar',
    innerScreenInch: 6.8,
    outerScreenInch: -1,
    resolution: '1280 x 2800',
    source: 'https://nanoreview.net/en/phone/honor-magic-6-pro',
    os: '',
    note: ''
  },
  'FCP-N49': {
    name: 'HONOR Magic V3',
    brand: 'HONOR',
    formFactor: 'foldable',
    innerScreenInch: 7.92,
    outerScreenInch: 6.43,
    resolution: '内屏 2156 x 2344',
    source: 'https://www.honor.com/cn/phones/honor-magic-v3/',
    os: '',
    note: 'GCF 认证库: Model name FCP-N49 / Marketing name HONOR Magic V3'
  },
  'MAG-AN00': {
    name: 'HONOR 90 GT',
    brand: 'HONOR',
    formFactor: 'bar',
    innerScreenInch: 6.7,
    outerScreenInch: -1,
    resolution: '1200 x 2664',
    source: 'https://nanoreview.net/en/phone/honor-90-gt',
    os: '',
    note: ''
  },
  'MBH-N49': {
    name: 'HONOR Magic V5',
    brand: 'HONOR',
    formFactor: 'foldable',
    innerScreenInch: 7.95,
    outerScreenInch: 6.43,
    resolution: '内屏 2172 x 2352',
    source: 'https://www.honor.com/cn/phones/honor-magic-v5/',
    os: '',
    note: ''
  },
  'PHY110': {
    name: 'OPPO Find X7 Ultra',
    brand: 'OPPO',
    formFactor: 'bar',
    innerScreenInch: 6.82,
    outerScreenInch: -1,
    resolution: '3168 x 1440',
    source: 'https://www.oppo.com/cn/smartphones/series-find-x/find-x7-ultra/specs/',
    os: '',
    note: 'OPPO 官网参数页明文写 产品型号: PHY110'
  },
  'PKB110': {
    name: 'OPPO Find X8',
    brand: 'OPPO',
    formFactor: 'bar',
    innerScreenInch: 6.59,
    outerScreenInch: -1,
    resolution: '1256 x 2760',
    source: 'https://nanoreview.net/en/phone/oppo-find-x8',
    os: '',
    note: ''
  },
  'PKJ110': {
    name: 'OPPO Find X8 Ultra',
    brand: 'OPPO',
    formFactor: 'bar',
    innerScreenInch: 6.82,
    outerScreenInch: -1,
    resolution: '3168 x 1440',
    source: 'https://www.oppo.com/cn/smartphones/series-find-x/find-x8-ultra/specs/',
    os: '',
    note: 'OPPO 官网参数页明文写 产品型号: PKJ110'
  },
  'CPH2581': {
    name: 'OnePlus 12（国际版）',
    brand: 'OnePlus',
    formFactor: 'bar',
    innerScreenInch: 6.82,
    outerScreenInch: -1,
    resolution: '3168 x 1440',
    source: 'https://nanoreview.net/en/phone/oneplus-12',
    os: '',
    note: '与 PJD110 同为 OnePlus 12，仅区域 SKU 不同'
  },
  'PJD110': {
    name: 'OnePlus 12（国行）',
    brand: 'OnePlus',
    formFactor: 'bar',
    innerScreenInch: 6.82,
    outerScreenInch: -1,
    resolution: '3168 x 1440',
    source: 'https://nanoreview.net/en/phone/oneplus-12',
    os: '',
    note: '同机型海外 SKU 为 CPH2581'
  },
  'PJZ110': {
    name: 'OnePlus 13',
    brand: 'OnePlus',
    formFactor: 'bar',
    innerScreenInch: 6.82,
    outerScreenInch: -1,
    resolution: '3168 x 1440',
    source: 'https://nanoreview.net/en/phone/oneplus-13',
    os: '',
    note: ''
  },
  'PKX110': {
    name: 'OnePlus 13T',
    brand: 'OnePlus',
    formFactor: 'bar',
    innerScreenInch: 6.32,
    outerScreenInch: -1,
    resolution: '1216 x 2640',
    source: 'https://nanoreview.net/en/phone/oneplus-13t',
    os: '',
    note: ''
  },
  '24069PC21G': {
    name: 'POCO F6',
    brand: 'POCO',
    formFactor: 'bar',
    innerScreenInch: 6.67,
    outerScreenInch: -1,
    resolution: '2712 x 1220',
    source: 'https://www.mi.com/global/product/poco-f6/specs/',
    os: '',
    note: ''
  },
  '25053PC47G': {
    name: 'POCO F7',
    brand: 'POCO',
    formFactor: 'bar',
    innerScreenInch: 6.83,
    outerScreenInch: -1,
    resolution: '2772 x 1280',
    source: 'https://cdn.alsgp0.fds.api.mi-img.com/phone-pads/POCO_F7_25053PC47G.pdf',
    os: '',
    note: '小米官方 CDN 合规文件写明 Nombre comercial del equipo: POCO F7 / Modelo 25053PC47G'
  },
  '2312DRA50C': {
    name: 'Redmi Note 13 Pro 5G（国行名 Redmi Note 13 Pro）',
    brand: 'Redmi',
    formFactor: 'bar',
    innerScreenInch: 6.67,
    outerScreenInch: -1,
    resolution: '2712 x 1220',
    source: 'https://www.mi.com/global/product/redmi-note-13-pro-5g/specs/',
    os: '',
    note: ''
  },
  'SM-F966B': {
    name: 'Samsung Galaxy Z Fold7',
    brand: 'Samsung',
    formFactor: 'foldable',
    innerScreenInch: 8,
    outerScreenInch: 6.5,
    resolution: '内屏 1968 x 2184',
    source: 'https://www.samsung.com/my/smartphones/galaxy-z-fold7/',
    os: '',
    note: ''
  },
  'SM-S928B': {
    name: 'Samsung Galaxy S24 Ultra',
    brand: 'Samsung',
    formFactor: 'bar',
    innerScreenInch: 6.8,
    outerScreenInch: -1,
    resolution: '1440 x 3088',
    source: 'https://www.samsung.com/my/smartphones/galaxy-s24-ultra/specs/',
    os: '',
    note: ''
  },
  'SM-S948B': {
    name: 'Samsung Galaxy S26 Ultra',
    brand: 'Samsung',
    formFactor: 'bar',
    innerScreenInch: -1,
    outerScreenInch: -1,
    resolution: '3120 x 1440',
    source: 'https://www.samsung.com/pt/business/smartphones/galaxy-s/galaxy-s26-ultra-silver-shadow-1tb-sm-s948bzsheub/',
    os: '',
    note: '三星官网商品页 URL/标题内嵌完整型号码 SM-S948B(-Z* 为颜色容量 SKU); 屏幕尺寸官网该页未给出'
  },
  '23127PN0CC': {
    name: 'Xiaomi 14',
    brand: 'Xiaomi',
    formFactor: 'bar',
    innerScreenInch: 6.36,
    outerScreenInch: -1,
    resolution: '2670 x 1200',
    source: 'https://www.mi.com/global/product/xiaomi-14/specs/',
    os: '',
    note: 'Google Play 官方设备清单 + deviceinfohw 双向印证; 注意 deviceinfohw 另有一条错误提交把该码写成 POCO X3 Pro(实为 Xiaomi 14 hòuji)'
  },
  '2410DPN6CC': {
    name: 'Xiaomi 15 Pro',
    brand: 'Xiaomi',
    formFactor: 'bar',
    innerScreenInch: 6.73,
    outerScreenInch: -1,
    resolution: '3200 x 1440',
    source: 'https://nanoreview.net/en/phone/xiaomi-15-pro',
    os: '',
    note: ''
  },
  '24129PN74G': {
    name: 'Xiaomi 15',
    brand: 'Xiaomi',
    formFactor: 'bar',
    innerScreenInch: 6.36,
    outerScreenInch: -1,
    resolution: '2670 x 1200',
    source: 'https://www.mi.com/global/product/xiaomi-15/specs/',
    os: '',
    note: ''
  },
  '25010PN30G': {
    name: 'Xiaomi 15 Ultra',
    brand: 'Xiaomi',
    formFactor: 'bar',
    innerScreenInch: 6.73,
    outerScreenInch: -1,
    resolution: '3200 x 1440',
    source: 'https://www.mi.com/global/product/xiaomi-15-ultra/specs/',
    os: '',
    note: ''
  },
  'V2408A': {
    name: 'iQOO 13',
    brand: 'iQOO (vivo)',
    formFactor: 'bar',
    innerScreenInch: 6.82,
    outerScreenInch: -1,
    resolution: '3168 x 1440',
    source: 'https://www.vivo.com.cn/vivo/param/iqoo13',
    os: '',
    note: 'vivo 官网参数页明文写 设备型号 V2408A'
  },
  'V2309A': {
    name: 'vivo X100',
    brand: 'vivo',
    formFactor: 'bar',
    innerScreenInch: 6.78,
    outerScreenInch: -1,
    resolution: '1260 x 2800',
    source: 'https://www.vivo.com.cn/vivo/param/x100',
    os: '',
    note: 'vivo 官网参数页明文写 全网通版 V2309A'
  },
  'V2324A': {
    name: 'vivo X100 Pro',
    brand: 'vivo',
    formFactor: 'bar',
    innerScreenInch: 6.78,
    outerScreenInch: -1,
    resolution: '1260 x 2800',
    source: 'https://www.vivo.com.cn/vivo/param/x100pro',
    os: '',
    note: 'vivo 官网参数页明文写 全网通版 V2324A'
  },
};

/**
 * 备用型号码 -> 主型号码(用于查到规格)。
 */
export const MODEL_SPEC_ALIAS: Record<string, string> = {
  'ALN-AL80': 'ALN-AL00',
  'BKY-W10': 'BKY-W00',
  'BKY-W20': 'BKY-W00',
  'BKY-W21': 'BKY-W00',
  'CLS-AL30': 'CLS-AL00',
  'DEL-AL20': 'DEL-AL10',
  'GEM-W10': 'GEM-W00',
  'GRL-AL10D': 'GRL-AL10',
  'HBN-AL80': 'HBN-AL10',
  'HOP-AL10': 'HOP-AL00',
  'ICL-AL20': 'ICL-AL10',
  'LRT-W20': 'LRT-W00',
  'LRT-W30': 'LRT-W00',
  'MRDI-W10': 'MRDI-W00',
  'MRDI-W20': 'MRDI-W00',
  'MRO-W00': 'MRO-AL10',
  'MRO-W10': 'MRO-AL10',
  'PAL-AL10': 'PAL-AL00',
  'PCE-W30': 'PCE-AL30',
  'PCE-W40': 'PCE-AL30',
  'PCE-AL40': 'PCE-AL30',
  'PLR-AL30': 'PLR-AL00',
  'PLR-AL80': 'PLR-AL50',
  'QXS-W10': 'QXS-W00',
  'QXS-W20': 'QXS-W00',
  'SGT-AL50': 'SGT-AL00',
  'SLG-W30': 'SLG-W00',
  'SLG-W60': 'SLG-W00',
  'SLG-W50': 'SLG-W00',
  'SUP-AL90a': 'SUP-AL90',
  'TET-AN10': 'TET-AN00',
  'TET-AN50': 'TET-AN00',
  'TET-AL00': 'TET-AN00',
  'TGR-W00G': 'TGR-W00',
  'TGR-W10': 'TGR-W00',
  'TGR-W10G': 'TGR-W00',
  'TGR-W20': 'TGR-W00',
  'TGR-W40': 'TGR-W00',
  'TXZ-W20': 'TXZ-W10',
  'TXZ-AL10': 'TXZ-W10',
  'VDE-AL10': 'VDE-AL00',
  'VYG-AL30': 'VYG-AL00',
  'WEB-AL10': 'WEB-AL00',
  'WEB-W00': 'WEB-AL00',
  'WEB-W10': 'WEB-AL00',
};

/**
 * 前缀冲突清单: 这些前缀在本表内对应多个传播名, 一律不许用于前缀匹配
 * (例如 ALT- 同时是 Mate X3 与 Mate X5, SGT- 同时是 Mate 80 Pro 与 Mate 80 Pro Max,
 *  SM- 同时是三星 S948B / S928B / F966B —— 三星只能靠"整串精确 / 去后缀"匹配)。
 * 由 ref/gen_device_table.py 统计得到, 共 19 个。
 */
export const PREFIX_CONFLICT: string[] = [
  'ADA', 'ADY', 'ALN', 'ALT', 'BAL', 'BLK', 'DAL', 'GEM', 'GRL', 'HBN', 'JUY', 'LEM', 'LMR', 'MLR', 'PLR', 'PSD', 'SGT', 'SM', 'TAH'
];

/**
 * 表内主型号码总数(生成时的条目数, 供关于页显示)。
 */
export const TABLE_ENTRY_COUNT: number = 110;

/**
 * 三星带后缀型号码(BM-S948BZVCXME)的长度门槛: 只对长度 >= 8 且以 SM- 开头的值启用去后缀。
 */
export const SAMSUNG_POSTFIX_MIN_LEN: number = 8;

export const SAMSUNG_PREFIX: string = 'SM-';

/**
 * 由型号码取规格(备用型号码先转到主型号码); 查不到返回 null。
 */
export function lookupSpec(model: string): DeviceSpec | null {
  if (model.length === 0) {
    return null;
  }
  const s: DeviceSpec | undefined = MODEL_SPEC[model];
  if (s !== undefined) {
    return s;
  }
  const main: string | undefined = MODEL_SPEC_ALIAS[model];
  if (main !== undefined) {
    const s2: DeviceSpec | undefined = MODEL_SPEC[main];
    if (s2 !== undefined) {
      return s2;
    }
  }
  return null;
}

/**
 * 三星型号码去后缀: 表内以 SM- 开头的键 K, 若 model 以 K 开头且下一字符是 A-Za-z 或已结束,
 * 则 K 就是"去掉颜色/容量后缀"后的型号码。取最长的那个 K(避免短键误吃长键)。
 * 只对 长度 >= SAMSUNG_POSTFIX_MIN_LEN 且以 SAMSUNG_PREFIX 开头的值启用; 其它一律返回空串。
 */
export function samsungBaseKey(model: string): string {
  if (model.length < SAMSUNG_POSTFIX_MIN_LEN || !model.startsWith(SAMSUNG_PREFIX)) {
    return '';
  }
  let best: string = '';
  const keys: string[] = Object.keys(MODEL_ALIAS);
  for (const k of keys) {
    if (!k.startsWith(SAMSUNG_PREFIX)) {
      continue;
    }
    if (model === k || k.length >= model.length) {
      continue;
    }
    if (!model.startsWith(k)) {
      continue;
    }
    const next: string = model.charAt(k.length);
    const nextIsLetter: boolean = (next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z');
    if (!nextIsLetter) {
      continue;
    }
    if (k.length > best.length) {
      best = k;
    }
  }
  return best;
}

/**
 * 完整匹配(精确键 + 三星去后缀); 返回实际命中的"表内键", 未命中返回空串。
 */
export function lookupModelWithPostfix(model: string): string {
  if (model.length === 0) {
    return '';
  }
  const exact: string | undefined = MODEL_ALIAS[model];
  if (exact !== undefined) {
    return model;
  }
  const base: string = samsungBaseKey(model);
  if (base.length > 0) {
    const n: string | undefined = MODEL_ALIAS[base];
    if (n !== undefined) {
      return base;
    }
  }
  return '';
}

/**
 * 前缀是否处于冲突状态(冲突则不许用于前缀匹配)。
 */
export function isPrefixConflicted(prefix: string): boolean {
  if (prefix.length === 0) {
    return false;
  }
  for (const p of PREFIX_CONFLICT) {
    if (p === prefix) {
      return true;
    }
  }
  return false;
}
