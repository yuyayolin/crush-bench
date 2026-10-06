#ifndef AURORA_GPU_H
#define AURORA_GPU_H

#include <string>

int gpuSceneCount();
std::string gpuSceneName(int id);
bool gpuSurfaceReady();
std::string gpuInitHeadless();
void gpuSetWindow(void* nativeWindow, int width, int height);
void gpuSetSize(int width, int height);
void gpuDestroySurface();
std::string gpuStartScene(int sceneId, int frames);
std::string gpuPoll();
void gpuStop();
void gpuShutdown();

#endif
