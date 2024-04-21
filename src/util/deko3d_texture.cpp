#include "deko3d_texture.h"

#include "common/align.h"
#include "common/log.h"
#include "common/string_util.h"

#include "deko3d_device.h"

Log_SetChannel(Deko3D_Texture);

std::unique_ptr<Deko3DTexture> Deko3DTexture::Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                     Type type, Format format, uint32_t flags)
{
  DkImageType dk_image_type;
  if (layers > 1)
    dk_image_type = samples > 1 ? DkImageType_2DMSArray : DkImageType_2DArray;
  else
    dk_image_type = samples > 1 ? DkImageType_2DMS : DkImageType_2D;

  static constexpr std::array<DkImageFormat, static_cast<u32>(Format::MaxCount)> dk_image_format_mapping{
    {DkImageFormat_None,         DkImageFormat_RGBA8_Unorm,  DkImageFormat_BGRA8_Unorm,  DkImageFormat_BGR565_Unorm,
     DkImageFormat_BGR5A1_Unorm, DkImageFormat_R8_Unorm,     DkImageFormat_Z16,          DkImageFormat_R16_Unorm,
     DkImageFormat_R16_Sint,     DkImageFormat_R16_Uint,     DkImageFormat_R16_Float,    DkImageFormat_R32_Sint,
     DkImageFormat_R32_Uint,     DkImageFormat_R32_Float,    DkImageFormat_RG8_Unorm,    DkImageFormat_RG16_Unorm,
     DkImageFormat_RG16_Float,   DkImageFormat_RG32_Float,   DkImageFormat_RGBA16_Unorm, DkImageFormat_RGBA16_Float,
     DkImageFormat_RGBA32_Float, DkImageFormat_RGB10A2_Unorm}};

  Deko3DDevice& dev = Deko3DDevice::GetInstance();

  Log_TraceFmt("Creating texture {}x{} levels={} levels={} samples={} type={} format={} flags={}", width, height,
               layers, levels, samples, static_cast<u32>(type), static_cast<u32>(format), flags);

  dk::ImageLayout layout;
  dk::ImageLayoutMaker{dev.GetDevice()}
    .setDimensions(width, height, layers)
    .setMipLevels(levels)
    .setType(dk_image_type)
    .setMsMode(static_cast<DkMsMode>(__builtin_ctz(samples) - 1))
    .setFormat(dk_image_format_mapping[static_cast<u32>(format)])
    .setFlags(flags)
    .initialize(layout);

  auto memory = dev.GetTextureHeap().Alloc(layout.getSize(), layout.getAlignment());

  return std::unique_ptr<Deko3DTexture>(
    new Deko3DTexture(width, height, layers, levels, samples, type, format, layout, memory));
}

std::unique_ptr<GPUTexture> Deko3DDevice::CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                        GPUTexture::Type type, GPUTexture::Format format,
                                                        const void* data, u32 data_stride)
{
  u32 flags = 0;
  if (type == GPUTexture::Type::RenderTarget || type == GPUTexture::Type::DepthStencil)
    flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;

  std::unique_ptr<Deko3DTexture> tex =
    Deko3DTexture::Create(width, height, layers, levels, samples, type, format, flags);

  if (tex && data)
    tex->Update(0, 0, width, height, data, data_stride);
  /*else
  {
    // pretty terrible
    void* data = calloc(width * height * GPUTexture::GetPixelSize(format), 1);
    tex->Update(0, 0, width, height, data, width * GPUTexture::GetPixelSize(format));
    free(data);
  }*/

  return tex;
}

void Deko3DTexture::MakeReadyForSampling()
{
  Deko3DDevice& device = Deko3DDevice::GetInstance();
  if (device.GetCurrentBarrierCounter() == m_barrier_counter)
  {
    dk::CmdBuf cmdbuf = device.GetCurrentCommandBuffer();
    cmdbuf.barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);

    device.IncreaseBarrierCounter();
  }
}

Deko3DTexture::Deko3DTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format,
                             const dk::ImageLayout& layout, const Deko3DMemoryHeap::Allocation& memory)
  : GPUTexture(width, height, layers, levels, samples, type, format), m_memory(memory)
{
  auto& textureHeap = Deko3DDevice::GetInstance().GetTextureHeap();
  m_image.initialize(layout, textureHeap.GetMemBlock(), m_memory.offset);

  dk::ImageView view{m_image};
  m_descriptor.initialize(view);
}

