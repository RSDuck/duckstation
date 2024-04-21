#include "deko3d_pipeline.h"

#include "common/log.h"

#include <cstring>

#include <uam.h>

#include "deko3d_device.h"

Log_SetChannel(Deko3D_Pipeline);

Deko3DInternalShader::Deko3DInternalShader(const dk::Shader& shader, Deko3DMemoryHeap::Allocation memory)
  : shader(shader), memory(memory)
{
}

Deko3DInternalShader::~Deko3DInternalShader()
{
  Deko3DDevice::GetInstance().DeferedFree(Deko3DDevice::GetInstance().GetShaderHeap(), memory);
}

Deko3DShader::Deko3DShader(GPUShaderStage stage, dk::Shader shader, Deko3DMemoryHeap::Allocation memory)
  : GPUShader(stage), m_internal_shader(std::make_shared<Deko3DInternalShader>(shader, memory))
{
}

// https://github.com/switchbrew/switch-examples/blob/master/graphics/deko3d/deko_examples/source/SampleFramework/CShader.cpp#L7
struct DkshHeader
{
  uint32_t magic;     // DKSH_MAGIC
  uint32_t header_sz; // sizeof(DkshHeader)
  uint32_t control_sz;
  uint32_t code_sz;
  uint32_t programs_off;
  uint32_t num_programs;
};

std::unique_ptr<GPUShader> Deko3DDevice::CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data)
{
  auto& device = Deko3DDevice::GetInstance();
  auto& shaderHeap = device.GetShaderHeap();

  const DkshHeader* header = reinterpret_cast<const DkshHeader*>(data.data());
  u8* control = new u8[header->control_sz];
  memcpy(control, &data[0], header->control_sz);

  auto memory = shaderHeap.Alloc(header->code_sz, DK_SHADER_CODE_ALIGNMENT);
  std::memcpy(shaderHeap.CPUPointer<void>(memory), &data[header->control_sz], data.size_bytes());

  dk::Shader shader;
  dk::ShaderMaker{shaderHeap.GetMemBlock(), memory.offset}.setControl(control).setProgramId(0).initialize(shader);

  delete[] control;

  return std::unique_ptr<Deko3DShader>(new Deko3DShader(stage, shader, memory));
}

std::unique_ptr<GPUShader> Deko3DDevice::CreateShaderFromSource(GPUShaderStage stage, const std::string_view& source,
                                                                const char* entry_point,
                                                                DynamicHeapArray<u8>* out_binary)
{
  if (stage >= GPUShaderStage::MaxCount)
  {
    Log_ErrorPrintf("Unknown shader stage %u\n", static_cast<u32>(stage));
    return {};
  }
  if (std::strcmp(entry_point, "main") != 0)
  {
    Log_ErrorPrintf("Entry point must be 'main', but got '%s' instead.", entry_point);
    return {};
  }

  const uam_pipeline_stage to_uam_stage[] = {uam_pipeline_stage_vertex, uam_pipeline_stage_fragment,
                                             uam_pipeline_stage_geometry, uam_pipeline_stage_compute};

  std::string sourceNullTerminated(source);

  u8* shader_out;
  u32 shader_size;
  if (!uam_compileDksh(to_uam_stage[static_cast<u32>(stage)], sourceNullTerminated.c_str(), 3, &shader_out,
                       &shader_size))
  {
    const char* const stageStrings[] = {"vertex", "fragment", "geometry", "compute"};
    Log_ErrorPrintf("Failed to compile %s shader:\n%s", stageStrings[static_cast<u32>(stage)],
                    sourceNullTerminated.c_str());
    return {};
  }

  auto result = CreateShaderFromBinary(
    stage, std::span<const u8>(static_cast<u8* const>(shader_out), static_cast<size_t>(shader_size)));

  std::free(shader_out);

  return result;
}

Deko3DPipeline::~Deko3DPipeline()
{
}

void Deko3DPipeline::SetDebugName(const std::string_view& name)
{
  // not implementable
}

