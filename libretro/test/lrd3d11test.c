/* Headless Direct3D 11 frontend for the core's D3D11 backend.
 *
 *   lrd3d11test core.dll content frames v1|v2|v2-threaded
 *
 * v1           the frontend knows no negotiation: interface version 1,
 *              the frame read from pixel shader slot 0 inside video_refresh
 * v2           version 2 with one sync index, the frame read inside
 *              video_refresh, after the frontend has drawn on the context
 * v2-threaded  version 2 with three sync indices and a frontend thread
 *              that reads each frame late, so the core laps it
 *
 * Every frame is read back and hashed. Frame dump playback is paced by
 * the core, not by retro_run, so the sequence varies from run to run but
 * settles on one image: all three modes must end on the same last frame.
 * The frontend checks the version 2 contract as it goes:
 * video_refresh and wait_sync_index without the lock, set_texture with
 * it, the handle of the latest context_reset only, its own render target
 * never drawn into, and (v2-threaded) a texture it holds never drawn into
 * before wait_sync_index has returned for it - it fills each one with a
 * marker after reading it and checks the marker is still there before it
 * gives the texture back.
 *
 * On the way it runs a save state, a resolution change, a context destroy
 * and reset with a new handle, a reset alone with a new handle (a driver
 * rebuilt over a cached context), and retro_reset.
 *
 * Windows only; lrd3d11test.sh builds it with mingw-w64 and runs it under
 * Wine, whose wined3d implements D3D11 over OpenGL. */
#ifndef CINTERFACE
#define CINTERFACE
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "libretro.h"
#include "libretro_d3d11.h"

#define SLOTS   3
#define QMAX    64
#define MAXDIM  2048
#define LAG_MS  3

enum { MODE_V1 = 0, MODE_V2, MODE_V2_THREADED };

typedef struct
{
   ID3D11Texture2D *tex;
   unsigned slot;
   unsigned w;
   unsigned h;
} work_t;

static int mode;
static ID3D11Device *dev;
static ID3D11DeviceContext *imm;
static D3D_FEATURE_LEVEL feature_level;
static struct retro_hw_render_callback hw;
static int hw_set;
static const struct retro_hw_render_context_negotiation_interface_d3d11 *negotiation;
static struct retro_hw_render_interface_d3d11 iface;
/* The handle changes at every context_reset; only the latest is valid. */
static int handles[2];
static int generation;

static CRITICAL_SECTION lock;
static DWORD core_thread;
static int core_depth;
static int used;

/* The frontend's own frame: a render target the core must never reach. */
static ID3D11Texture2D *fe_tex;
static ID3D11RenderTargetView *fe_rtv;
static ID3D11Texture2D *staging;
static ID3D11Texture2D *probe;
static int fe_drawn;

/* v2-threaded */
static unsigned sync_index;
static ID3D11Texture2D *handed;
static int slot_out[SLOTS];
static HANDLE slot_done[SLOTS];
static work_t queue[QMAX];
static unsigned q_head, q_tail;
static CRITICAL_SECTION qlock;
static HANDLE have_work;
static HANDLE fe_thread;
static volatile LONG fe_quit;
static volatile LONG fe_busy;

static unsigned long long video_digest = 1469598103934665603ULL;
static unsigned long long last_frame;
static unsigned long frames_video, frames_dupe, frames_lit;
static volatile LONG errors;
static unsigned long audio_frames;
static int update_pending;
static int shutdown_requested;
static const char *resolution = "480x272";
static const char *system_dir;
static const char *save_dir;
static int verbose;
static int trace;

static void fail(const char *fmt, ...)
{
   va_list ap;
   if (InterlockedIncrement(&errors) > 20)
      return;
   va_start(ap, fmt);
   fputs("ERROR: ", stdout);
   vprintf(fmt, ap);
   fputs("\n", stdout);
   va_end(ap);
   fflush(stdout);
}

static void fnv(unsigned long long *h, const void *data, size_t len)
{
   const unsigned char *p = (const unsigned char *)data;
   size_t i;
   for (i = 0; i < len; i++)
   {
      *h ^= p[i];
      *h *= 1099511628211ULL;
   }
}

static int on_core_thread(void) { return GetCurrentThreadId() == core_thread; }

static void check_handle(void *h, const char *what)
{
   if (h != (void *)&handles[generation & 1])
      fail("%s: handle of an earlier context_reset", what);
}

/* --- the context lock ------------------------------------------------ */

