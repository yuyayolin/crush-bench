export const count: () => number;
export const name: (id: number) => string;
export const ready: () => boolean;
export const prepare: () => string;
/**
 * 跑第 id 项 CS1 GPU 负载。返回 JSON:
 * {"ok":true,"ms":..,"metric":"..","unit":"..","fps":..,"score":..,"basis":".."}
 * score = 0 表示该项未计分(单位语义不可比), 原因在 basis。
 */
export const run: (id: number) => string;
/**
 * GPU 复合分 = 已计分项(11 项里 Feature Matching / Horizon Detection 未计分)几何平均。
 * scores 可省略: 省略时用最近一次 run 的单项分(按负载 id 索引)。
 * 返回 JSON: {"ok":true,"mode":"gpu","composite":..,"count":..,"items":[..],"basis":".."}
 */
export const composite: (scores?: number[]) => string;
export const lastError: () => string;