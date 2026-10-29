# jtupdate

A small Wayland-native repository updater styled like `jtspeedtest` and
`jtnetworks`. It clones/pulls a set of repos in the background, runs their
builds, installs everything that changed in one helper pass, and shows per-repo
and overall progress.

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
    { "JTLab",       "JTLab",       "https://github.com/arch-user-sudo/JTLab.git",       "make -j2", "make install" },
    { "jtspeedtest", "jtspeedtest", "https://github.com/arch-user-sudo/jtspeedtest.git", "make -j2", "make install" },
    { "jtnetworks",  "jtnetworks",  "https://github.com/arch-user-sudo/jtnetworks.git",  "make -j2", "make install" },
    { "jtlabwc",     "jtlabwc",     "https://github.com/arch-user-sudo/jtlabwc.git",     "make -j2", "make install" },
    { "jtupdate",    "jtupdater",   "https://github.com/arch-user-sudo/jtupdater.git",   "make -j2", "make install" },
    { "jtlX",        "jtlX",        "https://github.com/arch-user-sudo/jtlX.git",        "make -j2", "make install" },
};
```

Checkouts live in `~/jtupdate/repos/<slug>`. If a checkout already exists as
a git repository, an empty URL pulls its existing origin. Clones are staged in
a temporary folder and moved into place only after they succeed, so an
interrupted or failed clone never leaves a half-written checkout behind. If a
non-git folder is already sitting at a target, it is moved aside as
`<slug>.incomplete-<timestamp>-<pid>` before the fresh clone.

## Install

Changed repositories are built first. After the build phase, the updater asks
`jtupdate-install` to run `make install` in every changed checkout. One helper
run means one authorization prompt and one `foot`/`footclient` window for the
whole batch. The helper checks for `doas`, then `sudo`, then `pkexec`.
If a `doas`/`sudo` timestamp is already cached, no terminal is opened.

The helper is installed next to the updater:

```bash
sudo make install
```

That installs `jtupdate`, `jtupdatectl`, and `jtupdate-install` under
`/usr/local/bin`. When `jtupdate` is run from its source tree, the
`jtupdater` install uses that source tree instead of the cached clone, so a
freshly built updater never replaces itself with an older checkout.

The helper can also be run by hand:

```bash
jtupdate-install ~/jtupdate/repos/JTLab ~/jtupdate/repos/jtnetworks
```

## Controls

- `Space` / `Enter` — check for updates, or stop while running
- `Esc` / `q` — quit
- Drag the header to move the window
- `jtupdatectl accent '#RRGGBB'` — live theme change

JTLab's workspace popup has an Updater button: it launches `jtupdate` with the
current wallpaper accent and forwards later accent changes over the same
control socket while the window is open.
