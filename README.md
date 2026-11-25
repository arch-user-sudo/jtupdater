# jtupdate

A small Wayland-native repository updater styled like `jtspeedtest` and
`jtnetworks`. It clones/pulls a set of repos in the background, runs their
builds, installs everything that changed in one elevated pass, and shows per-repo
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
    { "jtsettings",  "jtsettings",  "https://github.com/arch-user-sudo/jtsettings.git",  "make -j2", "make install" },
    { "jtwpi",       "jtwpi",       "https://github.com/arch-user-sudo/jtwpi.git",       "make -j2", "make install" },
    { "jtpolkit",    "jtpolkit",    "https://github.com/arch-user-sudo/jtpolkit.git",    "make -j2", "make install" },
    { "jtgreeter",   "jtgreeter",   "https://github.com/arch-user-sudo/jtgreeter.git",   "make -j2", "make install" },
    { "jtfiles",     "jtfiles",     "https://github.com/arch-user-sudo/jtfiles.git",     "make -j2", "make install" },
};
```

Checkouts live in `<base-dir>/<slug>`, where the base directory defaults to
`~/jtupdate/repos`. Pass `--base-dir DIR` (or `--base-dir=DIR`) to point the
updater at another location; a leading `~` is expanded. JTLab's Updater button
passes `$HOME/jtupdate/repos` explicitly, so the location does not depend on
the directory the bar was started from.

If a checkout already exists as
a git repository, an empty URL pulls its existing origin. Clones are staged in
a temporary folder and moved into place only after they succeed, so an
interrupted or failed clone never leaves a half-written checkout behind. If a
non-git folder is already sitting at a target, it is moved aside as
`<slug>.incomplete-<timestamp>-<pid>` before the fresh clone.

## Install

Changed repositories are built first. For the install phase the app requests
authorization from **Polkit** by re-executing itself through `pkexec` with a
private install plan. The graphical Polkit agent shows a single dialog, so no
terminal — and no `doas`/`sudo` password entry — is needed. The elevated
process runs each checkout's install command and reports the results back to
the window. There is no separate helper script or second binary.

A Polkit authentication agent must be running in your session (most desktops
start one automatically). `make install` also installs an action at
`$(PREFIX)/share/polkit-1/actions/com.jtlab.jtupdate.install.policy`, which
gives the prompt a clear description and keeps the authorization briefly so a
batch does not ask twice. On systems without `pkexec`, a cached `doas`/`sudo`
timestamp is used as a silent fallback; that path opens no terminal either.

```bash
sudo make install
```

That installs `jtupdate` and `jtupdatectl` under `/usr/local/bin`, the matching
icon under `/usr/local/share/icons/hicolor`, and the Polkit policy described
above. When `jtupdate` is run from its source tree, the `jtupdater` install
uses that source tree instead of the cached clone, so a freshly built updater
never replaces itself with an older checkout.

## Failed and cancelled builds

A build failure opens an in-app window listing the missing packages, headers,
or linker libraries found in the compiler output. Failed projects are also
recorded in `$HOME/.cache/jt/jtupdate`, one slug per line. A compile that is
cancelled is recorded in the same file as `<slug>\tcancelled`, so the project
whose build was interrupted is remembered too. Both kinds are retried on the
next update even when HEAD has not changed. Once a project builds and installs
successfully, its line is removed; the cache file is deleted when no pending
builds remain.

## Controls

- `Space` / `Enter` — check for updates, or stop while running
- `Esc` / `q` — quit, or close the build-failure window when it is open
- Drag the header to move the window
- Scroll over the repository list, drag the scrollbar, or click its track to
  page through repositories
- Resize the window down to 760×620; the list scrolls while the progress,
  button, and footer stay in place
- `jtupdatectl accent '#RRGGBB'` — live theme change

JTLab's workspace popup has an Updater button: it launches `jtupdate` with the
current wallpaper accent and forwards later accent changes over the same
control socket while the window is open.
