#pragma once
#include "d3d11_shader.hpp"
#include "d3d11_pipeline.hpp"
#include "sha1/sha1_util.hpp"
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include <unordered_set>

namespace dxmt {
struct Prediction {
  MTL_GRAPHICS_PIPELINE_DESC pDesc;
  bool hit = false;

  bool
  operator==(const Prediction &other) const {
    return std::equal_to<MTL_GRAPHICS_PIPELINE_DESC>{}(pDesc, other.pDesc);
  }
};

struct RenderPassSignature {
  uint8_t num_color_attachments;
  WMTPixelFormat color_formats[8]; // only [0..num_color_attachments-1] are meaningful
  WMTPixelFormat depth_stencil_format;
  uint8_t sample_count;

  bool
  operator==(const RenderPassSignature &other) const {
    if (num_color_attachments != other.num_color_attachments)
      return false;
    if (depth_stencil_format != other.depth_stencil_format)
      return false;
    if (sample_count != other.sample_count)
      return false;
    for (uint8_t i = 0; i < num_color_attachments; i++)
      if (color_formats[i] != other.color_formats[i])
        return false;
    return true;
  }

  static RenderPassSignature
  from_desc(const MTL_GRAPHICS_PIPELINE_DESC &d) {
    RenderPassSignature rps{};
    rps.num_color_attachments = static_cast<uint8_t>(d.NumColorAttachments);
    rps.depth_stencil_format = d.DepthStencilFormat;
    rps.sample_count = d.SampleCount;
    for (uint8_t i = 0; i < rps.num_color_attachments; i++)
      rps.color_formats[i] = d.ColorAttachmentFormats[i];
    return rps;
  }

  std::string
  to_string() const {
    std::string s = "RPS { NCA=" + std::to_string(num_color_attachments);
    s += " CAF=[";
    for (uint8_t i = 0; i < num_color_attachments; i++) {
      if (i)
        s += ",";
      s += std::to_string(static_cast<uint32_t>(color_formats[i]));
    }
    s += "] DSF=" + std::to_string(static_cast<uint32_t>(depth_stencil_format));
    s += " SC=" + std::to_string(sample_count) + " }";
    return s;
  }
};

template <typename A, typename B>
struct PairHash {
    size_t operator()(const std::pair<A, B>& p) const noexcept {
        size_t h = std::hash<A>{}(p.first);
        h ^= std::hash<B>{}(p.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

} // namespace dxmt

namespace std {
template <> struct hash<dxmt::Prediction> {
  size_t
  operator()(const dxmt::Prediction &p) const noexcept {
    return std::hash<MTL_GRAPHICS_PIPELINE_DESC>{}(p.pDesc);
  }
};

template <> struct hash<dxmt::RenderPassSignature> {
  size_t
  operator()(const dxmt::RenderPassSignature &r) const noexcept {
    size_t h = 0;
    auto mix = [&](size_t v) { h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2); };
    mix(r.num_color_attachments);
    mix(static_cast<uint32_t>(r.depth_stencil_format));
    mix(r.sample_count);
    for (uint8_t i = 0; i < r.num_color_attachments; i++)
      mix(static_cast<uint32_t>(r.color_formats[i]));
    return h;
  }
};

} // namespace std

namespace dxmt {

