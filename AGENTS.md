# PBZ development instructions

This independent Phantom Blade Zero UE4SS C++ mod repository uses a root
super-build: RE-UE4SS/, mods/PBZEditor/, scripts/, config/ and game-configs/.
The approved pbz-palworld-layout plan supersedes the old nested PBZEditor layout
and version-lock.json. The user removed the old .legacy and .omo directories; do not
recreate those caches. The original devkit remains an optional historical reference.

## Ownership and source boundaries

- Start work on feature/fix/chore branches from develop. Preserve existing work;
  do not invent Git identity or remotes. Push, PR and publication need explicit
  authorization. The layout migration permits local atomic commits only.
- RE-UE4SS is a real Git submodule. Its parent gitlink is the source authority;
  pin official upstream commit 1ceefac8b8c286f0fb3eb62b4aae075ff81df0bd.
  Initialize recursive dependencies at their recorded commits, never --remote.
  Do not ignore the gitlink or edit upstream sources to bypass build failures.
- Keep PBZ layout and the GUObjectArray/GNatives signatures in
  game-configs/PhantomBladeZero with source, hashes and compatibility metadata.
  The tested commit must agree with the gitlink before deployment. Unknown
  game versions fail closed; a dump does not establish future compatibility.
- Framework sources, game adaptation, general runtime assets and our mod are
  separate inputs. No Palworld business code, PalSchema or /Script/Pal paths.
- Preserve old code, build trees, fixtures, downloads and evidence. The Palworld
  checkout and original devkit are read-only references. Do not share old CMake
  caches, PCH or Cargo outputs with the new root build.
- Use MSVC x64, CMake >= 3.26, Ninja, PowerShell 7 and Rust/Cargo. The primary
  build configuration is Game__Shipping__Win64. Build UE4SS, proxy and PBZEditor
  from the same pinned source; downloaded DLLs are not build verification.
- Installation is explicit. Ordinary deployment updates the paired runtime and
  mod DLLs only after process/lock checks; preserve settings, profiles and other
  mods. Do not write the game during build or fixture tests.

## Minimal structure and runtime safety

- Search existing code with rg before adding helpers. Keep the small menu in
  mods/PBZEditor/src/dllmain.cpp until consumers justify splitting it. Preserve
  ImGui context/allocator setup and GUITabs duplicate guard.
- No empty feature modules, unused helpers, generic event buses, transaction
  frameworks or copies of another game's architecture without consumers.
- Future pure-value rules, requests and results belong in inc/<feature> and use
  only the standard library. UObject access belongs in game-thread gateways;
  UI collects input and displays synchronized standard-library snapshots.
- One owner holds each feature's state. Across frames store only values and
  identities, never UObject/UFunction/FProperty pointers or container addresses.
- Dumps identify names, not current layouts. Validate full runtime signatures,
  parameter counts, input/output/return directions and exact property types
  before every reflected call. Missing or ambiguous metadata refuses the call.
- Do not imitate parameter layouts with fixed structs, offsets or casts. Use
  narrowly scoped RAII parameter initialization/destruction and property APIs
  with the first consuming gateway. Const-reference flags are not writable output.
- Resolve short-lived handles on the game thread and revalidate after
  ProcessEvent. No reflection from UI, idle registry scans or unbounded retries.
- Catch exceptions at engine boundaries, clear failed outputs and report typed
  results. Writes require preflight and original values, minimal changes, native
  notifications and full readback. Roll back only owned changes and verify it;
  failed recovery retains responsibility and safely disables the affected domain.
- Pair callback registration with unregister/drain and unload safety. Pair every
  reversible override with restoration and bounded retries in the same owner.
  Define disable, LoadMap and unload behavior before adding a runtime feature.
- Never treat known incomplete AI enum fields as callable APIs or claim native
  debug menus/gameplay functionality without fresh runtime evidence.

## Verification and preservation

- Follow checked-in formatting, UTF-8, LF and four-space C++ indentation.
- Exercise actual script failure paths in short owned fixtures outside evidence
  directories. Reject reparse points and verify resolved path bounds first.
- Cleanup removes only individually allowlisted files and nonrecursive empty
  directories. Never recursively delete fixture parents or old archives.
- Record fresh commands, outcomes, hashes and artifacts under
  .omo/evidence/pbz-palworld-layout for new work. This migration uses the original
  devkit's .omo/evidence/pbz-palworld-layout after the user removed the target's
  old evidence; only that evidence/staging subtree may be written in the devkit.
  Old logs are historical, not proof of a new build. Distinguish local validation
  from game-thread/UI verification.
- Do not perform game installation or launch as an implicit build step. Keep
  unexecuted runtime scenarios explicitly unverified.
