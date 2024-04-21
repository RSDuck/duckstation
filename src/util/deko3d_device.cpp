#include "deko3d_device.h"

#include "common/align.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/log.h"
#include "common/string_util.h"

#include <utility>

#include <switch.h>
#include <uam.h>

Log_SetChannel(Deko3D_Device);

enum : u32
{
  GENERAL_HEAP_SIZE = 1024 * 1024 * 256,
  TEXTURE_HEAP_SIZE = 1024 * 1024 * 512,
  SHADER_HEAP_SIZE = 1024 * 1024 * 32,

  MAX_DRAW_CALLS_PER_FRAME = 2048,
  // 16 thousand seemed a bit too high for me, so I lowered it
  MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME =
    1024 /*GPUDevice::MAX_TEXTURE_SAMPLERS * MAX_DRAW_CALLS_PER_FRAME*/,

  GENERAL_HEAP_MAX_ALLOCS = 4096,
  TEXTURE_HEAP_MAX_ALLOCS = 4096,
  SHADER_HEAP_MAX_ALLOCS = 4096,

  VERTEX_BUFFER_SIZE = 32 * 1024 * 1024,
  INDEX_BUFFER_SIZE = 16 * 1024 * 1024,
  UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
  TEXTURE_BUFFER_SIZE = 64 * 1024 * 1024,

  UNIFORM_PUSH_CONSTANTS_SIZE = 128,

  MAX_UNIFORM_BUFFER_SIZE = DK_UNIFORM_BUF_MAX_SIZE,

  COMMAND_BUFFER_GROW_MIN = 1024 * 1024,
};

static void deko3D_DebugOut(void* userData, const char* context, DkResult result, const char* message)
{
  if (result == DkResult_Success)
    Log_DebugPrintf("deko3D debug message: %s\n", message);
  else
    Log_ErrorPrintf("deko3D error message: %s -> %d\n", message, result);
}

static void deko3D_CmdBufAddMem(void* userData, DkCmdBuf cmdbuf, size_t min_req_size)
{
  Deko3DDevice* device = reinterpret_cast<Deko3DDevice*>(userData);
  device->AddCommandBufferMemory(cmdbuf, min_req_size);
}

Deko3DDevice::Deko3DDevice()
{
}

Deko3DDevice::~Deko3DDevice()
{
}

RenderAPI Deko3DDevice::GetRenderAPI() const
{
  return RenderAPI::Deko3D;
}

bool Deko3DDevice::HasSurface() const
{
  return true;
}

void Deko3DDevice::DestroySurface()
{
}

bool Deko3DDevice::UpdateWindow()
{
  return false;
}

void Deko3DDevice::ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale)
{
  // TODO: mich fertig machen
  if (!m_swap_chain)
    return;

  if (m_swap_chain->GetWidth() == static_cast<u32>(new_window_width) &&
      m_swap_chain->GetHeight() == static_cast<u32>(new_window_height))
  {
    // skip unnecessary resizes
    m_window_info.surface_scale = new_window_scale;
    return;
  }

  // make sure previous frames are presented
  WaitForGPUIdle();

  m_swap_chain.reset();

  m_window_info.surface_width = new_window_width;
  m_window_info.surface_height = new_window_height;
  m_window_info.surface_scale = new_window_scale;

  m_swap_chain = Deko3DSwapChain::Create(m_window_info);
}

std::string Deko3DDevice::GetDriverInfo() const
{
  return "There no driver, there is only Zuul";
}

GPUDevice::AdapterAndModeList Deko3DDevice::GetAdapterAndModeList()
{
  return AdapterAndModeList();
}

s32 Deko3DDevice::IsRenderTargetBound(const GPUTexture* tex) const
{
  for (u32 i = 0; i < m_num_current_render_targets; i++)
  {
    if (m_current_render_targets[i] == tex)
      return static_cast<s32>(i);
  }

  return -1;
}

void Deko3DDevice::ClearRenderTarget(GPUTexture* t, u32 c)
{
  GPUDevice::ClearRenderTarget(t, c);
  if (const s32 idx = IsRenderTargetBound(t); idx >= 0)
    CommitRTClearInFB(static_cast<Deko3DTexture*>(t), static_cast<u32>(idx));
}

