# EndfieldDataBridge

《明日方舟：终末地》数据桥接插件。本插件不会修改任何游戏内容，仅通过共享内存供外部程序读取：

- **技力（SP）**：当前值、上限、每格技力
- **干员**：终结技充能、连携技冷却、主控干员
- **Buff / Debuff**：干员和敌人的状态，包括层数、剩余时间、持续时间、计时速率，以及拥有者、施加者（子 Buff 追溯到父 Buff 的施加者）以及添加、刷新、层数变化、结束事件
- **单位**：编队干员与所有视野内单位的HP/HP上限、失衡值/上限、是否失衡、失衡恢复状态和恢复倒计时
- **技能释放**：编队干员/(视野内)敌人的技能开始释放、结束释放，附技能类型、结束方式和打断原因

## 用户协议与免责声明

<details>
<summary>在下载、安装或使用本插件之前，请您仔细阅读本协议。<b>使用本插件即代表您已完整阅读、充分理解并同意遵守以下所有条款。</b></summary>

### 1. 开源许可与最终用户权利

- 本插件基于 **AGPL-3.0** 许可证在 GitHub 平台完全开源。用户可在遵守该许可证的前提下自由使用、修改和分发本插件的源代码。
- 最终用户（End User）在不对本插件进行修改的前提下，使用和分发本插件**不受任何限制**。此权利不因用户是否违反本协议而改变。

### 2. 反欺诈声明

- 您**不得**在网络销售平台公然售卖本插件**软件本体**且未提供 GitHub 仓库地址与售后服务。
- 本插件在 GitHub 平台完全免费开源，如果您是付费购买获取的，请知悉本插件可从 GitHub 免费获取。

### 3. 内容合规与行为约束

- 本插件本身不包含任何游戏美术资产。用户知悉并同意，《明日方舟：终末地》游戏内置的官方动画、场景、模型等资产其版权完全隶属于鹰角网络，并不适用 AGPL-3.0 协议。您**不应该**利用本插件，或利用游戏内置的官方动画、场景、模型等游戏资产，制作、播放或传播任何不合适的动作/动画（包括但不限于色情、暴力、政治敏感等违反法律法规或引起社区不适的内容）。使用者应自行负责确保起拥有任何汇入资源的版权，并遵守其许可和使用条款。

### 4. 风险与免责声明

- 本项目仅供学习、技术研究和交流目的。作者不对使用者如何使用本插件负责，包括任何误用情况。本插件中使用的明日方舟游戏数据资产版权均隶属于鹰角网络。使用本工具可能违反游戏服务条款，存在账号封禁的风险。因使用本插件而直接或间接导致的任何损失（包括但不限于账号封禁、游戏数据损坏等），**本项目不承担任何法律或经济责任**。用户需自行承担所有风险，强烈建议您在测试账号上运行。

</details>

## 下载

