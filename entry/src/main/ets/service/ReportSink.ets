// ---------------------------------------------------------------------------
// 「下载」目录落盘 —— 让用户不用 hdc、不用在应用里翻, 直接在系统「文件管理 → 下载」里
// 就能拿到跑分报告。
//
// 目录命名规范(本次查证结论; 有官方明文与官方源码两重依据, 不是猜的)
//   手机(HarmonyOS NEXT)上, 第三方应用在公共「下载」目录里的文件夹 **物理名字就是包名
//   bundleName**, 例如 /storage/Users/currentUser/Download/com.aurora.bench/;
//   系统「文件管理」在界面上会把包名目录显示成应用名(即显示为「下载/Crush分」)。
//   依据:
//     (a) 华为官方指南《保存用户文件》·「DOWNLOAD模式保存文件 / 模式特点」原文:
//         "自动创建在 Download/包名/ 目录。跳过文件选择界面直接保存。
//          返回的URI已具备持久化权限, 用户可在该URI下创建文件。"
//     (b) 华为官方 FAQ《使用DOWNLOAD模式保存的文件如何访问》(faqs-local-file-manager-67) 原文:
//         "DOWNLOAD模式保存的文件URI示例:
//          'file://docs/storage/Users/currentUser/Download/<bundleName>/***.mp3'",
//         并给出用 bundleManager.getBundleInfoForSelfSync 取 bundleName、再用
//         fileUri.FileUri(uri).path 转真实路径的官方示例代码。
//     (c) OpenHarmony 参考实现 applications_filepicker / entry/.../DownloadAuth.ets:
//         const bundleName = this.startModeOptions.callerBundleName;
//         this.downloadNewUri = VirtualUri.DOWNLOAD + '/' + bundleName;   // 目录名 = 包名
//   所以本应用也用 包名 建目录 —— 与其它应用完全一致。
//   (界面文案里直接告诉用户"文件管理里显示为 下载/Crush分", 免得用户按包名去找找不到。)
//
// 手机上怎么写进去(官方唯一合法路径)
//   ohos.permission.READ_WRITE_DOWNLOAD_DIRECTORY 的支持设备写明是 PC/2in1 | Tablet
//   (没有 Phone), Environment.getUserDownloadDir() 在手机上直接返回 801;
//   所以手机上不能"直连公共下载目录", 正确做法是官方文档写的 **下载模式(DOWNLOAD 模式)的
//   DocumentViewPicker**: 它跳过文件选择界面, 直接把文件保存到 Download/<包名>/,
//   且返回的 URI 自带持久化权限 —— 授权一次, 以后直接写。
//
//   本文件的落地顺序(每一层的失败原因都返回给界面, 不静默失败):
//     ① 直接写 file://docs/storage/Users/currentUser/Download/<包名>/report-….txt
//        (已经授权过 / 系统允许访问自己包名目录时, 一次成功且全程无弹框)
//     ② ① 不行 -> DocumentViewPicker(pickerMode = DOWNLOAD) 让系统建好目录并授权
//     ③ ② 也不行 -> 由 Index.ets 走原有的"另存为"文档选择器 + 剪贴板兜底, 并把原因显示出来
//
// 性能说明: 本文件只在报告已经生成好之后被调用一次(纯文件 IO, 无定时器、无动画、
//           不进入任何渲染循环), 跑分期间不会被调用。
// ---------------------------------------------------------------------------
import fs from '@ohos.file.fs';
import { common, bundleManager } from '@kit.AbilityKit';
import { picker, fileUri } from '@kit.CoreFileKit';
// 报告 .txt 的 UTF-8 合法性守卫(2026-10 真机报告缺陷 4) —— 这里是第二道:
// 正文拼装阶段(FullRun.textChunks)已经把孤立代理项换成 U+FFFD 并把个数写进报告正文;
// 用户从「文件管理 → 下载」里拿到的这一份也必须满足同一条保证, 所以在真正 writeSync 之前
// 再过一次(带断言: 有孤立代理项就必须替换, 且不许静默)。口径见 common/Utf8Guard.ets。
import { assertTxtUtf8Safe, sanitizeReportText } from '../common/Utf8Guard';

