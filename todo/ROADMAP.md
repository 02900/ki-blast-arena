# Ki Blast Arena — Migration Roadmap

Porting **Power of Pong** (Unity 2018, ~1,900 LOC C#) to PS3 homebrew (C / PSL1GHT).

This is the general plan, structured like
[ps3-remote-play/plans/NEXT_STEPS.md](https://github.com/02900/ps3-remote-play):
a phase list with **Done / In progress / Open**, each phase naming the original
Unity scripts it replaces and the PS3 subsystem that takes over.

Legend: ✅ done · 🚧 in progress · ⬜ not started

---

## Source game at a glance

What we are reproducing (from the Unity project `powerofpong`):

- **Core combat** (`BalanceOfPower.cs`, 326 LOC): health + ki bars, paddle-collision
  damage, ki-blast launching, win/loss, best-of-three round progression.
- **Player movement** (`SimpleController.cs`, `PongPaddle.cs`): bounded 3D movement
  (arena bounds X ±12, Z ±6, Y ±1), speed scales with ki level.
- **Ki projectiles** (`Projectile.cs`): charge-and-release blasts at 3 power levels
  (30 / 60 / 80 ki cost), raycast hit detection, explosion FX.
- **CPU AI** (`CPUController.cs` + `FMS.cs`): FSM — Patrol → Chase → Melee / ChargeKi
  → Attack; difficulty via a power level (1–10). Uses Unity **NavMesh** (must be replaced).
- **UI / flow** (`StartOptions.cs`, `SelectCharacters.cs`, `ScorePanel.cs`,
  `PongEndPanel.cs`, `MisionManager.cs`, `Pause.cs`, `PlayMusic.cs`, …): menus,
  ~30-character select, Battle / Tournament / Mission modes, HUD, pause, music.
- **Content**: ~30 character prefabs, 3 ki-blast variants, 7 arenas (Namek, Cell Ring,
  Desert, Diablo Desert, Kamehouse, Planet Supreme Kaio, Torneo Mundial), music + SFX.

---

## Phases

### Phase 0 — Scaffold ✅ (this commit)
- Repo layout mirroring the PS3 template (`source/ include/ data/ pkgfiles/ extern/
  docs/ scripts/ todo/ .github/`).
- Adapted `Makefile` (TITLE *Ki Blast Arena*, APPID `KIBLASTAR`), `sfo.xml`,
  `.gitignore`, Docker `build.sh` / `deploy.sh`, CI (`build` + `lint`).
- `source/main.c` placeholder stub, this roadmap, README.

### Phase 1 — Build & toolchain green ✅
- ✅ Added `extern/clay-ps3` as a git submodule (relative URL → `02900/clay-ps3`);
  re-added it to `SOURCES`/`INCLUDES` in the `Makefile`.
- ✅ Copied the `ttf_render` helper (`source/ttf_render.c`, `include/ttf_render.h`).
- ✅ `source/main.c` inits Tiny3D + YA2D + fonts + the Clay backend, renders a static
  "blank arena" frame (playfield box + two placeholder fighters + title) and exits
  cleanly on START / XMB exit via `sysUtilCheckCallback`.
- ✅ `make` builds a valid `src.self` through the Docker toolchain (verified locally;
  CI `build` job runs the same image).
- ⬜ **Exit criteria — boots on PS3 / RPCS3 to the test screen:** still to be confirmed
  on hardware/emulator (cannot be verified from the build host).

### Phase 2 — Input ✅
- ✅ DualShock3 via the PSL1GHT pad API (`ioPadInit`, `ioPadGetData`) with an analog
  deadzone and edge-detected actions.
- ✅ Control map ported from `SimpleController.cs` / `BalanceOfPower.cs`: left stick /
  D-pad move the P1 fighter (clamped to the arena box), Cross = charge, Circle = blast,
  Square = melee, Start = pause, Select+Start = quit. Replaces Unity CrossPlatformInput
  + the CNControls plugin.
- ✅ On-screen readout: the P1 fighter slides with input and MOVE/CHARGE/BLAST/MELEE/
  PAUSE chips light up as their buttons are used.
- ⬜ **Exit criteria — readout reacts to the pad on hardware:** confirm on PS3/RPCS3
  (builds green; on-console behavior to be verified by playtest).
- Note: world-space bounds (X ±12, Z ±6) and ki-scaled move speed are approximated in
  screen space here; the faithful port lands with the arena (Phase 3) and ki (Phase 5).

### Phase 3 — Arena & rendering ✅
- ✅ Real world-space arena drawn in perspective: a projected floor quad + grid
  spanning the `PongPaddle.cs` bounds (X ±12, Z ±6), with depth shading.
- ✅ Two fighters as depth-scaled **billboards** (with floor shadow + head cap),
  painter's-algorithm sorted by camera depth. (Tiny3D mesh upgrade is later.)
- ✅ Bounded world-space movement: both fighters move at world (x, 0, z), with the
  fighter's **footprint edge** (not its center) clamped to X ±12 / Z ±6; F1 = left
  stick / D-pad, F2 = right stick (local-2P placeholder until the CPU AI in Phase 7).
  Translation uses `SimpleController.cs`'s speed formula (`moveSpeed = 0.2 +
  levelPower/50`, ki-scaled).
- ✅ Renderer: a small **deterministic software pinhole projection** (lookAt camera +
  perspective divide) feeding Tiny3D's 2D primitives — chosen over Tiny3D's matrix
  pipeline so the camera is fully predictable without on-device trial-and-error.
- ⬜ **Exit criteria — two movable fighters inside the bounds on hardware:** confirm on
  PS3/RPCS3 (builds green; on-console look/feel to be verified by playtest).
- Note: `curKi` in the speed formula is a fixed placeholder until Phase 5; Y-axis
  movement (the `±1` clamp) is unused for now (fighters stay grounded at y=0).

### Phase 4 — Core combat ✅
- ✅ Ported `BalanceOfPower.cs` as a **power tug-of-war**: a single balance bar
  starts at 50; a melee hit TRANSFERS power (attacker +1.5·levelPower, opponent −the
  same) and grants the attacker +7 ki. Fill the bar to 100 to win the round.
- ✅ Per-fighter **ki state**: charges while holding the charge button (faster when
  standing still: +25/s vs +10/s), drains −3/s otherwise (`chargeKi()`).
- ✅ **Hand-rolled collision** (XZ center distance ≤ contact radius) — no Rigidbody;
  melee is edge-triggered, cooldown-gated, and knocks the opponent back.
- ✅ **Round/match flow**: round ends when the bar hits a cap; first to 3 round wins
  takes the match; round-over and match-over banners; Start rematches.
- ✅ Both fighters can fight (P1 = Cross charge / Square melee, P2 = L1 charge / R1
  melee), so they can deplete each other and end a round. Fight HUD: balance bar +
  two ki bars + score.
- ⬜ **Exit criteria — fighters deplete each other and a round ends, on hardware:**
  confirm on PS3/RPCS3 (builds green; on-console feel to be verified by playtest).
- Note: ki blasts (Projectile.cs) and the single battle/tournament/mission mode split
  are later (Phases 5 / 9); this runs one continuous first-to-3 match.

### Phase 5 — Ki system ⬜
- Port `Projectile.cs`: hold-to-charge, release to fire at 3 power levels
  (30 / 60 / 80 ki), travel + sphere/ray hit detection, explosion FX, ki cost/regen.
- **Exit criteria:** chargeable ki blasts deal tiered damage on hit.

### Phase 6 — HUD & UI ⬜
- Health + ki bars and menus via **Clay** (`extern/clay-ps3`) + YA2D, replacing the
  Unity Canvas. Port `ScorePanel.cs`, `PongEndPanel.cs`, HUD scripts.
- **Exit criteria:** live HUD bars + a round-result panel.

### Phase 7 — CPU AI ⬜
- Port `CPUController.cs` + `FMS.cs` FSM (Patrol → Chase → Melee / ChargeKi → Attack),
  with the 1–10 difficulty scalar.
- **Replace Unity NavMesh** with hand-rolled steering/seek toward the player (no
  pathfinding library on PS3). See Risks.
- **Exit criteria:** a playable single-player match vs the CPU.

### Phase 8 — Audio ⬜
- Music + SFX via **MikMod** (module playback) and/or `libaudio` (PCM). Convert the
  Unity OGG/MP3 music to `.s3m`/`.mod` or PCM; SFX for melee, blast, explosion,
  teleport. Replaces Unity AudioMixer (`PlayMusic.cs`).
- **Exit criteria:** background music + core combat SFX.

### Phase 9 — Game modes & flow ⬜
- Start menu, character select (~30 characters), and the three modes: **Battle**,
  **Tournament** (best-of-3 bracket), **Mission**. Pause menu. Port `StartOptions.cs`,
  `SelectCharacters.cs` / `ChooseYourPlayer.cs`, `SelectMision.cs` / `MisionManager.cs`,
  `Pause.cs` / `PauseMenu.cs`, `ResetGame.cs`.
- **Exit criteria:** full menu → select → match → result → menu loop.

### Phase 10 — Arenas & asset pipeline ⬜
- Bring the 7 arenas over as backgrounds / scene configs; establish the conversion
  pipeline from Unity assets into `data/` (embedded) and `pkgfiles/assets/` (PKG).
- Decide character representation (sprite sheets vs exported meshes).
- **Exit criteria:** multiple selectable arenas with their art.

### Phase 11 — Packaging & polish ⬜
- `ICON0.PNG` / `PIC1.PNG`, `make pkg` for XMB install, performance pass, `docs/`.
- **Exit criteria:** an installable PKG that runs from the XMB.

---

## Risks & open questions

- **Physics replacement.** Unity Rigidbody/Collider/raycasts have no PS3 equivalent.
  The game's physics are simple (bounded movement + overlap/ray hit tests), so a small
  hand-rolled AABB/sphere math layer should suffice — but it must be built from scratch.
- **NavMesh AI.** The CPU pathfinds via Unity NavMesh; there is no pathfinding lib on
  PS3. The arena is small and open, so direct steering/seek is likely enough — needs
  validation against the original AI feel (Phase 7).
- **3D models.** ~30 character prefabs are Unity meshes. Either export to a
  PS3-renderable mesh format for Tiny3D, or fall back to 2D sprite/billboard fighters.
  Sprites are the cheaper, lower-risk starting point.
- **Audio formats.** Unity uses OGG/MP3 + an AudioMixer. MikMod wants tracker modules
  (`.mod`/`.s3m`); PCM via `libaudio` is the alternative. Music will need conversion and
  possibly re-authoring as modules.
- **Memory & performance.** Embedding all art via `bin2o` inflates the `.self`; large
  assets should go in the PKG (`pkgfiles/assets/`) instead. Profile once content lands.

---

## Reference

- Original Unity project: `powerofpong` (sibling checkout).
- Structural templates: `ps3-homebrew-template`, `ps3-remote-play`.
- Toolchain image: `ghcr.io/02900/ps3-toolchain:latest`.
