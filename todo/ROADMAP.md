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
- ✅ **Two controllers**: pad on port 0 drives P1, port 1 drives P2 (same layout each —
  stick move, hold Cross charge, Square melee), read via `ioPadGetInfo().status[i]` +
  `ioPadGetData(i, …)`. Start/Select+Start accepted from either pad; HUD shows a
  "connect controller 2" hint when port 1 is empty. Fight HUD: balance bar + two ki
  bars + score.
- ⬜ **Exit criteria — fighters deplete each other and a round ends, on hardware:**
  confirm on PS3/RPCS3 (builds green; on-console feel to be verified by playtest).
- Note: ki blasts (Projectile.cs) and the single battle/tournament/mission mode split
  are later (Phases 5 / 9); this runs one continuous first-to-3 match.

### Phase 5 — Ki system ✅
- ✅ Ported `Projectile.cs` + `LaunchKiBlast()`: Circle fires the strongest tier the
  current ki affords (>30 / >60 / >80 → tier 1/2/3), spending that ki. (Ki charging
  itself landed in Phase 4.)
- ✅ Projectiles fly **straight toward where the opponent was** at launch (LookAt +
  forward, not homing), with a fixed-pool of blasts + explosion effects.
- ✅ Hit detection (XZ radius vs the opponent) transfers power like melee but ranged
  and stronger: `BLAST_DMGBASE·tier + levelPower·1.25`; blasts expire on lifetime or
  when leaving the arena, spawning an explosion either way.
- ✅ HUD: glowing depth-scaled orbs (per-owner colour), expanding explosion rings, and
  tier-cost ticks (30/60/80) drawn on the ki bars.
- ⬜ **Exit criteria — chargeable blasts deal tiered damage on hit, on hardware:**
  confirm on PS3/RPCS3 (builds green; on-console feel to be verified by playtest).
- ✅ Explosion knockback ported (`OverlapSphere`): an impact shoves every fighter
  within `EXPL_KNOCK_RADIUS` radially outward, scaled by proximity (the original pushed
  toward the world origin — we use the intuitive radial push).
- Note: damage uses a single `BLAST_DMGBASE` rather than the original's quirky
  inner/outer 5-vs-10 split (which is really boundary timing noise, not a zone system).

### Phase 6 — HUD & UI ✅
- ✅ First real use of the **Clay** layout engine (`extern/clay-ps3`): a `build_and_
  render_ui()` overlay laid out with `CLAY`/`CLAY_TEXT` and drawn via `clay_render()`
  over the 3D scene.
- ✅ Ported `ScorePanel.cs` → a Clay **score pill** (top-centre) and `PongEndPanel.cs`
  → a Clay **result card** (centred, winner-coloured border, title; on match: final
  score + "Press START to rematch"). Pause shows a Clay panel too.
- ✅ Live HUD bars (balance + ki with tier ticks) stay custom-drawn — Clay can't cleanly
  do the two-colour fill / tick marks, and they already work.
- ⬜ **Exit criteria — live bars + a round-result panel, on hardware:** confirm on
  PS3/RPCS3 (builds green; on-console look to be verified by playtest).
- Note: establishes the Clay foundation for the Phase 9 menus (start / character
  select / mode).

### Phase 7 — CPU AI ✅
- ✅ Ported the `CPUController.cs` + `FMS.cs` FSM: **Patrol → Chase → Melee / ChargeKi**.
  Patrol roams synthetic waypoints; Chase seeks the player and fires blasts at
  `shootRate`; ChargeKi retreats to the mirror point and is the **only** state that
  refills ki (+15/s, matching the CPU branch of `chargeKi()`); Melee auto-punches on
  contact at `punchRate`.
- ✅ Difficulty from the 1–10 `LEVEL_POWER` scalar drives `shootRate`, `punchRate`, and
  `speedBase` (ki-scaled), via the original formulas.
- ✅ **NavMesh replaced** with hand-rolled seek (move toward the destination at the
  ki-scaled speed, stop within 0.5) — no pathfinding lib.
