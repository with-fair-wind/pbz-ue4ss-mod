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

The recorded baseline is Steam App 4115450, build 26266538,
`PBZ.exe` FileVersion `CL-301524_JKS-17219`, with official UE4SS commit
`1ceefac8b8c286f0fb3eb62b4aae075ff81df0bd`.
This is an inherited adaptation baseline, not a new runtime test of this project.
The parent repository's `RE-UE4SS` gitlink selects framework source;
`testedUE4SSCommit` records this profile's compatibility baseline.
Deployment must refuse unknown game versions, mismatched profile hashes, or a
framework gitlink that differs from this baseline until explicitly revalidated.
Neither a dump nor the historical file comments establish compatibility with future updates.

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
