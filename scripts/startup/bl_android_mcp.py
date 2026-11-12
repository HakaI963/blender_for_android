# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""
Android: keep the bundled MCP bridge add-on enabled.

The MCP bridge (``blender_mcp_addon``) ships inside the APK under
``scripts/addons_core`` and starts its socket server on Blender start
(auto-start is its default). Without this module it would still sit in
Preferences -> Add-ons asking to be enabled, because add-ons bundled in
``addons_core`` ship *disabled*: the enabled set lives in the user
preferences, and a fresh install has none.

Seeding the preferences here is what makes it always active. ``register()``
runs during the ``bpy`` import, which happens before add-ons are enabled
(``bpy.utils.load_scripts_extensions`` -> ``addon_utils._initialize_once``),
so the entry is already in place when that pass reads the enabled set, and
the add-on takes the exact same enable path a manual click would have taken.

Android-only, following the precedent of ``bl_android_browser.py``: CPython
marks itself with ``sys.getandroidapilevel`` there and nowhere else, so the
same check gates this. Desktop builds keep stock Blender behaviour.
"""

import sys

__all__ = (
    "register",
    "unregister",
)

# Module name of the bundled add-on: ``scripts/addons_core/blender_mcp_addon``.
_MCP_MODULE = "blender_mcp_addon"


def register():
    if not hasattr(sys, "getandroidapilevel"):
        return

    import bpy

    addons = bpy.context.preferences.addons
    if _MCP_MODULE in addons:
        # Already enabled in the preferences: nothing to do. The standard
        # startup pass picks it up from here.
        return

    # Same thing `addon_utils._addon_ensure()` does: create the preferences
    # entry without enabling the add-on yet, so the regular enable path
    # (which also registers classes and the CLI command, in the right order)
    # is the one that loads it.
    addon = addons.new()
    addon.module = _MCP_MODULE
    print("[blender-android] MCP bridge add-on enabled by default")


def unregister():
    # Left enabled on purpose: the add-on's own unregister() runs when
    # Blender disables it, and this module's job is only to make sure the
    # *next* start finds it enabled again.
    pass