static bool fe_lock_context(void *h)
{
   int d;
   check_handle(h, "lock_context");
   EnterCriticalSection(&lock);
   if (on_core_thread())
      core_depth++;
   d    = used;
   used = 0;
   return d ? true : false;
}

static void fe_unlock_context(void *h)
{
   check_handle(h, "unlock_context");
   if (on_core_thread())
   {
      if (core_depth <= 0)
         fail("unlock_context without the lock");
      else
         core_depth--;
   }
   LeaveCriticalSection(&lock);
}

static void fe_set_texture(void *h, ID3D11Texture2D *t)
{
   check_handle(h, "set_texture");
   if (core_depth <= 0)
      fail("set_texture without the lock");
   handed = t;
}

static unsigned fe_get_sync_index(void *h)
{
   check_handle(h, "get_sync_index");
   return mode == MODE_V2_THREADED ? sync_index : 0;
}

static unsigned fe_get_sync_index_mask(void *h)
{
   check_handle(h, "get_sync_index_mask");
   return mode == MODE_V2_THREADED ? (1u << SLOTS) - 1 : 1;
}

static void fe_wait_sync_index(void *h)
{
   unsigned i = sync_index;
   check_handle(h, "wait_sync_index");
   if (core_depth > 0)
      fail("wait_sync_index with the lock held");
   if (mode == MODE_V2_THREADED && slot_out[i])
   {
      WaitForSingleObject(slot_done[i], INFINITE);
      slot_out[i] = 0;
   }
}

/* --- the frontend's side of a frame, with the lock held -------------- */

/* Its own drawing: everything a frontend's frame binds, so a core that
 * relies on what it bound before draws with the wrong state. */
static void fe_disturb(void)
{
   static const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
   D3D11_VIEWPORT vp;
   ID3D11ShaderResourceView *null_srv = NULL;
   vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = 1; vp.Height = 1;
   vp.MinDepth = 0; vp.MaxDepth = 1;
   ID3D11DeviceContext_OMSetRenderTargets(imm, 1, &fe_rtv, NULL);
   ID3D11DeviceContext_ClearRenderTargetView(imm, fe_rtv, grey);
   ID3D11DeviceContext_RSSetViewports(imm, 1, &vp);
   ID3D11DeviceContext_RSSetState(imm, NULL);
   ID3D11DeviceContext_OMSetBlendState(imm, NULL, NULL, 0xffffffff);
   ID3D11DeviceContext_OMSetDepthStencilState(imm, NULL, 0);
   ID3D11DeviceContext_IASetInputLayout(imm, NULL);
   ID3D11DeviceContext_VSSetShader(imm, NULL, NULL, 0);
   ID3D11DeviceContext_PSSetShader(imm, NULL, NULL, 0);
   ID3D11DeviceContext_PSSetShaderResources(imm, 0, 1, &null_srv);
   used     = 1;
   fe_drawn = 1;
}

static int read_pixel(ID3D11Texture2D *tex, unsigned x, unsigned y, unsigned *out)
{
   D3D11_BOX box;
   D3D11_MAPPED_SUBRESOURCE m;
   box.left = x; box.top = y; box.front = 0;
   box.right = x + 1; box.bottom = y + 1; box.back = 1;
   ID3D11DeviceContext_CopySubresourceRegion(imm, (ID3D11Resource *)probe, 0, 0, 0, 0,
         (ID3D11Resource *)tex, 0, &box);
   if (FAILED(ID3D11DeviceContext_Map(imm, (ID3D11Resource *)probe, 0, D3D11_MAP_READ, 0, &m)))
      return 0;
   *out = *(const unsigned *)m.pData;
   ID3D11DeviceContext_Unmap(imm, (ID3D11Resource *)probe, 0);
   return 1;
}

static void fe_check_own_target(void)
{
   unsigned px;
   if (fe_drawn && read_pixel(fe_tex, 0, 0, &px) && px != 0xff808080u && px != 0xff7f7f7fu)
      fail("the core drew into the frontend's render target (%08x)", px);
}

