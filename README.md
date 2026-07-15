# Skyrim Click-Casting

An SKSE64 plugin for Skyrim SE/AE that turns charge-and-release spellcasting into single clicks. Click LMB/RMB as normal — the plugin holds the charge for you and releases the moment the spell is ready. Fire-and-forget spells, scrolls, and staves are supported, including dual casting and hold-to-recast. Concentration spells (Flames etc.) are deliberately untouched and work exactly as vanilla.

**Nexus Mods page:** https://www.nexusmods.com/skyrimspecialedition/mods/185317

## Features

- **Click-to-cast:** one click charges and fires any fire-and-forget spell with a cast time — no holding required.
- **Dual casting:** click both buttons to dual-cast (requires the Dual Casting perk). Master/ritual spells work with a single click.
- **Hold-to-recast:** keep the button held to chain casts automatically, with a 0.25 s delay between casts.
- **Scrolls and staves:** same click-cast behavior, including two-handed scrolls.
- **Pass-through by design:** concentration spells, weapons, gamepad input, and everything else the plugin doesn't target is handed to the game untouched.
- No ESP/ESL, no record edits, no scripts, no MCM. Safe to install or uninstall mid-playthrough.

## How it works

The plugin hooks `AttackBlockHandler::ProcessButton` to gate, latch, and swallow attack-button events for hands holding a click-cast-eligible item (fire-and-forget spell, scroll, or FaF-enchanted staff with charge time > 0). A `PlayerCharacter::Update` hook polls the hand's magic caster state each frame; when the caster reaches its Ready state, the plugin synthesizes the button release, firing the spell exactly as a manual release would. Exactly one down/up pair is delivered to the game per cast, so vanilla input balance is preserved.

## Requirements

- Skyrim SE/AE (built and tested on 1.6.1170; version-independent via Address Library, other versions unconfirmed)
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

Keyboard and mouse only — gamepad input is passed through untouched.

## Building from source

Prerequisites: Visual Studio 2022 Build Tools (MSVC, C++ workload), CMake, Ninja, vcpkg (`VCPKG_ROOT` set).

```bat
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build
```

Output: `build\SkyrimClickCasting.dll`. Package for MO2 as a zip with root `SKSE\Plugins\SkyrimClickCasting.dll`.

Dependencies are resolved via the vcpkg manifest (`vcpkg.json`), including CommonLibSSE-NG from the [colorglass registry](https://gitlab.com/colorglass/vcpkg-colorglass).

## AI transparency

This mod was developed with Claude (Anthropic) as developer and Claude Code as coding agent, with the project owner acting as decision-maker, quality tester, and publisher. See the Nexus page for the full disclosure.

## Credits

- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) (MIT)
- [SKSE64](https://skse.silverlock.org/) team
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) by meh321
- spdlog, fmt, xbyak (via vcpkg)

## License

MIT — see [LICENSE](LICENSE).