Deko3DPipeline::Deko3DPipeline(Layout layout, const RasterizationState& rs, const DepthState& ds, const BlendState& bs,
                               DkPrimitive topology, u32 num_attribs,
                               const std::array<DkVtxAttribState, VertexAttribute::MaxAttributes>& attribs, u32 stride,
                               std::shared_ptr<Deko3DInternalShader> vtx_shader,
                               std::shared_ptr<Deko3DInternalShader> frg_shader,
                               std::shared_ptr<Deko3DInternalShader> geom_shader)
  : m_layout(layout), m_blend_state(bs), m_rasterization_state(rs), m_depth_state(ds), m_topology(topology),
    m_vertex_shader(vtx_shader), m_fragment_shader(frg_shader), m_geometry_shader(geom_shader), m_attributes(attribs),
    m_num_attributes(num_attribs), m_stride(stride)
{
}

std::unique_ptr<GPUPipeline> Deko3DDevice::CreatePipeline(const GPUPipeline::GraphicsConfig& config)
{
  static constexpr std::array<DkPrimitive, static_cast<u32>(GPUPipeline::Primitive::MaxCount)> primitives = {{
    DkPrimitive_Points,        // Points
    DkPrimitive_Lines,         // Lines
    DkPrimitive_Triangles,     // Triangles
    DkPrimitive_TriangleStrip, // TriangleStrips
  }};

  std::shared_ptr<Deko3DInternalShader> vtx_shader =
    static_cast<Deko3DShader*>(config.vertex_shader)->GetInternalShader();
  std::shared_ptr<Deko3DInternalShader> frg_shader =
    static_cast<Deko3DShader*>(config.fragment_shader)->GetInternalShader();
  std::shared_ptr<Deko3DInternalShader> geom_shader =
    config.geometry_shader ? static_cast<Deko3DShader*>(config.geometry_shader)->GetInternalShader() : nullptr;

  struct VAMapping
  {
    DkVtxAttribType type;
    std::array<DkVtxAttribSize, 4> sizes;
  };
  static constexpr const std::array<VAMapping, static_cast<u8>(GPUPipeline::VertexAttribute::Type::MaxCount)>
    format_mapping = {{
      // Float
      {DkVtxAttribType_Float, {DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32}},
      // UInt8
      {DkVtxAttribType_Uint, {DkVtxAttribSize_1x8, DkVtxAttribSize_2x8, DkVtxAttribSize_3x8, DkVtxAttribSize_4x8}},
      // SInt8
      {DkVtxAttribType_Sint, {DkVtxAttribSize_1x8, DkVtxAttribSize_2x8, DkVtxAttribSize_3x8, DkVtxAttribSize_4x8}},
      // UNorm8
      {DkVtxAttribType_Unorm, {DkVtxAttribSize_1x8, DkVtxAttribSize_2x8, DkVtxAttribSize_3x8, DkVtxAttribSize_4x8}},
      // UInt16
      {DkVtxAttribType_Uint, {DkVtxAttribSize_1x16, DkVtxAttribSize_2x16, DkVtxAttribSize_3x16, DkVtxAttribSize_4x16}},
      // SInt16
      {DkVtxAttribType_Sint, {DkVtxAttribSize_1x16, DkVtxAttribSize_2x16, DkVtxAttribSize_3x16, DkVtxAttribSize_4x16}},
      // UNorm16
      {DkVtxAttribType_Unorm, {DkVtxAttribSize_1x16, DkVtxAttribSize_2x16, DkVtxAttribSize_3x16, DkVtxAttribSize_4x16}},
      // UInt32
      {DkVtxAttribType_Uint, {DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32}},
      // SInt32
      {DkVtxAttribType_Sint, {DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32}},
    }};

  std::array<DkVtxAttribState, GPUPipeline::VertexAttribute::MaxAttributes> attributes;
  for (size_t i = 0; i < config.input_layout.vertex_attributes.size(); i++)
  {
    const GPUPipeline::VertexAttribute& va = config.input_layout.vertex_attributes[i];
    const VAMapping& m = format_mapping[static_cast<u8>(va.type.GetValue())];
    attributes[i] = DkVtxAttribState{0, 0, va.offset, m.sizes[va.components.GetValue() - 1], m.type, 0};
  }

  return std::unique_ptr<GPUPipeline>(
    new Deko3DPipeline(config.layout, config.rasterization, config.depth, config.blend,
                       primitives[static_cast<u32>(config.primitive)], config.input_layout.vertex_attributes.size(),
                       attributes, config.input_layout.vertex_stride, vtx_shader, frg_shader, geom_shader));
}

