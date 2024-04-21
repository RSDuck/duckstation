// Copyright 2022 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once

#include "window_info.h"
#include "common/assert.h"
#include <array>
#include <memory>
#include <optional>

#include <deko3d.hpp>

class Deko3DTexture;

class Deko3DSwapChain
{
public:
  static constexpr size_t NUM_SWAPCHAIN_IMAGES = 2;

  ~Deko3DSwapChain();

  static std::unique_ptr<Deko3DSwapChain> Create(const WindowInfo& wi);

  ALWAYS_INLINE const WindowInfo& GetWindowInfo() const { return m_window_info; }
  ALWAYS_INLINE Deko3DTexture* GetCurrentImage() { return m_images[m_current_slot].get(); }

  ALWAYS_INLINE u32 GetWidth() const { return m_window_info.surface_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_window_info.surface_height; }

  void AcquireNextImage();
  void PresentImage();

  void ReleaseImage();

  //dk::Fence& GetAcquireFence() { return m_acquire_fences[m_current_fence]; }

private:
  Deko3DSwapChain(const WindowInfo& wi, dk::Swapchain swapchain, std::array<std::unique_ptr<Deko3DTexture>, NUM_SWAPCHAIN_IMAGES>& images);

  dk::Swapchain m_swapchain;
  WindowInfo m_window_info;
  int m_current_slot;

  std::array<std::unique_ptr<Deko3DTexture>, NUM_SWAPCHAIN_IMAGES> m_images;
};