// 公共「下载」目录的虚拟 URI(官方 FAQ 里写死的那个)
export const DL_ROOT_URI: string = 'file://docs/storage/Users/currentUser/Download';
// 文件管理里显示的那一层目录名(给界面文案用; 物理目录名是包名)
export const DL_DIR_SHOWN: string = 'Crush分';

export interface ReportWriteResult {
  ok: boolean;
  dir: string;      // 目标目录(物理路径; 失败时可能为空)
  name: string;     // 文件名
  path: string;     // 完整路径
  method: string;   // '下载目录直写' / '下载模式保存' / '未写入'
  err: string;      // 失败的真实原因(用户可见, 不吞)
}

function pad2(n: number): string {
  return n < 10 ? ('0' + n.toString()) : n.toString();
}

// 文件名: report-YYYYMMDD-HHMMSS.txt(与沙箱里的 report-<时间戳>.txt 同一套口径)
export function reportStampName(ts: number): string {
  const d = new Date(ts);
  return 'report-' + d.getFullYear().toString() + pad2(d.getMonth() + 1) + pad2(d.getDate()) +
    '-' + pad2(d.getHours()) + pad2(d.getMinutes()) + pad2(d.getSeconds()) + '.txt';
}

export class ReportSink {
  private static bundle: string = '';
  private static dirPath: string = '';
  private static lastErr: string = '';
  private static grantedOnce: boolean = false;

  static lastError(): string {
    return ReportSink.lastErr;
  }

  // 本应用包名(官方法: bundleManager.getBundleInfoForSelfSync)
  static bundleName(): string {
    if (ReportSink.bundle.length > 0) {
      return ReportSink.bundle;
    }
    try {
      const info = bundleManager.getBundleInfoForSelfSync(
        bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT);
      if (info.name.length > 0) {
        ReportSink.bundle = info.name;
      } else {
        ReportSink.lastErr = 'bundleManager 返回了空包名。';
      }
    } catch (e) {
      ReportSink.lastErr = '取 bundleName 失败：' + e.message;
    }
    return ReportSink.bundle;
  }

  // 界面上给用户看的目录名(文件管理显示的是应用名, 不是包名)
  static displayDir(): string {
    return '下载/' + DL_DIR_SHOWN;
  }

  static dirShown(): string {
    return ReportSink.displayDir();
  }

  // <下载>/<包名> 的真实路径(官方法: fileUri.FileUri(uri).path)
  static resolveDir(): string {
    if (ReportSink.dirPath.length > 0) {
      return ReportSink.dirPath;
    }
    const b: string = ReportSink.bundleName();
    if (b.length === 0) {
      return '';
    }
    const uri: string = DL_ROOT_URI + '/' + b;
    try {
      const u = new fileUri.FileUri(uri);
      const p: string = u.path;
      if (p.length > 0) {
        ReportSink.dirPath = p;
        return p;
      }
      ReportSink.lastErr = 'fileUri 没有解析出路径：' + uri;
    } catch (e) {
      ReportSink.lastErr = '解析「下载」目录路径失败：' + e.message + '（uri=' + uri + '）';
    }
    return '';
  }

  private static exists(path: string): boolean {
    try {
      return fs.accessSync(path);
    } catch (e) {
      return false;
    }
  }