void Deko3DDevice::ClearDepth(GPUTexture* t, float d)
{
  GPUDevice::ClearDepth(t, d);
  if (m_current_depth_target == t)
    CommitRTClearInFB(static_cast<Deko3DTexture*>(t), 0);
}

void Deko3DDevice::InvalidateRenderTarget(GPUTexture* t)
{
  GPUDevice::InvalidateRenderTarget(t);
  if (t->IsRenderTarget())
  {
    if (const s32 idx = IsRenderTargetBound(t); idx >= 0)
      CommitRTClearInFB(static_cast<Deko3DTexture*>(t), static_cast<u32>(idx));
  }
  else
  {
    DebugAssert(t->IsDepthStencil());
    if (m_current_depth_target == t)
      CommitRTClearInFB(static_cast<Deko3DTexture*>(t), 0);
  }
}

bool Deko3DDevice::CreateDevice(const std::string_view& adapter, bool threaded_presentation,
                                std::optional<bool> exclusive_fullscreen_control, FeatureMask disabled_features,
                                Error* error)
{
  uam_init();

  m_features.dual_source_blend = true;
  m_features.per_sample_shading = true;
  m_features.noperspective_interpolation = true;
  m_features.texture_copy_to_self = true;
  m_features.supports_texture_buffers = true;
  m_features.geometry_shaders = true;
  m_features.partial_msaa_resolve = true;
  m_features.shader_cache = true;
  m_features.explicit_present = false;
  m_features.memory_import = false;
  m_features.feedback_loops = false;

  m_max_texture_size = 4096; // ????
  m_max_multisamples = 8;

  m_device = dk::DeviceMaker{}
               .setFlags(DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginLowerLeft)
               .setCbDebug(deko3D_DebugOut)
               .create();

  m_queue = dk::QueueMaker{m_device}.setFlags(DkQueueFlags_Graphics).create();

  if (!CreateBuffers() || !CreateCommandBuffers())
    return false;

  m_swap_chain = Deko3DSwapChain::Create(m_window_info);

  CreateNullTexture();

  MoveToNextCommandBuffer();

  dk::CmdBuf command_buffer = GetCurrentCommandBuffer();
  command_buffer.bindVtxBuffer(0, m_vertex_buffer->GetPointer(), m_vertex_buffer->GetCurrentSize());
  static_assert(sizeof(DrawIndex) == 2);
  command_buffer.bindIdxBuffer(DkIdxFormat_Uint16, m_index_buffer->GetPointer());

  m_push_buffer = m_general_heap.Alloc(UNIFORM_PUSH_CONSTANTS_SIZE, DK_UNIFORM_BUF_ALIGNMENT);
  command_buffer.bindUniformBuffer(DkStage_Vertex, 0, m_general_heap.GPUPointer(m_push_buffer), m_push_buffer.size);
  command_buffer.bindUniformBuffer(DkStage_Geometry, 0, m_general_heap.GPUPointer(m_push_buffer), m_push_buffer.size);
  command_buffer.bindUniformBuffer(DkStage_Fragment, 0, m_general_heap.GPUPointer(m_push_buffer), m_push_buffer.size);

  return true;
}

void Deko3DDevice::DestroyDevice()
{
  WaitForGPUIdle();

  m_null_texture->Destroy(false);

  m_swap_chain.reset();

  m_texture_upload_buffer.reset();
  m_vertex_buffer.reset();
  m_index_buffer.reset();
  m_uniform_buffer.reset();

  m_swap_chain.reset();

  m_general_heap.Destroy();
  m_texture_heap.Destroy();
  m_shader_heap.Destroy();

  m_queue.destroy();
  m_device.destroy();

  uam_deinit();
}

