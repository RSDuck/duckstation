// Copyright 2022 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#include "deko3d_swap_chain.h"

#include "deko3d_device.h"
#include "deko3d_texture.h"

Deko3DSwapChain::Deko3DSwapChain(const WindowInfo& wi, dk::Swapchain swapchain,
                                 std::array<std::unique_ptr<Deko3DTexture>, NUM_SWAPCHAIN_IMAGES>& images)
  : m_swapchain(swapchain), m_window_info(wi), m_images(std::move(images))
{
}

Deko3DSwapChain::~Deko3DSwapChain()
{
  m_swapchain.destroy();

  for (size_t i = 0; i < NUM_SWAPCHAIN_IMAGES; i++)
    m_images[i]->Destroy(false);
}

std::unique_ptr<Deko3DSwapChain> Deko3DSwapChain::Create(const WindowInfo& wi)
{
  Deko3DDevice& device = Deko3DDevice::GetInstance();

  std::array<std::unique_ptr<Deko3DTexture>, NUM_SWAPCHAIN_IMAGES> images;
  std::array<const DkImage*, NUM_SWAPCHAIN_IMAGES> dk_images;
  for (size_t i = 0; i < NUM_SWAPCHAIN_IMAGES; i++)
  {
    images[i] = Deko3DTexture::Create(
      wi.surface_width, wi.surface_height, 1, 1, 1, GPUTexture::Type::RenderTarget, GPUTexture::Format::RGBA8,
      DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression);

    dk_images[i] = &images[i]->GetImage();
  }

  dk::Swapchain swapchain = dk::SwapchainMaker{device.GetDevice(), wi.window_handle, dk_images}.create();

  return std::unique_ptr<Deko3DSwapChain>(new Deko3DSwapChain(wi, swapchain, images));
}

void Deko3DSwapChain::AcquireNextImage()
{
  m_current_slot = Deko3DDevice::GetInstance().GetQueue().acquireImage(m_swapchain);
  // m_swapchain.acquireImage(m_current_slot, m_acquire_fences[m_current_fence++]);
}

void Deko3DSwapChain::PresentImage()
{
  Deko3DDevice::GetInstance().GetQueue().presentImage(m_swapchain, m_current_slot);
}

void Deko3DSwapChain::ReleaseImage()
{
  // m_current_slot.reset();
}
