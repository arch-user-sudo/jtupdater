# jtupdate

A small Wayland-native repository updater styled like `jtspeedtest` and
`jtnetworks`. It clones/pulls a set of repos in the background, runs their
builds, and shows per-repo and overall progress.

## Build

```bash
make
./jtupdate
```

Live accent changes:

```bash
./jtupdatectl accent '#378484'
./jtupdatectl       '#378484'
```

`jtupdatectl update`, `stop`, and `quit` are also available.

## Repositories

The managed repositories are configured in the `g_repos` table at the top of
`jtupdate.c`:

```c
static const RepoDef g_repos[REPO_COUNT] = {
    { "JTLab",       "JTLab",       "https://github.com/arch-user-sudo/JTLab.git",       "make -j2" },
    { "jtspeedtest", "jtspeedtest", "https://github.com/arch-user-sudo/jtspeedtest.git", "make -j2" },
    { "jtnetworks",  "jtnetworks",  "https://github.com/arch-user-sudo/jtnetworks.git",  "make -j2" },
    { "jtlabwc",     "jtlabwc",     "https://github.com/arch-user-sudo/jtlabwc.git",     "make -j2" },
    { "jtupdate",    "jtupdater",   "https://github.com/arch-user-sudo/jtupdater.git",   "" },
    { "jtlX",        "jtlX",        "https://github.com/arch-user-sudo/jtlX.git",        "make -j2" },
};
```

Checkouts live in `~/jtupdate/repos/<slug>`. If a checkout already exists as
a git repository, an empty URL pulls its existing origin.

`jtupdater` currently has no build step because the repository only contains a
LICENSE file. Change its build string to `"make -j2"` once the updater sources
are pushed there.

The updater only builds; installing under `/usr/local` remains manual.

## Controls

- `Space` / `Enter` — check for updates, or stop while running
- `Esc` / `q` — quit
- Drag the header to move the window
- `jtupdatectl accent '#RRGGBB'` — live theme change

JTLab's workspace popup has an Updater button: it launches `jtupdate` with the
current wallpaper accent and forwards later accent changes over the same
control socket while the window is open.

## Install

```bash
sudo make install
```
