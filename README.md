# WeaponFX for Nixware

Custom HLSL pixel shaders on your own weapon and gloves in CS2, loaded as a
[Nixware](https://nixware.cc) Lua script. Everything is drawn **only on your
screen**: other players and demos see the normal skin. Meant for private
servers and scripter modes where everyone agreed to play with scripts.

Shaders included: `blackhole`, `chrome`, `cybergrid`, `galaxy`, `hologram`,
`lava`, `lightning`, `liquid_glass`, `liquid_gold`, `plasma`, `prism_crystal`.
Your own `.hlsl` files dropped into the `shaders` folder are picked up on
script load.

## Credits

The renderer, hooks and original shaders are the work of
**[Garagoro](https://github.com/Garagoro)** —
[Garagoro/WeaponFX](https://github.com/Garagoro/WeaponFX) (MIT).
This repository is a fork-style adaptation of that project for the Nixware
Lua loader: the in-game ImGui menu is replaced by a Nixware menu tab, the
ImGui/log code is compiled out of the public build, and some shaders are new
or reworked. The original license and copyright notice are kept in
[`weaponfx/LICENSE`](weaponfx/LICENSE). Please support the original author.

## Install

1. Download `WeaponFX-Nixware.zip` from
   [Releases](../../releases/latest) and extract it into `C:\nix\scripts\`:

   ```
   C:\nix\scripts\weaponfx_loader.lua
   C:\nix\scripts\WeaponFX\WeaponFX.dll
   C:\nix\scripts\WeaponFX\shaders\*.hlsl
   ```

2. Start CS2 offline (`-insecure`, a local or private server).
3. In Nixware open **Scripts → Library** and load **WeaponFX**.
4. A **`</> WeaponFX`** tab appears in the menu. Pick a mode and a shader
   for the weapon, gloves, hands or sleeves.

Settings are saved to `C:\nix\scripts\WeaponFX\weaponfx.ini`.

## Verify the download

Every release is built by GitHub Actions straight from the tagged commit
(`.github/workflows/release.yml`), with the SHA-256 of the DLL and the zip in
the release notes and a build attestation:

```powershell
gh attestation verify WeaponFX-Nixware.zip --repo z3nghoul/WeaponFX-nixware-lua
```

## Build it yourself

Needs Visual Studio 2019+ or Build Tools with the C++ workload.

```bat
cd weaponfx
build.bat public
```

Output: `weaponfx\build\WeaponFX.dll`. Plain `build.bat` builds the
developer version with the ImGui menu. Details: [GUIDE](weaponfx/GUIDE.md),
[RENDERING](weaponfx/RENDERING.md).

## Releases

Push a tag and the workflow builds and publishes it:

```powershell
git tag v1.0.0
git push origin v1.0.0
```

## Disclaimer

This is foreign code running inside the game process. Use it offline or on
servers where scripts are allowed; not for public matchmaking. Provided as is,
no warranty.

## License

MIT. Original copyright © 2026 Garagoro (see `weaponfx/LICENSE`); repository
LICENSE © 2026 z3n1337.