bool Deko3DDevice::CreateBuffers()
{
  if (!m_general_heap.Create(GENERAL_HEAP_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                             GENERAL_HEAP_MAX_ALLOCS))
  {
    Log_ErrorPrintf("Failed to allocate general heap");
    return false;
  }

  if (!m_texture_heap.Create(TEXTURE_HEAP_SIZE, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image,
                             TEXTURE_HEAP_MAX_ALLOCS))
  {
    Log_ErrorPrintf("Failed to allocate texture heap");
    return false;
  }

  if (!m_shader_heap.Create(SHADER_HEAP_SIZE,
                            DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
                            SHADER_HEAP_MAX_ALLOCS))
  {
    Log_ErrorPrintf("Failed to allocate shader heap");
    return false;
  }

  m_texture_upload_buffer = Deko3DStreamBuffer::Create(TEXTURE_BUFFER_SIZE);

  m_vertex_buffer = Deko3DStreamBuffer::Create(VERTEX_BUFFER_SIZE);
  m_index_buffer = Deko3DStreamBuffer::Create(INDEX_BUFFER_SIZE);
  m_uniform_buffer = Deko3DStreamBuffer::Create(UNIFORM_BUFFER_SIZE);

  return true;
}

bool Deko3DDevice::CreateCommandBuffers()
{
  for (CommandBuffer& resources : m_frame_resources)
  {
    for (u32 i = 0; i < COMMAND_BUFFER_TYPES; i++)
    {
      resources.command_buffers[i] =
        dk::CmdBufMaker{m_device}.setCbAddMem(deko3D_CmdBufAddMem).setUserData(this).create();

      if (!resources.command_buffers[i])
      {
        Log_ErrorPrint("Failed to create command buffer");
        return false;
      }
    }

    resources.image_descriptors = m_general_heap.Alloc(
      sizeof(dk::ImageDescriptor) * MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME, DK_IMAGE_DESCRIPTOR_ALIGNMENT);
    resources.sampler_descriptors =
      m_general_heap.Alloc(sizeof(dk::SamplerDescriptor) * MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME,
                           DK_SAMPLER_DESCRIPTOR_ALIGNMENT);
  }

  return true;
}

void Deko3DDevice::AddCommandBufferMemory(dk::CmdBuf cmdbuf, size_t min_size)
{
  CommandBuffer& resources = m_frame_resources[m_current_frame];
  Assert(cmdbuf == resources.command_buffers[COMMAND_BUFFER_INIT] ||
         cmdbuf == resources.command_buffers[COMMAND_BUFFER_REGULAR]);

  size_t size = std::max<size_t>(min_size, COMMAND_BUFFER_GROW_MIN);
  Deko3DMemoryHeap::Allocation mem = m_general_heap.Alloc(size, DK_CMDMEM_ALIGNMENT);
  cmdbuf.addMemory(m_general_heap.GetMemBlock(), mem.offset, size);

  int command_buffer_type =
    cmdbuf == resources.command_buffers[COMMAND_BUFFER_INIT] ? COMMAND_BUFFER_INIT : COMMAND_BUFFER_REGULAR;

  resources.command_memory[command_buffer_type].push_back(mem);
}

dk::CmdBuf Deko3DDevice::GetCurrentInitCommandBuffer()
{
  CommandBuffer& resources = m_frame_resources[m_current_frame];
  resources.init_buffer_used = true;
  return resources.command_buffers[COMMAND_BUFFER_INIT];
}

void Deko3DDevice::WaitForFenceCounter(u64 fence_counter)
{
  if (m_completed_fence_counter >= fence_counter)
    return;

  // Find the first command buffer which covers this counter value.
  u32 index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
  while (index != m_current_frame)
  {
    if (m_frame_resources[index].fence_counter >= fence_counter)
      break;

    index = (index + 1) % NUM_COMMAND_BUFFERS;
  }

  DebugAssert(index != m_current_frame);
  WaitForCommandBufferCompletion(index);
}

void Deko3DDevice::WaitForGPUIdle()
{
  m_queue.waitIdle();
}

void Deko3DDevice::SubmitCommandBuffer(bool wait_for_completion)
{
  wait_for_completion = true;
  CommandBuffer& resources = m_frame_resources[m_current_frame];

  const u32 current_frame = m_current_frame;
  SubmitCommandBuffer();
  MoveToNextCommandBuffer();

  if (wait_for_completion)
    WaitForCommandBufferCompletion(current_frame);
}

void Deko3DDevice::SubmitCommandBuffer(bool wait_for_completion, const char* reason, ...)
{
  std::va_list ap;
  va_start(ap, reason);
  const std::string reason_str(StringUtil::StdStringFromFormatV(reason, ap));
  va_end(ap);

  Log_WarningPrintf("Executing command buffer due to '%s'", reason_str.c_str());
  SubmitCommandBuffer(wait_for_completion);
}