  static bool wmt_pixel_format_is_integer(WMTPixelFormat fmt) {
    switch (fmt) {
    // Unsigned integer formats
    case WMTPixelFormatR8Uint:
    case WMTPixelFormatR16Uint:
    case WMTPixelFormatR32Uint:
    case WMTPixelFormatRG8Uint:
    case WMTPixelFormatRG16Uint:
    case WMTPixelFormatRG32Uint:
    case WMTPixelFormatRGBA8Uint:
    case WMTPixelFormatRGBA16Uint:
    case WMTPixelFormatRGBA32Uint:
    // Signed integer formats
    case WMTPixelFormatR8Sint:
    case WMTPixelFormatR16Sint:
    case WMTPixelFormatR32Sint:
    case WMTPixelFormatRG8Sint:
    case WMTPixelFormatRG16Sint:
    case WMTPixelFormatRG32Sint:
    case WMTPixelFormatRGBA8Sint:
    case WMTPixelFormatRGBA16Sint:
    case WMTPixelFormatRGBA32Sint:
        return true;
    default:
        return false;
    }
}

static bool wmt_pixel_format_is_unsigned_integer(WMTPixelFormat fmt) {
    switch (fmt) {
    case WMTPixelFormatR8Uint:
    case WMTPixelFormatR16Uint:
    case WMTPixelFormatR32Uint:
    case WMTPixelFormatRG8Uint:
    case WMTPixelFormatRG16Uint:
    case WMTPixelFormatRG32Uint:
    case WMTPixelFormatRGBA8Uint:
    case WMTPixelFormatRGBA16Uint:
    case WMTPixelFormatRGBA32Uint:
        return true;
    default:
        return false;
    }
}

static bool wmt_pixel_format_is_signed_integer(WMTPixelFormat fmt) {
    switch (fmt) {
    case WMTPixelFormatR8Sint:
    case WMTPixelFormatR16Sint:
    case WMTPixelFormatR32Sint:
    case WMTPixelFormatRG8Sint:
    case WMTPixelFormatRG16Sint:
    case WMTPixelFormatRG32Sint:
    case WMTPixelFormatRGBA8Sint:
    case WMTPixelFormatRGBA16Sint:
    case WMTPixelFormatRGBA32Sint:
        return true;
    default:
        return false;
    }
}

static bool blend_compatible_with_ps(
    IMTLD3D11BlendState* bs,
    uint8_t nca,
    uint32_t ps_valid_render_targets)
{
    if (!bs) return true;
    D3D11_BLEND_DESC1 bd;
    bs->GetDesc1(&bd);

    if (!bd.IndependentBlendEnable) {
        // Only RT[0] applies to slot 0, all other slots are irrelevant
        const auto& rt = bd.RenderTarget[0];
        bool slot0_active = rt.BlendEnable || rt.RenderTargetWriteMask != 0;
        if (!slot0_active) return true;
        bool slot0_bound  = nca > 0;
        bool ps_writes_0  = (ps_valid_render_targets & 1) != 0;
        if (slot0_active && slot0_bound && !ps_writes_0) return false;
        if (slot0_active && !slot0_bound)                return false;
        return true;
    }

    // IndependentBlendEnable=true: check each slot individually
    for (uint8_t slot = 0; slot < 8; slot++) {
        const auto& rt = bd.RenderTarget[slot];
        bool slot_active = rt.BlendEnable || rt.RenderTargetWriteMask != 0;
        if (!slot_active) continue;
        bool slot_bound  = slot < nca;
        bool ps_writes   = (ps_valid_render_targets >> slot) & 1;
        if (slot_active && slot_bound && !ps_writes) return false;
        if (slot_active && !slot_bound)              return false;
    }
    return true;
}

static bool rps_compatible_with_ps(
    const RenderPassSignature& rps,
    ManagedShader ps)
{
    // Count how many slots actually have a real (non-Invalid) format.
    uint8_t real_attachments = 0;
    for (uint8_t i = 0; i < rps.num_color_attachments; i++) {
        if (rps.color_formats[i] != WMTPixelFormatInvalid)
            real_attachments++;
    }

    if (!ps) {
        // No PS: only valid if no real color attachments.
        return true;
    }

    const auto& outs = ps->ps_outs;

    // PS output count must cover all real (non-Invalid) slots.
    if (real_attachments > outs.count)
        return false;

    // Per-slot format class check — only for non-Invalid slots.
    for (uint8_t slot = 0; slot < rps.num_color_attachments; slot++) {
        WMTPixelFormat fmt = rps.color_formats[slot];
        if (fmt == WMTPixelFormatInvalid)
            continue;

        ScalarClass cls = outs.classes[slot];

        switch (cls) {
        case ScalarClass::None:
            return false;
        case ScalarClass::Float:
            if (wmt_pixel_format_is_integer(fmt))
                return false;
            break;
        case ScalarClass::UInt:
            if (!wmt_pixel_format_is_unsigned_integer(fmt))
                return false;
            break;
        case ScalarClass::SInt:
            if (!wmt_pixel_format_is_signed_integer(fmt))
                return false;
            break;
        }
    }

    return true;
}

static std::string
format_desc(const MTL_GRAPHICS_PIPELINE_DESC &d) {
  std::string s;
  s += "VS=" + (d.VertexShader ? d.VertexShader->sha1().string().substr(0, 8) : "null");
  s += " PS=" + (d.PixelShader ? d.PixelShader->sha1().string().substr(0, 8) : "null");
  s += " NCA=" + std::to_string(d.NumColorAttachments);
  s += " CAF=[";
  for (uint8_t i = 0; i < 8; i++) {
    if (i)
      s += ",";
    s += std::to_string(static_cast<uint32_t>(d.ColorAttachmentFormats[i]));
  }
  s += "] DSF=" + std::to_string(static_cast<uint32_t>(d.DepthStencilFormat));
  s += " TOPO=" + std::to_string(static_cast<uint32_t>(d.TopologyClass));
  s += " RASTER=" + std::to_string(d.RasterizationEnabled);
  s += " SC=" + std::to_string(d.SampleCount);
  s += " GSSTrip=" + std::to_string(d.GSStripTopology);
  s += " IBF=" + std::to_string(static_cast<uint32_t>(d.IndexBufferFormat));
  s += " SMASK=" + std::to_string(d.SampleMask);
  s += " GSPass=" + std::to_string(d.GSPassthrough);
  s += " BLEND=" + std::to_string(reinterpret_cast<uintptr_t>(d.BlendState));
  s += " IL=" + std::to_string(reinterpret_cast<uintptr_t>(d.InputLayout));
  s += " SO=" + std::to_string(reinterpret_cast<uintptr_t>(d.SOLayout));
  s += " HS=" + std::to_string(reinterpret_cast<uintptr_t>(d.HullShader));
  s += " GS=" + std::to_string(reinterpret_cast<uintptr_t>(d.GeometryShader));
  return s;
}
// Walks the PS output signature (OSGN) and returns:
//   - count: highest SV_Target slot index + 1 (0 if none)
//   - classes[i]: float/uint/sint class the PS writes to slot i, None if unused
//   - masks[i]:   xyzw write mask for slot i (4 LSBs), 0 if unused
//
// Cardinality note: the class only narrows the format down to one of three
// families (float-like / uint / sint). It does NOT pin RGBA8 vs RGBA16Float
// vs sRGB vs BGRA — those come from history/live context, not from bytecode.
PSColorOutputs
ExtractPSColorOutputs(const void *dxbc, size_t size) {
  PSColorOutputs out{};
  out.classes.fill(ScalarClass::None);

  microsoft::CDXBCParser container;
  if (FAILED(container.ReadDXBC(dxbc, size)))
    return out;
  UINT idx = container.FindNextMatchingBlob(microsoft::DXBC_OutputSignature, 0);
  if (idx == DXBC_BLOB_NOT_FOUND) {
    Logger::info("PSColorOutputs: OSGN blob not found");
    return out;
  }

  microsoft::CSignatureParser parser;
  if (FAILED(parser.ReadSignature4(container.GetBlob(idx), container.GetBlobSize(idx)))) {
    Logger::info("PSColorOutputs: ReadSignature4 failed");
    return out;
  }

  const microsoft::D3D11_SIGNATURE_PARAMETER *params = nullptr;
  UINT count = parser.GetParameters(&params);

  int max_slot = -1;
  for (UINT i = 0; i < count; ++i) {
    const auto &p = params[i];
    bool is_target =
        p.SystemValue == D3D_NAME_TARGET || (p.SystemValue == D3D_NAME_UNDEFINED && p.SemanticName != nullptr &&
                                             _stricmp(p.SemanticName, "SV_TARGET") == 0);
    if (!is_target)
      continue;

    int slot = static_cast<int>(p.SemanticIndex);
    if (slot < 0 || slot >= 8)
      continue;

    // A given SV_TargetN normally appears as one signature entry, but
    // OR the mask defensively in case fxc emits split-register entries.
    out.classes[slot] = to_scalar_class(p.ComponentType);
    out.masks[slot] = static_cast<uint8_t>(out.masks[slot] | (p.Mask & 0xF));
    if (slot > max_slot)
      max_slot = slot;
  }

  out.count = static_cast<uint8_t>(max_slot + 1);
  return out;
}

VSInputRequirement
ExtractVSInputRequirement(const void *dxbc, size_t size) {
  VSInputRequirement req;
  microsoft::CDXBCParser container;
  if (FAILED(container.ReadDXBC(dxbc, size))) {
    Logger::info("ExtractVSInputRequirement: ReadDXBC failed");
    return req;
  }
  UINT idx = container.FindNextMatchingBlob(microsoft::DXBC_InputSignature, 0);
  if (idx == DXBC_BLOB_NOT_FOUND) {
    Logger::info("ExtractVSInputRequirement: FindNextMatchingBlob failed");
    return req;
  }

  microsoft::CSignatureParser parser;
  if (FAILED(parser.ReadSignature4(container.GetBlob(idx), container.GetBlobSize(idx)))) {

    Logger::info("ExtractVSInputRequirement: ReadSignature4 failed");
    return req;
  }

  const microsoft::D3D11_SIGNATURE_PARAMETER *params = nullptr;
  UINT count = parser.GetParameters(&params);

  for (UINT i = 0; i < count; ++i) {
    const auto &p = params[i];
    if (p.SystemValue != D3D_NAME_UNDEFINED)
      continue; // SV_VertexID/InstanceID aren't IA inputs

    VSInputRequirement::Element e{};
    const char *s = p.SemanticName ? p.SemanticName : "";
    for (size_t n = 0; s[n] && n < sizeof(e.semantic) - 1; ++n)
      e.semantic[n] = (char)toupper((unsigned char)s[n]);
    e.semantic_index = p.SemanticIndex;
    e.reg = p.Register;
    e.mask = p.Mask & 0xF;
    e.component_class = static_cast<uint8_t>(to_scalar_class(p.ComponentType)); // your existing helper
    req.elements.push_back(e);
  }
  return req;
}

VSInputRequirement
ExtractVSInputRequirementNoSize(const void *dxbc) {
  VSInputRequirement req;
  microsoft::CDXBCParser container;
  // Trusts the total-size field in the DXBC header instead of an outside length.
  if (FAILED(container.ReadDXBCAssumingValidSize(dxbc)))
    return req;
  UINT idx = container.FindNextMatchingBlob(microsoft::DXBC_InputSignature, 0);
  if (idx == DXBC_BLOB_NOT_FOUND)
    return req;

  microsoft::CSignatureParser parser;
  if (FAILED(parser.ReadSignature4(container.GetBlob(idx), container.GetBlobSize(idx))))
    return req;

  const microsoft::D3D11_SIGNATURE_PARAMETER *params = nullptr;
  UINT count = parser.GetParameters(&params);
  for (UINT i = 0; i < count; ++i) {
    const auto &p = params[i];
    if (p.SystemValue != D3D_NAME_UNDEFINED)
      continue;
    VSInputRequirement::Element e{};
    const char *s = p.SemanticName ? p.SemanticName : "";
    for (size_t n = 0; s[n] && n < sizeof(e.semantic) - 1; ++n)
      e.semantic[n] = (char)toupper((unsigned char)s[n]);
    e.semantic_index = p.SemanticIndex;
    e.reg = p.Register;
    e.mask = p.Mask & 0xF;
    e.component_class = static_cast<uint8_t>(to_scalar_class(p.ComponentType));
    req.elements.push_back(e);
  }
  return req;
}

Sha1Digest
HashVSInputRequirement(const VSInputRequirement &req) {
  if (req.elements.empty()) {
    Logger::info("HashVSInputRequirement: no IA inputs.");
    return {}; // no IA inputs => associates with null layout
  }
  Sha1HashState h;
  for (const auto &e : req.elements)
    h.update(e); // Element is trivially copyable
  h.update(req.elements.size());
  return h.final();
}

// Does this CachedInputLayout satisfy the VS's input signature?
bool
LayoutCoversVS(InputLayout *il, const VSInputRequirement &req) {
  if (req.elements.empty())
    return il == nullptr; // VS reads nothing => only a null layout is "correct"
  if (!il)
    return false;

  MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC *els = nullptr;
  uint32_t n = il->input_layout_element(&els);

  for (const auto &need : req.elements) {
    bool found = false;
    for (uint32_t j = 0; j < n; ++j) {
      // DXMT's IA element keys vertex-fetch by input register; match on that.
      // (Adjust the field name to whatever MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC
      //  exposes — it's the register/attribute index the VS reads.)
      if (els[j].Index == need.reg) {
        found = true;
        break;
      }
    }
    if (!found)
      return false; // VS reads a register the layout doesn't feed
  }
  return true;
}

void
lock_shader_deterministic_fields(MTL_GRAPHICS_PIPELINE_DESC *pDesc) {
  pDesc->GSStripTopology = false;
  pDesc->GSPassthrough = ~0u;
  pDesc->RasterizationEnabled = pDesc->PixelShader != nullptr;
  pDesc->SampleMask = 0xFFFFFFFFu;
  pDesc->SOLayout = nullptr;
}

void
set_shader_defaults(
    MTL_GRAPHICS_PIPELINE_DESC *pDesc, std::unique_ptr<ManagedDeviceChild<IMTLD3D11BlendState>> &default_blend_state_ptr
) {
  // pDesc->IndexBufferFormat = SM50_INDEX_BUFFER_FORMAT_UINT16;
  // pDesc->DepthStencilFormat = WMTPixelFormatDepth32Float_Stencil8;
  pDesc->TopologyClass = WMTPrimitiveTopologyClassTriangle;
  pDesc->BlendState = default_blend_state_ptr.get();
}

std::vector<Prediction>
predictor_reflection_and_default(
    ManagedShader vs, ManagedShader ps,
    std::unique_ptr<ManagedDeviceChild<IMTLD3D11BlendState>> &default_blend_state_ptr,
    std::unordered_map<Sha1Digest, std::unordered_set<Sha1Digest>> &input_requirement_to_layouts,
    std::unordered_map<Sha1Digest, InputLayout *> &layout_by_sha1, std::unordered_set<Prediction> &previous_predictions
) {
  // Idea: Get the cartesian product of all CONSTRAINED fields.
  // Use defaults and locked fields on each candidate desc.
  // Output into a vector of predictions.

  /**
   * Constrained fields:
   *  - InputLayout: Constrain through compatibility of given ILs and known VS.
   *  - NumColorAttachments: 0 or max. Only 0 if no PS.
   *  - ColorAttachmentFormats: Same as above. Only do 1 each (expand later).
   */

  std::vector<Prediction> predictions;
  std::vector<InputLayout *> valid_ils;

  // Get list of valid input layouts
  for (const auto input_req_digest : input_requirement_to_layouts[HashVSInputRequirement(vs->vs_input_req)]) {
    valid_ils.push_back(layout_by_sha1[input_req_digest]);
  }

  // Sample count cross product
  for (uint8_t i = 0; i < 2; i++) {
    MTL_GRAPHICS_PIPELINE_DESC pDesc{};
    pDesc.SampleCount = i + 1;
    pDesc.VertexShader = vs;
    pDesc.PixelShader = ps;

    for (int dsf_selector = 0; dsf_selector < 2; ++dsf_selector) {
      switch (dsf_selector) {
      case 0:
        pDesc.DepthStencilFormat = WMTPixelFormatDepth32Float_Stencil8;
        break;

      case 1:
        pDesc.DepthStencilFormat = WMTPixelFormatInvalid;
        break;

      default:
        pDesc.DepthStencilFormat = WMTPixelFormatDepth16Unorm;
      }

      for (int ibf_selector = 0; ibf_selector < 2; ++ibf_selector) {
        pDesc.IndexBufferFormat =
            ibf_selector % 2 == 0 ? SM50_INDEX_BUFFER_FORMAT_UINT16 : SM50_INDEX_BUFFER_FORMAT_NONE;

        for (const auto input_layout : valid_ils) {
          lock_shader_deterministic_fields(&pDesc);
          set_shader_defaults(&pDesc, default_blend_state_ptr);

          pDesc.InputLayout = input_layout;

          pDesc.NumColorAttachments = 1;
          for (uint8_t i = 0; i < 8; ++i) {
            pDesc.ColorAttachmentFormats[i] = WMTPixelFormatInvalid;
          }

          Prediction pred_no_color{pDesc};
          if (previous_predictions.count(pred_no_color) == 0) {
            predictions.push_back(pred_no_color);
            previous_predictions.insert(pred_no_color);
            Logger::info(str::format("PREDICTED: ", format_desc(pred_no_color.pDesc)));
          }

          if (ps == nullptr) {
            continue;
          }

          pDesc.NumColorAttachments = ps->ps_outs.count;

          for (int j = 0; j < 2; ++j) {
            for (uint8_t i = 0; i < pDesc.NumColorAttachments; i++) {
              switch (ps->ps_outs.classes[i]) {
              case ScalarClass::Float:
                pDesc.ColorAttachmentFormats[i] =
                    j % 2 == 0 ? WMTPixelFormatRG11B10Float : WMTPixelFormatRGBA8Unorm_sRGB;
                break;

              case ScalarClass::SInt:
                pDesc.ColorAttachmentFormats[i] = WMTPixelFormatR32Sint;
                break;

              case ScalarClass::UInt:
                pDesc.ColorAttachmentFormats[i] = WMTPixelFormatR32Uint;
                break;

              default:
                pDesc.ColorAttachmentFormats[i] = WMTPixelFormatInvalid;
              }
            }

            Prediction pred_with_color{pDesc};
            if (previous_predictions.count(pred_with_color) == 0) {
              predictions.push_back(pred_with_color);
              previous_predictions.insert(pred_with_color);
              Logger::info(str::format("PREDICTED: ", format_desc(pred_with_color.pDesc)));
            }
          }
        }
      }
    }
  }

  return predictions;
}

// Returns the top_n most frequently observed descriptors globally,
// with vs/ps substituted in. Shader handles are stripped before counting
// so descriptors from different pairs are comparable.
std::vector<Prediction>
predictor_most_frequent_global(
    ManagedShader vs, ManagedShader ps, std::unordered_map<MTL_GRAPHICS_PIPELINE_DESC, uint32_t> &freq_table,
    std::unordered_set<Prediction> &previous_predictions, uint32_t top_n = 5
) {
  // Build a sorted list of (count, desc) pairs
  std::vector<std::pair<uint32_t, MTL_GRAPHICS_PIPELINE_DESC>> ranked;
  ranked.reserve(freq_table.size());
  for (const auto &[desc, count] : freq_table)
    ranked.push_back({count, desc});

  std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });

