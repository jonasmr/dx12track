# dx12track — handoff notes

Snapshot taken 2026-06-01 while moving the working environment between
machines. This document records everything that lives *outside* the git repo
or that's easy to miss when picking the project back up cold. For "what is
this project and how do I build it" see [README.md](README.md). For project
intent see [CLAUDE.md](CLAUDE.md).

## Repo state

| | |
| --- | --- |
| Branch | `main` |
| HEAD | `2d83b85` ("Emit Hello when Dx12Track_StartLogging opens the log") |
| `origin/main` | in sync with HEAD |
| Remote | `git@github.com:jonasmr/dx12track.git` |
| Latest tag | `v0.1.0` (pushed; points at `28ec6d4`) |
| Working tree | clean |

Commit timeline (newest first):

```
2d83b85  Emit Hello when Dx12Track_StartLogging opens the log
abbeb8a  Add README
99d851f  Add Dx12Track_StartLogging / StopLogging C API for self-injection
28ec6d4  Add --callstacks flag: capture create-site stacks + module metadata  ← v0.1.0
2cf8cf7  Hook SetPrivateData for WKPDID_D3DDebugObjectName{,W} naming path
1b2561e  Initial dx12track scaffold: console launcher + injected DLL
```

## Build environment expected on the new machine

- **VS 2022** (any 17.x will do; project uses PlatformToolset `v143` and
  `WindowsTargetPlatformVersion = 10.0`). Desktop C++ workload must be
  installed. Verified compiler: MSVC 14.44.35207.
- **vcpkg** available either on `PATH` or via `VCPKG_ROOT`. The
  VS-bundled vcpkg at
  `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg\`
  works. A user-installed vcpkg (this machine had it at `D:\vcpkg`) also
  works — it doesn't matter which, because `vcpkg-configuration.json`
  pins a `default-registry` of `kind: git` at commit
  `00d899c410b31467733472fc3a83a25729046b13`. The vcpkg tool just needs to
  be able to fetch from GitHub.
- **Git** in `PATH` (vcpkg shells out to it for the registry checkout).
- **PowerShell** (Windows PowerShell 5.1 was used; pwsh.exe works too).

First build on the new machine:

```powershell
git clone git@github.com:jonasmr/dx12track.git D:\git\dx12-track
cd D:\git\dx12-track
MSBuild.exe dx12track.sln /p:Configuration=Debug   /p:Platform=x64
MSBuild.exe dx12track.sln /p:Configuration=Release /p:Platform=x64
```

vcpkg will fetch `directx-headers:x64-windows@1.619.1` into
`vcpkg_installed/` on the first build (~1 minute). The path
`vcpkg_installed/x64-windows/x64-windows/include/directx/d3d12.h` is the
header used.

Outputs land in `build/x64/{Debug,Release}/`:

```
dx12track.exe   the launcher
dx12track.dll   the injected payload
dx12track.lib   import library (clients can link instead of GetProcAddress)
dx12track.pdb   PDB for the DLL
```

## Things NOT in the repo that you'll want on the new machine

| Source on old machine | Why | How |
| --- | --- | --- |
| `D:\git\dx12-track\DirectX-Graphics-Samples\` (6.8 GB) | ModelViewer is the primary smoke-test target | `git clone https://github.com/microsoft/DirectX-Graphics-Samples` inside the repo. The relevant binary is `MiniEngine\Build\x64\Debug\Output\ModelViewer\ModelViewer.exe`; build it from `MiniEngine\ModelViewer\ModelViewer.sln` once. |
| `D:\git\dx12-track\EvictionHelper\` | Second test target — see "EvictionHelper" section below | Re-clone its own git repo from wherever you keep it; uncommitted changes documented below |
| `~\.claude\projects\D--git-dx12-track\` | This conversation + memory | Already copied per the move plan. Run `claude --continue` from `D:\git\dx12-track\` to resume. If the project path differs on the new machine, rename the folder so it encodes the new cwd (`\` → `-`, `:` → `-`). |
| `~\.claude\plans\floofy-dreaming-raven.md` | Plan file written at the start of this session | Copy alongside the projects folder if you want to be able to reference the original architecture decisions. |
| `~\.claude\settings.local.json` (also `.claude\settings.local.json` inside the repo, gitignored) | Per-machine permission allowlist for Claude Code | Either copy across or let Claude re-prompt as it goes. |

## EvictionHelper sample (uncommitted work)

`EvictionHelper/` is a separate git repo (`5a85eb4 LICENSE` is its HEAD;
upstream is the user's own), gitignored from dx12-track. It was modified
during the last session to exercise the public `Dx12Track_StartLogging`
API via `LoadLibrary`, but **the modifications are NOT committed**.
Re-applying them on the new machine if you don't move them:

`EvictionHelper/src/eviction_helper.cpp` — two diffs:

1. After the `g_CurrentUnusedVRAMPriority` global, add:
   - `g_EnableTracking`, `g_TrackCallstacks`, `g_TrackPath` globals
   - `PFN_Dx12Track_StartLogging` typedef
   - `TryLoadDx12Track()` function that `LoadLibraryW`s `dx12track.dll`,
     `GetProcAddress`s `Dx12Track_StartLogging`, calls it
2. Inside `WinMain` after the existing `-gpu` arg block, add `-track` /
   `-nostacks` parsing and call `TryLoadDx12Track()` before any D3D12
   call.

If you brought the modified file across, just `git diff` will show the
changes — decide whether to commit them in the EvictionHelper repo or
keep them as a sandbox edit.

To use:

```powershell
# Copy the Release DLL next to EvictionHelper.exe (or put it on PATH)
copy build\x64\Release\dx12track.dll EvictionHelper\bin\Release\
cd EvictionHelper\bin\Release
.\EvictionHelper.exe -track evict.jsonl -noshared
```

Expected JSONL: `hello`, ~59 `module_loaded`, ~27 `created` (with stacks),
some `renamed`, some `module_unloaded`.

## Things that bit during the session — heads up

### 1. Windows Defender deletes the Release exe

`Behavior:Win32/DefenseEvasion.A!ml` fires on the
`CreateProcess(CREATE_SUSPENDED) → CreateRemoteThread(LoadLibraryW)` pattern
in the launcher. We saw it on this machine; expect it on the new one too.
Defender deletes `build\x64\Release\dx12track.exe` shortly after the launcher
runs. The Debug build was unaffected during the session.

Diagnose with:
```powershell
Get-MpThreatDetection | Sort InitialDetectionTime -Desc | Select -First 5
Get-WinEvent -LogName 'Microsoft-Windows-Windows Defender/Operational' -MaxEvents 50 |
  Where-Object Id -in 1015,1116,1117