void Deko3DDevice::SubmitCommandBuffer(Deko3DSwapChain* present_swap_chain)
{
  CommandBuffer& resources = m_frame_resources[m_current_frame];
  /*
    While I'm trying to stay as close as possible to the Vulkan backend
    we don't have a threaded presentation and the final fence + present
    are handled together by deko3D this function also does the work of
    DoSubmitCommandBuffer and DoPresent.
  */
  // if (present_swap_chain)
  //   m_queue.waitFence(present_swap_chain->GetAcquireFence());

  // if (resources.init_buffer_used)
  //   m_queue.submitCommands(resources.command_buffers[COMMAND_BUFFER_INIT].finishList());

  if (resources.init_buffer_used)
    m_queue.submitCommands(resources.command_buffers[COMMAND_BUFFER_INIT].finishList());
  m_queue.submitCommands(resources.command_buffers[COMMAND_BUFFER_REGULAR].finishList());

  m_queue.signalFence(resources.fence);

  if (present_swap_chain)
  {
    present_swap_chain->PresentImage();

    // supposed trick about acquiring earlier from Vulkan backend
    // still need to wrap my headaround whether this is useful for deko3D too
    // present_swap_chain->AcquireNextImage();
  }
  else
  {
    m_queue.flush();
  }
}

void Deko3DDevice::MoveToNextCommandBuffer()
{
  BeginCommandBuffer((m_current_frame + 1) % NUM_COMMAND_BUFFERS);
}

void Deko3DDevice::BeginCommandBuffer(u32 idx)
{
  CommandBuffer& resources = m_frame_resources[idx];

  if (resources.fence_counter > m_completed_fence_counter)
    WaitForCommandBufferCompletion(idx);

  if (resources.command_memory.size() > 0)
  {
    // clear rolls back to the start of the last memory block
    // we'll free all the previous blocks
    for (u32 buffer = 0; buffer < COMMAND_BUFFER_TYPES; buffer++)
    {
      if (resources.command_memory[buffer].size() > 1)
      {
        // stupid unsigned data types...
        for (size_t i = 0; i < resources.command_memory[buffer].size() - 1; i++)
          m_general_heap.Free(resources.command_memory[buffer][i]);
      }

      if (resources.command_memory[buffer].size() > 0)
      {
        auto lastBlock = resources.command_memory[buffer][resources.command_memory[buffer].size() - 1];
        resources.command_memory[buffer].clear();
        resources.command_memory[buffer].push_back(lastBlock);
      }
    }
  }

  resources.init_buffer_used = false;
  resources.fence_counter = m_next_fence_counter++;

  m_current_frame = idx;

  resources.next_image_descriptor = 0;
  resources.next_sampler_descriptor = 0;

  resources.command_buffers[COMMAND_BUFFER_REGULAR].bindImageDescriptorSet(
    m_general_heap.GPUPointer(resources.image_descriptors), MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME);
  resources.command_buffers[COMMAND_BUFFER_REGULAR].bindSamplerDescriptorSet(
    m_general_heap.GPUPointer(resources.sampler_descriptors), MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME);

  m_textures_dirty = (1 << MAX_TEXTURE_SAMPLERS) - 1;
}

void Deko3DDevice::WaitForCommandBufferCompletion(u32 index)
{
  m_frame_resources[index].fence.wait();

  const u64 now_completed_counter = m_frame_resources[index].fence_counter;

  m_completed_fence_counter = now_completed_counter;
  while (!m_cleanup_objects.empty())
  {
    auto& it = m_cleanup_objects.front();
    if (std::get<0>(it) > now_completed_counter)
      break;
    std::get<1>(it).Free(std::get<2>(it));
    m_cleanup_objects.pop_front();
  }
}

void Deko3DDevice::DeferedFree(Deko3DMemoryHeap& heap, Deko3DMemoryHeap::Allocation allocation)
{
  m_cleanup_objects.push_back(std::make_tuple(GetCurrentFenceCounter(), std::ref(heap), allocation));
}

