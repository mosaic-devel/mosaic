#!/usr/bin/env python3
"""Author a .DS_Store for the Mosaic .dmg (PLAN.md S59): Finder window size, icon-view mode with a
background image, large icons, and the app / Applications icon positions -- the drag-to-Applications
layout. Runs on Linux (no Mac) via ds_store + mac_alias.

THE BACKGROUND ALIAS is the load-bearing, undocumented part. Hand-building one from VolumeInfo/
TargetInfo (mac_alias.Alias.for_file needs a live macOS volume) yields an incomplete record --
missing the disk-image-alias chain and the HFS `carbon_path` -- and a malformed alias makes Finder
discard the ENTIRE `icvp`, so NEITHER the background NOR the icon size takes. The fix (Bitcoin Core's
`custom_dsstore.py`, MIT) is to start from a REAL, Mac-recorded alias blob, parse it, and swap in the
volume name + carbon paths. Finder resolves it by volume name + relative path once the DMG mounts;
the baked disk-image path is a hint that fails gracefully. Background must be `.background/background.tiff`.

Usage:
  dmg_dsstore.py <dsstore_out> <volume_name> <win_w> <win_h> <icon_size> \
                 <app_x> <app_y> <apps_x> <apps_y>
"""
import base64
import sys

from ds_store import DSStore
from mac_alias import Alias

# A real background.tiff-in-a-mounted-DMG alias, recorded on macOS (from Bitcoin Core's
# custom_dsstore.py, MIT). We only swap the volume name + carbon paths below. Whitespace is stripped
# before decoding, so the wrapping below can't corrupt the blob (base64 must align to 4-char groups).
_ALIAS_TEMPLATE_B64 = """
AAAAAAIeAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADRlFywSCsABQAAAJgPYmFja2dy
b3VuZC50aWZmAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
AAAAmdE5sPgAAAAAAAAAAP////8AAA0CAAAAAAAAAAAAAAAAAAAACy5iYWNrZ3JvdW5kAAAQAAgA
ANGUXLAAAAARAAgAANE5sPgAAAABAAQAAACYAA4AIAAPAGIAYQBjAGsAZwByAG8AdQBuAGQALgB0
AGkAZgBmAA8AAgAAABIAHC8uYmFja2dyb3VuZC9iYWNrZ3JvdW5kLnRpZmYAFAEGAAAAAAEGAAIA
AAxNYWNpbnRvc2ggSEQAAAAAAAAAAAAAAAAAAADOl6vDSCsAAAGIW4gAAAAAAAAAAAAAAAAAAAAA
AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAnWrjdGUXLBk
ZXZyZGRza/////8AAAkgAAAAAAAAAAAAAAAAAAAAB2JpdGNvaW4AABAACAAAzperwwAAABEACAAA
0ZRcsAAAAAEAFAGIW4gAFqkJAAj6UgAI+lEAAmSOAA4AAgAAAA8AGgAMAE0AYQBjAGkAbgB0AG8A
cwBoACAASABEABMAAS8AABUAAgAU//8AAP//AAA=
"""
ALIAS_TEMPLATE = base64.b64decode("".join(_ALIAS_TEMPLATE_B64.split()))


def background_alias(volume_name):
    a = Alias.from_bytes(ALIAS_TEMPLATE)
    a.volume.name = volume_name
    a.volume.posix_path = "/Volumes/" + volume_name
    # The disk-image-alias hint (where the .dmg file lives) never resolves on the user's machine;
    # Finder falls back to volume-name + carbon_path. Give it plausible, harmless values.
    a.volume.disk_image_alias.target.filename = volume_name + ".temp.dmg"
    a.volume.disk_image_alias.target.carbon_path = "Macintosh HD:Users:\x00user:\x00" + volume_name + ".temp.dmg"
    a.volume.disk_image_alias.target.posix_path = "Users/user/" + volume_name + ".temp.dmg"
    # The one that actually resolves: <Volume>:.background:background.tiff (HFS colon path).
    a.target.carbon_path = volume_name + ":.background:\x00background.tiff"
    return a.to_bytes()


def main():
    (dsstore, vol, win_w, win_h, icon_size,
     app_x, app_y, apps_x, apps_y) = sys.argv[1:10]
    win_w, win_h, icon_size = int(win_w), int(win_h), int(icon_size)
    app_x, app_y, apps_x, apps_y = int(app_x), int(app_y), int(apps_x), int(apps_y)

    with DSStore.open(dsstore, "w+") as ds:
        # Window: top-left (100,100); height + ~28 for the title bar so the content area == the
        # background image size. Chrome hidden.
        ds["."]["bwsp"] = {
            "WindowBounds": "{{100, 100}, {%d, %d}}" % (win_w, win_h + 28),
            "ShowStatusBar": False,
            "ShowToolbar": False,
            "ShowPathbar": False,
            "ShowSidebar": False,
            "ShowTabView": False,
            "SidebarWidth": 0,
        }
        ds["."]["icvp"] = {
            "viewOptionsVersion": 1,
            "backgroundType": 2,                      # 2 = picture
            "backgroundImageAlias": background_alias(vol),
            "backgroundColorRed": 1.0,
            "backgroundColorGreen": 1.0,
            "backgroundColorBlue": 1.0,
            "iconSize": float(icon_size),
            "gridSpacing": 100.0,
            "gridOffsetX": 0.0,
            "gridOffsetY": 0.0,
            "textSize": 12.0,
            "arrangeBy": "none",
            "labelOnBottom": True,
            "showIconPreview": False,
            "showItemInfo": False,
            "scrollPositionX": 0.0,
            "scrollPositionY": 0.0,
        }
        ds["."]["vSrn"] = ("long", 1)                 # view serial -- Finder wants it present
        ds["Mosaic.app"]["Iloc"] = (app_x, app_y)
        ds["Applications"]["Iloc"] = (apps_x, apps_y)

    print(f"wrote {dsstore} (vol '{vol}', {win_w}x{win_h}, icon {icon_size})")


if __name__ == "__main__":
    sys.exit(main())
