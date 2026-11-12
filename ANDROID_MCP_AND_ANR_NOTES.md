# MCP bridge + export-freeze fix + CI APK builds

Five related changes to this fork, all requested together:

1. The Blender Lab **MCP bridge add-on** (v1.0.3) is bundled in the APK, enabled
   by default, and its network permission is permanently available.
2. The **export freeze** (Android's "Blender isn't responding" dialog while a
   long operation runs) is fixed by moving Blender's frame loop off the activity
   thread.
3. The APK is built by **GitHub Actions** ([.github/workflows/android-apk.yml](.github/workflows/android-apk.yml)),
   so no local gradle/NDK run is needed to produce an APK.
4. The app runs **landscape** and **stays open in the background**.
5. The bundled bridge add-on is fixed up for the realities of a phone:
   frozen processes, port conflicts, a writable temp directory, and the
   `bl_info` warning.

## 1. The MCP add-on, always active

`scripts/addons_core/blender_mcp_addon/` holds the add-on, unmodified except for
one Android-specific change (below). It is a socket bridge: it binds
`localhost:9876` and executes the Python an MCP client (an LLM tool server) sends,
returning JSON. Long operations are expected to use the deferred
`check_is_finished` convention so the response comes back on a later timer tick
instead of blocking the request.

Two Android-only files carry it into the app:

- `scripts/startup/bl_android_mcp.py` — a startup module in the pattern of
  `bl_android_browser.py` (same `sys.getandroidapilevel` guard). It seeds the
  user preferences with the add-on entry before the add-on enable pass runs
  (`bpy` import happens before `load_scripts_extensions`), so the add-on takes
  the ordinary enable path on every launch. Nothing has to be clicked in
  Preferences; it is simply always on. Disabling it in Preferences sticks for
  that session and is re-enabled on the next start — this is the "always active"
  fork behaviour, not an accident.
- `blender_mcp_addon/__init__.py` — `startup_online_ok_or_error()` answers True on
  Android. The add-on only ever binds a loopback socket; `bpy.app.online_access`
  is about outbound internet access and there is no per-app switch an Android
  build could ask the user to flip, so the check only ever produced "Online access
  must be enabled in the system preferences" on a device where nothing could be
  enabled. Desktop builds keep the upstream check.

The add-on auto-starts its server on Blender start (`use_autostart` defaults to
True, one second after startup) — the same thing the manual Preferences button
does, without the button.

## 2. Why the export froze, and what changed

Android's ANR watchdog fires when the app's main thread stops consuming input for
five seconds, whatever the app is otherwise doing. In this port everything ran on
that one thread: init, the frame loop, every operator. An export (a synchronous
glTF/USD write, a render issued with `EXEC_DEFAULT`, an MCP tool call that runs
`bpy.ops` inline) is one long `WM_main_loop_body`, so the Android looper was not
polled for the duration of the operation — the input channel went unconsumed,
the system put up "Blender isn't responding", and the dialog only went away once
the export finished. Nothing was broken; the watchdog simply could not tell that
from a real hang.

The fix is the standard NativeActivity layout (what games do): the activity
thread keeps owning the looper forever and only queues work, while a dedicated
**frame thread** runs `GHOST_android_launch` and then the frame loop.

- `intern/ghost/intern/GHOST_AndroidMain.cc` — split described at the top of the
  file. The activity thread now blocks in `ALooper_pollOnce(16ms)`: input events
  are consumed immediately (and queued into GHOST), the timed touch checks run
  there too (they touch the state the input handlers write), and surface/focus
  commands are only *flagged*. The frame thread runs init plus
  `processPendingWindowCommand()` + `WM_main_loop_body()` per iteration — the
  same work, on a thread Android does not watch.
- `intern/ghost/intern/GHOST_EventManager.{hh,cc}` — the event stack gained a
  mutex. It is now written from two threads (activity thread pushes, frame thread
  pops). Dispatch happens with the lock released, because a consumer can push the
  next event while handling one.
- `intern/ghost/intern/GHOST_SystemAndroid.{hh,cc}` — `handleNativeWindowInit /
  Resize / Term` and `handleWindowFocus` record an atomic pending command; the
  new `processPendingWindowCommand()` performs it at the top of a frame-loop
  iteration, which is exactly the point in the frame where the glue used to run it
  inline — and the only safe one, since `setNativeWindow()` rebuilds the Vulkan
  surface and must not race the frame being drawn. The timed touch checks moved
  to `checkInputTimers()` on the activity thread (same reasoning: state locality).
  `meta_state_` is atomic because `getModifierKeys()` reads it from the frame
  thread.

Behaviour preserved on purpose: the frame loop is still unthrottled (paced by the
swap, as the looper-spin it replaces was), rotation/background/foreground surface
handling happens at the same point in the frame, and `WM_exit` still ends the
process (it calls `exit()`), now from the frame thread.

## 3. CI builds

`android-apk.yml`, two jobs, both on `ubuntu-24.04`:

- **Dependencies** — host toolchain (GCC 14+ via the toolchain PPA if the image
  lacks one), host Python 3.13 (deadsnakes; the numpy cross-build needs the
  target's version), the `lib/linux_x64` submodule for the host code generators,
  then `deps/build.sh all`. The prefix is cached per run.
- **Blender + APK** — restores that cache (or rebuilds it on a cache miss),
  installs the pinned NDK `28.2.13676358` + platform 35 + build-tools 35.0.1,
  runs `build.py <config> --repackage` (repackage is required for the add-on to
  reach the payload, and it also rebuilds the config-matched host generators),
  and uploads `blender-<config>.apk` as an artifact — the same debug-signed
  development APK a local build produces, installable with `adb install -r`.

Triggered by a push to `main` or manually from the Actions tab (`full` or `lite`).
Each job is capped at 350 minutes; if a run is cut short, the deps cache of that
run is simply not written and the next run starts clean.

## 4. Landscape, and staying open in the background

Two separate causes, both in the activity, not in Blender.

**Portrait.** The manifest declared `android:screenOrientation="user"`, so the
device's own rotation setting decided the orientation, and on a phone held
upright that is portrait. `setRequestedOrientation(LANDSCAPE)` in
`BlenderActivity.onCreate` loses either way: it runs after the native side has
seen a portrait window, and a manifest `user` leaves the app's own request
subordinate to the device setting. Fixed in the manifest
(`sensorLandscape`, applied before any surface exists, so the window is created
landscape from the first frame), with the Java call matched to it. Landscape in
either direction; a rotation lock is still respected, and it can never come out
in portrait.

**Background.** An activity that is recreated is not a display hiccup here: it
runs `ANativeActivity_onCreate` again, which is a second `android_main` and a
second Blender init inside the one process — the app visibly restarts, and that
is what "does not stay open" is. Two halves:

- The manifest's `configChanges` now also lists the settings that change while an
  app sits in the background (locale, layout direction, font scale, color mode,
  font weight, grammatical gender, smallest screen size). A change in any of
  them used to recreate the activity, and with it the app.
- The frame loop itself idles while there is no surface. Backgrounding destroys
  the surface (TERM_WINDOW), and without this the loop pinned a core on the
  largest process on the device — a background app at 100% CPU is first in line
  for the low-memory killer. It now sleeps 8 ms per iteration instead: timers
  (the MCP bridge's poll among them) keep running, so a backgrounded Blender
  still answers its client, but the process is quiet. `hasValidWindow()` on
  GHOST_SystemAndroid is the signal, and the Vulkan side already expected this
  (`recreateSwapchain` returns early with no surface: "Android window
  backgrounded").

Also fixed on the way: a reborn surface that arrives before the window exists is
no longer dropped (the pending command retries) — that race is what left a black
window after a quick app-switch during startup.

## 5. Making the bundled bridge usable on a phone

Five problems found while driving the port from a PC-side MCP client, and what
changed for each.

**Port conflict, silently skipped.** `_autostart_timer` used to catch the bind
failure and store it in a string only the Preferences panel shows, so an
auto-start that did nothing left no trace anywhere. Now the failure is printed
to the console (logcat, tag `blender`), and a new preference pair --
*Port Fallback* / *Fallback Range* -- makes it try the next free port above the
preferred one and log which port it actually took. Two bridges can therefore
coexist: whichever held the preferred port keeps it, and the other reports its
own. With fallback off the original error is still printed rather than dropped.

**A frozen process answers nothing.** Android moves a backgrounded app into a
cpuset with no CPU. The listening socket stays open, so a client connects,
sends its request and waits forever; what it sees is a timeout, or a "socket
closed" it reads as a stuck add-on thread. The bridge now measures the gap
between polls and treats one that no timer interval can produce -- with no tool
code running in between, so a long render is not mistaken for a freeze -- as the
process having been frozen. It closes the listening socket at that point (clients
get connection-refused instead of hanging) and rebinds on the first normal tick,
so the bridge comes back by itself on resume. The timer that drives it is no
longer unregistered by a pause, which is what made the old code unable to come
back at all.

**No writable temp directory.** `/tmp` is not writable by an app uid on Android,
and SELinux answers on policy rather than mode bits: a screenshot written to
`/tmp` raises PermissionError however permissive the directory looks, with no
graceful fallback. On Android the add-on now redirects `TMPDIR`/`TEMP`/`TMP` and
`tempfile.tempdir` into the app's own data directory (`resource_path('LOCAL')`
+ `tmp`, write-tested before it is trusted). Tool code should write scratch files
through `tempfile` or `bpy.utils.resource_path('LOCAL')`, never a literal
`/tmp` path. Does nothing on any other platform, and never fails the add-on.

**`bl_info` warning spam.** The add-on's metadata lives in its manifest, which
only the extensions platform reads; sitting in the legacy `addons_core` folder
it is scanned by `addon_utils`, which logs "add-on missing 'bl_info'" on every
refresh. A literal `bl_info` dict is now declared, silencing it and naming the
Preferences entry "MCP".

**Client-side notes for whoever writes the tool code** (not add-on changes, but
this is where they cost time):

- A long operation must use the add-on's deferred convention: start the job with
  `INVOKE_DEFAULT`, define `check_is_finished` returning `None` while it runs,
  and the response is sent on a later timer tick. A synchronous
  `EXEC_DEFAULT` export or render still blocks the frame loop -- the app no
  longer trips Android's watchdog for it, but the UI does not update until the
  operation finishes. The 1.6 s glTF export and the ~20 s shaded capture are
  both cases of the same thing.
- In Blender 5.3 it is `bpy.ops.export_scene.gltf(export_format='GLB', ...)`;
  there is no `export_scene.glb`.
- `MeshEdge.is_boundary` was removed: count boundary edges with
  `bmesh` (`edge.link_faces`), not the RNA attribute.