void Deko3DDevice::PushDebugGroup(const char* name)
{
  // not supported
}

void Deko3DDevice::PopDebugGroup()
{
  // not supported
}

void Deko3DDevice::InsertDebugMessage(const char* msg)
{
  // not supported
}

void Deko3DDevice::MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                                   u32* map_base_vertex)
{
  const u32 req_size = vertex_size * vertex_count;
  if (!m_vertex_buffer->ReserveMemory(req_size, vertex_size))
  {
    SubmitCommandBuffer(false, "out of vertex space");
    if (!m_vertex_buffer->ReserveMemory(req_size, vertex_size))
      Panic("Failed to allocate vertex space");
  }

  *map_ptr = m_vertex_buffer->GetCurrentHostPointer();
  *map_space = m_vertex_buffer->GetCurrentSpace() / vertex_size;
  *map_base_vertex = m_vertex_buffer->GetCurrentOffset() / vertex_size;
  // printf("mapping vertex buffer %u %u %p %u %u\n", vertex_size, vertex_count, *map_ptr, *map_space,
  // *map_base_vertex);
  Assert((m_vertex_buffer->GetCurrentOffset() % vertex_size) == 0);
}

void Deko3DDevice::UnmapVertexBuffer(u32 vertex_size, u32 vertex_count)
{
  // printf("unmap vertex %u %u\n", vertex_size, vertex_count);
  const u32 size = vertex_size * vertex_count;
  s_stats.buffer_streamed += size;
  m_vertex_buffer->CommitMemory(size);
}

void Deko3DDevice::MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index)
{
  const u32 req_size = sizeof(DrawIndex) * index_count;
  if (!m_index_buffer->ReserveMemory(req_size, sizeof(DrawIndex)))
  {
    SubmitCommandBuffer(false, "out of index space");
    if (!m_index_buffer->ReserveMemory(req_size, sizeof(DrawIndex)))
      Panic("Failed to allocate index space");
  }

  *map_ptr = reinterpret_cast<DrawIndex*>(m_index_buffer->GetCurrentHostPointer());
  *map_space = m_index_buffer->GetCurrentSpace() / sizeof(DrawIndex);
  *map_base_index = m_index_buffer->GetCurrentOffset() / sizeof(DrawIndex);

  // printf("mapping index buffer %u %p %u %u\n", index_count, *map_ptr, *map_space, *map_base_index);
}

void Deko3DDevice::UnmapIndexBuffer(u32 used_index_count)
{
  const u32 size = sizeof(DrawIndex) * used_index_count;
  s_stats.buffer_streamed += size;
  m_index_buffer->CommitMemory(size);

  // printf("unmapping index %u\n", used_index_count);
}

void Deko3DDevice::PushUniformBuffer(const void* data, u32 data_size)
{
  DebugAssert(data_size < UNIFORM_PUSH_CONSTANTS_SIZE);

  dk::CmdBuf cmdbuf = GetCurrentCommandBuffer();
  cmdbuf.pushConstants(m_general_heap.GPUPointer(m_push_buffer), m_push_buffer.size, 0, data_size, data);

  s_stats.buffer_streamed += data_size;
}

void* Deko3DDevice::MapUniformBuffer(u32 size)
{
  const u32 used_space = Common::AlignUpPow2(size, DK_UNIFORM_BUF_ALIGNMENT);
  if (!m_uniform_buffer->ReserveMemory(used_space + MAX_UNIFORM_BUFFER_SIZE, DK_UNIFORM_BUF_ALIGNMENT))
  {
    SubmitCommandBuffer(false, "out of uniform space");
    if (!m_uniform_buffer->ReserveMemory(used_space + MAX_UNIFORM_BUFFER_SIZE, DK_UNIFORM_BUF_ALIGNMENT))
      Panic("Failed to allocate uniform space.");
  }

  return m_uniform_buffer->GetCurrentHostPointer();
}