前置插件管理器：[Applepie Manager](https://github.com/zonemodAE/ApplepieManager)

您可以在 [Releases](https://github.com/PParticle/EndfieldDataBridge/releases) 下载最新发布版或从源代码自行编译。

## 安装

先安装前置插件管理器：[Applepie Manager](https://github.com/zonemodAE/ApplepieManager)，再将此插件移动到游戏目录下的plugin文件夹：

```
EndfieldDataBridge.dll  →  Endfield Game / plugin / EndfieldDataBridge.dll
```

> 如果您**没有安装过此类型的插件**，您可能需要自行创建 `plugin` 文件夹。

## 使用说明

1. 按上述方式安装后启动游戏，插件按采样频率（默认 50ms）读取游戏资源，写入共享内存 `Local\EndfieldDataBridge.1`。
2. 外部程序可按 [`include/endfield_bridge.h`](include/endfield_bridge.h) 的布局只读映射这块内存。头文件中有读取规则，[`include/endfield_bridge_reader.h`](include/endfield_bridge_reader.h) 是这些规则的 C++ 参考实现：
   - 开头是头部和段目录，技力、干员、Buff、事件、单位各是一段。各段的位置、元素大小和数量都从目录读取。
   - 头部和每一段各有自己的 `seq`：为奇数表示正在写入，读取前后相同才算一次完整读取。读完所需段后再确认头部 `seq` 未变，否则重读。
   - 兼容规则：同一主版本（`abi_major`）内只做追加——结构末尾加字段、新增段、新增事件类型和标志位。读取方只复制自己认识的长度，跳过不认识的段、事件类型和标志位。不兼容的改动会同时更换主版本号和共享内存名称。
   - 游戏崩溃后 `active` 可能仍为 1，需用 `writer_pid` 和 `writer_start_time` 确认写入进程仍在运行。
   - 技力、各干员、Buff 各自带 `sampled_at`（0 表示本次会话尚未采到），超过约 0.5 秒未更新（暂停、HUD 隐藏）即视为过期。
   - 头部 `provides` 表示可选数据是否可用（头顶血条单位、技能释放事件）；对应的 hook 装不上时其余数据照常提供。
   - Buff 段头部的 `flags` 标记表可能不完整：`PARTIAL` 表示插件启用前已存在的 Buff 可能缺失，`DROPPED` 表示表满后有 Buff 未记录（数量见 `dropped`）。
   - `session` 变化时丢弃缓存，重新读取 Buff 表。
   - Buff 的 `key` 在刷新后保持不变；事件环 `events` 按顺序记录每一次添加、刷新、层数变化和结束，`frame` 和 `at` 为游戏报告该变化的帧与时刻。
   - 剩余时间按 `life_time - min(距 sampled_at 的秒数, 0.25) × time_dilation` 计算。
3. 附带测试工具 `bin\efb_reader.exe`（从源码编译后生成）：

   ```powershell
   .\bin\efb_reader.exe            # 实时显示所有单位状态
   .\bin\efb_reader.exe --once     # 输出一次当前状态
   .\bin\efb_reader.exe --events   # 滚动显示 Buff 事件
   ```

## 基础配置

在 ApplePie Manager 的插件配置或游戏内的插件配置中修改以下选项，修改后自动保存并立即生效：

- **调试日志**（默认关闭）：开启时写入 `plugin\EndfieldDataBridge_log.txt`，插件启用时同时导出元数据 `plugin\EndfieldDataBridge_dump.txt`；关闭时不写任何文件。
- **采样频率**（默认 50ms，可选 50ms / 100ms / 150ms / 200ms）：读取技力、干员和 Buff 计时的间隔。原地刷新和层数变化最多晚一个间隔才出现；Buff 的添加和结束不受影响。当前值也写在共享内存头部的 `sample_interval_ms` 中。

设置保存在 `plugin\config\applepie.endfield.data_bridge.ini`。

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

脚本使用 `vswhere` 查找 Build Tools 并初始化 x64 MSVC；支持已配置好的 x64 Developer Command Prompt。输出位于 `bin\`（`EndfieldDataBridge.dll` 和 `efb_reader.exe`），使用静态 MSVC 运行库。

## 离线验证

```powershell
.\build.bat /verify
```

验证宿主链接相邻仓库的真实 Core 和其 MinHook 依赖，再加载实际生成的 DLL。检查只读清单、导出入口、Attach、启用、停用恢复、再次启用及旧 epoch 拒绝。随后运行数据验证：用伪造的 Buff 对象检查刷新合并、层数事件、同帧添加结束、对象复用、溢出计数、存活判断、分段独立和并发读取一致性，并用手工构造的新旧版本布局检查兼容读取。验证在独立进程中运行，不需要启动游戏。

## 源码

- `src/plugin.cpp`：插件入口、Manager 配置、调试日志、IL2CPP 解析和 hook 安装。
- `src/data.cpp`：游戏线程采集（技力、干员、Buff、单位、技能释放）和共享内存发布。
- `src/dump.cpp`：调试模式下的元数据导出。
- `src/bridge.h`：以上三个文件共用的内部声明。
- `include/endfield_bridge.h`：共享内存布局和读取规则，供外部程序使用。
- `include/endfield_bridge_reader.h`：读取规则的 C++ 参考实现（目录查找、分段一致读取、兼容复制、事件游标、进程存活、过期判断）。
- `tools/efb_reader.cpp`：共享内存的测试读取工具。
- `build.bat`：MSVC 构建及可选离线验证。
- `tests/verify.cpp`：实际 DLL 的离线生命周期验证。
- `tests/data_test.cpp`：Buff 跟踪、事件、溢出、存活判断、分段独立、跨版本兼容和并发读取的离线验证，全部经由参考读取实现检查。

使用仓库现有的 [AGPL-3.0 许可证](LICENSE)。
