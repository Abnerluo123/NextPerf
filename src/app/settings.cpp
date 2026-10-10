#include "np_app.h"

#include <shlobj.h>
#include <cstdio>

#include "common/np_json.h"

namespace npa {

static std::wstring gPath;

std::wstring SettingsPath() {
    if (!gPath.empty()) return gPath;
    wchar_t appData[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        std::wstring dir = std::wstring(appData) + L"\\NextPerf";
        CreateDirectoryW(dir.c_str(), nullptr);
        gPath = dir + L"\\config.json";
    } else {
        gPath = L"nextperf_config.json";
    }
    return gPath;
}

static std::string ReadFile(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) fread(&s[0], 1, (size_t)n, f);
    fclose(f);
    return s;
}

static bool WriteFile(const std::wstring& path, const std::string& data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

bool SettingsLoad() {
    NPDefaultConfig(&gApp.cfg);
    std::string txt = ReadFile(SettingsPath());
    if (txt.empty()) return false;
    bool ok = false;
    np::Json::Value v = np::Json::parse(txt, &ok);
    if (!ok || v.type != np::Json::Obj) return false;

    auto get = [&](const char* k) -> np::Json::Value* { return v.find(k); };
    if (auto* p = get("counters")) gApp.cfg.counters = (uint64_t)p->numOr((double)gApp.cfg.counters);
    // 自动注入开关（默认关闭）。旧配置没有这个键 -> 保持 0（关）。
    if (auto* p = get("autoInject")) gApp.cfg.autoInject = (uint32_t)p->numOr(0.0);
    // Low 帧口径（默认 0 = 窗口平均，与驱动面板对齐；1 = 严格的单帧百分位）
    if (auto* p = get("lowStrict")) gApp.cfg.lowStrict = (uint32_t)p->numOr(0.0);
    // 旧配置升级：v2 新增的图表计数器默认勾选一次；v3 改为行内小图开关
    {
        int ver = 1;
        if (auto* p = get("cfgVersion")) ver = (int)p->numOr(1);
        if (ver < 2) gApp.cfg.counters |= NP_C_CHART_USAGE | NP_C_CHART_FPS | NP_C_CHART_LATENCY;
        if (ver < 3) gApp.cfg.counters |= NP_C_GRAPH | NP_C_CHART_FPS | NP_C_CHART_LATENCY;
        // v4 新增「CPU 频率」「CPU 功耗」——旧配置默认勾选一次，
        // 否则新开关对老用户是关的，会让人以为功能没做出来。
        if (ver < 4) gApp.cfg.counters |= NP_C_CPU_CLOCK | NP_C_CPU_POWER;
        // v5：CPU Busy / CPU Wait 是「高级」项，**默认不显示**（用户要求）。
        // 这里显式清位而不是把它们踢出复选框 —— 用户仍可手动勾选启用；
        // 清位只在 ver<5 时执行一次，不会把用户的勾选反复抹掉。
        if (ver < 5) gApp.cfg.counters &= ~(NP_C_CPU_BUSY | NP_C_CPU_WAIT);
    }
    // 背景固定纯黑（用户明确要求），旧配置里的蓝黑色一律丢弃
    gApp.cfg.bgColor = 0xFF000000u;
    if (auto* p = get("textColor")) gApp.cfg.textColor = (uint32_t)p->numOr(gApp.cfg.textColor);
    if (auto* p = get("accentColor")) gApp.cfg.accentColor = (uint32_t)p->numOr(gApp.cfg.accentColor);
    if (auto* p = get("warnColor")) gApp.cfg.warnColor = (uint32_t)p->numOr(gApp.cfg.warnColor);
    if (auto* p = get("scale")) gApp.cfg.scale = (float)p->numOr(gApp.cfg.scale);
    if (auto* p = get("bgOpacity")) gApp.cfg.bgOpacity = (float)p->numOr(gApp.cfg.bgOpacity);
    if (auto* p = get("textOpacity")) gApp.cfg.textOpacity = (float)p->numOr(gApp.cfg.textOpacity);
    if (auto* p = get("offsetX")) gApp.cfg.offsetX = (int32_t)p->numOr(gApp.cfg.offsetX);
    if (auto* p = get("offsetY")) gApp.cfg.offsetY = (int32_t)p->numOr(gApp.cfg.offsetY);
    if (auto* p = get("fontHeight")) gApp.cfg.fontHeight = (uint32_t)p->numOr(gApp.cfg.fontHeight);
    if (auto* p = get("graphHeight")) gApp.cfg.graphHeight = (uint32_t)p->numOr(gApp.cfg.graphHeight);
    if (auto* p = get("overlayMode")) gApp.cfg.overlayMode = (uint32_t)p->numOr(gApp.cfg.overlayMode);
    if (auto* p = get("fpsCap")) gApp.cfg.fpsCap = (uint32_t)p->numOr(gApp.cfg.fpsCap);
    if (auto* p = get("pollMs")) gApp.cfg.pollMs = (uint32_t)p->numOr(gApp.cfg.pollMs);
    if (auto* p = get("updateHz")) gApp.cfg.updateHz = (uint32_t)p->numOr(gApp.cfg.updateHz);
    if (auto* p = get("deepEngineHook")) gApp.cfg.deepEngineHook = (uint32_t)p->numOr(1);
    if (auto* p = get("vtableProbe")) gApp.cfg.vtableProbe = (uint32_t)p->numOr(1);
    if (auto* p = get("simulate")) gApp.cfg.simulate = (uint32_t)p->numOr(0);

    gApp.games.clear();
    if (auto* arr = get("games")) {
        if (arr->type == np::Json::Arr) {
            for (auto& e : arr->arr) {
                if (e.type == np::Json::Str) {
                    GameEntry g;
                    g.path = np::Utf8ToWide(e.str);
                    gApp.games.push_back(g);
                }
            }
        }
    }
    return true;
}

void SettingsSave() {
    using np::Json;
    Json::Value root = Json::mkObj();
    root.obj["cfgVersion"] = Json::mkNum(5);
    root.obj["counters"] = Json::mkNum((double)gApp.cfg.counters);
    root.obj["autoInject"] = Json::mkNum((double)gApp.cfg.autoInject);
    root.obj["lowStrict"] = Json::mkNum((double)gApp.cfg.lowStrict);
    root.obj["bgColor"] = Json::mkNum(gApp.cfg.bgColor);
    root.obj["textColor"] = Json::mkNum(gApp.cfg.textColor);
    root.obj["accentColor"] = Json::mkNum(gApp.cfg.accentColor);
    root.obj["warnColor"] = Json::mkNum(gApp.cfg.warnColor);
    root.obj["scale"] = Json::mkNum(gApp.cfg.scale);
    root.obj["bgOpacity"] = Json::mkNum(gApp.cfg.bgOpacity);
    root.obj["textOpacity"] = Json::mkNum(gApp.cfg.textOpacity);
    root.obj["offsetX"] = Json::mkNum(gApp.cfg.offsetX);
    root.obj["offsetY"] = Json::mkNum(gApp.cfg.offsetY);
    root.obj["fontHeight"] = Json::mkNum(gApp.cfg.fontHeight);
    root.obj["graphHeight"] = Json::mkNum(gApp.cfg.graphHeight);
    root.obj["overlayMode"] = Json::mkNum(gApp.cfg.overlayMode);
    root.obj["fpsCap"] = Json::mkNum(gApp.cfg.fpsCap);
    root.obj["pollMs"] = Json::mkNum(gApp.cfg.pollMs);
    root.obj["updateHz"] = Json::mkNum(gApp.cfg.updateHz);
    root.obj["deepEngineHook"] = Json::mkNum(gApp.cfg.deepEngineHook);
    root.obj["vtableProbe"] = Json::mkNum(gApp.cfg.vtableProbe);
    root.obj["simulate"] = Json::mkNum(gApp.cfg.simulate);

    Json::Value games = Json::mkArr();
    for (auto& g : gApp.games) games.arr.push_back(Json::mkStr(np::WideToUtf8(g.path)));
    root.obj["games"] = games;

    WriteFile(SettingsPath(), Json::dump(root));
}

}  // namespace npa
