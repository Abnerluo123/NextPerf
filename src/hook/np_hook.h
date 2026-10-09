// 注入到游戏进程里的部分：钩子入口
#pragma once

#include <windows.h>

void NpHookAttach(HMODULE self);
void NpHookDetach();