- ✅ The CPU drives **fighter 2 whenever pad 2 is absent** (HUD shows "P2: CPU"); plug
  in a second controller and it becomes human 2P. Single-player match vs CPU is playable.
- ⬜ **Exit criteria — playable single-player match on hardware:** confirm on PS3/RPCS3.
- Note: patrol waypoints are synthetic (the original read a `PatrolPoints` object);
  `rand()` is unseeded so the AI's random retreats follow a fixed sequence.

### Phase 8 — Audio ✅
- ✅ Audio subsystem (`source/audio.c`, `include/audio.h`) on **MikMod**, fully
  defensive: any init failure degrades to silence and never hangs the console.
- ✅ Uses the **original game audio**, converted to mono 16-bit PCM WAV with ffmpeg and
  embedded via `bin2o`, loaded from memory through a custom `MREADER`:
  - **Music**: the original `battle ambient.ogg` (OGG → full 2:42 PCM WAV) played as a
    **looping sample** (`SF_LOOP`, `SFX_CRITICAL` so SFX can't steal its voice). MikMod
    can't play OGG/modules-only, so the track loops as one long PCM sample.
  - **SFX**: the original `meleehit1` (melee), `basicbeam_fire` (blast) and `kiplosion`
    (explosion) WAVs, triggered from `melee()`, `spawn_blast()`, `spawn_expl()`.
- ✅ `MikMod_Update()` runs each frame. Replaces Unity AudioMixer (`PlayMusic.cs`).
- ⬜ **Exit criteria — music + combat SFX audible on hardware:** confirm on PS3/RPCS3
  (builds green; **cannot be verified on the dev host** — listen carefully and be ready
  to quit, since audio bugs can hang a console).
- Note: the full-length music makes the `.self` ~7.4 MB. The original's other SFX
  (teleport/kick/miss variants) and the AudioMixer volume buses aren't wired up.

### Phase 9 — Game modes & flow ✅
- ✅ **App state machine** (`APP_MENU → APP_CHARSEL → APP_FIGHT → result → menu`) with a
  **unified pad poll** (`poll_pads`) feeding every screen (retain-last + edge bits).
- ✅ **Mode menu** (Clay): Battle / Tournament / Mission / Quit (`StartOptions.cs`).
- ✅ **Character select** (Clay): the real **30-fighter roster** (names from the prefabs;
  `levelPower` 1–10) in a 10×3 grid; pick P1, then P2 in Battle (`SelectCharacters.cs` /
  `ChooseYourPlayer.cs`). Tournament picks a random opponent; Mission uses a fixed enemy
  per mission (cycled with L1/R1, mirroring `SelectMision.cs` / `MisionManager.cs`).
- ✅ **Per-fighter power**: each fighter's `levelPower` now drives melee/blast damage,
  move speed, and CPU difficulty (so the chosen character actually matters).
- ✅ **Modes**: Battle = first-to-1 (P2 human with pad 2, else CPU); Tournament =
  first-to-3 vs random CPU; Mission = first-to-1 vs the mission's fixed CPU.
- ✅ **Pause** (Clay): Start resumes, O quits to the menu (`Pause.cs` / `PauseMenu.cs`);
  result → Start returns to the menu (`ResetGame.cs`). Full loop closes.
- ⬜ **Exit criteria — full menu→select→match→result→menu loop on hardware:** confirm on
  PS3/RPCS3 (builds green; on-console verification by playtest).
- ✅ **Tournament difficulty tiers** (`instancePrefabs.cs`): L1/R1 picks tier 1/2/3 in
  char-select; the random opponent is drawn from that tier's index range (0–6 / 7–13 /
  14–19), so higher tiers fight stronger characters.
- ✅ **Sequential mission unlock**: missions start with 1 unlocked; beating mission N
  unlocks N+1 (L1/R1 only cycles unlocked ones; the count shows in the UI).
- ✅ **Per-character art**: the real 30 character icons (alpha cutouts) are downscaled
  and embedded as sprites — each fighter renders as its character in the arena, on the
  menu backdrop, and as a large preview in char-select.
- Note: some `levelPower` values are interpolated (binary prefabs). Arenas/backgrounds
  still come in Phase 10.

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
