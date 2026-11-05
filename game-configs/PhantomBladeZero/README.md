# Phantom Blade Zero adaptation

This profile contains three unchanged files from KintaroModding's
[UE4SS v3.0.1-1195-g1ceefac8 release](https://github.com/KintaroModding/ue4ss/releases/tag/v3.0.1-1195-g1ceefac8),
package `phantombladezero_ue4ss_v3.0.1-1195-g1ceefac8.zip`.
The package and raw file SHA-256 values are recorded in `manifest.json`.
Normal builds consume the checked-in profile; they do not need the ZIP or a devkit directory.

| File | Responsibility |
| --- | --- |
| `MemberVariableLayout.ini` | PBZ's Unreal engine structure and field layout overrides |
| `UE4SS_Signatures/GUObjectArray.lua` | Signature scan for the global UObject array |
| `UE4SS_Signatures/GNatives.lua` | Signature scan for the native function table |

The file comments retain the original authors' derivation and checks. The layout
was derived from UE4SS's UE 5.7 template. The signature implementations are
attributed to the UE4SS team's MIT-licensed custom game configs: Metal Gear Solid
Delta/Split Fiction for GUObjectArray, and Far Far West for GNatives.
These files describe engine integration, not health, stamina, inventory or other gameplay APIs.

## Compatibility

The publisher's three files identify Steam App 4115450, build 26266538,
`PBZ.exe` FileVersion `CL-301524_JKS-17219`. The local profile now targets
build 26367565, FileVersion `CL-302256_JKS-17280`. The parent gitlink and
`testedUE4SSCommit` both select `1ceefac8b8c286f0fb3eb62b4aae075ff81df0bd`.
Deployment checks the game FileVersion, profile hashes and framework gitlink.

On 2026-11-05 the previously installed UE4SS 1195 runtime loaded the layout
and resolved both signatures on build 26367565. Its hash-table self-test
passed for 9,822 classes with instances. One PBZEditor tab appeared, a save
loaded, and the existing F8 probe logged a successful game-thread dispatch.
The C++ header export produced 3,967 files in 2.85 seconds. It omitted six
AI enum properties with invalid enum values. Gameplay APIs and frame times
were outside this trial.

After Alt+F4 the log recorded the event loop ending. A subsequent minidump
reported a read access violation at `UE4SSLocationLite.dll+0x2ea809` in the
Heybox Desktop installation.

The daily deployment then replaced only UE4SS.dll and PBZEditor's main.dll.
All nine protected file hashes matched before and after deployment. The existing
installation retained Keybinds and PBZEnvironmentProbe during these runs.
The clean staging package contains PBZEditor and Keybinds.

After the two cold starts, the old probe was removed. The original script, enable marker and
`mods.txt` are backed up under `build/cleanup-backups/old-probe-2a441b5e987c46aeacf7e4263d813552/`.
The two runs below preceded cleanup. A third cold start loaded only Keybinds and PBZEditor, passed the 9,822-class self-test, and showed one PBZEditor tab when Ctrl+O opened the GUI. The process exited after Alt+F4 with no new crash dump.

With Heybox closed, the new pair passed two cold starts. Each run passed the
9,822-class self-test and displayed one PBZEditor tab. The first loaded the
shrine save, dispatched the F8 probe on the game thread, and exited via Alt+F4.
The second opened and closed the GUI with Ctrl+O and exited from the main menu.
Both logs recorded the event loop ending. Both processes terminated and neither
run produced a new crash dump. The module snapshots lacked the previously
observed Heybox hooks. Both the runtime binaries and the Heybox state changed
between trials, so the earlier access violation's root cause remains open.

The 3,967 exported headers (14,554,621 bytes) are archived locally under
`build/dumps/CL-302256_JKS-17280-20261105-084806765/CXXHeaderDump/`, with a
per-file SHA-256 manifest in its parent directory. The matching in-game copy
was removed after verification.

General runtime defaults belong in `config/UE4SS-settings.ini`, separate from
this game profile. They derive from the pinned framework's settings with the
external console and initial GUI visibility disabled; GUI access remains enabled.
Hot reload, Lua automatic reload and force-loading assets for dumps/headers remain disabled.
Other setting values are unchanged; text formatting is UTF-8/LF without trailing whitespace.

## License and attribution

The source package includes the following UE4SS MIT license. Original attribution
comments in all three files are preserved verbatim.

```text
MIT License

Copyright (c) 2022 Narknon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
