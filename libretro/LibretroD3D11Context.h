#pragma once

#define HAVE_D3D11
#include <libretro_d3d.h>
#include "libretro/LibretroGraphicsContext.h"

/* One present texture per sync index the frontend can hand out. */
#define LIBRETRO_D3D11_MAX_SLOTS 32

class LibretroD3D11Context : public LibretroHWRenderContext {
public:
   LibretroD3D11Context() : LibretroHWRenderContext(RETRO_HW_CONTEXT_D3D11, 11) {}
   ~LibretroD3D11Context() override {
      LockGPU();
      ShutdownAPI();
      UnlockGPU();
   }
   bool InitAPI(void *wnd, std::string *deviceName, std::string *error_message) override;

   void SwapBuffers() override;
   void GotBackbuffer() override;
   void LostBackbuffer() override;
   void CreateDrawContext() override;
   void DestroyDrawContext() override;
   void ContextReset() override;
   void ContextDestroy() override;
   void FrameBegin() override;
   void LockGPU() override;
   void UnlockGPU() override;

   GPUCore GetGPUCore() override { return GPUCORE_DIRECTX11; }
   const char *Ident() override { return "DirectX 11"; }

   struct Slot {
      ID3D11Texture2D *tex;
      ID3D11RenderTargetView *rtv;
      UINT width;
      UINT height;
   };

private:
   void FreeSlots();

   retro_hw_render_interface_d3d11 hwInterface_ = {};
   ID3D11Device1 *device1_ = nullptr;
   ID3D11DeviceContext1 *context1_ = nullptr;
   /* Version 1: the one texture, left at PS slot 0 for the frontend. */
   ID3D11Texture2D *texture_ = nullptr;
   ID3D11RenderTargetView *texture_rt_view_ = nullptr;
   ID3D11ShaderResourceView *texture_sr_view_ = nullptr;
   /* Version 2: the frontend reads these itself, one per sync index. */
   Slot slots_[LIBRETRO_D3D11_MAX_SLOTS] = {};
   /* The gpu that was told its device is lost, until it is restored. */
   GPUCommon *lostGpu_ = nullptr;
   int boundSlot_ = -1;
   int frameSlot_ = -1;
   DXGI_FORMAT format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
   bool v2_ = false;
   bool live_ = false;
};
