# PBZ UE4SS C++ mod 开发工程

本工程面向《影之刃零》（Phantom Blade Zero），使用根目录 CMake super-build，
从固定源码构建 UE4SS、代理 `dwmapi.dll` 和 `PBZEditor.dll`。PBZEditor 0.2.0
支持已有普通背包物品的数量编辑、中文名称及 ID 搜索；已完成本机普通材料增减与恢复验证。

**当前状态（2026-11-06）**　本机游戏补丁 `CL-302256_JKS-17280`（Steam build `26367565`）
已用源码构建的 UE4SS 与 PBZEditor 完成两次冷启动和退出。两次均只出现一个 PBZEditor 页签，
退出后进程结束，均无新增崩溃转储。存档加载、F8 游戏线程探针、Ctrl+O 打开与关闭 GUI，以及 31 项隔离部署测试均已通过。

## 目录和四类输入

```text
RE-UE4SS/                         官方框架源码，真实 Git 子模块
mods/PBZEditor/src/dllmain.cpp     mod 入口和回调生命周期
mods/PBZEditor/inc/items/          数量规则、请求和不可变快照
mods/PBZEditor/src/items/          游戏线程接口和独立 UI
mods/PBZEditor/tests/             不依赖 Unreal 的数量及生命周期测试
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
替代构建验证。下文记录了本机的游戏内验证结果。

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
完整构建显式执行 `format-check`、`UE4SS`、`proxy`、`PBZEditor`、`PBZItemTests` 和 CTest，
全部通过后生成成功凭据并组装运行目录。
配置时用小型头文件探针读取 MSVC `/showIncludes` 原始字节，并将同一前缀交给 Ninja，避免本地化输出解码错误。
已有对象缓存的前缀或头文件依赖不匹配时拒绝构建，保留旧缓存并使用全新构建目录。
完成编译后还须确认 mod 和测试消费文件真实记录了公共头文件依赖，才会签发部署凭据。
这是增量构建的必要检查：纯 C++ 测试通过不能证明不同翻译单元使用了相同的类布局。构建凭据覆盖 mod 的所有源文件、头文件及测试；
增加、删除或修改这些文件后都需要重新构建才能部署。
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

## 物品数量编辑（0.2.0）

进入存档并可操作角色后，按 Ctrl+O，在 PBZEditor 页签点击“读取背包”。选择物品，
输入目标数量，再点击“应用数量”；UI 绘制和空闲帧不扫描游戏对象。读取分帧执行，每帧最多
16 项并设置 2ms 软预算；一次原生调用本身无法抢占，实际帧时间仍需测量。

只允许编辑已持有、数量大于零的普通材料、烹饪道具和宝物；任务、书信、属性增长、核心进度、
自动合成及饰品碎片等条目保持只读。数量编辑保留原有物品，目标数量至少为 1。
1–999999 是编辑器的输入边界，并非已确认的游戏堆叠上限；游戏拒绝或限制数量时不得显示成功。

接口适配以当前进程的元数据为准：验证 Inventories 映射的键值类型、GetItemNumInfo 和原生增减函数的完整签名，
以反射属性 API 和引擎初始化的参数缓冲区调用，不使用 dump 偏移或手写参数结构。
提交时重新确认世界、物品身份与原数量，通过 AddPropsToBackPack/RemovePropsFromBackPack
写最小差量，再读取确认；发现游戏已消耗物品时拒绝旧请求。未通过验证会尝试恢复可确认属于
本次操作的变更；恢复无法确认时保留原值记录，并停用后续写入，刷新或切图不能解除停用。

切图时清空请求与选择。关闭 mod 前钝化 GUI 并注销、排空回调；注销失败或存在待恢复记录时保留
实例。DLL 固定到进程退出以保护上游延迟销毁的闭包，因此更换 DLL 后必须重启游戏。

名称优先读取 FlowLocText 的有效缓存，否则通过当前世界唯一的 EventPipelineManager 调用
Conv_LocTextToString，验证 FlowLocText 输入与 FString 返回值的完整签名。无可用名称时显示内部 ID；
没有硬编码名称表。GetAllItems 在本机运行时没有返回参数，不调用该函数，也不依据 SDK 猜测返回布局。
Inventories 只通过引擎的映射布局与稀疏索引接口读取，不直接修改容器。

本机游戏内验证：先备份 28 个存档文件并逐文件校验哈希；读取真实背包 173 项；
将“污秽残晶”（Ac_Material_1）从 14 改为 13，再恢复为 14，两次均通过原生接口重读，
游戏原生道具界面分别显示 13 和 14。重启后仍为 14；书籍条目保持只读；
返回开始界面后旧列表与选择被清空。中文名称转换已实测，多次正常退出未新增崩溃转储。
存档备份位于本机 build/save-backups/item-quantity-20261106-194411-a11d4771/，
manifest.json 记录原路径及哈希；不自动恢复或删除存档。

11 个纯值测试用例通过，覆盖输入边界、特殊道具拒绝、过期快照、请求交接、切图与恢复记录；
另通过 4 项构建凭据源文件检查及 31 项隔离部署场景。纯值测试不能证明原生失败事务的恢复效果：
尚未在真实存档中主动制造写入失败或恢复失败，也未验证所有物品类型、极大数量、热重载和小黑盒共存。
999999 仅为输入边界；尚无游戏内帧时间对照数据。

## 从物品目录获得道具

进入可操作角色的存档，按 Ctrl+O，打开 PBZEditor → 物品目录 → 刷新目录。
勾选“仅未持有”，按分类、中文名称或 ID 查找条目，再选中具体物品。

- **仅获得**会把所选物品发给当前角色，按类别重读实际持有状态。需要保留进度时使用游戏原生存档。
- **获得 1 个并保存**还会调用游戏保存接口。只有当前载入槽和继续槽均可识别且一致时，此按钮才可用。保存请求被游戏受理后，最终落盘由游戏处理。

支持普通材料、主武器、影之武装、药品、饰品与背包中的整书道具。获得武器、药品和饰品时不请求自动装备。
书籍按道具表中的整书类型分类，合集记录可供浏览。未持有、数据来源唯一且有对应发放接口的条目才会显示获得按钮。
发放前会重新检查目标和持有状态，取得后不能从目录重复发放。已持有的可堆叠材料可在“已有背包”中修改数量。

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
`UE4SSLocationLite.dll+0x2ea809`。

随后通过日常部署脚本，仅替换了源码构建的 UE4SS 和 PBZEditor DLL。九个受保护文件的哈希均保持一致，
包括原设置和既有的 Keybinds、PBZEnvironmentProbe。新组装包只包含 PBZEditor 与 Keybinds。
本机恢复凭据为 `build/deploy-backups/02ac363805854af48cf37be63ce54ab9/receipt.json`。

两次冷启动完成后，已备份并移除旧验证探针。原始脚本、启用标记和清理前的 `mods.txt` 保存在
`build/cleanup-backups/old-probe-2a441b5e987c46aeacf7e4263d813552/`。下表记录清理前的两次运行。清理后的第三次冷启动仅加载 Keybinds 和 PBZEditor，9,822 个类自检通过。Ctrl+O 打开 GUI 后可见一个 PBZEditor 页签；按 Alt+F4 后进程退出，无新增崩溃转储。

| 新 DLL 冷启动 | 游戏内验证 | 退出结果 |
| --- | --- | --- |
| 第一次 | 9,822 个类自检通过；单一 PBZEditor 页签；读入神龛存档，F8 探针在游戏线程执行 | Alt+F4 后记录事件循环结束，进程退出，无新增转储 |
| 第二次 | 同一自检通过；在主菜单用 Ctrl+O 打开 GUI，确认单一页签，再用 Ctrl+O 关闭 | 在主菜单选择退出并按回车，事件循环结束，进程退出，无新增转储 |

这两次运行都关闭了小黑盒，模块快照中均未发现先前转储涉及的小黑盒模块。运行时 DLL 也已更换，
因此这组结果仍无法单独确定先前异常的根因。帧时间和玩法接口未测量或验证。

上述 3,967 个头文件共 14,554,621 字节，保存在本机
`build/dumps/CL-302256_JKS-17280-20261105-084806765/CXXHeaderDump/`。
同目录的 `sdk-files.sha256.json` 记录逐文件哈希。对照归档逐项核验后，已移除游戏目录中的重复副本。

旧根下 `.legacy` 和 `.omo` 已由用户清理，不依赖或重建旧缓存。原始 devkit 仍保留在
`D:/code/cpp/palworld-ue4ss-mod/build/pbz-devkit`，历史 dump 位于其 `dump/20261101`；
这些是本机可选历史参考，正常克隆、构建、组装和测试都不依赖该路径。
本次迁移证据另存在该 devkit 的 `.omo/evidence/pbz-palworld-layout`，不作为运行时输入。

后续玩法按纯值规则、游戏线程反射适配、UI 输入展示分层；先检索已有能力，再引入首个实际消费者
需要的模块。完整反射签名验证、短期句柄、写后重读/回滚和成对生命周期清理规则见 [AGENTS.md](AGENTS.md)。
