# Auto-Shot / Click-Casting

An SKSE64 plugin for Skyrim SE/AE that turns charge-and-release actions into single clicks.

Click LMB/RMB as normal — the plugin holds the charge for you and releases the moment the action is
ready. Fire-and-forget spells, scrolls and staves are click-cast, and bows can auto-fire the instant
they reach full draw. Concentration spells (Flames etc.) are deliberately untouched and behave
exactly as vanilla.

**Nexus Mods page:** https://www.nexusmods.com/skyrimspecialedition/mods/185317

## Features

- **Click-to-cast** — one click charges and fires any fire-and-forget spell that has a cast time.
  No holding required.
- **Dual casting** — click both buttons to dual-cast (requires the Dual Casting perk). Master and
  ritual spells work from a single click. This is part of Spell Casting; it has no separate toggle.
- **Hold-to-recast** — keep the button held to chain casts automatically. The pause between casts
  is adjustable in the MCM.
- **Scrolls and staves** — the same click-cast behaviour, including two-handed scrolls and
  fire-and-forget enchanted staves.
- **Bow Auto-Shot** — *click* to draw and let the plugin fire the moment the bow is fully drawn;
  *hold* to keep drawing and release manually. Both are always available — which one you get is
  decided by whether you let go of the button before or after full draw.
