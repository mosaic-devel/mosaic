# Desktop entry & icons

## If you are packaging Mosaic for a distribution

**You do not need anything in this directory.** `cmake --install` already lays down everything the
freedesktop specs want, from rules authored in `CMakeLists.txt`:

| installed path | source |
| --- | --- |
| `$datadir/applications/mosaic.desktop` | `data/desktop/mosaic.desktop` |
| `$datadir/mime/packages/mosaic.xml` | `data/desktop/mosaic.xml` (the `.mosaic` document type) |
| `$datadir/icons/hicolor/scalable/apps/mosaic.svg` | `assets/app_icon.svg` |
| `$datadir/icons/hicolor/scalable/mimetypes/image-x-mosaic.svg` | `assets/mimetype_icon.svg` |

Those rules are **authored, never run** by the build — refreshing the system caches is the
packager's job (and most distro helpers do it automatically in a post-install hook):

```sh
update-desktop-database "$datadir/applications"
update-mime-database    "$datadir/mime"
gtk-update-icon-cache -qtf "$datadir/icons/hicolor"
```

Three things must stay in lockstep, because the Wayland window icon depends on all three matching:
the `.desktop` **basename**, its `Icon=` value, and its `StartupWMClass` — all `mosaic`, which is
the `app_id` the binary pins with `Fl_Window::default_xclass("mosaic")`. Rename any one of them and
the compositor stops finding the entry. `docs/wayland.md` §3 has the full chain.

## If you are running an AppImage (or any uninstalled build)

`install-desktop-entry.sh` does the same job for one user, under `$XDG_DATA_HOME`
(default `~/.local/share`):

```sh
./install-desktop-entry.sh                      # from inside the AppImage; uses $APPIMAGE
./install-desktop-entry.sh --exec /path/to/Mosaic-0.3.1-linux-x86_64.AppImage
./install-desktop-entry.sh --uninstall
```

It writes a `.desktop` whose `Exec=`/`TryExec=` point at the AppImage where it actually sits, copies
the icon into the user's hicolor tree, and refreshes whatever caches are installed. `TryExec=` means
a desktop hides the entry if the AppImage is later deleted, rather than offering a launcher that
fails.

**Why an AppImage needs this at all.** A Wayland compositor finds a window's icon by matching its
`app_id` to a `.desktop` file **in the compositor's own search path**. An AppImage is never
installed, so there is no such file, and the window gets a placeholder — while the same binary looks
correct on X11, which carries icon pixels on the window itself (`_NET_WM_ICON`) and needs nothing
installed. Mosaic also sets its icon directly over `xdg-toplevel-icon-v1`, which fixes the *icon*
with nothing installed at all — but only on compositors that implement it (KWin does; Mutter
[does not yet](https://gitlab.gnome.org/GNOME/mutter/-/issues/4100)), and it does nothing for the
launcher entry, the MIME association or task-manager grouping. This script covers all of it,
everywhere. See `docs/wayland.md` §3.1.

This is deliberately **not** run automatically on first launch. Upstream AppImage guidance is that
self-integration is the system's job — [`appimaged`](https://docs.appimage.org/user-guide/run-appimages.html)
and [AppImageLauncher](https://github.com/TheAssassin/AppImageLauncher) exist for it, and if you use
either, you do not need this script. An AppImage that quietly wrote to `$HOME` on first run would
break the one-file, delete-it-and-it-is-gone property that makes the format worth shipping.
