# EndfieldDataBridge

《明日方舟：终末地》战斗数据桥接插件。插件只采集数据，不改动游戏画面，通过共享内存提供给外部小程序（如 EndfieldVT）读取：

- **技力（SP）**：当前值、上限（随模式变化，如 300 / 700 / 1000）、每格技力。
- **干员**：四名干员的终结技充能、连携技冷却、当前操控角色。
- **Buff / Debuff**：队友和敌人身上的状态，包括层数、剩余时间、持续时间、计时速率，以及拥有者、施加者（子 Buff 追溯到父 Buff 的施加者）和添加、刷新、层数变化、结束事件。

采集在游戏线程上只做内存读写，不写文件、不走网络。

## 下载

前置插件管理器：[Applepie Manager](https://github.com/zonemodAE/ApplepieManager)

您可以在 [Releases](https://github.com/PParticle/EndfieldDataBridge/releases) 下载最新发布版或从源代码自行编译。

## 安装

先安装前置插件管理器：[Applepie Manager](https://github.com/zonemodAE/ApplepieManager)，再将以下文件复制到游戏目录（`Endfield.exe` 所在文件夹）：

```
bin/EndfieldDataBridge.dll  → 游戏目录/plugin/EndfieldDataBridge.dll
```

> 如果您**没有安装过此类型的插件**，您可能需要自行创建 `plugin` 文件夹。

## 使用说明

1. 按上述方式安装后启动游戏，进入战斗。插件在战斗 HUD 显示时每 50 ms 采集一次，写入共享内存 `Local\EndfieldDataBridge.v1`。
2. 外部小程序按 [`include/endfield_bridge.h`](include/endfield_bridge.h) 的布局只读映射这块内存。头文件开头写明了读取规则：
   - `seq` 为奇数表示正在写入；读取前后 `seq` 相同才算一次完整读取。
   - `session` 变化时丢弃缓存，重新读取 Buff 表。
   - Buff 的 `key` 在刷新后保持不变；事件环 `events` 按顺序记录添加、刷新、层数变化和结束。
   - 剩余时间按 `life_time - min(距 sampled_at 的秒数, 0.25) × time_dilation` 计算，过场减速或暂停时也能对上游戏内倒计时。
   - `published_at` 不再更新（暂停、HUD 隐藏）时停止倒计时；`active` 为 0 时数据已失效。
3. 自带测试工具 `bin\efb_reader.exe`（从源码编译后生成），游戏运行时在终端中执行：

   ```powershell
   .\bin\efb_reader.exe            # 实时显示技力、干员和全部 Buff
   .\bin\efb_reader.exe --once     # 输出一次当前状态
   .\bin\efb_reader.exe --events   # 滚动显示 Buff 事件
   ```

4. 排查问题时，在 `plugin\` 目录中新建空文件 `EndfieldDataBridge_debug.txt` 并重启游戏，插件会写入详细日志 `EndfieldDataBridge_log.txt`，并导出元数据 `EndfieldDataBridge_dump.txt`。正常使用时请删除该文件。

插件目录只在启动时扫描，替换 DLL 后需要重启游戏。

## 构建

环境：Windows x64、Visual Studio C++ Build Tools（x64 MSVC 和 Windows SDK）。

```text
src/
├── ApplepieManager/
│   ├── sdk/
│   ├── src/
│   └── deps/
└── EndfieldDataBridge/
```

在 PowerShell 中运行：

```powershell
cd D:\Workspace\src\EndfieldDataBridge
.\build.bat
```

脚本使用 `vswhere` 查找 Build Tools 并初始化 x64 MSVC；也支持已配置好的 x64 Developer Command Prompt。输出在 `bin\`（`EndfieldDataBridge.dll` 和 `efb_reader.exe`），使用静态 MSVC 运行库。

## 离线验证

```powershell
.\build.bat /verify
```

验证宿主链接相邻仓库的真实 Core 和其 MinHook 依赖，再加载实际生成的 DLL。检查只读清单、导出入口、Attach、启用、停用恢复、再次启用及旧 epoch 拒绝。验证在独立进程中运行，不需要启动游戏。

## 源码

- `src/plugin.cpp`：生命周期、IL2CPP 字段解析、UI hook 采集和共享内存发布。
- `include/endfield_bridge.h`：共享内存布局和读取规则，供外部小程序使用。
- `tools/efb_reader.cpp`：共享内存的测试读取工具。
- `build.bat`：MSVC 构建及可选离线验证。
- `tests/verify.cpp`：实际 DLL 的离线生命周期验证。

使用仓库现有的 [AGPL-3.0 许可证](LICENSE)。
