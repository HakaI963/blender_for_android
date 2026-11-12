/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * NativeActivity entry point. Android owns the frame loop, so we invert
 * Blender's main loop: run init once, then WM_main_loop_body per frame.
 *
 * Since the export-freeze fix the two jobs run on two threads:
 *
 * - The activity's main thread keeps pumping the Android looper, exactly where
 *   Android expects input to be consumed. It never runs Blender code, so a long
 *   export (a synchronous glTF/USD write, a Cycles render, an MCP-bridge tool
 *   call) can no longer stall the main thread: input keeps being consumed and
 *   queued instead, and Android's "isn't responding" watchdog -- which counts
 *   the seconds until the window's input channel is drained -- never fires.
 *   Before, everything ran on this one thread and any operation longer than
 *   five seconds froze the app into an ANR dialog while it worked.
 *
 * - A dedicated Blender thread runs init and then the frame loop
 *   (WM_main_loop_body), which is where events are dispatched, drawn and
 *   presented -- the same work as before, just off the thread Android watches.
 *
 * The two sides share the GHOST event stack, which is mutex-protected for
 * exactly this reason (GHOST_EventManager), and the surface/focus commands the
 * activity thread receives: those only flag *what* happened
 * (GHOST_SystemAndroid::processPendingWindowCommand) and are carried out by the
 * frame thread between frames, where rebuilding the Vulkan surface cannot race
 * the frame being drawn.
 */

#include "GHOST_ISystem.hh"
#include "GHOST_SystemAndroid.hh"

#include <android/log.h>
#include <android_native_app_glue.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <jni.h>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

/* Route Blender's stdout/stderr to logcat (tag "blender") so init/errors are
 * visible; NativeActivity otherwise discards them. */
static int g_stdio_pipe[2];
static void *ghost_android_stdio_thread(void * /*arg*/)
{
  char line[1024];
  ssize_t count;
  while ((count = read(g_stdio_pipe[0], line, sizeof(line) - 1)) > 0) {
    if (line[count - 1] == '\n') {
      count--;
    }
    line[count] = '\0';
    __android_log_write(ANDROID_LOG_INFO, "blender", line);
  }
  return nullptr;
}
static void ghost_android_redirect_stdio()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  if (pipe(g_stdio_pipe) != 0) {
    return;
  }
  dup2(g_stdio_pipe[1], STDOUT_FILENO);
  dup2(g_stdio_pipe[1], STDERR_FILENO);
  pthread_t thread;
  if (pthread_create(&thread, nullptr, ghost_android_stdio_thread, nullptr) == 0) {
    pthread_detach(thread);
  }
}

namespace blender {
struct bContext;
/* Implemented in creator: runs Blender init + WM_main_entry, hands C back. */
int GHOST_android_launch(int argc, const char **argv);
void WM_main_loop_body(bContext *C);
}  // namespace blender

static blender::bContext *g_context = nullptr;
static std::atomic<bool> g_blender_launched{false};

namespace blender {
/* Called by the creator once init finished, to hand the context to the loop. */
void GHOST_androidfinalize(bContext *C)
{
  g_context = C;
}
}  // namespace blender

static GHOST_SystemAndroid *android_system()
{
  return static_cast<GHOST_SystemAndroid *>(GHOST_ISystem::getSystem());
}

static GHOST_SystemAndroid *android_system_if_ready()
{
  return GHOST_ISystem::getSystem() ? android_system() : nullptr;
}

/* Read optional launch arguments from <internalDataPath>/blender_args.txt.
 * Returns an empty vector when the file is absent, which keeps the built-in defaults. */
