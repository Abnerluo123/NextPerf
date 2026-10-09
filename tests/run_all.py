"""跑完整验证套件：vtable 下标 -> 编译测试宿主 -> 四条注入链路。

用法： python tests/run_all.py [--skip-build]
退出码 0 = 全绿。
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

STEPS = [
    ("结构体镜像一致性（C++ vs Python ctypes）", [sys.executable, os.path.join(HERE, "struct_check.py")]),
    ("vtable 下标核对（头文件 vs Vt:: 枚举）", [sys.executable, os.path.join(HERE, "vt_check.py")]),
    ("注入到已在运行的游戏 · D3D12", [sys.executable, os.path.join(HERE, "verify_inject.py")]),
    ("注入到已在运行的游戏 · D3D11", [sys.executable, os.path.join(HERE, "verify_inject.py"), "--d3d11"]),
    ("图形初始化前注入 · 工厂钩子", [sys.executable, os.path.join(HERE, "verify_early.py")]),
    ("「注入到前台进程」按钮", [sys.executable, os.path.join(HERE, "verify_frontinject.py")]),
    # 指标链路：原来这套「回归」只覆盖注入，传感器与计算一概没验证过。
    ("指标链路 · PDH GPU 帧时间端到端", [sys.executable, os.path.join(HERE, "verify_metrics.py")]),
]


def main():
    skip_build = "--skip-build" in sys.argv
    env = dict(os.environ, PYTHONIOENCODING="utf-8")

    sys.path.insert(0, HERE)
    from verify_inject import kill_all
    kill_all()   # 上一轮万一有残留，先清掉（残留会锁住 dist\NextPerf.exe）

    if not skip_build:
        print("### 编译测试宿主")
        r = subprocess.run(["cmd", "/c", os.path.join(HERE, "build_hosts.bat")], cwd=ROOT, env=env)
        if r.returncode != 0:
            print("测试宿主编译失败")
            return 1

    fails = []
    skipped = []
    for name, cmd in STEPS:
        print("\n### %s" % name)
        r = subprocess.run(cmd, cwd=ROOT, env=env)
        if r.returncode == 2:
            # 约定：退出码 2 = 环境不满足（例如抢不到前台），跳过而非失败
            skipped.append(name)
        elif r.returncode != 0:
            fails.append(name)

    kill_all()
    print("\n" + "=" * 60)
    if skipped:
        print("跳过 %d 项（环境不满足，非产品缺陷）：" % len(skipped))
        for s_ in skipped:
            print("  - %s" % s_)
    if fails:
        print("失败 %d 项：" % len(fails))
        for f in fails:
            print("  - %s" % f)
        return 1
    print("全部通过 ✓" + ("（其中 %d 项跳过）" % len(skipped) if skipped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