Deko3DTexture::~Deko3DTexture()
{
  if (m_memory.size)
    Destroy(true);
}

void Deko3DTexture::SetDebugName(const std::string_view& name)
{
  // nop
}

void Deko3DTexture::Destroy(bool defer)
{
  Deko3DDevice& dev = Deko3DDevice::GetInstance();
  dev.UnbindTexture(this);

  if (defer)
    dev.DeferedFree(dev.GetTextureHeap(), m_memory);
  else
    dev.GetTextureHeap().Free(m_memory);

  m_memory = {};
}

bool Deko3DTexture::Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer, u32 level)
{
  DebugAssert(layer < m_layers && level < m_levels);
  DebugAssert((x + width) <= GetMipWidth(level) && (y + height) <= GetMipHeight(level));

  const u32 upload_pitch = Common::AlignUpPow2(pitch, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
  const u32 required_size = height * upload_pitch;
  Deko3DDevice& dev = Deko3DDevice::GetInstance();
  Deko3DStreamBuffer* sbuffer = dev.GetTextureUploadBuffer();

  // If the texture is larger than half our streaming buffer size, use a separate buffer.
  // Otherwise allocation will either fail, or require lots of cmdbuffer submissions.
  Deko3DMemoryHeap::Allocation buffer;
  u32 buffer_offset;
  if (required_size > (sbuffer->GetCurrentSize() / 2))
  {
    buffer_offset = 0;
    buffer = AllocateUploadStagingBuffer(data, pitch, upload_pitch, width, height);
  }
  else
  {
    if (!sbuffer->ReserveMemory(required_size, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT))
    {
      dev.SubmitCommandBuffer(false, "While waiting for %u bytes in texture upload buffer", required_size);
      if (!sbuffer->ReserveMemory(required_size, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT))
      {
        Log_ErrorPrintf("Failed to reserve texture upload memory (%u bytes).", required_size);
        return false;
      }
    }

    buffer = sbuffer->GetBuffer();
    buffer_offset = sbuffer->GetCurrentOffset();
    CopyTextureDataForUpload(sbuffer->GetCurrentHostPointer(), data, width, height, pitch, upload_pitch);
    sbuffer->CommitMemory(required_size);
  }

  GPUDevice::GetStatistics().buffer_streamed += required_size;
  GPUDevice::GetStatistics().num_uploads++;

  dk::CmdBuf cmdbuf = GetCommandBufferForUpdate();

  // if we're an rt and have been cleared, and the full rect isn't being uploaded, do the clear
  if (m_type == Type::RenderTarget)
  {
    if (m_state == State::Cleared && (x != 0 || y != 0 || width != m_width || height != m_height))
      dev.CommitClear(cmdbuf, this);
    else
      m_state = State::Dirty;
  }

  UpdateFromBuffer(cmdbuf, x, y, width, height, layer, level, upload_pitch,
                   dev.GetGeneralHeap().GPUPointer(buffer) + buffer_offset);
  return true;
}

bool Deko3DTexture::Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer, u32 level)
{
  if ((x + width) > GetMipWidth(level) || (y + height) > GetMipHeight(level) || layer > m_layers || level > m_levels)
  {
    return false;
  }

  Deko3DDevice& dev = Deko3DDevice::GetInstance();
  if (m_state == GPUTexture::State::Cleared && (x != 0 || y != 0 || width != m_width || height != m_height))
    dev.CommitClear(GetCommandBufferForUpdate(), this);

  // see note in Update() for the reason why.
  const u32 aligned_pitch = Common::AlignUpPow2(width * GetPixelSize(), DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
  const u32 req_size = height * aligned_pitch;
  Deko3DStreamBuffer* buffer = dev.GetTextureUploadBuffer();
  if (req_size >= (buffer->GetCurrentSize() / 2))
    return false;

  if (!buffer->ReserveMemory(req_size, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT))
  {
    dev.SubmitCommandBuffer(false, "While waiting for %u bytes in texture upload buffer", req_size);
    if (!buffer->ReserveMemory(req_size, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT))
      Panic("Failed to reserve texture upload memory");
  }

  // map for writing
  *map = buffer->GetCurrentHostPointer();
  *map_stride = aligned_pitch;
  m_map_x = static_cast<u16>(x);
  m_map_y = static_cast<u16>(y);
  m_map_width = static_cast<u16>(width);
  m_map_height = static_cast<u16>(height);
  m_map_layer = static_cast<u8>(layer);
  m_map_level = static_cast<u8>(level);
  m_state = GPUTexture::State::Dirty;
  return true;
}

void Deko3DTexture::Unmap()
{
  Deko3DDevice& dev = Deko3DDevice::GetInstance();
  Deko3DStreamBuffer* sb = dev.GetTextureUploadBuffer();
  const u32 aligned_pitch = Common::AlignUpPow2(m_map_width * GetPixelSize(), DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
  const u32 req_size = m_map_height * aligned_pitch;
  const u32 offset = sb->GetCurrentOffset();
  sb->CommitMemory(req_size);

  GPUDevice::GetStatistics().buffer_streamed += req_size;
  GPUDevice::GetStatistics().num_uploads++;

  // first time the texture is used? don't leave it undefined
  dk::CmdBuf cmdbuf = GetCommandBufferForUpdate();

  UpdateFromBuffer(cmdbuf, m_map_x, m_map_y, m_map_width, m_map_height, m_map_layer, m_map_level, aligned_pitch,
                   dev.GetGeneralHeap().GPUPointer(sb->GetBuffer()) + offset);

  m_map_x = 0;
  m_map_y = 0;
  m_map_width = 0;
  m_map_height = 0;
  m_map_layer = 0;
  m_map_level = 0;
}

void Deko3DDevice::CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                     GPUTexture* src, u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width,
                                     u32 height)
{
  Deko3DTexture* const S = static_cast<Deko3DTexture*>(src);
  Deko3DTexture* const D = static_cast<Deko3DTexture*>(dst);

  dk::CmdBuf command_buffer = GetCurrentCommandBuffer();

  if (S->GetState() == GPUTexture::State::Cleared)
  {
    // source is cleared. if destination is a render target, we can carry the clear forward
    if (D->IsRenderTargetOrDepthStencil())
    {
      if (dst_level == 0 && dst_x == 0 && dst_y == 0 && width == D->GetWidth() && height == D->GetHeight())
      {
        // pass it forward if we're clearing the whole thing
        if (S->IsDepthStencil())
          D->SetClearDepth(S->GetClearDepth());
        else
          D->SetClearColor(S->GetClearColor());

        return;
      }

      if (D->GetState() == GPUTexture::State::Cleared)
      {
        // destination is cleared, if it's the same colour and rect, we can just avoid this entirely
        if (D->IsDepthStencil())
        {
          if (D->GetClearDepth() == S->GetClearDepth())
            return;
        }
        else
        {
          if (D->GetClearColor() == S->GetClearColor())
            return;
        }
      }

      // TODO: Could use attachment clear here..
    }

    // commit the clear to the source first, then do normal copy
    CommitClear(command_buffer, S);
  }

  // if the destination has been cleared, and we're not overwriting the whole thing, commit the clear first
  // (the area outside of where we're copying to)
  if (D->GetState() == GPUTexture::State::Cleared &&
      (dst_level != 0 || dst_x != 0 || dst_y != 0 || width != D->GetWidth() || height != D->GetHeight()))
  {
    CommitClear(command_buffer, D);
  }

  dk::ImageView src_view{S->GetImage()};
  src_view.setMipLevels(src_level);
  dk::ImageView dst_view{D->GetImage()};
  dst_view.setMipLevels(dst_level);
  command_buffer.copyImage(src_view, {src_x, src_y, src_layer, width, height, 1}, dst_view,
                           {dst_x, dst_y, dst_layer, width, height, 1});

  s_stats.num_copies++;

  D->SetState(GPUTexture::State::Dirty);
}

void Deko3DDevice::ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                        GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height)
{
  DebugAssert((src_x + width) <= src->GetWidth());
  DebugAssert((src_y + height) <= src->GetHeight());
  DebugAssert(src->IsMultisampled());
  DebugAssert(dst_level < dst->GetLevels() && dst_layer < dst->GetLayers());
  DebugAssert((dst_x + width) <= dst->GetMipWidth(dst_level));
  DebugAssert((dst_y + height) <= dst->GetMipHeight(dst_level));
  DebugAssert(!dst->IsMultisampled() && src->IsMultisampled());

  s_stats.num_copies++;

  Deko3DTexture* D = static_cast<Deko3DTexture*>(dst);
  Deko3DTexture* S = static_cast<Deko3DTexture*>(src);
  dk::CmdBuf cmdbuf = GetCurrentCommandBuffer();

  if (S->GetState() == GPUTexture::State::Cleared)
    CommitClear(cmdbuf, S);
  if (D->IsRenderTargetOrDepthStencil() && D->GetState() == GPUTexture::State::Cleared)
  {
    if (width < dst->GetWidth() || height < dst->GetHeight())
      CommitClear(cmdbuf, D);
    else
      D->SetState(GPUTexture::State::Dirty);
  }

  dk::ImageView src_view{S->GetImage()};
  dk::ImageView dst_view{D->GetImage()};

  cmdbuf.blitImage(src_view, {src_x, src_y, 0, width, height, 1}, dst_view, {dst_x, dst_y, dst_layer, width, height, 1},
                   DkBlitFlag_FilterLinear);
}