- **Bow Charge Plus Delay** — an opt-in extra hold after full draw, for players running
  [Bow Charge Plus](https://www.nexusmods.com/skyrimspecialedition/mods/59124).
- **Pass-through by design** — concentration spells, crossbows, melee weapons, bow bashes, gamepad
  input and everything else the plugin does not target is handed to the game untouched.

## MCM settings

The plugin ships with an MCM (MCM Helper). Every option is read at the point of use, so a change
takes effect from the next cast or shot without disturbing anything already in flight.

| Setting | Default | What it does |
|---|---|---|
| Spell Casting | On | Apply click-casting to equipped spells (including dual casting). |
| Scroll Casting | On | Apply click-casting to equipped scrolls. |
| Staff Casting | On | Apply click-casting to equipped staves. |
| Bow Auto-Shot | On | Click fires automatically at full draw; hold releases manually. |
| Bow Charge Plus Delay | **Off** | Hold a fully drawn bow 0.65 s longer before auto-firing. |
| Recast Delay | 0.25 s | Pause between chained casts while the button is held. Range 0.10–1.00 s. |

Turning a feature off makes that item type behave exactly as it does without the plugin — it is a
plain pass-through, not a different code path.

### About Bow Charge Plus Delay

Bow Charge Plus counts its charge levels from the moment the bow reaches full draw. Auto-Shot
normally releases about 13 ms after that point, which is far too fast for BCP to register even its
first level. With this option enabled the plugin keeps the draw held for **0.65 s** after full draw
before releasing, which clears BCP's first-level threshold reliably.

It is **off by default** and should stay off unless you actually run Bow Charge Plus — it makes
every auto-shot noticeably slower, which is the entire point of it.

## What's in the package

```
AutoShotClickCasting.esp                              ESL-flagged, holds the MCM quest only
Scripts\AutoShotClickCasting_MCM.pex                  MCM Helper config script
Source\Scripts\AutoShotClickCasting_MCM.psc           its source
MCM\Config\AutoShotClickCasting\config.json           MCM layout
MCM\Config\AutoShotClickCasting\settings.ini          shipped defaults
SKSE\Plugins\AutoShotClickCasting.dll                 the plugin
```

The ESP is ESL-flagged (it takes no load-order slot) and contains a single quest record that exists
purely to host the MCM menu. No records are edited, no game data is overridden, and no scripts run
in your save beyond MCM Helper's own config handling. Safe to install or uninstall mid-playthrough.

If you delete the ESP and the scripts, the DLL still runs with its built-in defaults — the ini files
are read directly, not through Papyrus.

## Requirements

- Skyrim SE/AE (built and tested on 1.6.1170; version-independent via Address Library, other
  versions unconfirmed)
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- [MCM Helper](https://www.nexusmods.com/skyrimspecialedition/mods/53000) — for the settings menu

Keyboard and mouse only. Gamepad input is passed through untouched.

## How it works

### Spells, scrolls and staves

The plugin hooks `AttackBlockHandler::ProcessButton` to gate, latch and swallow attack-button events
for a hand holding a click-cast-eligible item (a fire-and-forget spell or scroll, or a
fire-and-forget enchanted staff, with a charge time above zero). A `PlayerCharacter::Update` hook
polls that hand's magic caster once per frame; when the caster reaches its Ready state the plugin
synthesises the button release, firing the spell exactly as a manual release would.

Dual casting is handled by a separate controller that owns both hands for the duration of the
episode, because the engine drives only one caster during a dual and the other one has to be left
alone rather than treated as an interrupted cast.

### Bows

A bow has no magic caster, so the bow path is a self-contained machine that polls the actor's
attack state instead (`ATTACK_STATE_ENUM`, where full draw is `kBowDrawn`).

When you press the attack button the press always goes through to the game, so the draw starts
normally. What happens next is decided when you *release*:

- **Released while still drawing** — the plugin swallows that release and takes over the hold. It
  keeps feeding the game synthetic held-button frames, with a continuously rising hold duration, so
  the draw builds exactly as if you were still holding the button. Two consecutive frames at full
  draw and it delivers the release. The arrow leaves at full power because the release carries the
  whole accumulated hold time, not just the moment it was sent.
- **Released at or after full draw** — nothing is intercepted at all. This is the manual path, and
  it is what keeps bow charge mods and bow-bash combo mods working normally.

Every abnormal exit — a staggered draw, a weapon swap, a menu, an interrupted pull, a shot fired by
something else — delivers the release the plugin owes before it lets go, so the game never ends up
holding a draw that nothing will release.

### Input balance

Across every path, exactly one button-up is delivered to the game for every button-down it received.
That invariant is what keeps the plugin from desynchronising vanilla's own input state, and it holds
for click, hold, spam-clicking, dual casting, chained recasts and every failure path.

## Compatibility

- **Crossbows are not supported** and are passed through untouched. The engine does not expose a
  usable draw state for them — the actor attack state stays at `kNone` for the entire crossbow
  cycle — so there is nothing to poll.
- **Bow bash is untouched.** A bash never enters a draw state, so the bow machine never engages.
- **Bow Charge Plus / Bow PowerShot / bow combo mods** keep working on the hold path, and on the
  click path if you enable the delay option above.
- **Other plugins that hook `AttackBlockHandler::ProcessButton`** (One Click Power Attack and
  similar) share the same hook slot. No conflict has been observed, but load order determines which
  plugin sees an event first.

## Building from source

Prerequisites: Visual Studio 2022 Build Tools (MSVC, C++ workload), CMake, Ninja, and vcpkg with
`VCPKG_ROOT` set.

**Build from an "x64 Native Tools Command Prompt for VS 2022"**, or run `vcvars64.bat` first. The
MSVC compiler needs the environment that script sets up; without it the build fails part-way through
with a misleading error that does not name the real cause:

```
fatal error C1083: Cannot open include file: 'xmmintrin.h': No such file or directory
```

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build
```

Output: `build\AutoShotClickCasting.dll`.

To package for a mod manager, zip the layout shown under
[What's in the package](#whats-in-the-package) with the DLL at
`SKSE\Plugins\AutoShotClickCasting.dll`.

Dependencies are resolved through the vcpkg manifest (`vcpkg.json`), including CommonLibSSE-NG from
the [colorglass registry](https://gitlab.com/colorglass/vcpkg-colorglass).

## AI transparency

This mod was developed with Claude (Anthropic) as developer and Claude Code as coding agent, with
the project owner acting as decision-maker, quality tester, and publisher. See the Nexus page for
the full disclosure.

## Credits

- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) (MIT)
- [SKSE64](https://skse.silverlock.org/) team
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) by meh321
- [MCM Helper](https://www.nexusmods.com/skyrimspecialedition/mods/53000) by Parapets
- spdlog, fmt, xbyak, SimpleIni (via vcpkg)

## License

MIT — see [LICENSE](LICENSE).
