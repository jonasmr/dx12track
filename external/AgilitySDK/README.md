# Agility SDK headers (vendored)

`include/` is an unmodified copy of `build/native/include/` from the NuGet
package [Microsoft.Direct3D.D3D12](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12)
**1.619.5** (the DirectX 12 Agility SDK): `d3d12.h`, `D3D12Events.h`,
`d3dcommon.h`, `dxgiformat.h`, the `.idl` sources, `d3dx12/` and friends.
Per the package README, these files are under the MIT license in
[LICENSE-CODE.txt](LICENSE-CODE.txt), copied from the same package. The
package's runtime binaries (`build/native/bin/`, under its separate
`LICENSE.txt`) are not included and not needed.

## Why it is vendored

`src/etw/dx12track_etw.vcxproj` compiles
[DxTimingCaptureLibrary](../DxTimingCaptureLibrary), which includes these
headers unprefixed (`<d3d12.h>`, `<D3D12Events.h>`). The library's own
standalone build gets them through a NuGet `PackageReference`, but Visual
Studio does not restore `PackageReference` for native C++ projects, so a fresh
checkout failed to build from the IDE. dx12track sets
`DxTimingCaptureStandaloneBuild=false` (dropping that `PackageReference`) and
puts `external/AgilitySDK/include` first on the project's include path
instead, ahead of the Windows SDK's older `d3d12.h`. No NuGet restore or
network access is needed.

Only the `dx12track_etw` project uses this directory; the launcher and the DLL
get their headers from vcpkg's DirectX-Headers.

## Updating

1. Download a newer `Microsoft.Direct3D.D3D12` package (or let NuGet restore
   it into `%USERPROFILE%\.nuget\packages\microsoft.direct3d.d3d12\<version>\`).
2. Replace `include/` with the package's `build/native/include/` (delete the
   old files first so removed headers don't linger) and refresh
   `LICENSE-CODE.txt`.
3. Bump the version above. If the change is for the library's sake, keep it in
   step with the `PackageReference` version in
   `external/DxTimingCaptureLibrary/Directory.Build.props`.
4. Rebuild `dx12track_etw` (Debug and Release).