static void fe_read_frame(ID3D11Texture2D *tex, unsigned w, unsigned h)
{
   D3D11_BOX box;
   D3D11_MAPPED_SUBRESOURCE m;
   D3D11_TEXTURE2D_DESC d;
   unsigned long long frame_hash = 1469598103934665603ULL;
   unsigned y, x;
   int lit = 0;

   ID3D11Texture2D_GetDesc(tex, &d);
   if (d.Width < w || d.Height < h || w > MAXDIM || h > MAXDIM)
   {
      fail("frame %lu: %ux%u from a %ux%u texture", frames_video, w, h, d.Width, d.Height);
      return;
   }
   box.left = 0; box.top = 0; box.front = 0;
   box.right = w; box.bottom = h; box.back = 1;
   ID3D11DeviceContext_CopySubresourceRegion(imm, (ID3D11Resource *)staging, 0, 0, 0, 0,
         (ID3D11Resource *)tex, 0, &box);
   if (FAILED(ID3D11DeviceContext_Map(imm, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m)))
   {
      fail("frame %lu: map failed", frames_video);
      return;
   }
   for (y = 0; y < h; y++)
   {
      const unsigned *row = (const unsigned *)((const unsigned char *)m.pData + (size_t)y * m.RowPitch);
      fnv(&video_digest, row, (size_t)w * 4);
      fnv(&frame_hash, row, (size_t)w * 4);
      if (!lit)
         for (x = 0; x < w; x += 7)
            if (row[x] & 0x00ffffffu)
            {
               lit = 1;
               break;
            }
   }
   ID3D11DeviceContext_Unmap(imm, (ID3D11Resource *)staging, 0);
   if (lit)
      frames_lit++;
   last_frame = frame_hash;
   if (trace)
      printf("frame %lu %ux%u %016llx\n", frames_lit, w, h, frame_hash);
}

/* v2-threaded: the texture is the frontend's until it signals the slot;
 * a marker written now must still be there then. */
static void fe_mark(ID3D11Texture2D *tex)
{
   static const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
   ID3D11RenderTargetView *rtv = NULL;
   if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv)))
   {
      fail("no render target view on the core's texture");
      return;
   }
   ID3D11DeviceContext_ClearRenderTargetView(imm, rtv, magenta);
   ID3D11RenderTargetView_Release(rtv);
}

static void fe_check_mark(ID3D11Texture2D *tex, unsigned w, unsigned h)
{
   unsigned a = 0, b = 0;
   if (!read_pixel(tex, 0, 0, &a) || !read_pixel(tex, w - 1, h - 1, &b))
      return;
   if (a != 0xffff00ffu || b != 0xffff00ffu)
      fail("the core drew into a texture before wait_sync_index returned for it (%08x %08x)", a, b);
}

static DWORD WINAPI fe_thread_main(LPVOID arg)
{
   (void)arg;
   for (;;)
   {
      work_t wk;
      unsigned k;
      int have = 0;
      EnterCriticalSection(&qlock);
      if (q_head != q_tail)
      {
         k    = q_head % QMAX;
         wk   = queue[k];
         have = 1;
      }
      LeaveCriticalSection(&qlock);
      if (!have)
      {
         if (fe_quit)
            return 0;
         WaitForSingleObject(have_work, 50);
         continue;
      }

      /* Late, so the core runs ahead and has to wait. */
      Sleep(LAG_MS);
      EnterCriticalSection(&lock);
      fe_check_own_target();
      fe_disturb();
      fe_read_frame(wk.tex, wk.w, wk.h);
      fe_mark(wk.tex);
      LeaveCriticalSection(&lock);

      Sleep(LAG_MS);
      EnterCriticalSection(&lock);
      fe_check_mark(wk.tex, wk.w, wk.h);
      used = 1;
      LeaveCriticalSection(&lock);
      ID3D11Texture2D_Release(wk.tex);

      EnterCriticalSection(&qlock);
      q_head++;
      LeaveCriticalSection(&qlock);
      SetEvent(slot_done[wk.slot]);
      InterlockedDecrement(&fe_busy);
   }
}

/* Waits out every frame handed over, as a frontend does before it lets
 * a context go. */
static void fe_drain(void)
{
   unsigned i;
   while (fe_busy > 0)
      Sleep(1);
   for (i = 0; i < SLOTS; i++)
      if (slot_out[i])
      {
         WaitForSingleObject(slot_done[i], INFINITE);
         slot_out[i] = 0;
      }
}

/* --- libretro callbacks ---------------------------------------------- */

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
      return "d3d11";
   if (!strcmp(key, "ppsspp_internal_resolution"))
      return resolution;
   return NULL;
}