  std::vector<Prediction> predictions;
  for (uint32_t i = 0; i < std::min(top_n, (uint32_t)ranked.size()); i++) {
    MTL_GRAPHICS_PIPELINE_DESC desc = ranked[i].second;

    // Substitute the new pair's shaders
    desc.VertexShader = vs;
    desc.PixelShader = ps;
    desc.HullShader = nullptr;
    desc.DomainShader = nullptr;
    desc.GeometryShader = nullptr;

    // Re-lock deterministic fields for this specific pair
    lock_shader_deterministic_fields(&desc);

    Prediction pred{desc};
    if (previous_predictions.count(pred) == 0) {
      Logger::info(str::format("PREDICTED: ", format_desc(pred.pDesc)));
      predictions.push_back(pred);
      previous_predictions.insert(pred);
    }
  }

  return predictions;
}

std::vector<Prediction>
predictor_most_frequent_rps_blend(
    ManagedShader vs, ManagedShader ps, std::unordered_map<RenderPassSignature, uint32_t> &rps_freq,
    std::unordered_map<IMTLD3D11BlendState *, uint32_t> &blend_freq,
    std::unordered_map<Sha1Digest, std::unordered_set<Sha1Digest>> &input_req_to_layouts,
    std::unordered_map<Sha1Digest, InputLayout *> &layout_by_sha1, std::unordered_set<Prediction> &previous_predictions,
    uint32_t top_rps = 3, uint32_t top_blend = 3
) {
  // --- Rank RPS by frequency ---
  std::vector<std::pair<uint32_t, RenderPassSignature>> ranked_rps;
  ranked_rps.reserve(rps_freq.size());
  for (const auto &[rps, count] : rps_freq)
    ranked_rps.push_back({count, rps});
  std::sort(ranked_rps.begin(), ranked_rps.end(), [](const auto &a, const auto &b) { return a.first > b.first; });

  // --- Rank BlendState by frequency ---
  std::vector<std::pair<uint32_t, IMTLD3D11BlendState *>> ranked_blend;
  ranked_blend.reserve(blend_freq.size());
  for (const auto &[bs, count] : blend_freq)
    ranked_blend.push_back({count, bs});
  std::sort(ranked_blend.begin(), ranked_blend.end(), [](const auto &a, const auto &b) { return a.first > b.first; });

  // --- VS-compatible input layouts ---
  std::vector<InputLayout *> valid_ils;
  auto vs_sig = HashVSInputRequirement(vs->vs_input_req);
  auto it = input_req_to_layouts.find(vs_sig);
  if (it != input_req_to_layouts.end()) {
    for (const auto &il_sha1 : it->second) {
      auto jt = layout_by_sha1.find(il_sha1);
      if (jt != layout_by_sha1.end())
        valid_ils.push_back(jt->second);
    }
  }
  if (valid_ils.empty())
    return {};

  // --- Cross-product: top_rps × top_blend × layouts × {NONE, UINT16} ---
  std::vector<Prediction> predictions;

  uint32_t rps_limit = std::min(top_rps, (uint32_t)ranked_rps.size());
  uint32_t blend_limit = std::min(top_blend, (uint32_t)ranked_blend.size());

  static constexpr SM50_INDEX_BUFFER_FORAMT kIBFs[] = {
      SM50_INDEX_BUFFER_FORMAT_NONE,
      SM50_INDEX_BUFFER_FORMAT_UINT16,
  };

  for (uint32_t ri = 0; ri < rps_limit; ri++) {
    const auto &rps = ranked_rps[ri].second;

    for (uint32_t bi = 0; bi < blend_limit; bi++) {
      IMTLD3D11BlendState *blend = ranked_blend[bi].second;

      for (auto *il : valid_ils) {
        for (auto ibf : kIBFs) {

          MTL_GRAPHICS_PIPELINE_DESC desc{};
          desc.VertexShader = vs;
          desc.PixelShader = ps;
          desc.HullShader = nullptr;
          desc.DomainShader = nullptr;
          desc.GeometryShader = nullptr;
          desc.TopologyClass = WMTPrimitiveTopologyClassTriangle;

          // Surface fields from RPS
          desc.NumColorAttachments = rps.num_color_attachments;
          for (uint8_t i = 0; i < 8; i++)
            desc.ColorAttachmentFormats[i] =
                (i < rps.num_color_attachments) ? rps.color_formats[i] : WMTPixelFormatInvalid;
          desc.DepthStencilFormat = rps.depth_stencil_format;
          desc.SampleCount = rps.sample_count;

          // State fields
          desc.BlendState = blend;
          desc.InputLayout = il;
          desc.IndexBufferFormat = ibf;

          // Deterministic fields
          lock_shader_deterministic_fields(&desc);

          Prediction pred{desc};
          if (previous_predictions.count(pred) == 0) {
            predictions.push_back(pred);
            previous_predictions.insert(pred);
            Logger::info(str::format("PREDICTED: ", format_desc(pred.pDesc)));
          }
        }
      }
    }
  }

  return predictions;
}

// Controls whether the cross-product is capped.
// Set to 0 to disable the cap and emit all combinations.
constexpr uint32_t kPerFieldModeMaxCandidates = 20;

template <typename T>
static std::vector<std::pair<uint32_t, T>>
top_n(const std::unordered_map<T, uint32_t> &freq, uint32_t n) {
  std::vector<std::pair<uint32_t, T>> ranked;
  ranked.reserve(freq.size());
  for (const auto &[val, count] : freq)
    ranked.push_back({count, val});
  std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  if (n > 0 && ranked.size() > n)
    ranked.resize(n);
  return ranked;
}

std::vector<Prediction>
predictor_per_field_mode(
    ManagedShader vs, ManagedShader ps, const std::unordered_map<WMTPixelFormat, uint32_t> &dsf_freq,
    const std::array<std::unordered_map<WMTPixelFormat, uint32_t>, 8> &caf_freq,
    const std::unordered_map<uint8_t, uint32_t> &sc_freq,
    const std::unordered_map<SM50_INDEX_BUFFER_FORAMT, uint32_t> &ibf_freq,
    const std::unordered_map<IMTLD3D11BlendState *, uint32_t> &blend_freq,
    std::unordered_map<Sha1Digest, std::unordered_set<Sha1Digest>> &input_req_to_layouts,
    std::unordered_map<Sha1Digest, InputLayout *> &layout_by_sha1, std::unordered_set<Prediction> &previous_predictions,
    uint32_t top_dsf = 3, uint32_t top_caf = 2, uint32_t top_sc = 2, uint32_t top_ibf = 2, uint32_t top_blend = 3
) {
  std::vector<Prediction> predictions;

  // Bail out if we have no frequency data yet (cold boot)
  if (dsf_freq.empty() || blend_freq.empty())
    return predictions;

  // --- Rank each field ---
  auto ranked_dsf = top_n(dsf_freq, top_dsf);
  auto ranked_sc = top_n(sc_freq, top_sc);
  auto ranked_ibf = top_n(ibf_freq, top_ibf);
  auto ranked_blend = top_n(blend_freq, top_blend);

  // NCA from PS reflection
  uint8_t nca = ps ? ps->ps_outs.count : 0;

  // Per-slot color format rankings (only for slots [0..nca-1])
  std::vector<std::vector<std::pair<uint32_t, WMTPixelFormat>>> ranked_caf(nca);
  for (uint8_t i = 0; i < nca; i++) {
    if (!caf_freq[i].empty())
      ranked_caf[i] = top_n(caf_freq[i], top_caf);
    else
      ranked_caf[i] = {{0, WMTPixelFormatRGBA8Unorm}}; // fallback
  }

  // --- VS-compatible input layouts ---
  std::vector<InputLayout *> valid_ils;
  auto vs_sig = HashVSInputRequirement(vs->vs_input_req);
  auto it = input_req_to_layouts.find(vs_sig);
  if (it != input_req_to_layouts.end()) {
    for (const auto &sha1 : it->second) {
      auto jt = layout_by_sha1.find(sha1);
      if (jt != layout_by_sha1.end())
        valid_ils.push_back(jt->second);
    }
  }
  if (valid_ils.empty()) {
    if (vs->vs_input_req.elements.empty())
      valid_ils.push_back(nullptr);
    else
      return predictions;
  }

  // --- Build all candidates as (joint_score, desc) pairs ---
  // Joint score = product of per-field counts, used for ranking before cap.
  std::vector<std::pair<uint64_t, MTL_GRAPHICS_PIPELINE_DESC>> candidates;

  for (const auto &[dsf_cnt, dsf] : ranked_dsf)
    for (const auto &[sc_cnt, sc] : ranked_sc)
      for (const auto &[ibf_cnt, ibf] : ranked_ibf)
        for (const auto &[blend_cnt, bs] : ranked_blend)
          for (auto *il : valid_ils) {
            // Build the color format combination for this candidate.
            // We iterate over the Cartesian product of per-slot rankings.
            // For nca=0 there are no color slots, so one iteration with empty formats.
            // For nca>0 we recurse via a small stack to avoid deep nesting.

            // Collect per-slot candidates into a flat list we can iterate.
            // Use a simple index-vector approach to enumerate the product.
            std::vector<size_t> slot_sizes(nca);
            for (uint8_t s = 0; s < nca; s++)
              slot_sizes[s] = ranked_caf[s].size();

            // Total color combinations = product of slot sizes (or 1 if nca=0)
            size_t color_combos = 1;
            for (auto sz : slot_sizes)
              color_combos *= sz;

            for (size_t ci = 0; ci < color_combos; ci++) {
              // Decode the combination index into per-slot indices
              std::array<WMTPixelFormat, 8> formats{};
              uint64_t color_score = 1;
              size_t rem = ci;
              for (int s = (int)nca - 1; s >= 0; s--) {
                size_t idx = rem % slot_sizes[s];
                rem /= slot_sizes[s];
                formats[s] = ranked_caf[s][idx].second;
                color_score *= ranked_caf[s][idx].first;
              }
              // Unused slots: Invalid
              for (uint8_t s = nca; s < 8; s++)
                formats[s] = WMTPixelFormatInvalid;

              MTL_GRAPHICS_PIPELINE_DESC desc{};
              desc.VertexShader = vs;
              desc.PixelShader = ps;
              desc.HullShader = nullptr;
              desc.DomainShader = nullptr;
              desc.GeometryShader = nullptr;

              desc.NumColorAttachments = nca;
              for (uint8_t s = 0; s < 8; s++)
                desc.ColorAttachmentFormats[s] = formats[s];

              desc.DepthStencilFormat = dsf;
              desc.SampleCount = sc;
              desc.IndexBufferFormat = ibf;
              desc.BlendState = bs;
              desc.InputLayout = il;

              lock_shader_deterministic_fields(&desc);
              desc.TopologyClass = WMTPrimitiveTopologyClassTriangle;

              // Joint score: product of all per-field counts
              uint64_t score = (uint64_t)dsf_cnt * sc_cnt * ibf_cnt * blend_cnt * color_score;
              candidates.push_back({score, desc});
            }
          }

  // --- Sort by joint score descending ---
  std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.first > b.first; });

  // --- Apply cap (set kPerFieldModeMaxCandidates = 0 to disable) ---
  size_t limit = (kPerFieldModeMaxCandidates > 0) ? std::min((size_t)kPerFieldModeMaxCandidates, candidates.size())
                                                  : candidates.size();

  for (size_t i = 0; i < limit; i++) {
    Prediction pred{candidates[i].second};
    if (previous_predictions.count(pred) == 0) {
      if (candidates[i].second.PixelShader && !blend_compatible_with_ps(candidates[i].second.BlendState, candidates[i].second.PixelShader->ps_outs.count, candidates[i].second.PixelShader->reflection().PSValidRenderTargets))
        continue;
      predictions.push_back(pred);
      previous_predictions.insert(pred);
      // Logger::info(str::format("Is rps compatible with this ps? ", rps_compatible_with_ps(
      //       RenderPassSignature::from_desc(pred.pDesc),
      // pred.pDesc.PixelShader)));
      // Logger::info(str::format("PREDICTED: ", format_desc(pred.pDesc)));
    }
  }

  return predictions;
}