static void ghost_android_read_args_file(const char *dir, std::vector<std::string> &args)
{
  if (dir == nullptr) {
    return;
  }
  const std::string path = std::string(dir) + "/blender_args.txt";
  FILE *file = fopen(path.c_str(), "r");
  if (file == nullptr) {
    return;
  }
  const size_t count_before = args.size();
  char token[512];
  while (fscanf(file, "%511s", token) == 1) {
    args.push_back(token);
  }
  fclose(file);
  __android_log_print(ANDROID_LOG_INFO,
                      "blender",
                      "[BlenderAndroid] %zu launch argument(s) from %s",
                      args.size() - count_before,
                      path.c_str());
}

/* Launch arguments from `blender_args.txt`, read from the app's internal files directory and then
 * from its external one, `/sdcard/Android/data/org.blender.blender/files`.
 *
 * The external one is the debug mode's way in (`build_files/android/debug.sh on ...`): `adb` can
 * write there on any build, where the internal directory needs a debuggable APK and `run-as`. No
 * other app can, scoped storage keeps every app out of the others' directories, and only
 * arguments are read from it. Returns an empty vector when neither file exists, which keeps the
 * built-in defaults. */
static std::vector<std::string> ghost_android_read_launch_args(android_app *app)
{
  std::vector<std::string> args;
  if (app == nullptr || app->activity == nullptr) {
    return args;
  }
  ghost_android_read_args_file(app->activity->internalDataPath, args);
  ghost_android_read_args_file(app->activity->externalDataPath, args);
  return args;
}

/* Set when the activity is being destroyed; the frame thread checks it each
 * iteration so it stops driving Blender. */
static std::atomic<bool> g_stop_frame_loop{false};

/* Everything the frame thread needs, collected on the activity thread where the
 * looper (and so the launch arguments) is available. */
struct GhostAndroidFrameArgs {
  android_app *app;
  std::vector<std::string> file_args;
  std::string open_file;
};

/* The Blender half: init once, then a frame per iteration.
 *
 * Runs on its own thread for as long as the app lives. Everything it touches --
 * GL context, Vulkan device, Python, the depsgraph -- is created and used here,
 * and the activity thread never calls into Blender, which is what keeps the
 * input channel responsive during long operations. */
static void *ghost_android_frame_thread(void *arg)
{
  auto *args = static_cast<GhostAndroidFrameArgs *>(arg);
  android_app *app = args->app;

  /* A .blend the system asked us to open, resolved to a path by BlenderActivity
   * and left in the environment before the native thread started. Passed as an
   * argument rather than opened afterwards, so it is the file that loads instead
   * of the startup file loading and being replaced -- the same route a
   * double-clicked file takes into argv[1] on macOS. Blender's own fall-through
   * argument handler (main_args_handle_load_file) takes it from here.
   *
   * Copied out rather than used in place: argv holds borrowed pointers and the
   * unsetenv below would invalidate what getenv returned. */
  std::vector<const char *> argv;
  argv.push_back("blender");
  for (const std::string &arg : args->file_args) {
    argv.push_back(arg.c_str());
  }
  argv.push_back("--disable-crash-handler");
  if (!args->open_file.empty()) {
    argv.push_back(args->open_file.c_str());
  }
  for (const char *arg : argv) {
    __android_log_print(ANDROID_LOG_INFO, "blender", "[BlenderAndroid] argv: %s", arg);
  }

  blender::GHOST_android_launch(int(argv.size()), argv.data());

  /* One launch only. A later tap arrives through onNewIntent instead, and must
   * not find a stale path here. */
  unsetenv("BLENDER_ANDROID_OPEN_FILE");
  delete args;

  blender::bContext *context = g_context;
  g_blender_launched.store(true);
  if (context == nullptr) {
    /* Init failed before handing a context back; WM_exit already ran and the
     * process is on its way out. */
    return nullptr;
  }

  GHOST_SystemAndroid *system = android_system();
  if (system == nullptr) {
    return nullptr;
  }

  fprintf(stderr, "[BlenderAndroid] creator: event loop ready\n");
  fflush(stderr);

  while (!g_stop_frame_loop.load(std::memory_order_relaxed)) {
    /* Surface create/resize/destroy and focus changes wait here until they can
     * run between frames -- see processPendingWindowCommand(). */
    system->processPendingWindowCommand();
    blender::WM_main_loop_body(context);
  }
  return nullptr;
}