dk::CmdBuf Deko3DTexture::GetCommandBufferForUpdate()
{
  Deko3DDevice& dev = Deko3DDevice::GetInstance();
  // if ((m_type != Type::Texture && m_type != Type::DynamicTexture) || m_fence_counter == dev.GetCurrentFenceCounter())
  {
    return dev.GetCurrentCommandBuffer();
  }

  // return dev.GetCurrentInitCommandBuffer();
}

void Deko3DTexture::CopyTextureDataForUpload(void* dst, const void* src, u32 width, u32 height, u32 pitch,
                                             u32 upload_pitch) const
{
  StringUtil::StrideMemCpy(dst, upload_pitch, src, pitch, GetPixelSize() * width, height);
}

Deko3DMemoryHeap::Allocation Deko3DTexture::AllocateUploadStagingBuffer(const void* data, u32 pitch, u32 upload_pitch,
                                                                        u32 width, u32 height) const
{
  const u32 size = upload_pitch * height;
  auto& device = Deko3DDevice::GetInstance();
  auto buffer = device.GetGeneralHeap().Alloc(size, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);

  // Immediately queue it for freeing after the command buffer finishes, since it's only needed for the copy.
  device.DeferedFree(device.GetGeneralHeap(), buffer);

  // And write the data.
  CopyTextureDataForUpload(device.GetGeneralHeap().CPUPointer<void>(buffer), data, width, height, pitch, upload_pitch);
  return buffer;
}

