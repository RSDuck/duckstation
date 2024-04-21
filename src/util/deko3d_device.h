// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_device.h"
#include "gpu_framebuffer_manager.h"
#include "gpu_shader_cache.h"

#include "deko3d_memory_heap.h"
#include "deko3d_pipeline.h"
#include "deko3d_stream_buffer.h"
#include "deko3d_swap_chain.h"
#include "deko3d_texture.h"

#include "common/rectangle.h"

#include <cstdio>
#include <memory>
#include <tuple>

#include <deko3d.hpp>

class Deko3DDevice final : public GPUDevice
{
public:
  Deko3DDevice();
  ~Deko3DDevice();

  ALWAYS_INLINE static Deko3DDevice& GetInstance() { return *static_cast<Deko3DDevice*>(g_gpu_device.get()); }

  RenderAPI GetRenderAPI() const override;

  bool HasSurface() const override;
  void DestroySurface() override;

  bool UpdateWindow() override;
  void ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale) override;

  std::string GetDriverInfo() const override;

  AdapterAndModeList GetAdapterAndModeList() override;

  std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                            GPUTexture::Type type, GPUTexture::Format format,
                                            const void* data = nullptr, u32 data_stride = 0) override;
  std::unique_ptr<GPUSampler> CreateSampler(const GPUSampler::Config& config) override;
  std::unique_ptr<GPUTextureBuffer> CreateTextureBuffer(GPUTextureBuffer::Format format, u32 size_in_elements) override;
  std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format) override;
  std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format,
                                                            void* memory, size_t memory_size,
                                                            u32 memory_stride) override;

  bool SupportsTextureFormat(GPUTexture::Format format) const override;
  void CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                         u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width, u32 height) override;
  void ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                            u32 src_x, u32 src_y, u32 width, u32 height) override;
  void ClearRenderTarget(GPUTexture* t, u32 c) override;
  void ClearDepth(GPUTexture* t, float d) override;
  void InvalidateRenderTarget(GPUTexture* t) override;

  std::unique_ptr<GPUShader> CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data) override;
  std::unique_ptr<GPUShader> CreateShaderFromSource(GPUShaderStage stage, const std::string_view& source,
                                                    const char* entry_point, DynamicHeapArray<u8>* out_binary) override;
  std::unique_ptr<GPUPipeline> CreatePipeline(const GPUPipeline::GraphicsConfig& config) override;

  void PushDebugGroup(const char* name) override;
  void PopDebugGroup() override;
  void InsertDebugMessage(const char* msg) override;

  void MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                       u32* map_base_vertex) override;
  void UnmapVertexBuffer(u32 vertex_size, u32 vertex_count) override;
  void MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index) override;
  void UnmapIndexBuffer(u32 used_index_count) override;
  void PushUniformBuffer(const void* data, u32 data_size) override;
  void* MapUniformBuffer(u32 size) override;
  void UnmapUniformBuffer(u32 size) override;
  void SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds,
                        GPUPipeline::RenderPassFlag render_pass_flags = GPUPipeline::NoRenderPassFlags) override;
  void SetPipeline(GPUPipeline* pipeline) override;
  void SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler) override;
  void SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer) override;
  void SetViewport(s32 x, s32 y, s32 width, s32 height) override;
  void SetScissor(s32 x, s32 y, s32 width, s32 height) override;
  void Draw(u32 vertex_count, u32 base_vertex) override;
  void DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex) override;
  void DrawIndexedWithBarrier(u32 index_count, u32 base_index, u32 base_vertex, DrawBarrier type) override;

  bool BeginPresent(bool skip_present) override;
  void EndPresent(bool explicit_submit) override;
  void SubmitPresent() override;

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

  void CommitClear(dk::CmdBuf command_buffer, Deko3DTexture* tex);
  void CommitRTClearInFB(Deko3DTexture* tex, u32 idx);

  // deko3D object getters
  ALWAYS_INLINE dk::Device GetDevice() { return m_device; }
  ALWAYS_INLINE dk::Queue GetQueue() { return m_queue; }

  ALWAYS_INLINE Deko3DMemoryHeap& GetGeneralHeap() { return m_general_heap; }
  ALWAYS_INLINE Deko3DMemoryHeap& GetTextureHeap() { return m_texture_heap; }
  ALWAYS_INLINE Deko3DMemoryHeap& GetShaderHeap() { return m_shader_heap; }

  void DeferedFree(Deko3DMemoryHeap& heap, Deko3DMemoryHeap::Allocation allocation);

  ALWAYS_INLINE dk::CmdBuf GetCurrentCommandBuffer()
  {
    return m_frame_resources[m_current_frame].command_buffers[COMMAND_BUFFER_REGULAR];
  }
  dk::CmdBuf GetCurrentInitCommandBuffer();

  ALWAYS_INLINE Deko3DStreamBuffer* GetTextureUploadBuffer() { return m_texture_upload_buffer.get(); }

  ALWAYS_INLINE u64 GetCurrentFenceCounter() const { return m_frame_resources[m_current_frame].fence_counter; }
  ALWAYS_INLINE u64 GetCompletedFenceCounter() const { return m_completed_fence_counter; }

  ALWAYS_INLINE u64 GetCurrentBarrierCounter() const { return m_barrier_counter; }
  ALWAYS_INLINE void IncreaseBarrierCounter() { m_barrier_counter++; }

  // Wait for a fence to be completed.
  // Also invokes callbacks for completion.
  void WaitForFenceCounter(u64 fence_counter);

  void WaitForGPUIdle();

  void SubmitCommandBuffer(bool wait_for_completion);
  void SubmitCommandBuffer(bool wait_for_completion, const char* reason, ...);

  // to be called by callback command buffer callback to allocation more memory
  void AddCommandBufferMemory(dk::CmdBuf cmdbuf, size_t min_size);

  void UnbindTexture(Deko3DTexture* tex);

