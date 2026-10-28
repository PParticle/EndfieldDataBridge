# EndfieldDataBridge

供 EndfieldVT 使用的终末地游戏数据桥接插件，目前仅支持 Applepie Manager。

当前实现官方 SDK 接入和启停生命周期，尚未采集 Buff、技力等游戏数据，也未提供进程间 API。正式产物为 `EndfieldDataBridge.dll`。

## 构建

环境：Windows x64、Visual Studio C++ Build Tools（x64 MSVC 和 Windows SDK）。不需要 MinGW 或 CMake。

两个仓库保持同级：

```text
src/
├── ApplepieManager/
│   ├── sdk/
│   ├── src/
│   └── deps/
└── EndfieldDataBridge/
```

直接引用相邻仓库的官方 SDK，不复制或改写 SDK。当前已验证 ApplepieManager v0.2.3（提交 `a237d96936a3a184f70edb2cd20ba9e6ed30e91d`，SDK ABI 1.1）；更新该仓库后需重新构建和验证。

在 PowerShell 中运行：

```powershell
cd D:\Workspace\src\EndfieldDataBridge
.\build.bat
```

脚本使用 `vswhere` 查找 Build Tools 并初始化 x64 MSVC；也支持已配置好的 x64 Developer Command Prompt。输出在 `bin\`，使用静态 MSVC 运行库。

## 离线验证

```powershell
.\build.bat /verify
```

验证宿主链接相邻仓库的真实 Core 和其 MinHook 依赖，再加载实际生成的 DLL。检查只读清单、导出入口、Attach、启用、停用恢复、再次启用及旧 epoch 拒绝。

验证在独立进程中运行，不需要启动游戏。只有验证宿主使用有超时的状态查询等待停用完成，因为 SDK 未提供生命周期完成事件；插件本身没有后台线程或轮询。

## 运行

先按 [ApplepieManager 说明](https://github.com/zonemodAE/ApplepieManager)安装 Manager 和对应图形后端的代理 DLL，再将 `bin\EndfieldDataBridge.dll` 放入游戏 EXE 同级的 `plugin\` 目录，重启游戏。插件 DLL 通过 Manager 加载，不能直接运行。

开发编译需要 SDK 头文件；Manager 和代理 DLL 用于游戏运行，不参与本插件链接。启停日志通过 `OutputDebugStringA` 输出，前缀为 `[EndfieldDataBridge]`。插件目录只在启动时扫描，替换 DLL 后需要重启游戏。

## 源码

- `src/plugin.cpp`：只读清单、Attach、Enable 和生命周期回调。
- `build.bat`：MSVC 构建及可选离线验证。
- `tests/verify.cpp`：实际 DLL 的离线生命周期验证。

使用仓库现有的 [AGPL-3.0 许可证](LICENSE)。