void Deko3DTexture::UpdateFromBuffer(dk::CmdBuf cmdbuf, u32 x, u32 y, u32 width, u32 height, u32 layer, u32 level,
                                     u32 pitch, DkGpuAddr buffer)
{
  dk::ImageView dstView{m_image};
  dstView.setMipLevels(level);

  cmdbuf.copyBufferToImage(DkCopyBuf{buffer, pitch, 0}, dstView, DkImageRect{x, y, layer, width, height, 1});
}

// Texture buffers

std::unique_ptr<GPUTextureBuffer> Deko3DDevice::CreateTextureBuffer(GPUTextureBuffer::Format format,
                                                                    u32 size_in_elements)
{
  printf("creating texture buffer\n");
  Deko3DDevice& dev = Deko3DDevice::GetInstance();
  const u32 buffer_size = GPUTextureBuffer::GetElementSize(format) * size_in_elements;

  static constexpr std::array<DkImageFormat, static_cast<u8>(GPUTextureBuffer::Format::MaxCount)> format_mapping = {{
    DkImageFormat_R16_Uint, // R16UI
  }};

  std::unique_ptr<Deko3DStreamBuffer> buffer = Deko3DStreamBuffer::Create(buffer_size);

  dk::ImageLayout layout;
  dk::ImageLayoutMaker{dev.GetDevice()}
    .setType(DkImageType_Buffer)
    .setDimensions(size_in_elements)
    .setFormat(format_mapping[static_cast<u8>(format)])
    .initialize(layout);

  return std::unique_ptr<GPUTextureBuffer>(
    new Deko3DTextureBuffer(format, size_in_elements, std::move(buffer), layout));
}