static void fill_interface(void)
{
   memset(&iface, 0, sizeof(iface));
   iface.interface_type    = RETRO_HW_RENDER_INTERFACE_D3D11;
   iface.interface_version = (mode != MODE_V1 && negotiation
         && negotiation->max_render_interface_version >= RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2)
      ? RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2 : RETRO_HW_RENDER_INTERFACE_D3D11_VERSION;
   iface.handle            = &handles[generation & 1];
   iface.device            = dev;
   iface.context           = imm;
   iface.featureLevel      = feature_level;
   iface.D3DCompile        = D3DCompile;
   if (iface.interface_version >= RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2)
   {
      iface.lock_context        = fe_lock_context;
      iface.unlock_context      = fe_unlock_context;
      iface.set_texture         = fe_set_texture;
      iface.get_sync_index      = fe_get_sync_index;
      iface.get_sync_index_mask = fe_get_sync_index_mask;
      iface.wait_sync_index     = fe_wait_sync_index;
   }
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
         *(bool *)data = update_pending ? true : false;
         update_pending = 0;
         return true;
      case RETRO_ENVIRONMENT_SHUTDOWN:
         shutdown_requested = 1;
         return true;
      case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
         *(unsigned *)data = RETRO_HW_CONTEXT_D3D11;
         return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
      {
         struct retro_hw_render_callback *cb = (struct retro_hw_render_callback *)data;
         if (cb->context_type != RETRO_HW_CONTEXT_D3D11)
            return false;
         hw     = *cb;
         hw_set = 1;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
      {
         struct retro_hw_render_context_negotiation_interface *n =
            (struct retro_hw_render_context_negotiation_interface *)data;
         n->interface_version = (mode != MODE_V1
               && n->interface_type == RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D11) ? 1 : 0;
         return true;
      }
      case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
         if (mode == MODE_V1)
            return false;
         negotiation = (const struct retro_hw_render_context_negotiation_interface_d3d11 *)data;
         return true;
      case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
         fill_interface();
         *(const struct retro_hw_render_interface **)data = (const struct retro_hw_render_interface *)&iface;
         return true;
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
   (void)pitch;
   if (core_depth > 0)
      fail("video_refresh with the lock held");
   if (data != RETRO_HW_FRAME_BUFFER_VALID)
   {
      frames_dupe++;
      return;
   }
   frames_video++;

   if (mode == MODE_V1)
   {
      ID3D11ShaderResourceView *srv = NULL;
      ID3D11Resource *res = NULL;
      ID3D11DeviceContext_PSGetShaderResources(imm, 0, 1, &srv);
      if (!srv)
      {
         fail("frame %lu: nothing at pixel shader slot 0", frames_video);
         return;
      }
      ID3D11ShaderResourceView_GetResource(srv, &res);
      fe_read_frame((ID3D11Texture2D *)res, width, height);
      ID3D11Resource_Release(res);
      ID3D11ShaderResourceView_Release(srv);
      /* The frontend draws inside video_refresh, as RetroArch does. */
      fe_disturb();
      return;
   }

   if (!handed)
   {
      fail("frame %lu: no set_texture before video_refresh", frames_video);
      return;
   }

   if (mode == MODE_V2)
   {
      EnterCriticalSection(&lock);
      fe_check_own_target();
      fe_disturb();
      fe_read_frame(handed, width, height);
      LeaveCriticalSection(&lock);
      handed = NULL;
      return;
   }

   {
      unsigned k = sync_index;
      ID3D11Texture2D_AddRef(handed);
      slot_out[k] = 1;
      InterlockedIncrement(&fe_busy);
      EnterCriticalSection(&qlock);
      queue[q_tail % QMAX].tex  = handed;
      queue[q_tail % QMAX].slot = k;
      queue[q_tail % QMAX].w   = width;
      queue[q_tail % QMAX].h   = height;
      q_tail++;
      LeaveCriticalSection(&qlock);
      SetEvent(have_work);
      handed     = NULL;
      sync_index = (k + 1) % SLOTS;
   }
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
   (void)data;
   audio_frames += frames;
   return frames;
}

static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; audio_frames++; }
static void input_poll_cb(void) { }
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
   (void)port; (void)device; (void)index; (void)id;
   return 0;
}

static int make_device(void)
{
   static const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
   };
   D3D11_TEXTURE2D_DESC td;
   if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 3,
               D3D11_SDK_VERSION, &dev, &feature_level, &imm)))
      return 0;
   memset(&td, 0, sizeof(td));
   td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1;
   td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
   td.BindFlags = D3D11_BIND_RENDER_TARGET;
   if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &fe_tex))
         || FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)fe_tex, NULL, &fe_rtv)))
      return 0;
   td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   td.Width = MAXDIM; td.Height = MAXDIM;
   if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging)))
      return 0;
   td.Width = 1; td.Height = 1;
   if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &probe)))
      return 0;
   return 1;
}