void Deko3DDevice::UnmapUniformBuffer(u32 size)
{
  s_stats.buffer_streamed += size;
  DkGpuAddr gpu_addr = m_uniform_buffer->GetCurrentPointer();
  m_uniform_buffer->CommitMemory(size);

  dk::CmdBuf cmdbuf = GetCurrentCommandBuffer();
  cmdbuf.bindUniformBuffer(DkStage_Vertex, 1, gpu_addr, size);
  // hack
  // currently no geometry shader requires uniforms, so we can
  // avoid this state change
  // cmdbuf.bindUniformBuffer(DkStage_Geometry, 1, gpu_addr, size);
  cmdbuf.bindUniformBuffer(DkStage_Fragment, 1, gpu_addr, size);
}

void Deko3DDevice::SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds,
                                    GPUPipeline::RenderPassFlag render_pass_flags)
{
  bool changed = (m_num_current_render_targets != num_rts || m_current_depth_target != ds);
  bool needs_ds_clear = (ds && ds->IsClearedOrInvalidated());
  bool needs_rt_clear = false;

  dk::CmdBuf command_buffer = GetCurrentCommandBuffer();

  m_current_depth_target = static_cast<Deko3DTexture*>(ds);
  if (m_current_depth_target)
    m_current_depth_target->SetBarrierCounter(m_barrier_counter);

  // printf("binding render targets (%d) depth %p\n", num_rts, ds);
  for (u32 i = 0; i < num_rts; i++)
  {
    Deko3DTexture* const dt = static_cast<Deko3DTexture*>(rts[i]);
    changed |= m_current_render_targets[i] != dt;
    m_current_render_targets[i] = dt;
    needs_rt_clear |= dt->IsClearedOrInvalidated();

    // printf("%p\n", rts[i]);
    dt->SetBarrierCounter(m_barrier_counter);
  }
  // printf("changed %d\n", changed);
  for (u32 i = num_rts; i < m_num_current_render_targets; i++)
    m_current_render_targets[i] = nullptr;
  m_num_current_render_targets = num_rts;
  if (changed)
  {
    s_stats.num_render_passes++;

    // due to C++ stupidness this is all very hacky
    // but I don't want to give in and heap memory for something this trivial
    DkImageView color_targets[MAX_RENDER_TARGETS];
    const DkImageView* color_target_ptrs[MAX_RENDER_TARGETS] = {nullptr};
    for (size_t i = 0; i < num_rts; i++)
    {
      color_targets[i] = dk::ImageView{static_cast<Deko3DTexture*>(rts[i])->GetImage()};
      color_target_ptrs[i] = &color_targets[i];
    }

    DkImageView depth_target;
    if (ds)
      depth_target = dk::ImageView{static_cast<Deko3DTexture*>(ds)->GetImage()};

    command_buffer.bindRenderTargets(dk::detail::ArrayProxy<const DkImageView* const>(num_rts, color_target_ptrs),
                                     ds ? &depth_target : nullptr);
  }

  if (needs_rt_clear)
  {
    for (u32 i = 0; i < num_rts; i++)
    {
      Deko3DTexture* const dt = static_cast<Deko3DTexture*>(rts[i]);
      if (dt->IsClearedOrInvalidated())
      {
        CommitRTClearInFB(dt, i);
      }
    }
  }

  if (needs_ds_clear)
    CommitRTClearInFB(static_cast<Deko3DTexture*>(ds), 0);
}

void Deko3DDevice::SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler)
{
  Deko3DTexture* T = texture ? static_cast<Deko3DTexture*>(texture) : m_null_texture.get();
  Deko3DSampler* S = static_cast<Deko3DSampler*>(sampler ? sampler : m_nearest_sampler.get());
  // printf("set texture sampler %u %p %p\n", slot, texture, sampler);
  if (m_current_textures[slot] != T || m_current_samplers[slot] != S)
  {
    m_current_textures[slot] = T;
    m_current_samplers[slot] = S;
  }

  if (texture)
    CommitClear(GetCurrentCommandBuffer(), T);

  m_textures_dirty |= 1 << slot;
}

void Deko3DDevice::SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer)
{
  DebugAssert(slot == 0);
  if (m_current_texture_buffer == buffer)
    return;

  m_current_texture_buffer = static_cast<Deko3DTextureBuffer*>(buffer);

  m_textures_dirty |= 1;
}

void Deko3DDevice::CreateNullTexture()
{
  m_null_texture = Deko3DTexture::Create(1, 1, 1, 1, 1, GPUTexture::Type::RenderTarget, GPUTexture::Format::RGBA8, 0);

  u32 data = 0xFFFFFFFF;
  m_null_texture->Update(0, 0, 1, 1, &data, 4);
}