std::vector<Prediction> predictor_ps_rps_blend_history(
    ManagedShader vs,
    ManagedShader ps,
    const std::unordered_map<Sha1Digest,
        std::unordered_map<std::pair<RenderPassSignature, IMTLD3D11BlendState*>, uint32_t,
            PairHash<RenderPassSignature, IMTLD3D11BlendState*>>>& ps_rps_blend_table,
    std::unordered_map<Sha1Digest, std::unordered_set<Sha1Digest>>& input_req_to_layouts,
    std::unordered_map<Sha1Digest, InputLayout*>& layout_by_sha1,
    std::unordered_set<Prediction>& previous_predictions,
    uint32_t top_k = 3)
{
    std::vector<Prediction> predictions;

    if (!ps) return predictions;

    // Look up this PS's historical (RPS, blend) pairs.
    auto ps_it = ps_rps_blend_table.find(ps->sha1());
    if (ps_it == ps_rps_blend_table.end())
        return predictions;  // never seen this PS before — emit nothing

    const auto& rps_blend_map = ps_it->second;

    // Rank by frequency, take top-k.
    std::vector<std::pair<uint32_t, std::pair<RenderPassSignature, IMTLD3D11BlendState*>>> ranked;
    ranked.reserve(rps_blend_map.size());
    for (const auto& [key, count] : rps_blend_map)
        ranked.push_back({count, key});
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b){ return a.first > b.first; });

    uint32_t limit = std::min(top_k, (uint32_t)ranked.size());

    // VS-compatible input layouts.
    std::vector<InputLayout*> valid_ils;
    auto vs_sig = HashVSInputRequirement(vs->vs_input_req);
    auto il_it = input_req_to_layouts.find(vs_sig);
    if (il_it != input_req_to_layouts.end()) {
        for (const auto& sha1 : il_it->second) {
            auto jt = layout_by_sha1.find(sha1);
            if (jt != layout_by_sha1.end())
                valid_ils.push_back(jt->second);
        }
    }
    if (valid_ils.empty()) {
        if (vs->vs_input_req.elements.empty())
            valid_ils.push_back(nullptr);
        else
            return predictions;  // no known layouts yet — emit nothing
    }

    // Cross-product: top-k (RPS, blend) × compatible ILs × {NONE, UINT16}.
    for (uint32_t i = 0; i < limit; i++) {
        const auto& [rps, blend] = ranked[i].second;

        for (auto* il : valid_ils) {
            for (auto ibf : {SM50_INDEX_BUFFER_FORMAT_NONE,
                             SM50_INDEX_BUFFER_FORMAT_UINT16}) {

                MTL_GRAPHICS_PIPELINE_DESC desc{};
                desc.VertexShader   = vs;
                desc.PixelShader    = ps;
                desc.HullShader     = nullptr;
                desc.DomainShader   = nullptr;
                desc.GeometryShader = nullptr;

                desc.NumColorAttachments = rps.num_color_attachments;
                for (uint8_t s = 0; s < 8; s++)
                    desc.ColorAttachmentFormats[s] =
                        (s < rps.num_color_attachments)
                        ? rps.color_formats[s]
                        : WMTPixelFormatInvalid;
                desc.DepthStencilFormat = rps.depth_stencil_format;
                desc.SampleCount        = rps.sample_count;
                desc.BlendState         = blend;
                desc.InputLayout        = il;
                desc.IndexBufferFormat  = ibf;
                desc.TopologyClass      = WMTPrimitiveTopologyClassTriangle;
                desc.RasterizationEnabled = (ps != nullptr);
                desc.SampleMask         = 0xFFFFFFFF;

                lock_shader_deterministic_fields(&desc);

                Prediction pred{desc};
                if (previous_predictions.count(pred) == 0) {
                    predictions.push_back(pred);
                    previous_predictions.insert(pred);
                    // Logger::info(str::format("PREDICTED: ", format_desc(pred.pDesc)));
                }
            }
        }
    }

    return predictions;
}

} // namespace dxmt