# gpu7 着色器离线核对报告

由 `tools/gpu7_shader_check.py` 自动生成; 核对对象 `entry/src/main/cpp/gpu7/gpu7_renderer.cpp`。

- 程序数: 34 (无错误 34 / 有错误 0 / 无错误但有告警 3)
- 着色器源码字符串: 39
- 表结构检查(条数/顺序/取用): 通过

## 逐程序核对表

| # | ProgId | 程序名 | VS | FS | 绘制 | VS out | FS in | uniform(VS+FS) | C++ set | 结论 |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | P_BLUR_H | blur_h | kQuadVS | FS_BLUR_H | TRIANGLES | vec2 vUV | vec2 vUV | uRadius, uSigma, uTex, uTexel | uRadius, uSigma, uTex, uTexel | 通过 |
| 1 | P_BLUR_V | blur_v | kQuadVS | FS_BLUR_V | TRIANGLES | vec2 vUV | vec2 vUV | uRadius, uSigma, uTex, uTexel | uRadius, uSigma, uTex, uTexel | 通过 |
| 2 | P_SCENE | scene | kQuadVS | FS_SCENE | TRIANGLES | vec2 vUV | vec2 vUV | uSeed | uSeed | 通过 |
| 3 | P_FT_ACC | ft_acc | kPointIndexVS | FS_FT_ACC | POINTS | float vIdx | float vIdx | uCurr, uDisp, uLevelSize, uLevelTexel, uPrev, uSeed | uCurr, uDisp, uLevelSize, uLevelTexel, uPrev, uSeed | 通过 |
| 4 | P_FT_UPDATE | ft_update | kQuadVS | FS_FT_UPDATE | TRIANGLES | vec2 vUV | vec2 vUV | uAcc, uDisp, uIter | uAcc, uDisp, uIter | 通过 |
| 5 | P_FM_HIST | fm_hist | kQuadVS | FS_FM_HIST | TRIANGLES | vec2 vUV | vec2 vUV | uFrame, uTex, uTexel | uFrame, uTex, uTexel | 通过 |
| 6 | P_FM_MATCH | fm_match | kQuadVS | FS_FM_MATCH | TRIANGLES | vec2 vUV | vec2 vUV | uDesc, uFrame, uSummary, uTile | uDesc, uFrame, uSummary, uTile | 通过(告警 1) |
| 7 | P_FM_SUMMARY | fm_summary | kPointVS | FS_FM_SUMMARY | POINTS | vec2 vUV | vec2 vUV | uMatch | uMatch | 通过(告警 1) |
| 8 | P_FLUID_ADV | fluid_advect | kQuadVS | FS_FLUID_ADV | TRIANGLES | vec2 vUV | vec2 vUV | uDt, uSrc, uTexel, uVel | uDt, uSrc, uTexel, uVel | 通过 |
| 9 | P_FLUID_DIVERG | fluid_divergence | kQuadVS | FS_FLUID_DIVERG | TRIANGLES | vec2 vUV | vec2 vUV | uTexel, uVel | uTexel, uVel | 通过 |
| 10 | P_FLUID_JACOBI | fluid_jacobi | kQuadVS | FS_FLUID_JACOBI | TRIANGLES | vec2 vUV | vec2 vUV | uDiv, uPres, uRbeta, uTexel | uDiv, uPres, uRbeta, uTexel | 通过 |
| 11 | P_FLUID_GRADSUB | fluid_gradsub | kQuadVS | FS_FLUID_GRADSUB | TRIANGLES | vec2 vUV | vec2 vUV | uFrame, uPres, uTexel, uVel | uFrame, uPres, uTexel, uVel | 通过 |
| 12 | P_FLUID_DISPLAY | fluid_display | kQuadVS | FS_FLUID_DISPLAY | TRIANGLES | vec2 vUV | vec2 vUV | uVel | uVel | 通过 |
| 13 | P_HOUGH_EDGE | hough_edge | kQuadVS | FS_HOUGH_EDGE | TRIANGLES | vec2 vUV | vec2 vUV | uTex, uTexel | uTex, uTexel | 通过 |
| 14 | P_HOUGH_VOTE_A | hough_vote_a | kPointIndexVS | FS_HOUGH_VOTE_A | POINTS | float vIdx | float vIdx | uAngleScale, uEdges, uScale | uAngleScale, uEdges, uScale | 通过 |
| 15 | P_HOUGH_VOTE_B | hough_vote_b | kPointIndexVS | FS_HOUGH_VOTE_B | POINTS | float vIdx | float vIdx | uEdges, uFrame, uScale | uEdges, uFrame, uScale | 通过 |
| 16 | P_HOUGH_DRAW | hough_draw | kQuadVS | FS_HOUGH_DRAW | TRIANGLES | vec2 vUV | vec2 vUV | uAcc, uFrame | uAcc, uFrame | 通过 |
| 17 | P_PART_UPDATE | particle_update | kQuadVS | FS_PART_UPDATE | TRIANGLES | vec2 vUV | vec2 vUV | uDt, uFrame, uState | uDt, uFrame, uState | 通过 |
| 18 | P_PART_DRAW | particle_draw | VS_PART_DRAW | FS_PART_DRAW | POINTS | vec2 vUV | vec2 vUV | uCount, uFrame, uPoint, uState | uCount, uPoint, uState | 通过(告警 1) |
| 19 | P_PT_TRACE | path_trace | kQuadVS | FS_PT_TRACE | TRIANGLES | vec2 vUV | vec2 vUV | uAccum, uFrame, uRes, uSpp | uAccum, uFrame, uRes, uSpp | 通过 |
| 20 | P_PT_DRAW | path_draw | kQuadVS | FS_PT_DRAW | TRIANGLES | vec2 vUV | vec2 vUV | uAccum | uAccum | 通过 |
| 21 | P_PF_LUT | photo_lut | kQuadVS | FS_PF_LUT | TRIANGLES | vec2 vUV | vec2 vUV | uFrame, uLut, uTex | uFrame, uLut, uTex | 通过 |
| 22 | P_PF_SAT | photo_saturation | kQuadVS | FS_PF_SAT | TRIANGLES | vec2 vUV | vec2 vUV | uTex | uTex | 通过 |
| 23 | P_PF_SHARPEN | photo_sharpen | kQuadVS | FS_PF_SHARPEN | TRIANGLES | vec2 vUV | vec2 vUV | uTex, uTexel | uTex, uTexel | 通过 |
| 24 | P_PF_VIGNETTE | photo_vignette | kQuadVS | FS_PF_VIGNETTE | TRIANGLES | vec2 vUV | vec2 vUV | uTex | uTex | 通过 |
| 25 | P_PF_SEPIA | photo_sepia | kQuadVS | FS_PF_SEPIA | TRIANGLES | vec2 vUV | vec2 vUV | uTex | uTex | 通过 |
| 26 | P_RAW_BAYER | raw_bayer | kQuadVS | FS_RAW_BAYER | TRIANGLES | vec2 vUV | vec2 vUV | uFrame | uFrame | 通过 |
| 27 | P_RAW_DEMOSAIC | raw_demosaic | kQuadVS | FS_RAW_DEMOSAIC | TRIANGLES | vec2 vUV | vec2 vUV | uBayer, uTexel | uBayer, uTexel | 通过 |
| 28 | P_RAW_COLOR | raw_color | kQuadVS | FS_RAW_COLOR | TRIANGLES | vec2 vUV | vec2 vUV | uFrame, uTex | uFrame, uTex | 通过 |
| 29 | P_SR_CONV | super_resolution_conv | VS_PASS | FS_SR_CONV | TRIANGLES | vec2 vUV | vec2 vUV | uCh, uFrame, uIn0, uIn1, uIn2, uIn3, uOffset, uRelu, uTexel | uCh, uFrame, uIn0, uIn1, uIn2, uIn3, uOffset, uRelu, uTexel | 通过 |
| 30 | P_VF_DENOISE | video_denoise | kQuadVS | FS_VF_DENOISE | TRIANGLES | vec2 vUV | vec2 vUV | uFrame, uMotion, uPrev | uFrame, uMotion, uPrev | 通过 |
| 31 | P_VF_BLEND | video_blend | kQuadVS | FS_VF_BLEND | TRIANGLES | vec2 vUV | vec2 vUV | uA, uB, uT | uA, uB, uT | 通过 |
| 32 | P_VF_SHARP | video_sharpen | kQuadVS | FS_VF_SHARP | TRIANGLES | vec2 vUV | vec2 vUV | uTex, uTexel | uTex, uTexel | 通过 |
| 33 | P_VF_GRADE | video_grade | kQuadVS | FS_VF_GRADE | TRIANGLES | vec2 vUV | vec2 vUV | uFrame, uTex | uFrame, uTex | 通过 |

## 告警明细(可以通过, 但值得清理)

- `fm_match` (P_FM_MATCH): R2 FS 的 in vec2 vUV 声明后没有使用
- `fm_summary` (P_FM_SUMMARY): R2 FS 的 in vec2 vUV 声明后没有使用
- `particle_draw` (P_PART_DRAW): R8 FS 声明了 uniform uFrame(float) 但既没用到也没被 set(死声明)

## 规则清单

错误级: R1 VS out == FS in(名字+类型); R3 片元里禁止 gl_VertexID/gl_InstanceID;
R4 gl_PointCoord 只在点程序 FS; R5 gl_PointSize 只在点程序 VS;
R6 被用到的 uniform 必须有 C++ set; R7 C++ set 的 uniform 必须已声明;
R9 登记表/枚举/取用一致(34 条); R10 #version 300 es + FS precision;
R11 顶点里禁止片元专用内建; R12 vecN 构造函数分量数正好等于 N。

告警级: R2 varying 只声明不使用(驱动消除后可能两端不一致); R8 死 uniform 声明。