void Deko3DDevice::ApplyRasterizationState(GPUPipeline::RasterizationState rs)
{
  if (m_last_rasterization_state == rs)
    return;

  static constexpr std::array<DkFace, static_cast<u32>(GPUPipeline::CullMode::MaxCount)> map_cull_face{
    {DkFace_None, DkFace_Front, DkFace_Back}};

  dk::RasterizerState d3d_state;
  d3d_state.setCullMode(map_cull_face[static_cast<u32>(rs.cull_mode.GetValue())]);

  dk::CmdBuf cmdbuf = GetCurrentCommandBuffer();
  cmdbuf.bindRasterizerState(d3d_state);
}

void Deko3DDevice::ApplyDepthState(GPUPipeline::DepthState ds)
{
  if (m_last_depth_state == ds)
    return;

  // is deko3D like OpenGL in that disabling depth testing also disables depth writing?
  // probably considering the GPU is an OpenGL hardware implementation in a lot of ways
  // stil:
  // TODO: test me

  static constexpr std::array<DkCompareOp, static_cast<u32>(GPUPipeline::DepthFunc::MaxCount)> map_func{
    {DkCompareOp_Never, DkCompareOp_Always, DkCompareOp_Less, DkCompareOp_Lequal, DkCompareOp_Greater,
     DkCompareOp_Gequal, DkCompareOp_Equal}};

  dk::DepthStencilState d3d_state;
  d3d_state.setDepthTestEnable(ds.depth_test != GPUPipeline::DepthFunc::Always || ds.depth_write)
    .setDepthCompareOp(map_func[static_cast<u32>(ds.depth_test.GetValue())])
    .setDepthWriteEnable(ds.depth_write);

  dk::CmdBuf cmdbuf = GetCurrentCommandBuffer();
  cmdbuf.bindDepthStencilState(d3d_state);
}

