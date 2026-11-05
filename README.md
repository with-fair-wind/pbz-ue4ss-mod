# PBZ UE4SS C++ mod 开发工程

本工程面向《影之刃零》（Phantom Blade Zero），使用根目录 CMake super-build，
从固定源码构建 UE4SS、代理 `dwmapi.dll` 和 `PBZEditor.dll`。PBZEditor 0.1.0
目前只有加载日志和 UE4SS GUI 验证页签，尚未实现血量、体力、背包等玩法功能。

**当前状态（2026-11-05）**　本机游戏补丁 `CL-302256_JKS-17280`（Steam build `26367565`）
已用原安装的 UE4SS 1195 完成启动、签名、自检和存档验证。退出时产生了异常转储。
本地源码构建、运行目录组装和 31 项隔离部署测试已通过；新构建 DLL 的游戏内验收仍待执行。

## 目录和四类输入

```text
RE-UE4SS/                         官方框架源码，真实 Git 子模块
mods/PBZEditor/src/dllmain.cpp     本工程的 C++ mod
game-configs/PhantomBladeZero/     三文件游戏适配、来源和 SHA-256 清单
config/UE4SS-settings.ini          通用运行默认设置
scripts/                         初始化、构建、初装和日常部署
cmake/PBZTooling.cmake             本工程格式检查
tests/scripts/deploy_tests.ps1     隔离文件部署测试
CMakeLists.txt / CMakePresets.json 根构建入口
build/msvc/                      默认独立构建缓存、DLL 和 build-info.json
build/runtime/                   本地组装目录，不是游戏安装目录
build/deploy-backups/             显式安装/部署的备份及恢复凭据
```

| 输入 | 来源和职责 |
| --- | --- |
| 框架源码 | [官方 RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) 的真实 gitlink；父仓库记录的提交决定源码版本 |
| PBZ 适配 | `MemberVariableLayout.ini` 提供引擎结构布局；`UE4SS_Signatures/GUObjectArray.lua` 和 `GNatives.lua` 定位全局对象数组与原生函数表；不提供玩法字段地址 |
| 通用运行资产 | 固定提交的 `RE-UE4SS/assets` 提供 Keybinds；项目 `config` 保存默认设置；运行目录另带上游许可证 |
| 自己的 mod | `mods/PBZEditor` 的菜单与入口，从同一构建链接 UE4SS；不包含 Palworld 业务代码 |

框架固定提交为 `1ceefac8b8c286f0fb3eb62b4aae075ff81df0bd`；其递归依赖
Unreal 为 `56bd81252f9081e98928fb1eec5b5a07a820d70a`，PatternSleuth 为
`1d90b02c7610b595f940af04ce97e0f6feb06923`。不独立执行 `--remote` 更新。
三份适配来自 KintaroModding 的 1195 包，来源、文件哈希和许可证见
[适配说明](game-configs/PhantomBladeZero/README.md) 与
[manifest](game-configs/PhantomBladeZero/manifest.json)。三文件已入库，正常构建不需要旧 ZIP。

发布者适配包记录的是 Steam App `4115450`、build `26266538`（`CL-301524_JKS-17219`）。
当前项目清单记录本机实测的 build `26367565`（`CL-302256_JKS-17280`）及实测框架提交。
游戏 FileVersion、适配文件哈希或框架 gitlink 不匹配时，脚本拒绝部署。

`experimental` 是 UE4SS 发布渠道；`Debug`、`Dev`、`Shipping` 是构建配置，`zDEV`
是发行内容分类。本工程固定 `Game__Shipping__Win64`，该配置仍包含 GUI 和导出工具能力，
不需要切换 Debug 才能显示 PBZEditor。框架运行时、代理和菜单均由源码构建，不使用预编译 ZIP
替代构建验证；新产物的实际 GUI 行为仍须游戏验收。

## 初始化与源码构建

需要 Windows x64、Visual Studio 的“使用 C++ 的桌面开发”工作负载（MSVC x64、Windows SDK、
MASM）、支持 C++23 的工具链、CMake >= 3.26、Ninja、PowerShell 7、Git for Windows
及其 Bash、Rust stable 的 `cargo`/`rustc`。关闭 `UE4SS_VERSION_CHECK` 不会消除 Rust 依赖。
构建脚本通过 VS Installer 查找可用安装并激活 x64 环境，不要求固定编译器绝对路径。
实际工具版本写入 `build-info.json`；本机已验证 MSVC 19.51、SDK 10.0.26100.0、
CMake 4.4.5、Ninja 1.13.2 和 Rust/Cargo 1.97.1。

取得本仓库后，以下命令可从任意工作目录运行；将 `$Project` 改为自己的仓库根路径。
首次初始化需要网络下载固定递归依赖；脚本不创建远端，也不推送或发布。