/* True once the frame thread exists. */
static std::atomic<bool> g_frame_thread_started{false};

static void ghost_android_start_frame_thread(android_app *app)
{
  if (g_frame_thread_started.exchange(true)) {
    return;
  }
  auto *args = new GhostAndroidFrameArgs();
  args->app = app;
  args->file_args = ghost_android_read_launch_args(app);
  if (const char *env = getenv("BLENDER_ANDROID_OPEN_FILE")) {
    args->open_file = env;
  }
  pthread_t thread;
  if (pthread_create(&thread, nullptr, ghost_android_frame_thread, args) != 0) {
    /* Nothing else can drive Blender; without the thread the app is dead. */
    __android_log_print(ANDROID_LOG_ERROR, "blender", "[BlenderAndroid] frame thread failed");
    g_frame_thread_started.store(false);
    delete args;
    return;
  }
  pthread_detach(thread);
}

/* Milliseconds the activity thread sleeps in ALooper_pollOnce between events.
 * Caps how long the timed touch checks (long-press, hover-exit) wait, and is far
 * below anything the input watchdog measures. */
static constexpr int GHOST_ANDROID_POLL_MS = 16;

static void on_app_cmd(android_app *app, int32_t cmd)
{
  switch (cmd) {
    case APP_CMD_GAINED_FOCUS:
      if (GHOST_SystemAndroid *system = android_system_if_ready()) {
        system->handleWindowFocus(true);
      }
      break;

    case APP_CMD_LOST_FOCUS:
      if (GHOST_SystemAndroid *system = android_system_if_ready()) {
        system->handleWindowFocus(false);
      }
      break;

    case APP_CMD_INIT_WINDOW:
      /* Launch arguments come from blender_args.txt, whitespace separated, when that file
       * exists. A driver quirk on one device is diagnosed by toggling flags such as
       * --debug-gpu-force-workarounds or --log while watching logcat, and having to rebuild and
       * reinstall a 170 MB APK for each attempt makes that loop useless.
       *
       * Nothing is passed by default beyond --disable-crash-handler. There used to be a
       * --debug-gpu-force-workarounds here, added while bringing the port up to get past a
       * driver that refused compute
       * pipelines. That turned out to be the SPIR-V version the shaders were emitted as, which
       * is fixed at the source now, and the flag was making the viewport pay for it: taking the
       * forced path skips feature detection entirely and leaves dynamic rendering local read
       * off -- the one extension Blender enables specifically for Qualcomm, because reading
       * attachments from tile memory is what a deferred renderer needs on a tiler. Measured on
       * an S24 Ultra with a 292k vertex scene, turning it back on is worth 5-18% of the frame
       * while orbiting.
       *
       * The flag is still available through blender_args.txt when a driver needs it.
       *
       * Blender used to start right here, on this thread. It now starts on its own thread (see
       * ghost_android_frame_thread) so this thread stays free to consume input forever after;
       * every later INIT_WINDOW is the surface coming back after TERM_WINDOW. */
      if (!g_frame_thread_started.load()) {
        /* First window: the frame thread starts Blender, which creates the window. */
        ghost_android_start_frame_thread(app);
      }
      else if (GHOST_SystemAndroid *system = android_system_if_ready()) {
        /* Reborn surface: rebuild for it between frames. */
        system->handleNativeWindowInit(app);
      }
      break;

    /* Rotation. The activity declares orientation and screenSize in
     * configChanges, so it is not recreated: the surface is resized under it
     * and these are the only notice we get. CONFIG_CHANGED arrives for the
     * orientation switch itself and WINDOW_RESIZED once the surface follows;
     * both funnel to the same place, which is idempotent. */
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
      if (GHOST_SystemAndroid *system = android_system_if_ready()) {
        system->handleNativeWindowResize();
      }
      break;

    case APP_CMD_TERM_WINDOW:
      if (GHOST_SystemAndroid *system = android_system_if_ready()) {
        system->handleNativeWindowTerm();
      }
      break;

    case APP_CMD_DESTROY:
      /* Tell the frame thread to stop driving Blender; the process is going
       * away either way (WM_exit exits, and the activity is finishing). */
      g_stop_frame_loop.store(true);
      break;

    default:
      break;
  }
}