protected:
  bool CreateDevice(const std::string_view& adapter, bool threaded_presentation,
                    std::optional<bool> exclusive_fullscreen_control, FeatureMask disabled_features,
                    Error* error) override;
  void DestroyDevice() override;

  bool ReadPipelineCache(const std::string& filename) override;
  bool GetPipelineCacheData(DynamicHeapArray<u8>* data) override;

private:
  bool CreateBuffers();
  bool CreateCommandBuffers();

  // since the deko3D backend does not support threaded presentation
  // it is kind of uncessary to split everything into as many functions
  // as the Vulkan backend.
  void SubmitCommandBuffer(Deko3DSwapChain* present_swap_chain = nullptr);
  void MoveToNextCommandBuffer();

  void BeginCommandBuffer(u32 idx);
  void WaitForCommandBufferCompletion(u32 idx);

  void ApplyRasterizationState(GPUPipeline::RasterizationState rs);
  void ApplyDepthState(GPUPipeline::DepthState ds);
  void ApplyBlendState(GPUPipeline::BlendState bs);

  s32 IsRenderTargetBound(const GPUTexture* tex) const;

  void UpdateViewport();
  void UpdateScissor();

  void CreateNullTexture();

  void PrepareTextures();

  enum : u32
  {
    COMMAND_BUFFER_INIT,
    COMMAND_BUFFER_REGULAR,
    COMMAND_BUFFER_TYPES
  };

  struct CommandBuffer
  {
    dk::Fence fence = {};
    u64 fence_counter = 0;
    std::array<dk::CmdBuf, 2> command_buffers;
    std::array<std::vector<Deko3DMemoryHeap::Allocation>, 2> command_memory;

    bool init_buffer_used = false;

    Deko3DMemoryHeap::Allocation image_descriptors;
    Deko3DMemoryHeap::Allocation sampler_descriptors;

    u32 next_image_descriptor = 0;
    u32 next_sampler_descriptor = 0;
  };

  std::deque<std::tuple<u64, Deko3DMemoryHeap&, Deko3DMemoryHeap::Allocation>> m_cleanup_objects;

  enum : u32
  {
    NUM_COMMAND_BUFFERS = 3,
  };
  std::array<CommandBuffer, NUM_COMMAND_BUFFERS> m_frame_resources;
  u32 m_current_frame = 0;
  u64 m_completed_fence_counter = 0, m_next_fence_counter = 1;

  u64 m_barrier_counter = 0;

  dk::Device m_device = {};
  dk::Queue m_queue = {};

  Deko3DMemoryHeap m_general_heap, m_texture_heap, m_shader_heap;

  std::unique_ptr<Deko3DStreamBuffer> m_texture_upload_buffer;

  Deko3DPipeline* m_current_pipeline = nullptr;
  GPUPipeline::BlendState m_last_blend_state = {};
  GPUPipeline::RasterizationState m_last_rasterization_state = {};
  GPUPipeline::DepthState m_last_depth_state = {};

  Common::Rectangle<s32> m_last_viewport{0, 0, 1, 1};
  Common::Rectangle<s32> m_last_scissor{0, 0, 1, 1};

  std::unique_ptr<Deko3DStreamBuffer> m_vertex_buffer, m_index_buffer, m_uniform_buffer;

  Deko3DMemoryHeap::Allocation m_push_buffer;

  std::unique_ptr<Deko3DSwapChain> m_swap_chain;

  std::array<Deko3DTexture*, MAX_TEXTURE_SAMPLERS> m_current_textures = {};
  std::array<Deko3DSampler*, MAX_TEXTURE_SAMPLERS> m_current_samplers = {};

  Deko3DTextureBuffer* m_current_texture_buffer;

  Deko3DMemoryHeap::Allocation m_download_buffer = {};

  u32 m_textures_dirty = 0;

  u32 m_num_current_render_targets = 0;
  std::array<Deko3DTexture*, MAX_RENDER_TARGETS> m_current_render_targets = {};
  Deko3DTexture* m_current_depth_target = nullptr;

  std::unique_ptr<Deko3DTexture> m_null_texture;
};