Deko3DTextureBuffer::Deko3DTextureBuffer(Format format, u32 size_in_elements,
                                         std::unique_ptr<Deko3DStreamBuffer> buffer, const dk::ImageLayout& layout)
  : GPUTextureBuffer(format, size_in_elements), m_buffer(std::move(buffer))
{
  m_image.initialize(layout, Deko3DDevice::GetInstance().GetGeneralHeap().GetMemBlock(), m_buffer->GetBuffer().offset);
}

Deko3DTextureBuffer::~Deko3DTextureBuffer()
{
}

void Deko3DTextureBuffer::SetDebugName(const std::string_view& name)
{
  // nop
}

void* Deko3DTextureBuffer::Map(u32 required_elements)
{
  const u32 esize = GetElementSize(m_format);
  const u32 req_size = esize * required_elements;
  if (!m_buffer->ReserveMemory(req_size, esize))
  {
    Deko3DDevice::GetInstance().SubmitCommandBuffer(false, "out of space in texture buffer");
    if (!m_buffer->ReserveMemory(req_size, esize))
      Panic("Failed to allocate texture buffer space.");
  }

  m_current_position = m_buffer->GetCurrentOffset() / esize;
  return m_buffer->GetCurrentHostPointer();
}

void Deko3DTextureBuffer::Unmap(u32 used_elements)
{
  const u32 size = GetElementSize(m_format) * used_elements;
  GPUDevice::GetStatistics().buffer_streamed += size;
  GPUDevice::GetStatistics().num_uploads++;
  m_buffer->CommitMemory(size);
}

std::unique_ptr<GPUSampler> Deko3DDevice::CreateSampler(const GPUSampler::Config& config)
{
  static constexpr std::array<DkWrapMode, static_cast<u8>(GPUSampler::AddressMode::MaxCount)> ta = {{
    DkWrapMode_Repeat,         // Repeat
    DkWrapMode_ClampToEdge,    // ClampToEdge
    DkWrapMode_ClampToBorder,  // ClampToBorder
    DkWrapMode_MirroredRepeat, // MirrorRepeat
  }};

  static constexpr std::array<DkFilter, static_cast<u8>(GPUSampler::Filter::MaxCount)> filter = {
    {DkFilter_Nearest, DkFilter_Linear}};
  static constexpr std::array<DkMipFilter, static_cast<u8>(GPUSampler::Filter::MaxCount)> mip_filter = {
    {DkMipFilter_Nearest, DkMipFilter_Linear}};

  dk::Sampler sampler;
  sampler.setFilter(filter[static_cast<u8>(config.min_filter.GetValue())],
                    filter[static_cast<u8>(config.mag_filter.GetValue())],
                    mip_filter[static_cast<u8>(config.mip_filter.GetValue())]);
  sampler.setWrapMode(ta[static_cast<u8>(config.address_u.GetValue())],
                      ta[static_cast<u8>(config.address_v.GetValue())],
                      ta[static_cast<u8>(config.address_w.GetValue())]);
  sampler.setLodClamp(static_cast<float>(config.min_lod), static_cast<float>(config.max_lod));
  sampler.setBorderColor(config.GetBorderRed(), config.GetBorderGreen(), config.GetBorderBlue(),
                         config.GetBorderAlpha());
  sampler.setMaxAnisotropy(static_cast<float>(config.anisotropy.GetValue()));

  dk::SamplerDescriptor descriptor;
  descriptor.initialize(sampler);

  return std::unique_ptr<Deko3DSampler>(new Deko3DSampler(descriptor));
}

Deko3DSampler::Deko3DSampler(const dk::SamplerDescriptor& descriptor) : m_descriptor(descriptor)
{
}

Deko3DSampler::~Deko3DSampler()
{
}

void Deko3DSampler::SetDebugName(const std::string_view& name)
{
  // nop
}

bool Deko3DDevice::SupportsTextureFormat(GPUTexture::Format format) const
{
  // deko3D/Tegra should support all texture formats, yay!
  return true;
}

