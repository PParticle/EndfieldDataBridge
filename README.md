# EndfieldDataBridge

《明日方舟：终末地》数据桥接插件。本插件不会修改任何游戏内容，仅通过共享内存提供给外部程序读取：

- **技力（SP）**：当前值、上限、每格技力。
- **干员**：终结技充能(Ultimate Energy)、连携技(Combo Skill)冷却、当前操控角色。
- **Buff / Debuff**：层数、剩余时间、持续时间、计时速率，拥有者、施加者（子 Buff 追溯到父 Buff 施加者）、添加、刷新、层数变化、结束事件。
- **单位**：编队干员、所有视野内单位的HP、失衡状态。
- **锁定目标**：锁定的目标，是否手动/自动锁定。
- **技能释放**：技能开始/结束释放事件（不含普攻、重击、闪避、投射物/召唤物等附属单位的技能），附着、结束方式和打断来源。

## 下载

前置插件管理器：[Applepie Manager](https://github.com/zonemodAE/ApplepieManager)

您可以在 [Releases](https://github.com/PParticle/EndfieldDataBridge/releases) 下载最新发布版或从源代码自行编译。

## 安装

安装前置插件管理器：[Applepie Manager](https://github.com/zonemodAE/ApplepieManager)，再将以下文件复制到游戏目录：

```
EndfieldDataBridge.dll  → 游戏目录/plugin/EndfieldDataBridge.dll
```

> 如果您**没有安装过此类型的插件**，您可能需要自行创建 `plugin` 文件夹。

## 使用说明

1. 按上述方式安装后启动游戏。插件按采样频率（默认 50ms）读取对应资源，写入共享内存 `Local\EndfieldDataBridge.1`。
2. 外部程序按 [`include/endfield_bridge.h`](include/endfield_bridge.h) 的布局只读映射这块内存。[`include/endfield_bridge_reader.h`](include/endfield_bridge_reader.h) 是这些规则的 C++ 参考实现：
   - 开头是头部和段目录，技力、干员、Buff、事件、单位各一段。各段的位置、元素大小和数量都从目录读取。
   - 头部和每一段各有自己的 `seq`：为奇数表示正在写入，读取前后相同才算一次完整读取。读完需要的段后再确认头部 `seq` 未变，否则重读。
   - 兼容规则：同一主版本（`abi_major`）内只做追加——结构末尾加字段、新增段、新增事件类型和标志位。读取方只复制自己认识的长度，跳过不认识的段、事件类型和标志位，旧插件不会破坏新读取方，新插件也不会破坏旧读取方。不兼容的改动会同时更换主版本号和共享内存名称。
   - 游戏崩溃后 `active` 可能仍为 1，需用 `writer_pid` 和 `writer_start_time` 确认写入进程仍在运行。
   - 技力、各干员、Buff 各自带 `sampled_at`（0 表示本次会话尚未采到），超过约 0.5 秒未更新（暂停、HUD 隐藏）即视为过期。
   - 头部 `provides` 表示可选数据是否可用（头顶血条单位、技能释放事件）；对应的 hook 装不上时其余数据照常提供。
   - Buff 段头部的 `flags` 标记表可能不完整：`PARTIAL` 表示插件启用前已存在的 Buff 可能缺失，`DROPPED` 表示表满后有 Buff 未记录（数量见 `dropped`）。
   - `session` 变化时丢弃缓存，重新读取 Buff 表。
   - Buff 的 `key` 在刷新后保持不变；事件环 `events` 按顺序记录每一次添加、刷新、层数变化和结束，`frame` 和 `at` 为游戏报告该变化的帧与时刻。
   - 剩余时间按 `life_time - min(距 sampled_at 的秒数, 0.25) × time_dilation` 计算。
3. 自带测试工具 `bin\efb_reader.exe`（从源码编译后生成），游戏运行时在终端中执行：

   ```powershell
   .\bin\efb_reader.exe            # 实时显示技力、干员和全部 Buff
   .\bin\efb_reader.exe --once     # 输出一次当前状态
   .\bin\efb_reader.exe --events   # 滚动显示 Buff 事件
   ```

## 基础配置

在 Manager 的插件设置中修改以下选项，修改后自动保存并立即生效：

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

在 Shell 中运行：

```powershell
build.bat
```

脚本使用 `vswhere` 查找 Build Tools 并初始化 x64 MSVC；也支持已配置好的 x64 Developer Command Prompt。输出位于 `bin\`（`EndfieldDataBridge.dll` 和 `efb_reader.exe`），使用静态 MSVC 运行库。

使用仓库现有的 [AGPL-3.0 许可证](LICENSE)。
