# Ki Blast Arena

A PlayStation 3 homebrew port of **Power of Pong** — a Dragon Ball-style 1v1
arena fighter that mixes pong/paddle mechanics with charged **ki** energy blasts.
Two fighters trade paddle strikes and launch projectiles at three power levels,
draining each other's health bar across best-of-three rounds, with a CPU opponent
driven by a small finite-state machine.

The original is a Unity 2018 game (`powerofpong`); this repo re-implements it from
scratch in C on the PSL1GHT SDK, following the conventions of
[02900/ps3-homebrew-template](https://github.com/02900/ps3-homebrew-template) and
[02900/ps3-remote-play](https://github.com/02900/ps3-remote-play).

> ## 🚧 Status: scaffold
> This repo currently contains **only the project skeleton and the migration
> roadmap** — it is **not yet playable**. `source/main.c` is a placeholder. The
> step-by-step port plan lives in **[todo/ROADMAP.md](todo/ROADMAP.md)**.

---

## Building

You need the PSL1GHT toolchain. The easiest way is the prebuilt Docker image — no
local install, works on macOS/Windows/Linux. Mount the project at `/src` and run a
command inside the image:

```bash
# Build  ->  produces src.elf / src.self in the project root
docker run --rm -v "$PWD":/src -w /src ghcr.io/02900/ps3-toolchain make

# Build an installable PKG (for XMB)
docker run --rm -v "$PWD":/src -w /src ghcr.io/02900/ps3-toolchain make pkg

# Clean
docker run --rm -v "$PWD":/src -w /src ghcr.io/02900/ps3-toolchain make clean
```

Or use the helper wrappers:

```bash
./scripts/build.sh            # build
./scripts/build.sh pkg        # installable PKG
./scripts/build.sh clean      # clean
```

> **Platform notes**
> - **Apple Silicon (M1/M2/…):** add `--platform linux/amd64` to every `docker run`
>   (the image is x86_64; Docker emulates it).
> - **Windows:** run the commands from a **WSL2** shell.
> - **Linux:** if your user isn't in the `docker` group, prefix with `sudo`.

Because the project is mounted at `/src`, the build is named after that directory,
so the outputs are **`src.elf`**, **`src.self`** and **`src.fake.self`**.

## Sending to a PS3 (ps3load)

With **PS3LoadX running on the console** (listening on TCP `4299`):

```bash
PS3_IP=192.168.1.13 ./scripts/deploy.sh
```

The `--network host` flag (set inside the script) lets the container reach the PS3
on your LAN — without it the loader receives the file but never launches it.

---

## Project structure

```
ki-blast-arena/
├── .github/workflows/   # CI: build (via toolchain image) + docs link lint
├── source/              # C game source (PPU) — main.c (stub for now)
├── include/             # Shared headers
├── data/                # Embedded assets (bin2o): sprites, audio modules
├── pkgfiles/            # Files bundled into the PKG (ICON0.PNG, assets/)
├── extern/              # External deps (Clay UI submodule — added in Phase 1)
├── docs/api/            # Per-library API notes
├── scripts/             # Dockerized build.sh / deploy.sh wrappers
├── todo/                # → ROADMAP.md: the migration plan
├── Makefile             # PSL1GHT build
├── sfo.xml              # Application metadata (TITLE_ID: KIBLASTAR)
└── README.md
```

## Toolchain & libraries

Built against the libraries the toolchain image ships: **Tiny3D** (3D), **YA2D**
(2D sprites), **FreeType** (TTF text), **MikMod** (audio), **libcurl**/**PolarSSL**,
**Mini18n**, plus the PSL1GHT pad/audio/sysutil APIs. The Clay UI layout engine
(`extern/clay-ps3`) will be added as a submodule for the HUD and menus.

## Roadmap

The full, phase-by-phase migration plan is in **[todo/ROADMAP.md](todo/ROADMAP.md)**.

## Patterns & gotchas

Reusable conventions and traps hit while porting (PSL1GHT pad quirks, Tiny3D colour
formats, deterministic camera, Unity-port tips) are in
**[docs/PATTERNS.md](docs/PATTERNS.md)** — read it before adding input or rendering code.

## Credits

- Original game: **Power of Pong** (Unity).
- Placeholder battle music: the **"Haiku"** S3M tracker module from
  [02900/ps3-homebrew-template](https://github.com/02900/ps3-homebrew-template) (to be
  replaced with original/authored music). Combat SFX are synthesized at runtime.
- Toolchain & structural conventions: [02900/ps3-toolchain](https://github.com/02900/ps3-toolchain),
  [02900/ps3-homebrew-template](https://github.com/02900/ps3-homebrew-template),
  [02900/ps3-remote-play](https://github.com/02900/ps3-remote-play).

## License

MIT.