void Deko3DDevice::SetViewport(s32 x, s32 y, s32 width, s32 height)
{
  const Common::Rectangle<s32> rc = Common::Rectangle<s32>::FromExtents(x, y, width, height);
  // printf("set viewport %d %d %d %d\n", x, y, width, height);
  if (m_last_viewport == rc)
    return;

  m_last_viewport = rc;

  UpdateViewport();
}

void Deko3DDevice::SetScissor(s32 x, s32 y, s32 width, s32 height)
{
  const Common::Rectangle<s32> rc = Common::Rectangle<s32>::FromExtents(x, y, width, height);
  // printf("set scissor %d %d %d %d\n", x, y, width, height);
  if (m_last_scissor == rc)
    return;

  m_last_scissor = rc;
  UpdateScissor();
}

void Deko3DDevice::UpdateViewport()
{
  dk::CmdBuf cmdbuf = GetCurrentCommandBuffer();
  DkViewport viewport = {static_cast<float>(m_last_viewport.left),
                         static_cast<float>(m_last_viewport.top),
                         static_cast<float>(m_last_viewport.GetWidth()),
                         static_cast<float>(m_last_viewport.GetHeight()),
                         0.f,
                         1.f};
  cmdbuf.setViewports(0, {viewport});
}

void Deko3DDevice::UpdateScissor()
{
  dk::CmdBuf cmdbuf = GetCurrentCommandBuffer();
  DkScissor scissor;
  scissor.x = static_cast<u32>(m_last_scissor.left);
  scissor.y = static_cast<u32>(m_last_scissor.top);
  scissor.width = static_cast<u32>(m_last_scissor.GetWidth());
  scissor.height = static_cast<u32>(m_last_scissor.GetHeight());
  cmdbuf.setScissors(0, {scissor});
}

void Deko3DDevice::UnbindTexture(Deko3DTexture* tex)
{
  for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
  {
    if (m_current_textures[i] == tex)
    {
      m_current_textures[i] = m_null_texture.get();
      m_textures_dirty |= 1 << i;
    }
  }

  if (tex->IsRenderTarget())
  {
    for (u32 i = 0; i < m_num_current_render_targets; i++)
    {
      if (m_current_render_targets[i] == tex)
      {
        Log_WarningPrint("Unbinding current RT");
        SetRenderTargets(nullptr, 0, m_current_depth_target);
        break;
      }
    }
  }
  else if (tex->IsDepthStencil())
  {
    if (m_current_depth_target == tex)
    {
      Log_WarningPrint("Unbinding current DS");
      SetRenderTargets(nullptr, 0, nullptr);
    }
  }
}

