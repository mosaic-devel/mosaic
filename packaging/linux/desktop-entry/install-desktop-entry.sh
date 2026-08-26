#!/usr/bin/env sh
# Install (or remove) a Mosaic desktop entry + icons for ONE user, under $XDG_DATA_HOME.
#
# This exists for the case CMake's install rules cannot cover: a binary that is never installed.
# A Wayland compositor resolves a window's app_id -> a .desktop file IN ITS OWN search path ->
# Icon=, so an AppImage sitting in ~/Downloads has no icon, no launcher entry, no MIME association
# and no task-manager name, however correct the binary is. (Mosaic additionally sets its icon
# through xdg-toplevel-icon-v1, which fixes the ICON alone, and only on compositors that implement
# it -- KWin yes, Mutter not yet. This script is what fixes the rest, everywhere.)
#
# DISTRO PACKAGERS DO NOT NEED THIS. `cmake --install` already lays down the .desktop, the MIME XML
# and the hicolor icons in the right places; see this directory's README for the post-install cache
# commands. This is for AppImage users and for anyone testing an uninstalled build.
#
# Usage, from inside a running AppImage (both variables are set by the AppImage runtime):
#   ./install-desktop-entry.sh
# Or explicitly, for any binary:
#   ./install-desktop-entry.sh --exec /opt/mosaic/bin/mosaic --icon /opt/mosaic/share/.../mosaic.svg
#   ./install-desktop-entry.sh --uninstall
#
# Everything it writes is under $XDG_DATA_HOME (default ~/.local/share) and is listed on exit, so
# undoing it by hand is possible without this script. It touches nothing outside the user's home.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
APPS_DIR="$DATA_HOME/applications"
ICON_DIR="$DATA_HOME/icons/hicolor/scalable/apps"
MIME_DIR="$DATA_HOME/mime/packages"
DESKTOP_FILE="$APPS_DIR/mosaic.desktop"

# The .desktop basename, the Icon= value and StartupWMClass must all stay "mosaic": that string is
# the app_id Mosaic pins with Fl_Window::default_xclass("mosaic"), and the match is what makes the
# compositor find this file at all. Changing it here silently breaks the icon.
APP_ID=mosaic

EXEC=""
ICON=""
ACTION=install

while [ $# -gt 0 ]; do
    case "$1" in
        --exec) EXEC=${2:?--exec needs a path}; shift 2 ;;
        --icon) ICON=${2:?--icon needs a path}; shift 2 ;;
        --uninstall|--remove) ACTION=uninstall; shift ;;
        -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument '$1' (try --help)" >&2; exit 2 ;;
    esac
done

refresh_caches() {
    # Best-effort: a desktop that has none of these still picks the entry up, just later.
    [ -d "$APPS_DIR" ] && command -v update-desktop-database >/dev/null 2>&1 &&
        update-desktop-database "$APPS_DIR" 2>/dev/null || true
    command -v update-mime-database >/dev/null 2>&1 &&
        update-mime-database "$DATA_HOME/mime" 2>/dev/null || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 &&
        gtk-update-icon-cache -qtf "$DATA_HOME/icons/hicolor" 2>/dev/null || true
}

if [ "$ACTION" = uninstall ]; then
    removed=0
    for f in "$DESKTOP_FILE" "$ICON_DIR/$APP_ID.svg" "$ICON_DIR/image-x-mosaic.svg" \
             "$MIME_DIR/mosaic.xml"; do
        if [ -e "$f" ]; then rm -f "$f"; echo "removed $f"; removed=1; fi
    done
    [ "$removed" = 1 ] || echo "nothing to remove under $DATA_HOME"
    refresh_caches
    exit 0
fi

