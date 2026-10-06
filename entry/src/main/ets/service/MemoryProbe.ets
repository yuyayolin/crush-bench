import hidebug from '@ohos.hidebug';

// ---------------------------------------------------------------------------
// 可用内存采样(CS1 单核/多核跑分前的护栏)
//
// 官方 API: @ohos.hidebug 的 getSystemMemInfo()(API 12+, 无需权限),
// 返回 { totalMem, freeMem, availableMem }, 单位 KB(bigint)。
// 若该接口在本机不可用/抛异常/返回 0, 退化为"不检查但保留结构"(supported = false)。
// 只提示, 不阻止运行, 不改动任何测试项与计分。
// ---------------------------------------------------------------------------

// 低于该值给出警告(不阻止)
export const LOW_MEM_MB = 1536;
// 逐项内存闸门的系统余量: 要求 可用内存 >= 单项预估峰值 + 该余量,
// 否则跳过该项。余量是留给系统/图形栈/codec 的, 不计分。
export const GATE_HEADROOM_MB = 512;

export interface MemStatus {
  supported: boolean;
  availableMb: number;
  totalMb: number;
  low: boolean;
  at: number;
  note: string;
}

export class MemoryProbe {
  static sample(): MemStatus {
    try {
      const info: hidebug.SystemMemInfo = hidebug.getSystemMemInfo();
      const availMb: number = Math.round(Number(info.availableMem) / 1024);
      const totalMb: number = Math.round(Number(info.totalMem) / 1024);
      if (availMb <= 0) {
        return {
          supported: false, availableMb: 0, totalMb: totalMb, low: false, at: Date.now(),
          note: '内存接口返回 0，本次未做检查'
        };
      }
      const low: boolean = availMb < LOW_MEM_MB;
      return {
        supported: true, availableMb: availMb, totalMb: totalMb, low: low, at: Date.now(),
        note: low ? '可用内存低于 1.5 GB' : '可用内存正常'
      };
    } catch (e) {
      return {
        supported: false, availableMb: 0, totalMb: 0, low: false, at: Date.now(),
        note: '内存接口不可用，本次未做检查'
      };
    }
  }

  // 逐项闸门判定: 只在接口可用时生效(不可用就降级为"不做闸门")。
  // 返回 null 表示放行(接口不可用, 或可用内存足够); 返回字符串表示必须跳过该项, 内容是原因。
  static gate(s: MemStatus, needMb: number): string | null {
    if (!s.supported) {
      return null;
    }
    const need: number = needMb + GATE_HEADROOM_MB;
    if (s.availableMb >= need) {
      return null;
    }
    return '已跳过: 可用内存不足(' + s.availableMb.toString() + ' MB < 需求 ' +
      need.toString() + ' MB = 预估峰值 ' + needMb.toString() + ' MB + 余量 ' +
      GATE_HEADROOM_MB.toString() + ' MB)';
  }

  static text(s: MemStatus): string {
    if (!s.supported) {
      return s.note;
    }
    return '可用内存 ' + s.availableMb.toString() + ' MB / 总 ' + s.totalMb.toString() + ' MB';
  }

  // 主显示用: 只要拿到数字就按 MB 显示(题面要求"可用内存 xx MB")
  static short(s: MemStatus): string {
    if (!s.supported) {
      return s.note;
    }
    return '可用内存 ' + s.availableMb.toString() + ' MB';
  }
}
