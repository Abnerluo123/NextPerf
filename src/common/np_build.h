// 把「传感器快照 + 遥测数据 + 历史采样 + 用户配置」整理成面板要显示的一行行文本。
// 主程序和注入钩子共用，保证游戏内叠加与桌面叠加显示完全一致。
#pragma once

#include "np_common.h"
#include "np_panel.h"

namespace np {

void BuildPanelData(PanelData& out, const NPConfig& c, const NPSensors& s, const NPTelemetry& t,
                    const NPHistory* h);

}  // namespace np
