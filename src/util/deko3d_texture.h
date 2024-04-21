// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "gpu_device.h"
#include "gpu_texture.h"
#include <tuple>

#include <deko3d.hpp>

#include "deko3d_memory_heap.h"

class Deko3DDevice;
class Deko3DStreamBuffer;

class Deko3DTexture final : public GPUTexture
{
  friend Deko3DDevice;

public:
  ~Deko3DTexture();

  void Destroy(bool defer);

  bool Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer = 0, u32 level = 0) override;
  bool Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer = 0, u32 level = 0) override;
  void Unmap() override;

  void SetDebugName(const std::string_view& name) override;

  static std::unique_ptr<Deko3DTexture> Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type,
                                               Format format, uint32_t flags);

  ALWAYS_INLINE const dk::Image& GetImage() const { return m_image; }
  ALWAYS_INLINE const dk::ImageDescriptor& GetDescriptor() const { return m_descriptor; }

  ALWAYS_INLINE u64 GetBarrierCounter() const { return m_barrier_counter; }
  ALWAYS_INLINE void SetBarrierCounter(u64 counter) { m_barrier_counter = counter; }

  ALWAYS_INLINE u64 GetDescriptorFence() const { return m_descriptor_fence; }
  ALWAYS_INLINE void SetDescriptorFence(u64 counter) { m_descriptor_fence = counter; }

  ALWAYS_INLINE u32 GetDescriptorIdx() const { return m_descriptor_idx; }
  ALWAYS_INLINE void setDescriptorIdx(u32 idx) { m_descriptor_idx = idx; }

  void MakeReadyForSampling() override;
private:
  Deko3DTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format,
                const dk::ImageLayout& layout, const Deko3DMemoryHeap::Allocation& memory);

  dk::CmdBuf GetCommandBufferForUpdate();

  void CopyTextureDataForUpload(void* dst, const void* src, u32 width, u32 height, u32 pitch, u32 upload_pitch) const;
  Deko3DMemoryHeap::Allocation AllocateUploadStagingBuffer(const void* data, u32 pitch, u32 upload_pitch, u32 width,
                                                           u32 height) const;
  void UpdateFromBuffer(dk::CmdBuf cmdbuf, u32 x, u32 y, u32 width, u32 height, u32 layer, u32 level, u32 pitch,
                        DkGpuAddr buffer);

  // Contains the barrier counter from when the texture was last bound as render target
  // So we can check whether there was a barrier
  u64 m_barrier_counter = 0;

  // Fence counter for which the descriptor index is valid
  u64 m_descriptor_fence = std::numeric_limits<u64>::max();
  u32 m_descriptor_idx;

  u32 m_map_offset = 0;
  u16 m_map_x = 0;
  u16 m_map_y = 0;
  u16 m_map_width = 0;
  u16 m_map_height = 0;
  u8 m_map_layer = 0;
  u8 m_map_level = 0;

  Deko3DMemoryHeap::Allocation m_memory;
  dk::Image m_image;
  dk::ImageDescriptor m_descriptor;
};

class Deko3DSampler final : public GPUSampler
{
  friend Deko3DDevice;

public:
  ~Deko3DSampler() override;

  void SetDebugName(const std::string_view& name) override;

  ALWAYS_INLINE u64 GetDescriptorFence() const { return m_descriptor_fence; }
  ALWAYS_INLINE void SetDescriptorFence(u64 counter) { m_descriptor_fence = counter; }

  ALWAYS_INLINE u32 GetDescriptorIdx() const { return m_descriptor_idx; }
  ALWAYS_INLINE void setDescriptorIdx(u32 idx) { m_descriptor_idx = idx; }

  const dk::SamplerDescriptor& GetDescriptor() const { return m_descriptor; }

private:
  Deko3DSampler(const dk::SamplerDescriptor& descriptor);

  u64 m_descriptor_fence = std::numeric_limits<u64>::max();
  u32 m_descriptor_idx;

  dk::SamplerDescriptor m_descriptor;
};

class Deko3DTextureBuffer final : public GPUTextureBuffer
{
  friend Deko3DDevice;

public:
  ~Deko3DTextureBuffer() override;

  ALWAYS_INLINE Deko3DStreamBuffer* GetBuffer() const { return m_buffer.get(); }
  ALWAYS_INLINE const dk::Image& GetImage() const { return m_image; }

  // Inherited via GPUTextureBuffer
  void* Map(u32 required_elements) override;
  void Unmap(u32 used_elements) override;

  void SetDebugName(const std::string_view& name) override;

  ALWAYS_INLINE u64 GetDescriptorFence() const { return m_descriptor_fence; }
  ALWAYS_INLINE void SetDescriptorFence(u64 counter) { m_descriptor_fence = counter; }

  ALWAYS_INLINE u32 GetDescriptorIdx() const { return m_descriptor_idx; }
  ALWAYS_INLINE void setDescriptorIdx(u32 idx) { m_descriptor_idx = idx; }

private:
  Deko3DTextureBuffer(Format format, u32 size_in_elements, std::unique_ptr<Deko3DStreamBuffer> buffer,
                      const dk::ImageLayout& layout);

  std::unique_ptr<Deko3DStreamBuffer> m_buffer;
  dk::Image m_image;

  // Fence counter for which the descriptor index is valid
  u64 m_descriptor_fence = std::numeric_limits<u64>::max();
  u32 m_descriptor_idx;
};


class Deko3DDownloadTexture final : public GPUDownloadTexture
{
public:
  ~Deko3DDownloadTexture() override;

  static std::unique_ptr<Deko3DDownloadTexture> Create(u32 width, u32 height, GPUTexture::Format format, void* memory,
                                                       size_t memory_size, u32 memory_pitch);

  void CopyFromTexture(u32 dst_x, u32 dst_y, GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height,
                       u32 src_layer, u32 src_level, bool use_transfer_pitch) override;

  bool Map(u32 x, u32 y, u32 width, u32 height) override;
  void Unmap() override;

  void Flush() override;

  void SetDebugName(std::string_view name) override;

private:
  Deko3DDownloadTexture(u32 width, u32 height, GPUTexture::Format format, bool is_imported, Deko3DMemoryHeap::Allocation buffer,
                        const u8* map_ptr, u32 map_pitch);

  Deko3DMemoryHeap::Allocation m_buffer;
  u64 m_copy_fence_counter;
};
