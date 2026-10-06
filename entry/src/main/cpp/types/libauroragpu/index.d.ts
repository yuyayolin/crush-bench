export const sceneCount: () => number;
export const sceneName: (id: number) => string;
export const ready: () => boolean;
export const prepare: () => string;
export const start: (scene: number, frames: number) => string;
export const poll: () => string;
export const stop: () => void;
