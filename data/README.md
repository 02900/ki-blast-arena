# `data/` — embedded assets

Files placed here are converted to object files with `bin2o` at build time and
**embedded directly in the `.self`**, so they are available even when launching
over the network with `ps3load` (no PKG install required).

Supported extensions (see the `Makefile` rules): `.png`, `.jpg`, `.bin`, `.mod`, `.s3m`.

Each file `foo.png` becomes two C symbols you can reference from `source/`:

```c
extern const uint8_t  foo_png[];
extern const uint32_t foo_png_size;
```

Empty for now — the ki-blast / character / arena sprites and the music module land
during the porting phases. See [../todo/ROADMAP.md](../todo/ROADMAP.md).

> For assets that should ship inside the installable PKG instead (loaded from
> `/dev_hdd0/game/KIBLASTAR/USRDIR/assets/`), put them under `pkgfiles/assets/`.