```powershell
$Project = 'D:/code/cpp/pbz-ue4ss-mod'
pwsh -NoProfile -File "$Project/scripts/setup.ps1"
pwsh -NoProfile -File "$Project/scripts/setup.ps1" -VerifyOnly
pwsh -NoProfile -File "$Project/scripts/build.ps1"
```

`setup.ps1` 初始化父仓库已记录的递归子模块；`-VerifyOnly` 只检查。
脏依赖或外部对象库依赖会被拒绝并保留，不自动 reset；SSH 到 HTTPS 的转换仅作用于本次 Git 调用。
完整构建显式执行 `format-check`、`UE4SS`、`proxy`、`PBZEditor`，随后生成成功凭据并组装运行目录。
**构建、组装和测试不会安装到游戏目录或启动游戏。**

```powershell
# 可选检查；Jobs 范围为 1–64，默认 4。
pwsh -NoProfile -File "$Project/scripts/build.ps1" -PreflightOnly
pwsh -NoProfile -File "$Project/scripts/build.ps1" -ConfigureOnly
# 工具链或源码基线改变时使用新目录，保留旧缓存和产物。
pwsh -NoProfile -File "$Project/scripts/build.ps1" -BuildDirectory 'build/msvc-next' -Jobs 4
```

`-BuildDirectory` 的相对路径以仓库根为基准，必须是本仓库 `build/` 的子目录，默认
`build/msvc`。外来缓存、不同编译器和重解析路径会被拒绝。每次调用（包括预检、仅配置和失败）
都会使该目录的旧 `build-info.json` 失效；安装前须重新完成完整构建。
三个 DLL 位于 `<BuildDirectory>/Game__Shipping__Win64/bin/`；运行目录始终为根下
`build/runtime`。直接调用 CMake 需要自行激活 MSVC x64 环境，且不能替代脚本生成部署凭据。

在 VS Code 中打开仓库根，`cmake.sourceDirectory` 使用 `${workspaceFolder}`，选择根 preset
`ninja-msvc-x64`；不要使用已移除的嵌套 `PBZEditor` 源目录。

## 运行目录和显式安装

每次完整构建组装恰好 11 个文件：

```text
build/runtime/
  dwmapi.dll
  ue4ss/
    UE4SS.dll
    UE4SS-settings.ini
    MemberVariableLayout.ini
    LICENSE-UE4SS.txt
    UE4SS_Signatures/GUObjectArray.lua
    UE4SS_Signatures/GNatives.lua
    Mods/mods.txt
    Mods/Keybinds/Scripts/main.lua
    Mods/PBZEditor/enabled.txt
    Mods/PBZEditor/dlls/main.dll
```

新组装目录只启用官方 Keybinds 和 PBZEditor：`mods.txt` 只有 `Keybinds : 1`，PBZEditor
由空 `enabled.txt` 启用。没有 PBZProbe、旧 `mods.json` 或其他 Lua mod。当前 SDK 后端内建于
固定框架；外部 backend JSON、dump、PDB、LIB 和生成头文件都不是游戏运行必需文件。
默认 GUI 开启但初始隐藏、外部控制台和热重载关闭、强制加载资产关闭；GUI 切换键为 Ctrl+O。
Keybinds 包含手动对象/头文件导出快捷键，不自动导出，也不需要为验证环境重复生成大 dump。

以下命令仅适用于完成兼容验证且版本守卫接受的游戏。**先完全退出 PBZ**。
`-InstallDirectory` 是包含 `platform/steam/PBZ.exe` 的游戏根，不是 `platform/steam` 子目录。

```powershell
$Project = 'D:/code/cpp/pbz-ue4ss-mod'
# 初次完整安装，或显式替换旧适配；先备份白名单内已有文件，再复制全部 11 文件。
pwsh -NoProfile -File "$Project/scripts/install-runtime.ps1" -InstallDirectory 'D:/SteamLibrary/steamapps/common/PBZ'
# 日常更新：已有 PBZEditor、enable marker 和匹配适配时，仅替换配套两个 DLL。
pwsh -NoProfile -File "$Project/scripts/deploy.ps1" -InstallDirectory 'D:/SteamLibrary/steamapps/common/PBZ'
# 若使用了自定义构建目录，安装/部署也须传入同一目录。
pwsh -NoProfile -File "$Project/scripts/deploy.ps1" -InstallDirectory 'D:/SteamLibrary/steamapps/common/PBZ' -BuildDirectory 'build/msvc-next'
```

初装的目标是 `<InstallDirectory>/platform/steam/`，包括代理、运行时、设置、适配、Keybinds
和 mod；已存在的白名单文件会被替换并备份，未知文件及其他 mod 保留。因此“仅启用两者”描述的是
新组装目录，不是对既有游戏目录的清理承诺。日常部署必须已有 `ue4ss/UE4SS.dll`、
`ue4ss/Mods/PBZEditor/dlls/main.dll`、`enabled.txt` 和哈希匹配的三文件适配，只替换前两个 DLL，
不覆盖代理、设置、适配、启用标记或其他 mod。`install-runtime.ps1` 同样接受 `-BuildDirectory`。

