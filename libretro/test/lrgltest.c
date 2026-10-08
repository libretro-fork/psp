/* Headless OpenGL frontend for the core's emu thread paths.
 * Creates a surfaceless EGL context (Mesa llvmpipe is enough), loads content
 * on the GL backend and runs it, going through everything that pauses or stops
 * the emu thread on the way: save states, an option update, a context loss
 * and reset, retro_reset and unload. Prints a digest of the frames it read
 * back; a hang is the failure this mostly looks for. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "libretro.h"

static struct retro_hw_render_callback hw;
static bool hw_set;
static bool update_pending;
static bool shutdown_requested;
static const char *system_dir;
static const char *save_dir;
static unsigned fb_w = 480, fb_h = 272;
static GLuint fbo, color_rb, depth_rb;
static uint64_t video_digest = 1469598103934665603ULL;
static unsigned long frames_video, frames_dupe;
static unsigned long audio_frames;
static bool verbose;

static PFNGLGENFRAMEBUFFERSPROC p_glGenFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer;
static PFNGLGENRENDERBUFFERSPROC p_glGenRenderbuffers;
static PFNGLBINDRENDERBUFFERPROC p_glBindRenderbuffer;
static PFNGLRENDERBUFFERSTORAGEPROC p_glRenderbufferStorage;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC p_glFramebufferRenderbuffer;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC p_glCheckFramebufferStatus;
typedef void (*readpixels_t)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static readpixels_t p_glReadPixels;

static void fnv(uint64_t *h, const void *data, size_t len)
{
   const unsigned char *p = (const unsigned char *)data;
   size_t i;
   for (i = 0; i < len; i++)
   {
      *h ^= p[i];
      *h *= 1099511628211ULL;
   }
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (!verbose && level < RETRO_LOG_WARN)
      return;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static const char *option_value(const char *key)
{
   if (!strcmp(key, "ppsspp_backend"))
      return getenv("LRGL_BACKEND") ? getenv("LRGL_BACKEND") : "opengl";
   if (!strcmp(key, "ppsspp_internal_resolution"))
      return "480x272";
   return NULL;
}

static uintptr_t get_fb(void) { return fbo; }

static retro_proc_address_t get_proc(const char *sym)
{
   return (retro_proc_address_t)eglGetProcAddress(sym);
}

static bool environ_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback *)data)->log = core_log;
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
         *(const char **)data = system_dir;
         return true;
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char **)data = save_dir;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable *)data;
         var->value = option_value(var->key);
         return var->value != NULL;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool *)data = update_pending;
         update_pending = false;
         return true;
      case RETRO_ENVIRONMENT_SHUTDOWN:
         shutdown_requested = true;
         return true;
      case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
         *(unsigned *)data = getenv("LRGL_COMPAT") ? RETRO_HW_CONTEXT_OPENGL : RETRO_HW_CONTEXT_OPENGL_CORE;
         return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
      {
         struct retro_hw_render_callback *cb = (struct retro_hw_render_callback *)data;
         if (cb->context_type != RETRO_HW_CONTEXT_OPENGL && cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE)
            return false;
         cb->get_current_framebuffer = get_fb;
         cb->get_proc_address = get_proc;
         hw = *cb;
         hw_set = true;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
      case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
         return true;
      default:
         return false;
   }
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
   static unsigned char pixels[4096 * 4096 * 4];
   (void)pitch;
   if (data != RETRO_HW_FRAME_BUFFER_VALID)
   {
      frames_dupe++;
      return;
   }
   frames_video++;
   if (width > fb_w)
      width = fb_w;
   if (height > fb_h)
      height = fb_h;
   p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   p_glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   fnv(&video_digest, pixels, (size_t)width * height * 4);
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
   (void)data;
   audio_frames += frames;
   return frames;
}

static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; audio_frames++; }
static void input_poll_cb(void) { }
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) { return 0; }

static bool make_context(EGLDisplay *out_dpy)
{
   static const EGLint cfg_attr[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
      EGL_NONE
   };
   EGLint ctx_attr[16];
   int n = 0;
   EGLConfig cfg;
   EGLint ncfg = 0;
   EGLContext ctx;
   EGLDisplay dpy;
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

   dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL))
      return false;
   eglBindAPI(EGL_OPENGL_API);
   if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1)
      return false;
   if (hw.context_type == RETRO_HW_CONTEXT_OPENGL_CORE)
   {
      ctx_attr[n++] = EGL_CONTEXT_MAJOR_VERSION;
      ctx_attr[n++] = hw.version_major;
      ctx_attr[n++] = EGL_CONTEXT_MINOR_VERSION;
      ctx_attr[n++] = hw.version_minor;
      ctx_attr[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK;
      ctx_attr[n++] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;
   }
   ctx_attr[n] = EGL_NONE;
   ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
      return false;

#define LOAD(type, name) p_##name = (type)eglGetProcAddress(#name)
   LOAD(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers);
   LOAD(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
   LOAD(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers);
   LOAD(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer);
   LOAD(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage);
   LOAD(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer);
   LOAD(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus);
   LOAD(readpixels_t, glReadPixels);
#undef LOAD
   if (!p_glGenFramebuffers || !p_glReadPixels)
      return false;

   p_glGenFramebuffers(1, &fbo);
   p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   p_glGenRenderbuffers(1, &color_rb);
   p_glBindRenderbuffer(GL_RENDERBUFFER, color_rb);
   p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, fb_w, fb_h);
   p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rb);
   p_glGenRenderbuffers(1, &depth_rb);
   p_glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
   p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fb_w, fb_h);
   p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
   if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      return false;
   *out_dpy = dpy;
   return true;
}

#define SYM(name) do { *(void **)&p_##name = dlsym(h, #name); if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 2; } } while (0)

int main(int argc, char **argv)
{
   void (*p_retro_init)(void);
   void (*p_retro_deinit)(void);
   void (*p_retro_set_environment)(retro_environment_t);
   void (*p_retro_set_video_refresh)(retro_video_refresh_t);
   void (*p_retro_set_audio_sample)(retro_audio_sample_t);
   void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*p_retro_set_input_poll)(retro_input_poll_t);
   void (*p_retro_set_input_state)(retro_input_state_t);
   bool (*p_retro_load_game)(const struct retro_game_info *);
   void (*p_retro_unload_game)(void);
   void (*p_retro_run)(void);
   size_t (*p_retro_serialize_size)(void);
   bool (*p_retro_serialize)(void *, size_t);
   bool (*p_retro_unserialize)(const void *, size_t);
   void (*p_retro_reset)(void);
   struct retro_game_info info;
   EGLDisplay dpy;
   void *h;
   int frames, i;
   int ran = 0, boot_polls = 0;
   bool booted = false;

   if (argc < 4)
   {
      fprintf(stderr, "usage: %s core.so content frames\n", argv[0]);
      return 2;
   }
   frames = atoi(argv[3]);
   system_dir = getenv("LRTEST_SYSTEM") ? getenv("LRTEST_SYSTEM") : "/tmp";
   save_dir = getenv("LRTEST_SAVE") ? getenv("LRTEST_SAVE") : "/tmp";
   verbose = getenv("LRTEST_VERBOSE") != NULL;

   h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!h)
   {
      fprintf(stderr, "dlopen: %s\n", dlerror());
      return 2;
   }
   SYM(retro_init); SYM(retro_deinit); SYM(retro_set_environment);
   SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
   SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll);
   SYM(retro_set_input_state); SYM(retro_load_game); SYM(retro_unload_game);
   SYM(retro_run); SYM(retro_serialize_size); SYM(retro_serialize);
   SYM(retro_unserialize); SYM(retro_reset);

   p_retro_set_environment(environ_cb);
   p_retro_set_video_refresh(video_cb);
   p_retro_set_audio_sample(audio_cb);
   p_retro_set_audio_sample_batch(audio_batch_cb);
   p_retro_set_input_poll(input_poll_cb);
   p_retro_set_input_state(input_state_cb);
   p_retro_init();

   memset(&info, 0, sizeof(info));
   info.path = argv[2];
   if (!p_retro_load_game(&info) || !hw_set)
   {
      fprintf(stderr, "retro_load_game failed or no GL context requested\n");
      return 3;
   }
   if (!make_context(&dpy))
   {
      fprintf(stderr, "EGL context setup failed\n");
      return 4;
   }
   hw.context_reset();

   for (i = 0; i < frames && !shutdown_requested; i++)
   {
      p_retro_run();
      if (!booted && frames_video == 0)
      {
         /* Booting: retro_run returns straight away until the loader thread is
          * done. A frontend would wait for vsync here; give up after 60 s. */
         if (++boot_polls > 60000)
         {
            fprintf(stderr, "boot timeout\n");
            break;
         }
         usleep(1000);
         i--;
         continue;
      }
      booted = true;
      ran++;

      /* Each of these pauses or stops the emu thread and starts it again. */
      if (i == frames / 6)
      {
         size_t size = p_retro_serialize_size();
         void *buf = malloc(size);
         if (buf && p_retro_serialize(buf, size))
            p_retro_unserialize(buf, size);
         free(buf);
      }
      else if (i == 2 * frames / 6)
         update_pending = true;
      else if (i == 3 * frames / 6)
      {
         if (hw.context_destroy)
            hw.context_destroy();
         hw.context_reset();
      }
      else if (i == 4 * frames / 6)
         p_retro_reset();
      else if (i == 5 * frames / 6)
         p_retro_serialize_size();
   }

   p_retro_unload_game();
   p_retro_deinit();
   eglTerminate(dpy);
   printf("@@ frames=%d video=%lu audio_frames=%lu video_digest=%016llx\n",
         ran, frames_video, audio_frames, (unsigned long long)video_digest);
   dlclose(h);
   return ran == frames ? 0 : 1;
}