static int32_t on_input_event(android_app * /*app*/, AInputEvent *event)
{
  if (!GHOST_ISystem::getSystem()) {
    return 0;
  }
  return android_system()->handleInputEvent(event);
}

/* Soft-keyboard text/keys from the Java InputConnection (BlenderActivity). */
extern "C" JNIEXPORT void JNICALL Java_org_blender_blender_BlenderActivity_nativeOnCommitText(
    JNIEnv *env, jobject /*thiz*/, jstring text)
{
  GHOST_SystemAndroid *system = android_system_if_ready();
  if (!system || !text) {
    return;
  }
  const char *utf = env->GetStringUTFChars(text, nullptr);
  system->handleTextInput(utf);
  env->ReleaseStringUTFChars(text, utf);
}

extern "C" JNIEXPORT void JNICALL Java_org_blender_blender_BlenderActivity_nativeOnKey(
    JNIEnv * /*env*/,
    jobject /*thiz*/,
    jint keycode,
    jint action,
    jint meta_state,
    jint unicode,
    jboolean repeat)
{
  if (GHOST_SystemAndroid *system = android_system_if_ready()) {
    system->handleJavaKeyEvent(keycode, action, meta_state, unicode, repeat != JNI_FALSE);
  }
}

/* A .blend tapped in a file manager while Blender is already running, forwarded from
 * BlenderActivity.onNewIntent on the Android UI thread. */
extern "C" JNIEXPORT void JNICALL Java_org_blender_blender_BlenderActivity_nativeOpenMainFile(
    JNIEnv *env, jobject /*thiz*/, jstring path)
{
  GHOST_SystemAndroid *system = android_system_if_ready();
  if (!system || !path) {
    return;
  }
  const char *utf = env->GetStringUTFChars(path, nullptr);
  system->handleOpenMainFile(utf);
  env->ReleaseStringUTFChars(path, utf);
}

extern "C" void android_main(struct android_app *app)
{
  ghost_android_redirect_stdio();
  GHOST_SystemAndroid::setAndroidApp(app);
  app->onAppCmd = on_app_cmd;
  app->onInputEvent = on_input_event;

  /* The activity thread. It owns the Android looper, so it must keep polling it
   * for as long as the app lives: input events are only "consumed" (and Android
   * only considers the window responsive) when this thread reads them. Everything
   * it does with an event is queue it for the frame thread or flag it, none of
   * which can block. */
  while (!app->destroyRequested) {
    int events = 0;
    /* Reset each iteration: ALooper_pollOnce only fills this in when it has
     * something to deliver, and a stale value would replay one event twice. */
    android_poll_source *source = nullptr;
    if (ALooper_pollOnce(GHOST_ANDROID_POLL_MS, nullptr, &events, (void **)&source) < 0) {
      /* ALOOPER_POLL_ERROR / wake: nothing registered to wait on yet. Yield
       * instead of spinning, the activity registers its fds shortly. */
      usleep(1000);
      continue;
    }
    if (source) {
      source->process(app, source);
      if (app->destroyRequested) {
        break;
      }
    }
    /* A finger held still emits no events, so the long-press is timed here. Runs
     * on this thread because that is where the touch state it reads is written. */
    if (GHOST_SystemAndroid *system = android_system_if_ready()) {
      system->checkInputTimers();
    }
  }

  /* The frame thread notices g_stop_frame_loop and returns; the process is on
   * its way out either way. */
}