脚本在写入前检查游戏 FileVersion、进程、路径、现有目标文件独占锁、构建凭据和源/产物哈希。
游戏目标和仓库 `build/deploy-backups` 必须同卷，否则预检拒绝。所有旧文件先完成备份和校验，
然后再次检查游戏进程及两个 DLL 的锁。任一检查失败即保留备份并退出，两个 DLL 都保持原样。每次成功操作返回新的恢复凭据，允许连续部署，
不要求先恢复上一版，也没有长期 pending 状态标记。

## 恢复与失败边界

备份保存在 `build/deploy-backups/<id>/`。保留整个备份目录，使用命令输出的实际凭据路径：

```powershell
pwsh -NoProfile -File "$Project/scripts/deploy.ps1" -Restore "$Project/build/deploy-backups/<id>/receipt.json"
# 初装脚本也支持同一恢复入口。
pwsh -NoProfile -File "$Project/scripts/install-runtime.ps1" -Restore "$Project/build/deploy-backups/<id>/receipt.json"
```

`-Restore` 是独立参数组，不同时传 `-InstallDirectory` 或 `-BuildDirectory`；目标从凭据读取。
恢复也要求游戏退出、版本匹配和文件可写。恢复返回原文件字节，删除本次安装前不存在的白名单文件，
只移除本次创建且已空的目录，不清理未知内容。写入前逐项核对原文件存在标记、原始 SHA-256 和编号备份。日常部署必须具备两个原文件的有效备份；初装时原文件缺失的条目必须没有对应编号备份。

替换失败会逐项恢复已开始复制的目标（包括部分写入的文件），并重读校验原始哈希或确认文件已移除；某项恢复失败仍继续其余项，命令非零退出、报告
`Recovery incomplete`、保留备份，并在凭据中记录 `restore-failed`、错误及实际哈希（无法读取则
为 `unknown`，不存在则为 `absent`）。修复占用等原因后可用该凭据重试。此机制不保证跨文件原子性
或断电恢复；不要把失败退出解释为已恢复成功，也不要在状态不明时启动游戏。

## 验证和后续工作

完成默认 `build/msvc` 构建后，可以从任意目录运行真实脚本的 31 项隔离场景。
测试参数 `-EvidenceDirectory` 必填，目录需要预先存在；建议使用本工程 `build/` 下的新目录：

```powershell
$Project = 'D:/code/cpp/pbz-ue4ss-mod'
$Evidence = Join-Path $Project ('build/script-test-evidence-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $Evidence -ErrorAction Stop | Out-Null
pwsh -NoProfile -File "$Project/tests/scripts/deploy_tests.ps1" -EvidenceDirectory $Evidence
```

测试使用仓库内短路径 `.qa-*` 假游戏（含空格）、临时独占锁和测试作用域故障注入，覆盖首次安装、
重复双 DLL 更新、拒绝条件、失败回滚与显式恢复。它会短暂改动受测源/产物用于哈希拒绝检查并恢复
原字节，请不要同时构建或编辑。清理按本次操作登记的编号删除备份，只移除空目录，并检查源码及模拟游戏外的哨兵文件
不变。通过证据为退出码 0、31 个具名 PASS、`task-5-deploy-tests.json` 和 `task-5-cleanup.json`。

在本机新版游戏上，原安装的运行时加载了布局文件，解析了 GUObjectArray 和 GNatives，
通过了含 9,822 个类的对象哈希表自检。GUI 显示一个 PBZEditor 页签；进入存档后，
F8 探针成功在游戏线程执行。此次导出生成了 3,967 个 C++ 头文件，六项缺少有效枚举值的 AI 属性被跳过。

Alt+F4 后日志记录了事件循环结束。随后生成的转储记录一次读取访问异常，指令位于小黑盒的
`UE4SSLocationLite.dll+0x2ea809`。根因及正常退出仍待验证。新构建 DLL 的两次冷启动、
Ctrl+O 切换及存档验收尚待执行，此次试验也未测量帧时间或验证玩法接口。

旧根下 `.legacy` 和 `.omo` 已由用户清理，不依赖或重建旧缓存。原始 devkit 仍保留在
`D:/code/cpp/palworld-ue4ss-mod/build/pbz-devkit`，历史 dump 位于其 `dump/20261101`；
这些是本机可选历史参考，正常克隆、构建、组装和测试都不依赖该路径。
本次迁移证据另存在该 devkit 的 `.omo/evidence/pbz-palworld-layout`，不作为运行时输入。

后续玩法按纯值规则、游戏线程反射适配、UI 输入展示分层；先检索已有能力，再引入首个实际消费者
需要的模块。完整反射签名验证、短期句柄、写后重读/回滚和成对生命周期清理规则见 [AGENTS.md](AGENTS.md)。
