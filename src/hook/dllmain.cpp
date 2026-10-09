// NextPerf 注入式钩子 DLL
//
// 被 LoadLibrary 载入游戏进程后，在独立线程里完成：
//   1) 打开与主程序之间的共享内存
//   2) 劫持 DXGI/D3D12 的 vtable
//   3) 在每次 Present 前量帧时间、画叠加、写回遥测
//
// 任何一步失败都必须安静降级，绝不能影响游戏本身。
#include <windows.h>

#include "np_hook.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            NpHookAttach(module);
            break;
        case DLL_PROCESS_DETACH:
            NpHookDetach();
            break;
        default:
            break;
    }
    return TRUE;
}