void Deko3DDevice::CommitClear(dk::CmdBuf command_buffer, Deko3DTexture* tex)
{
  if (tex->GetState() == GPUTexture::State::Dirty)
    return;

  std::array<GPUTexture*, MAX_RENDER_TARGETS> restore_rts;
  for (size_t i = 0; i < m_num_current_render_targets; i++)
    restore_rts[i] = m_current_render_targets[i];
  u32 restore_rt_num = m_num_current_render_targets;
  GPUTexture* restore_depth_rt = m_current_depth_target;

  tex->SetState(GPUTexture::State::Dirty);
  tex->SetBarrierCounter(m_barrier_counter);

  dk::ImageView view{tex->GetImage()};
  if (tex->IsDepthStencil())
  {
    command_buffer.bindRenderTargets({}, &view);

    m_num_current_render_targets = 0;
    m_current_depth_target = tex;
  }
  else
  {
    command_buffer.bindRenderTargets({&view});

    m_num_current_render_targets = 1;
    m_current_render_targets[0] = tex;
    m_current_depth_target = nullptr;
  }

  if (tex->GetState() == GPUTexture::State::Cleared)
  {
    Common::Rectangle<s32> restore_rect = m_last_scissor;
    SetScissor(0, 0, tex->GetWidth(), tex->GetHeight());

    if (tex->IsDepthStencil())
    {
      const float depth = tex->GetClearDepth();
      command_buffer.clearDepthStencil(true, depth, 0xFF, 0);
    }
    else
    {
      GPUPipeline::BlendState blend_state = m_last_blend_state;
      u32 restore_write_mask = blend_state.write_mask;
      blend_state.write_mask = 0xF;
      ApplyBlendState(blend_state);

      const auto color = tex->GetUNormClearColor();
      command_buffer.clearColor(0, DkColorMask_RGBA, color[0], color[1], color[2], color[3]);

      blend_state.write_mask = restore_write_mask;
      ApplyBlendState(blend_state);
    }

    SetScissor(restore_rect.left, restore_rect.top, restore_rect.GetWidth(), restore_rect.GetHeight());
  }
  else // tex->GetState() == GPUTexture::State::Invalidated
  {
    tex->MakeReadyForSampling();

    if (tex->IsDepthStencil())
    {
      command_buffer.discardDepthStencil();
    }
    else
    {
      command_buffer.discardColor(0);
    }
  }

  SetRenderTargets(restore_rts.data(), restore_rt_num, restore_depth_rt);
}

void Deko3DDevice::CommitRTClearInFB(Deko3DTexture* tex, u32 idx)
{
  dk::CmdBuf command_buffer = GetCurrentCommandBuffer();

  switch (tex->GetState())
  {
    case GPUTexture::State::Invalidated:
    {
      tex->MakeReadyForSampling();

      if (tex->IsDepthStencil())
        command_buffer.discardDepthStencil();
      else
        command_buffer.discardColor(idx);

      tex->SetState(GPUTexture::State::Dirty);
    }
    break;

    case GPUTexture::State::Cleared:
    {
      const auto color = tex->GetUNormClearColor();

      Common::Rectangle<s32> restore_rect = m_last_scissor;
      SetScissor(0, 0, tex->GetWidth(), tex->GetHeight());

      if (tex->IsDepthStencil())
      {
        command_buffer.clearDepthStencil(true, tex->GetClearDepth(), 0xFF, 0);
      }
      else
      {
        GPUPipeline::BlendState blend_state = m_last_blend_state;
        u32 restore_write_mask = blend_state.write_mask;
        blend_state.write_mask = 0xF;
        ApplyBlendState(blend_state);

        command_buffer.clearColor(idx, DkColorMask_RGBA, color[0], color[1], color[2], color[3]);

        blend_state.write_mask = restore_write_mask;
        ApplyBlendState(blend_state);
      }

      SetScissor(restore_rect.left, restore_rect.top, restore_rect.GetWidth(), restore_rect.GetHeight());

      tex->SetState(GPUTexture::State::Dirty);
    }

    case GPUTexture::State::Dirty:
      break;

    default:
      UnreachableCode();
      break;
  }
}

Deko3DDownloadTexture::~Deko3DDownloadTexture()
{
  Deko3DDevice::GetInstance().DeferedFree(Deko3DDevice::GetInstance().GetGeneralHeap(), m_buffer);
}