void Deko3DDevice::PrepareTextures()
{
  if (m_textures_dirty)
  {
    CommandBuffer& frame_resources = m_frame_resources[m_current_frame];
    dk::ImageDescriptor* image_descriptors =
      m_general_heap.CPUPointer<dk::ImageDescriptor>(m_frame_resources[m_current_frame].image_descriptors);
    dk::SamplerDescriptor* sampler_descriptors =
      m_general_heap.CPUPointer<dk::SamplerDescriptor>(m_frame_resources[m_current_frame].sampler_descriptors);

    dk::CmdBuf cmdbuf = GetCurrentCommandBuffer();

    std::array<DkResHandle, GPUDevice::MAX_TEXTURE_SAMPLERS> handles;

    u32 first_dirty = CountTrailingZeros(m_textures_dirty);
    u32 last_dirty = 31 - CountLeadingZeros(m_textures_dirty);

    if (m_current_pipeline->GetLayout() == GPUPipeline::Layout::SingleTextureBufferAndPushConstants)
    {
      Deko3DTextureBuffer* texbuf = m_current_texture_buffer;

      if (texbuf)
      {
        if (texbuf->GetDescriptorFence() != GetCurrentFenceCounter())
        {
          u32 descriptor_idx = frame_resources.next_image_descriptor++;
          AssertMsg(descriptor_idx < MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME, "Ran out of image descriptors");
          dk::ImageView view{texbuf->GetImage()};
          image_descriptors[descriptor_idx].initialize(view);
          texbuf->setDescriptorIdx(descriptor_idx);
          texbuf->SetDescriptorFence(GetCurrentFenceCounter());
        }

        handles[0] = dkMakeImageHandle(texbuf->GetDescriptorIdx());
      }
    }
    else
    {
      Assert(last_dirty < MAX_TEXTURE_SAMPLERS);

      for (u32 i = 0; i <= last_dirty; i++)
      {
        Deko3DTexture* texture = m_current_textures[i];

        if (!texture)
          continue;

        if (texture->GetDescriptorFence() != GetCurrentFenceCounter())
        {
          u32 descriptor_idx = frame_resources.next_image_descriptor++;
          AssertMsg(descriptor_idx < MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME, "Ran out of image descriptors");
          memcpy(&image_descriptors[descriptor_idx], &texture->GetDescriptor(), sizeof(dk::ImageDescriptor));
          texture->setDescriptorIdx(descriptor_idx);
          texture->SetDescriptorFence(GetCurrentFenceCounter());
        }

        Deko3DSampler* sampler = m_current_samplers[i];
        if (sampler->GetDescriptorFence() != GetCurrentFenceCounter())
        {
          u32 descriptor_idx = frame_resources.next_sampler_descriptor++;
          AssertMsg(descriptor_idx < MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME,
                    "Ran out of sampler descriptors");
          memcpy(&sampler_descriptors[descriptor_idx], &sampler->GetDescriptor(), sizeof(dk::SamplerDescriptor));
          sampler->setDescriptorIdx(descriptor_idx);
          sampler->SetDescriptorFence(GetCurrentFenceCounter());
        }

        handles[i] = dkMakeTextureHandle(texture->GetDescriptorIdx(), sampler->GetDescriptorIdx());
      }
    }

    cmdbuf.bindTextures(DkStage_Fragment, first_dirty,
                        dk::detail::ArrayProxy<const u32>(last_dirty - first_dirty + 1, &handles[first_dirty]));

    m_textures_dirty = 0;
  }
}

void Deko3DDevice::Draw(u32 vertex_count, u32 base_vertex)
{
  PrepareTextures();
  // printf("drawing %u %u %u\n", m_current_pipeline->GetTopology(), base_vertex, vertex_count);

  s_stats.num_draws++;
  GetCurrentCommandBuffer().draw(m_current_pipeline->GetTopology(), vertex_count, 1, base_vertex, 0);
}

void Deko3DDevice::DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex)
{
  PrepareTextures();

  s_stats.num_draws++;
  // printf("drawing indexed %u %u %u %u\n", m_current_pipeline->GetTopology(), index_count, base_index, base_vertex);
  GetCurrentCommandBuffer().drawIndexed(m_current_pipeline->GetTopology(), index_count, 1, base_index, base_vertex, 0);
}

void Deko3DDevice::DrawIndexedWithBarrier(u32 index_count, u32 base_index, u32 base_vertex, DrawBarrier type)
{
  Panic("miauz");
}

bool Deko3DDevice::BeginPresent(bool skip_present)
{
  if (skip_present)
    return false;

  // svcSleepThread(1000 * 1000 * 20);

  m_swap_chain->AcquireNextImage();
  m_swap_chain->ReleaseImage();

  Deko3DTexture* swapchain_image = m_swap_chain->GetCurrentImage();

  ClearRenderTarget(swapchain_image, 0);
  SetRenderTarget(swapchain_image);

  SetScissor(0, 0, swapchain_image->GetWidth(), swapchain_image->GetHeight());
  SetViewport(0, 0, swapchain_image->GetWidth(), swapchain_image->GetHeight());

  return true;
}

void Deko3DDevice::EndPresent(bool explicit_submit)
{
  SubmitCommandBuffer(m_swap_chain.get());
  MoveToNextCommandBuffer();
  TrimTexturePool();
}

void Deko3DDevice::SubmitPresent()
{
}

bool Deko3DDevice::SetGPUTimingEnabled(bool enabled)
{
  // TODO: implement GPU timing for Switch
  return false;
}

float Deko3DDevice::GetAndResetAccumulatedGPUTime()
{
  return 0.f;
}