```

Fix (requires admin):
```powershell
Add-MpPreference -ExclusionPath 'D:\git\dx12-track\build'
```

We **did not** add the exclusion during this session — the user opted to
work in Debug. Same call is right on the new machine.

### 2. vcpkg builtin-baseline vs default-registry

`vcpkg.json` does *not* set `builtin-baseline`. The registry is pinned via
`vcpkg-configuration.json`'s `default-registry`. If MSBuild ever complains
"manifest requires a builtin-baseline", that means it's reading the local
vcpkg working tree instead of the configuration — usually because
`vcpkg-configuration.json` was missing or renamed. Don't add a
`builtin-baseline` field to fix it; restore `vcpkg-configuration.json`.

### 3. CRT mismatch from `<d3dcommon.h>`

If you ever see `LNK4098 'MSVCRTD' conflicts with use of other libs` after
adding an include, the cause is almost certainly that you pulled in the
Windows SDK's `<d3dcommon.h>` directly. Its auto-link pragmas drag in
`MSVCRTD` which conflicts with our static CRT. Get those types via
`<directx/d3d12.h>` instead (which transitively pulls in
`directx/d3dcommon.h`). The DLL's vcxproj has
`IgnoreSpecificDefaultLibraries=MSVCRTD;MSVCRT` as a belt-and-braces
fallback for the same reason.

### 4. Stale dx12track.dll vs newer EvictionHelper

EvictionHelper does `LoadLibraryW("dx12track.dll")` by name. Forgetting to
re-copy Release DLL after rebuilding is a silent-failure footgun: the
client gets an old DLL whose exports don't exist or whose behavior is
stale. `dumpbin /exports build\x64\Release\dx12track.dll` is the sanity
check.

### 5. Hello timing in self-inject mode

Already fixed in `2d83b85`, but worth knowing: in self-inject mode the
JSONL log opens *later* than the hooks install, so `SendHello` from
DllMain hits a closed file. `Dx12Track_StartLogging` now re-emits Hello
after opening the log so the JSONL always leads with a `hello`.

## File map of where to look

```
src/
  common/EventTypes.h          Wire format (protocol v2). Single source of truth
                               shared between launcher (consumer) and DLL (producer).
                               Includes #pragma pack(push,1) payloads for every
                               EventKind plus enum/name helpers.
  launcher/
    Main.cpp                   argv parse, CreateProcess(CREATE_SUSPENDED), inject,
                               resume, pipe reader, render loop, final summary.
    Injector.cpp/.h            CreateRemoteThread+LoadLibraryW
    PipeServer.cpp/.h          Server side of the per-PID named pipe
    Model.cpp/.h               Live object table + memory totals + recent tail
    Renderer.cpp/.h            WriteConsoleOutputW back-buffer renderer
  dll/
    DllMain.cpp                Bootstraps env-var-driven config (launcher path)
    Hooks.cpp/.h               MinHook on D3D12CreateDevice; vtable probe up to
                               ID3D12Device15 (#ifdef-guarded); installs the per-slot
                               Create* hooks. Slot indices documented inline.
    DeviceHooks.cpp/.h         One trampoline per Create* variant. Track() is the
                               central helper that captures stacks when enabled.
    ObjectHooks.cpp/.h         Release (slot 2), SetName (6), SetPrivateData (4).
                               Name extraction handles both WKPDID GUIDs.
    Modules.cpp/.h             EnumProcessModules + LdrRegisterDllNotification +
                               PE debug-dir walk for PDB GUID/Age.
    Tracker.cpp/.h             Thread-safe object table; idempotent OnSetName
    PipeClient.cpp/.h          Pipe write (mutex-serialized)
    JsonLog.cpp/.h             JSONL append (mutex-serialized). All field
                               serialization lives here.
    PublicApi.cpp              extern "C" Dx12Track_Start/StopLogging exports

include/
  dx12track.h                  Public C header for in-process clients.
                               DX12TRACK_BUILDING_DLL toggles dllexport/dllimport.

src/third_party/minhook/       Vendored (BSD). Inner .git was removed so it's
                               flat-vendored, not a submodule.

vcpkg.json                     Manifest. Lists directx-headers as the only
                               dependency. No builtin-baseline.
vcpkg-configuration.json       Pins the default-registry to a specific
                               microsoft/vcpkg commit for build reproducibility.
```

## Known follow-ups / open items

- **Defender exclusion not added** — see "Things that bit" #1.
- **Reserved-resource physical size**: we log virtual size only. Hooking
  `UpdateTileMappings` would let us track physical tile mappings, but the
  call is hot and we haven't done it.
- **Attach-to-PID** mode for the launcher: in the plan as out-of-scope for
  v0.1.0; the in-process API obviates it for the common case where you
  control the host.
- **EvictionHelper modifications are uncommitted** — see "EvictionHelper"
  section.
- **Symbolicator**: no offline tool yet. The JSONL format with module
  events + per-frame addresses is enough to write one; recipe is in the
  README's "Callstacks" section.

## Verification once you're set up on the new machine

Quick "everything still works" run:

```powershell
# 1. Build
MSBuild.exe dx12track.sln /p:Configuration=Debug /p:Platform=x64

# 2. Launcher mode against ModelViewer (5 sec sample, force-killed)
$exe = '.\build\x64\Debug\dx12track.exe'
$target = '.\DirectX-Graphics-Samples\MiniEngine\Build\x64\Debug\Output\ModelViewer\ModelViewer.exe'
$p = Start-Process $exe -ArgumentList @('-o','smoke.jsonl','--callstacks',$target) -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 5
Get-Process ModelViewer -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

# 3. Expected event mix (numbers will vary slightly):
#   ~430 created, ~410 renamed, ~20 destroyed, ~80 module_loaded, 1 hello
Get-Content smoke.jsonl |
  ForEach-Object { if ($_ -match '"event":"(\w+)"') { $matches[1] } } |
  Group-Object | Select Count,Name | Sort Count -Desc
```

If those numbers come back roughly right, the whole stack is healthy.