std::unique_ptr<Deko3DDownloadTexture> Deko3DDownloadTexture::Create(u32 width, u32 height, GPUTexture::Format format,
                                                                     void* memory, size_t memory_size, u32 memory_pitch)
{
  DebugAssertMsg(!memory, "Importing buffer for download textures not supported on deko3D.");

  Deko3DDevice& dev = Deko3DDevice::GetInstance();

  u32 map_pitch = Common::AlignUpPow2(GPUTexture::CalcUploadPitch(format, width), DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
  u32 buffer_size = map_pitch * height;

  Deko3DMemoryHeap::Allocation buffer = dev.GetGeneralHeap().Alloc(buffer_size, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);

  return std::unique_ptr<Deko3DDownloadTexture>(new Deko3DDownloadTexture(
    width, height, format, false, buffer, dev.GetGeneralHeap().CPUPointer<u8>(buffer), map_pitch));
}

Deko3DDownloadTexture::Deko3DDownloadTexture(u32 width, u32 height, GPUTexture::Format format, bool is_imported,
                                             Deko3DMemoryHeap::Allocation buffer, const u8* map_ptr, u32 map_pitch)
  : GPUDownloadTexture(width, height, format, is_imported), m_buffer(buffer)
{
  m_map_pointer = map_ptr;
  m_current_pitch = map_pitch;
}

void Deko3DDownloadTexture::CopyFromTexture(u32 dst_x, u32 dst_y, GPUTexture* src, u32 src_x, u32 src_y, u32 width,
                                            u32 height, u32 src_layer, u32 src_level, bool use_transfer_pitch)
{
  Deko3DTexture* T = static_cast<Deko3DTexture*>(src);
  Deko3DDevice& dev = Deko3DDevice::GetInstance();
  dk::CmdBuf cmdbuf = dev.GetCurrentCommandBuffer();

  dev.CommitClear(cmdbuf, T);

  dev.GetStatistics().num_downloads++;

  dk::ImageView src_view{T->GetImage()};
  if (T->GetFormat() == GPUTexture::Format::D16)
    src_view.setFormat(DkImageFormat_R16_Uint);

  cmdbuf.copyImageToBuffer(src_view, DkImageRect{src_x, src_y, 0, width, height, 1},
                           DkCopyBuf{dev.GetGeneralHeap().GPUPointer(m_buffer) + dst_y * m_current_pitch +
                                       dst_x * GPUTexture::GetPixelSize(m_format),
                                     m_current_pitch, 0});
  cmdbuf.barrier(DkBarrier_Primitives, DkInvalidateFlags_L2Cache);

  m_copy_fence_counter = dev.GetCurrentFenceCounter();
  m_needs_flush = true;
}

bool Deko3DDownloadTexture::Map(u32 x, u32 y, u32 width, u32 height)
{
  // the buffer is persistently mapped
  return true;
}

void Deko3DDownloadTexture::Unmap()
{
  // persistently mapped too
}

void Deko3DDownloadTexture::Flush()
{
  if (!m_needs_flush)
    return;

  Deko3DDevice& dev = Deko3DDevice::GetInstance();
  if (dev.GetCompletedFenceCounter() >= m_copy_fence_counter)
    return;

  // Need to execute command buffer.
  if (dev.GetCurrentFenceCounter() == m_copy_fence_counter)
    dev.SubmitCommandBuffer(true);
  else
    dev.WaitForFenceCounter(m_copy_fence_counter);
}

void Deko3DDownloadTexture::SetDebugName(std::string_view name)
{
  // nop
}

std::unique_ptr<GPUDownloadTexture> Deko3DDevice::CreateDownloadTexture(u32 width, u32 height,
                                                                        GPUTexture::Format format)
{
  return Deko3DDownloadTexture::Create(width, height, format, nullptr, 0, 0);
}

std::unique_ptr<GPUDownloadTexture> Deko3DDevice::CreateDownloadTexture(u32 width, u32 height,
                                                                        GPUTexture::Format format, void* memory,
                                                                        size_t memory_size, u32 memory_stride)
{
  return Deko3DDownloadTexture::Create(width, height, format, memory, memory_size, memory_stride);
}