  // 把一段文本写进指定目录; 目录不存在就建(建不了就把原因带回)
  private static writeInto(dir: string, name: string, text: string): ReportWriteResult {
    const res: ReportWriteResult = {
      ok: false, dir: dir, name: name, path: '', method: '未写入', err: ''
    };
    if (dir.length === 0) {
      res.err = '目标目录为空。';
      return res;
    }
    if (!ReportSink.exists(dir)) {
      try {
        fs.mkdirSync(dir, true);
      } catch (e) {
        res.err = '建目录 ' + dir + ' 失败：' + e.message;
        return res;
      }
    }
    const full: string = dir + '/' + name;
    // 硬断言: 落到「下载」目录的这一份 .txt 也必须是合法 UTF-8(名字不是 .txt 的照旧原样写)。
    let body: string = text;
    if (name.endsWith('.txt')) {
      const why: string = assertTxtUtf8Safe(body);
      if (why.length > 0) {
        // 不静默: 就地把孤立代理项换成 U+FFFD。为什么这里不额外记一条: 报告正文末尾
        // 本来就有「文本编码自检」一段(由 FullRun.textChunks 在拼装时写入, 含替换个数与
        // 第一处位置), 这一份写出去的正文里就带着那句自证, 比再记一个字段更不容易被漏看。
        body = sanitizeReportText(body);
      }
    }
    let f: fs.File | null = null;
    try {
      f = fs.openSync(full, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(f.fd, body);
    } catch (e) {
      res.err = '写 ' + full + ' 失败：' + e.message;
      return res;
    } finally {
      if (f !== null) {
        try {
          fs.closeSync(f);
        } catch (e2) {
          // 关不掉不影响"内容已写入"这个事实; 只记不抛
        }
      }
    }
    res.ok = true;
    res.path = full;
    res.method = '下载目录直写';
    return res;
  }

  // ① 直接写 <下载>/<包名>/<name>
  static writeText(name: string, text: string): ReportWriteResult {
    const dir: string = ReportSink.resolveDir();
    if (dir.length === 0) {
      return {
        ok: false, dir: '', name: name, path: '', method: '未写入',
        err: ReportSink.lastErr.length > 0 ? ReportSink.lastErr : '没有解析出「下载」目录路径。'
      };
    }
    return ReportSink.writeInto(dir, name, text);
  }

  // ② 官方「下载模式」: DocumentViewPicker(pickerMode = DOWNLOAD)
  //    跳过文件选择界面直接保存到 Download/<包名>/; 返回的 URI 自带持久化权限。
  //    返回的 uri 可能是目录 uri(官方示例就是这么用的), 也可能是文件 uri —— 两种都处理。
  static async saveViaDownloadPicker(ctx: common.UIAbilityContext, name: string,
    text: string): Promise<ReportWriteResult> {
    const res: ReportWriteResult = {
      ok: false, dir: '', name: name, path: '', method: '未写入', err: ''
    };
    let uri: string = '';
    try {
      const doc = new picker.DocumentViewPicker(ctx);
      const opt = new picker.DocumentSaveOptions();
      opt.pickerMode = picker.DocumentPickerMode.DOWNLOAD;
      opt.newFileNames = [name];
      const uris: string[] = await doc.save(opt);
      if (uris.length === 0) {
        res.err = '没有拿到保存结果 URI（可能被取消）。';
        return res;
      }
      uri = uris[0];
    } catch (e) {
      res.err = '下载模式保存器打不开：' + e.message;
      return res;
    }
    // uri -> 真实路径
    let p: string = '';
    try {
      p = new fileUri.FileUri(uri).path;
    } catch (e) {
      res.err = '把保存结果 URI 转成路径失败：' + e.message + '（uri=' + uri + '）';
      return res;
    }
    if (p.length === 0) {
      res.err = '保存结果 URI 解析出空路径（uri=' + uri + '）。';
      return res;
    }
    // 是文件还是目录: 以 uri 是不是以 name 结尾来判断(DOWNLOAD 模式官方示例给的是目录 uri)
    let dir: string = p;
    let full: string = p;
    if (!p.endsWith('/' + name)) {
      dir = p;
      full = p + '/' + name;
    } else {
      const cut: number = p.length - name.length - 1;
      dir = cut > 0 ? p.substring(0, cut) : p;
    }
    ReportSink.dirPath = dir;
    const w: ReportWriteResult = ReportSink.writeInto(dir, name, text);
    if (w.ok) {
      ReportSink.grantedOnce = true;
      w.method = '下载模式保存';
      return w;
    }
    res.dir = dir;
    res.err = '拿到保存位置（' + full + '）但写入失败：' + w.err;
    return res;
  }

  // 同名 report-latest.txt 便利副本(与沙箱口径一致: 打开就能看最近一次)
  static writeLatest(text: string): ReportWriteResult {
    return ReportSink.writeText('report-latest.txt', text);
  }

  // ---------------------------------------------------------------------------
  // 追加写(不覆盖) —— 给「历史成绩.txt」用
  // ---------------------------------------------------------------------------
  // 与 writeText 的区别只有一个: 打开方式从 TRUNC 改成 APPEND, 因此**历次记录都留在同一个
  // 文件里**, 不做"只留最近 N 次"的裁剪。
  //   head: 文件不存在或长度为 0 时才写(表头 + 口径那一句); 已有内容时一个字都不动。
  //   body: 本次要追加的那一行。
  // 目录解析、UTF-8 硬断言、失败原因回传都与 writeText **同一套**(同一个 ReportSink,
  // 因此落在同一个目录; 不另开一份目录逻辑, 免得两处各说各话)。
  static appendInto(dir: string, name: string, head: string, body: string): ReportWriteResult {
    const res: ReportWriteResult = {
      ok: false, dir: dir, name: name, path: '', method: '未写入', err: ''
    };
    if (dir.length === 0) {
      res.err = '目标目录为空。';
      return res;
    }
    if (!ReportSink.exists(dir)) {
      try {
        fs.mkdirSync(dir, true);
      } catch (e) {
        res.err = '建目录 ' + dir + ' 失败：' + e.message;
        return res;
      }
    }
    const full: string = dir + '/' + name;
    let fresh: boolean = true;
    try {
      if (fs.accessSync(full)) {
        fresh = fs.statSync(full).size === 0;
      }
    } catch (e) {
      fresh = true;
    }
    // 硬断言: 与报告同一套 —— 这一份也必须是合法 UTF-8(有孤立代理项就就地替换, 不静默写坏)
    let payload: string = fresh ? (head + body) : body;
    const why: string = assertTxtUtf8Safe(payload);
    if (why.length > 0) {
      payload = sanitizeReportText(payload);
    }
    let f: fs.File | null = null;
    try {
      f = fs.openSync(full, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.APPEND);
      fs.writeSync(f.fd, payload);
    } catch (e) {
      res.err = '追加写 ' + full + ' 失败：' + e.message;
      return res;
    } finally {
      if (f !== null) {
        try {
          fs.closeSync(f);
        } catch (e2) {
          // 关不掉不影响"内容已写入"这个事实; 只记不抛
        }
      }
    }
    res.ok = true;
    res.path = full;
    res.method = '下载目录追加';
    return res;
  }

  // <下载>/<包名>/<name> 追加写(目录没解析出来时如实返回原因)
  static appendText(name: string, head: string, body: string): ReportWriteResult {
    const dir: string = ReportSink.resolveDir();
    if (dir.length === 0) {
      return {
        ok: false, dir: '', name: name, path: '', method: '未写入',
        err: ReportSink.lastErr.length > 0 ? ReportSink.lastErr : '没有解析出「下载」目录路径。'
      };
    }
    return ReportSink.appendInto(dir, name, head, body);
  }

  // 某个文件当前有多少字节(不存在 / 读不到 -> -1)。给"要不要写表头"和状态显示用。
  static fileBytes(name: string): number {
    try {
      const dir: string = ReportSink.resolveDir();
      if (dir.length === 0) {
        return -1;
      }
      const full: string = dir + '/' + name;
      if (!ReportSink.exists(full)) {
        return -1;
      }
      return fs.statSync(full).size;
    } catch (e) {
      return -1;
    }
  }

  // 读一个文件末尾 max 个字符(不存在/读不到 -> '')。给"用时间戳去重"用。
  static tailOf(name: string, max: number): string {
    try {
      const dir: string = ReportSink.resolveDir();
      if (dir.length === 0) {
        return '';
      }
      const full: string = dir + '/' + name;
      if (!ReportSink.exists(full) || fs.statSync(full).size === 0) {
        return '';
      }
      const text: string = fs.readTextSync(full);
      if (text.length <= max) {
        return text;
      }
      return text.substring(text.length - max);
    } catch (e) {
      return '';
    }
  }

  // 列目录里已有的 report-*.txt(最多 max 个) —— 给界面显示"已经写进去哪些文件"
  static listReports(max: number): string[] {
    const out: string[] = [];
    const dir: string = ReportSink.resolveDir();
    if (dir.length === 0 || !ReportSink.exists(dir)) {
      return out;
    }
    try {
      const names: string[] = fs.listFileSync(dir);
      for (const n of names) {
        if (n.startsWith('report-') && n.endsWith('.txt')) {
          out.push(n);
          if (out.length >= max) {
            break;
          }
        }
      }
    } catch (e) {
      ReportSink.lastErr = '列「下载」目录失败：' + e.message;
    }
    return out;
  }
}