# ---- work out what to point Exec= at -------------------------------------------------------------
# $APPIMAGE is the path of the .AppImage FILE (what the user must launch); $APPDIR is the mounted
# read-only tree inside it (where the icon lives). Both are set by the AppImage runtime.
if [ -z "$EXEC" ]; then
    if [ -n "${APPIMAGE:-}" ]; then
        EXEC=$APPIMAGE
    else
        echo "no --exec given and \$APPIMAGE is unset: run this from inside the AppImage, or pass" >&2
        echo "  --exec /path/to/Mosaic.AppImage   (or the installed 'mosaic' binary)" >&2
        exit 2
    fi
fi
case "$EXEC" in /*) ;; *) EXEC=$(CDPATH= cd -- "$(dirname -- "$EXEC")" && pwd)/$(basename -- "$EXEC") ;; esac
[ -e "$EXEC" ] || { echo "no such file: $EXEC" >&2; exit 1; }

if [ -z "$ICON" ]; then
    for cand in \
        "${APPDIR:-}/usr/share/icons/hicolor/scalable/apps/$APP_ID.svg" \
        "${APPDIR:-}/$APP_ID.svg" \
        "$HERE/../../../assets/app_icon.svg"; do
        if [ -n "$cand" ] && [ -f "$cand" ]; then ICON=$cand; break; fi
    done
fi
[ -n "$ICON" ] && [ -f "$ICON" ] || { echo "could not find an icon; pass --icon <file.svg>" >&2; exit 1; }

# ---- write ---------------------------------------------------------------------------------------
mkdir -p "$APPS_DIR" "$ICON_DIR" "$MIME_DIR"
cp -f "$ICON" "$ICON_DIR/$APP_ID.svg"

# ⚠ Exec= IS A COMMAND LINE, NOT A PATH: the spec splits it on spaces, so an AppImage sitting in
# "~/My Apps/Mosaic 0.3.1.AppImage" would run `/home/you/My` with two stray arguments. It is quoted
# and escaped per the Desktop Entry spec's quoting rules. Note that `desktop-file-validate` does NOT
# catch the unquoted form -- it validates happily and then fails to launch, so do not use it as the
# check here. TryExec is deliberately left RAW: it is specified as a path, not a command line, and
# the implementations that read it (GLib's g_find_program_in_path among them) take it literally.
exec_quote() {
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/`/\\`/g' -e 's/\$/\\$/g'
}

# TryExec= is what makes a DELETED AppImage degrade gracefully: desktops hide an entry whose TryExec
# does not resolve, instead of offering a launcher that fails. %f, not %F: Mosaic opens one document
# per invocation. StartupWMClass must equal the app_id for the window to group under this entry.
cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=Mosaic
GenericName=Image Editor
Comment=A professional, GPU-accelerated image editor
TryExec=$EXEC
Exec="$(exec_quote "$EXEC")" %f
Icon=$APP_ID
Terminal=false
Categories=Graphics;2DGraphics;RasterGraphics;
MimeType=image/x-mosaic;image/png;image/jpeg;
StartupNotify=true
StartupWMClass=$APP_ID
EOF
chmod 644 "$DESKTOP_FILE"

# The .mosaic document type + its icon, when they can be found -- optional, and only worth doing
# from a real AppDir where both files exist.
for src_dst in \
    "${APPDIR:-}/usr/share/mime/packages/mosaic.xml:$MIME_DIR/mosaic.xml" \
    "${APPDIR:-}/usr/share/icons/hicolor/scalable/mimetypes/image-x-mosaic.svg:$ICON_DIR/image-x-mosaic.svg"; do
    src=${src_dst%%:*}; dst=${src_dst##*:}
    [ -n "${APPDIR:-}" ] && [ -f "$src" ] && cp -f "$src" "$dst" || true
done

refresh_caches

echo "installed:"
echo "  $DESKTOP_FILE"
echo "  $ICON_DIR/$APP_ID.svg"
[ -f "$MIME_DIR/mosaic.xml" ] && echo "  $MIME_DIR/mosaic.xml"
echo
echo "Exec=$EXEC"
echo "Undo with: $0 --uninstall"