void Deko3DDevice::ApplyBlendState(GPUPipeline::BlendState bs)
{
  static constexpr std::array<DkBlendFactor, static_cast<u32>(GPUPipeline::BlendFunc::MaxCount)> blend_mapping = {{
    DkBlendFactor_Zero,          // Zero
    DkBlendFactor_One,           // One
    DkBlendFactor_SrcColor,      // SrcColor
    DkBlendFactor_InvSrcColor,   // InvSrcColor
    DkBlendFactor_DstColor,      // DstColor
    DkBlendFactor_InvDstColor,   // InvDstColor
    DkBlendFactor_SrcAlpha,      // SrcAlpha
    DkBlendFactor_InvSrcAlpha,   // InvSrcAlpha
    DkBlendFactor_Src1Alpha,     // SrcAlpha1
    DkBlendFactor_InvSrc1Alpha,  // InvSrcAlpha1
    DkBlendFactor_DstAlpha,      // DstAlpha
    DkBlendFactor_InvDstAlpha,   // InvDstAlpha
    DkBlendFactor_ConstColor,    // ConstantColor
    DkBlendFactor_InvConstColor, // InvConstantColor
  }};

  static constexpr std::array<DkBlendOp, static_cast<u32>(GPUPipeline::BlendOp::MaxCount)> op_mapping = {{
    DkBlendOp_Add,    // Add
    DkBlendOp_Sub,    // Subtract
    DkBlendOp_RevSub, // ReverseSubtract
    DkBlendOp_Min,    // Min
    DkBlendOp_Max,    // Max
  }};

  if (bs == m_last_blend_state)
    return;

  dk::CmdBuf cmdbuf = GetCurrentCommandBuffer();

  if (bs.enable != m_last_blend_state.enable)
  {
    dk::ColorState dk_colorstate;
    dk_colorstate.setBlendEnable(0, bs.enable);
    cmdbuf.bindColorState(dk_colorstate);
  }

  if (bs.enable)
  {
    if (bs.blend_factors != m_last_blend_state.blend_factors || bs.blend_ops != m_last_blend_state.blend_ops)
    {
      dk::BlendState dk_blendstate;
      dk_blendstate
        .setFactors(blend_mapping[static_cast<u8>(bs.src_blend.GetValue())],
                    blend_mapping[static_cast<u8>(bs.dst_blend.GetValue())],
                    blend_mapping[static_cast<u8>(bs.src_alpha_blend.GetValue())],
                    blend_mapping[static_cast<u8>(bs.dst_alpha_blend.GetValue())])
        .setOps(op_mapping[static_cast<u8>(bs.blend_op.GetValue())],
                op_mapping[static_cast<u8>(bs.alpha_blend_op.GetValue())]);
      cmdbuf.bindBlendStates(0, {dk_blendstate});
    }

    if (bs.constant != m_last_blend_state.constant)
      cmdbuf.setBlendConst(bs.GetConstantRed(), bs.GetConstantGreen(), bs.GetConstantBlue(), bs.GetConstantAlpha());
  }
  else
  {
    // Keep old values for blend options to potentially avoid calls when re-enabling.
    bs.blend_factors.SetValue(m_last_blend_state.blend_factors);
    bs.blend_ops.SetValue(m_last_blend_state.blend_ops);
    bs.constant.SetValue(m_last_blend_state.constant);
  }

  if (bs.write_mask != m_last_blend_state.write_mask)
  {
    dk::ColorWriteState dk_colorwritestate;
    dk_colorwritestate.setMask(0, bs.write_mask.GetValue());
    cmdbuf.bindColorWriteState(dk_colorwritestate);
  }

  m_last_blend_state = bs;
}

void Deko3DDevice::SetPipeline(GPUPipeline* pipeline)
{
  // printf("setting pipeline %p\n", pipeline);
  if (pipeline == m_current_pipeline)
    return;

  Deko3DPipeline* const P = static_cast<Deko3DPipeline*>(pipeline);

  /*if (m_current_pipeline && P->GetLayout() != m_current_pipeline->GetLayout() &&
      (P->GetLayout() == GPUPipeline::Layout::SingleTextureBufferAndPushConstants ||
       m_current_pipeline->GetLayout() == GPUPipeline::Layout::SingleTextureBufferAndPushConstants))*/
  // a change in pipeline layout may switch between textures and texture buffer
  m_textures_dirty |= 1;

  ApplyRasterizationState(P->GetRasterizationState());
  ApplyDepthState(P->GetDepthState());
  ApplyBlendState(P->GetBlendState());
  dk::CmdBuf cmdbuf = GetCurrentCommandBuffer();

  cmdbuf.bindVtxAttribState(dk::detail::ArrayProxy<const DkVtxAttribState>(P->m_num_attributes, &P->m_attributes[0]));
  cmdbuf.bindVtxBufferState({{P->m_stride, 0}});

  if (P->m_geometry_shader)
    cmdbuf.bindShaders(DkStageFlag_GraphicsMask,
                       {&P->m_vertex_shader->shader, &P->m_geometry_shader->shader, &P->m_fragment_shader->shader});
  else
    cmdbuf.bindShaders(DkStageFlag_GraphicsMask, {&P->m_vertex_shader->shader, &P->m_fragment_shader->shader});

  m_current_pipeline = P;
}

bool Deko3DDevice::ReadPipelineCache(const std::string& filename)
{
  // we don't really need to cache anything besides
  // shaders which is already being taken care of
  return true;
}

bool Deko3DDevice::GetPipelineCacheData(DynamicHeapArray<u8>* data)
{
  return false;
}
