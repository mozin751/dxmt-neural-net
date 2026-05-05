#include "Metal.hpp"
#include "d3d11_private.h"
#include "d3d11_pipeline.hpp"
#include "d3d11_device.hpp"
#include "d3d11_shader.hpp"
#include "util_env.hpp"
#include "log/log.hpp"
#include <atomic>
#include <optional>

namespace dxmt {

class MTLCompiledGraphicsPipelineImpl
    : public MTLCompiledGraphicsPipeline {
public:
  MTLCompiledGraphicsPipelineImpl(MTLD3D11Device *pDevice,
                              MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                              std::unordered_map<size_t, uint32_t>& pso_cache,
                              std::vector<WMT::Reference<WMT::BinaryArchive>>& bin_archives)
      : num_rtvs(pDesc->NumColorAttachments),
        depth_stencil_format(pDesc->DepthStencilFormat),
        topology_class(pDesc->TopologyClass), device_(pDevice),
        pBlendState(pDesc->BlendState),
        RasterizationEnabled(pDesc->RasterizationEnabled),
        SampleCount(pDesc->SampleCount), 
        pso_cache_(pso_cache),
        bin_archives_(bin_archives) {
    uint32_t unorm_output_reg_mask = 0;
    for (unsigned i = 0; i < num_rtvs; i++) {
      rtv_formats[i] = pDesc->ColorAttachmentFormats[i];
      unorm_output_reg_mask |= (uint32_t(IsUnorm8RenderTargetFormat(pDesc->ColorAttachmentFormats[i])) << i);
    }

    if (pDesc->SOLayout) {
      VertexShader =
          pDesc->VertexShader->get_shader(ShaderVariantVertexStreamOutput{
              pDesc->InputLayout, (uint64_t)pDesc->SOLayout});
    } else {
      VertexShader = pDesc->VertexShader->get_shader(ShaderVariantVertex{
          pDesc->InputLayout, pDesc->GSPassthrough, !pDesc->RasterizationEnabled});
    }

    if (pDesc->PixelShader) {
      PixelShader = pDesc->PixelShader->get_shader(ShaderVariantPixel{
          pDesc->SampleMask, pDesc->BlendState->IsDualSourceBlending(),
          depth_stencil_format == WMTPixelFormatInvalid,
          unorm_output_reg_mask});
      ps_valid_render_targets = pDesc->PixelShader->reflection().PSValidRenderTargets;
    } else {
      PixelShader = nullptr;
      ps_valid_render_targets = 0;
    }
  }

  void GetPipeline(MTL_COMPILED_GRAPHICS_PIPELINE *pPipeline) final {
    ready_.wait(false, std::memory_order_acquire);
    *pPipeline = {state_};
  }

  ThreadpoolWork *RunThreadpoolWork() {

    TRACE("Start compiling 1 PSO");

    WMT::Reference<WMT::Error> err;
    MTL_COMPILED_SHADER vs, ps;
    if (!VertexShader->GetShader(&vs)) {
      return VertexShader;
    }
    if (PixelShader && !PixelShader->GetShader(&ps)) {
      return PixelShader;
    }

    WMTRenderPipelineInfo info;
    WMT::InitializeRenderPipelineInfo(info);

    info.vertex_function = vs.Function;

    if (PixelShader) {
      info.fragment_function = ps.Function;
    }
    info.rasterization_enabled = RasterizationEnabled;

    for (unsigned i = 0; i < num_rtvs; i++) {
      if (rtv_formats[i] == WMTPixelFormatInvalid)
        continue;
      info.colors[i].pixel_format = rtv_formats[i];
    }

    if (depth_stencil_format != WMTPixelFormatInvalid) {
      info.depth_pixel_format = depth_stencil_format;
    }
    if (DepthStencilPlanarFlags(depth_stencil_format) & 2) {
      info.stencil_pixel_format = depth_stencil_format;
    }

    if (pBlendState) {
      pBlendState->SetupMetalPipelineDescriptor((WMTRenderPipelineBlendInfo *)&info, num_rtvs, ps_valid_render_targets);
    }

    info.input_primitive_topology = topology_class;
    info.raster_sample_count = SampleCount;
    info.immutable_vertex_buffers = (1 << 16) | (1 << 29) | (1 << 30);
    info.immutable_fragment_buffers = (1 << 29) | (1 << 30);
    
    auto hash = hash_render_pipeline_info(info, VertexShader->GetDigest(), PixelShader ? std::optional<Sha1Digest>(PixelShader->GetDigest()) : std::nullopt);
    bool cache_hit = false;
    WMT::Reference<WMT::BinaryArchive> bin_archive;
    if (pso_cache_.find(hash) == pso_cache_.end()) {
      bin_archive = device_->GetMTLDevice().newBinaryArchive(nullptr, err);
      info.binary_archive_for_serialization = bin_archive;
    } else {
      // TODO: Cache hit code
    }

    state_ = device_->GetMTLDevice().newRenderPipelineState(info, err);

    if (!cache_hit) {
      auto start = std::chrono::high_resolution_clock::now();
      bin_archive.serialize((WMT::GetCacheDir() + "/metal_bin_archives/" + std::to_string(hash) + ".bin").c_str(), err);
      auto end = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
      Logger::info(str::format("Serialised archive, time taken: ", duration.count(),  " microseconds"));
      pso_cache_[hash] = 6;
    }

    if (state_ == nullptr) {
      ERR("Failed to create PSO: ", err.description().getUTF8String());
      return this;
    }

    TRACE("Compiled 1 PSO");

    return this;
  }

  bool GetIsDone() { return ready_; }

  void SetIsDone(bool state) {
    ready_.store(state);
    ready_.notify_all();
  }

private:
  static void hash_combine(size_t& seed, uint64_t value) {
    seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  }

  static void hash_combine(size_t& seed, uint32_t value) {
      hash_combine(seed, static_cast<uint64_t>(value));
  }

  static void hash_combine(size_t& seed, uint8_t value) {
      hash_combine(seed, static_cast<uint64_t>(value));
  }

  static void hash_combine(size_t& seed, bool value) {
      hash_combine(seed, static_cast<uint64_t>(value ? 1u : 0u));
  }

  static void hash_combine(size_t& seed, const Sha1Digest& digest) {
      uint64_t chunk;

      std::memcpy(&chunk, digest.data + 0,  sizeof(uint64_t)); hash_combine(seed, chunk);
      std::memcpy(&chunk, digest.data + 8,  sizeof(uint64_t)); hash_combine(seed, chunk);

      uint32_t tail;
      std::memcpy(&tail,  digest.data + 16, sizeof(uint32_t)); hash_combine(seed, tail);
  }

  size_t hash_render_pipeline_info(
      const WMTRenderPipelineInfo& info,
      const Sha1Digest& vertex_digest,
      const std::optional<Sha1Digest>& fragment_digest
  ) {
      size_t h = 0;

      // Shader functions — stable content digests, not runtime handles
      hash_combine(h, vertex_digest);

      if (fragment_digest.has_value()) {
        hash_combine(h, 1u);  // present
        hash_combine(h, fragment_digest.value());
      } else {
          hash_combine(h, 0u);  // absent
      }

      // Color attachments (all 8, inactive slots will be zero and hash consistently)
      for (int i = 0; i < 8; i++) {
          const auto& c = info.colors[i];
          hash_combine(h, static_cast<uint8_t>(c.pixel_format));
          hash_combine(h, static_cast<uint8_t>(c.rgb_blend_operation));
          hash_combine(h, static_cast<uint8_t>(c.alpha_blend_operation));
          hash_combine(h, static_cast<uint8_t>(c.src_rgb_blend_factor));
          hash_combine(h, static_cast<uint8_t>(c.dst_rgb_blend_factor));
          hash_combine(h, static_cast<uint8_t>(c.src_alpha_blend_factor));
          hash_combine(h, static_cast<uint8_t>(c.dst_alpha_blend_factor));
          hash_combine(h, c.write_mask);
          hash_combine(h, c.blending_enabled);
      }

      // Output merger state
      hash_combine(h, info.alpha_to_coverage_enabled);
      hash_combine(h, info.logic_operation_enabled);
      hash_combine(h, static_cast<uint8_t>(info.logic_operation));
      hash_combine(h, info.rasterization_enabled);
      hash_combine(h, info.raster_sample_count);

      // Attachment formats
      hash_combine(h, static_cast<uint8_t>(info.depth_pixel_format));
      hash_combine(h, static_cast<uint8_t>(info.stencil_pixel_format));

      // Buffer immutability masks
      hash_combine(h, info.immutable_vertex_buffers);
      hash_combine(h, info.immutable_fragment_buffers);

      // Topology and tessellation
      hash_combine(h, static_cast<uint8_t>(info.input_primitive_topology));
      hash_combine(h, static_cast<uint8_t>(info.tessellation_partition_mode));
      hash_combine(h, info.max_tessellation_factor);
      hash_combine(h, static_cast<uint8_t>(info.tessellation_output_winding_order));
      hash_combine(h, static_cast<uint8_t>(info.tessellation_factor_step));

      return h;
  }

  UINT num_rtvs;
  UINT ps_valid_render_targets;
  WMTPixelFormat rtv_formats[8];
  WMTPixelFormat depth_stencil_format;
  WMTPrimitiveTopologyClass topology_class;
  MTLD3D11Device *device_;
  std::atomic_bool ready_;
  CompiledShader *VertexShader;
  CompiledShader *PixelShader;
  IMTLD3D11BlendState *pBlendState;
  WMT::Reference<WMT::RenderPipelineState> state_;
  bool RasterizationEnabled;
  UINT SampleCount;
  std::unordered_map<size_t, uint32_t>& pso_cache_;
  std::vector<WMT::Reference<WMT::BinaryArchive>> bin_archives_;
};

std::unique_ptr<MTLCompiledGraphicsPipeline>
CreateGraphicsPipeline(MTLD3D11Device *pDevice,
                       MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                       std::unordered_map<size_t, uint32_t>& pso_cache,
                       std::vector<WMT::Reference<WMT::BinaryArchive>>& bin_archives) {
  return std::make_unique<MTLCompiledGraphicsPipelineImpl>(pDevice, pDesc, pso_cache, bin_archives);
}

class MTLCompiledComputePipelineImpl
    : public MTLCompiledComputePipeline {
public:
  MTLCompiledComputePipelineImpl(MTLD3D11Device *pDevice, ManagedShader shader)
      : device_(pDevice) {
    ComputeShader = shader->get_shader(ShaderVariantDefault{});
    uint32_t total_tgsize = shader->reflection().ThreadgroupSize[0] *
                            shader->reflection().ThreadgroupSize[1] *
                            shader->reflection().ThreadgroupSize[2];
    // FIXME: might be different on AMD GPU, if it's ever supported
    tgsize_is_multiple_of_sgwidth = (total_tgsize % 32) == 0;
  }

  void GetPipeline(MTL_COMPILED_COMPUTE_PIPELINE *pPipeline) final {
    ready_.wait(false, std::memory_order_acquire);
    *pPipeline = {state_};
  }

  ThreadpoolWork *RunThreadpoolWork() {

    TRACE("Start compiling 1 PSO");

    WMT::Reference<WMT::Error> err;
    MTL_COMPILED_SHADER cs;
    if (!ComputeShader->GetShader(&cs)) {
      return ComputeShader;
    }

    WMTComputePipelineInfo info;
    WMT::InitializeComputePipelineInfo(info);

    info.compute_function = cs.Function;
    info.tgsize_is_multiple_of_sgwidth = tgsize_is_multiple_of_sgwidth;
    info.immutable_buffers = (1 << 29) | (1 << 30);

    state_ = device_->GetMTLDevice().newComputePipelineState(info, err);

    if (!state_) {
      ERR("Failed to create compute PSO: ", err.description().getUTF8String());
      return this;
    }

    TRACE("Compiled 1 PSO");

    return this;
  }

  bool GetIsDone() { return ready_; }

  void SetIsDone(bool state) {
    ready_.store(state);
    ready_.notify_all();
  }

private:
  MTLD3D11Device *device_;
  std::atomic_bool ready_;
  CompiledShader *ComputeShader;
  WMT::Reference<WMT::ComputePipelineState> state_;
  bool tgsize_is_multiple_of_sgwidth;
};

std::unique_ptr<MTLCompiledComputePipeline>
CreateComputePipeline(MTLD3D11Device *pDevice, ManagedShader ComputeShader) {
  return std::make_unique<MTLCompiledComputePipelineImpl>(pDevice, ComputeShader);
}

} // namespace dxmt