#define SYM(name) do { *(FARPROC *)&p_##name = GetProcAddress(h, #name); if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 2; } } while (0)

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
   HMODULE h;
   unsigned k;
   int frames, i, ran = 0, boot_polls = 0, booted = 0;
   static const char *mode_names[] = { "v1", "v2", "v2-threaded" };

   if (argc < 5)
   {
      fprintf(stderr, "usage: %s core.dll content frames v1|v2|v2-threaded\n", argv[0]);
      return 2;
   }
   frames = atoi(argv[3]);
   if (!strcmp(argv[4], "v1"))
      mode = MODE_V1;
   else if (!strcmp(argv[4], "v2"))
      mode = MODE_V2;
   else if (!strcmp(argv[4], "v2-threaded"))
      mode = MODE_V2_THREADED;
   else
   {
      fprintf(stderr, "unknown mode %s\n", argv[4]);
      return 2;
   }
   system_dir = getenv("LRTEST_SYSTEM") ? getenv("LRTEST_SYSTEM") : ".";
   save_dir   = getenv("LRTEST_SAVE") ? getenv("LRTEST_SAVE") : ".";
   verbose    = getenv("LRTEST_VERBOSE") != NULL;
   trace      = getenv("LRTEST_TRACE") != NULL;
   core_thread = GetCurrentThreadId();
   InitializeCriticalSection(&lock);
   InitializeCriticalSection(&qlock);
   have_work = CreateEventA(NULL, FALSE, FALSE, NULL);
   for (k = 0; k < SLOTS; k++)
      slot_done[k] = CreateEventA(NULL, FALSE, FALSE, NULL);

   if (!make_device())
   {
      printf("no D3D11 device\n");
      return 4;
   }
   if (mode == MODE_V2_THREADED)
      fe_thread = CreateThread(NULL, 0, fe_thread_main, NULL, 0, NULL);

   h = LoadLibraryA(argv[1]);
   if (!h)
   {
      fprintf(stderr, "LoadLibrary %s failed (%lu)\n", argv[1], (unsigned long)GetLastError());
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
      fprintf(stderr, "retro_load_game failed or no D3D11 context requested\n");
      return 3;
   }
   if (mode != MODE_V1 && !negotiation)
      fail("the core did not ask for version 2");
   hw.context_reset();

   for (i = 0; i < frames && !shutdown_requested; i++)
   {
      p_retro_run();
      if (!booted && frames_video == 0)
      {
         if (++boot_polls > 60000)
         {
            fprintf(stderr, "boot timeout\n");
            break;
         }
         Sleep(1);
         i--;
         continue;
      }
      booted = 1;
      ran++;

      if (i == frames / 6)
      {
         size_t size = p_retro_serialize_size();
         void *buf   = malloc(size);
         if (!buf || !p_retro_serialize(buf, size) || !p_retro_unserialize(buf, size))
            fail("save state round trip failed");
         free(buf);
      }
      else if (i == 2 * frames / 6)
      {
         resolution     = "960x544";
         update_pending = 1;
      }
      else if (i == 3 * frames / 6)
      {
         /* A rebuilt driver: everything handed over is let go, and the
          * core gets a new handle. */
         if (mode == MODE_V2_THREADED)
            fe_drain();
         if (hw.context_destroy)
            hw.context_destroy();
         generation++;
         hw.context_reset();
      }
      else if (i == 7 * frames / 12)
      {
         /* A driver rebuilt over a cached context: no context_destroy,
          * and a new handle all the same. */
         if (mode == MODE_V2_THREADED)
            fe_drain();
         generation++;
         hw.context_reset();
      }
      else if (i == 4 * frames / 6)
         p_retro_reset();
      else if (i == 5 * frames / 6)
         p_retro_serialize_size();
   }

   if (mode == MODE_V2_THREADED)
      fe_drain();
   if (hw.context_destroy)
      hw.context_destroy();
   p_retro_unload_game();
   p_retro_deinit();
   if (fe_thread)
   {
      fe_quit = 1;
      SetEvent(have_work);
      WaitForSingleObject(fe_thread, INFINITE);
   }
   if (core_depth != 0)
      fail("the core still holds the lock at exit (%d)", core_depth);

   printf("@@ mode=%s frames=%d video=%lu dupe=%lu lit=%lu errors=%ld video_digest=%016llx last=%016llx\n",
         mode_names[mode], ran, frames_video, frames_dupe, frames_lit, (long)errors,
         video_digest, last_frame);
   return (ran == frames && !errors) ? 0 : 1;
}
