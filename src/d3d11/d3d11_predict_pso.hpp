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

  bool operator==(const Prediction& other) const {
    return std::equal_to<MTL_GRAPHICS_PIPELINE_DESC>{}(pDesc, other.pDesc);
  }
};
} // namespace dxmt

namespace std {
template <> struct hash<dxmt::Prediction> {
  size_t operator()(const dxmt::Prediction& p) const noexcept {
    return std::hash<MTL_GRAPHICS_PIPELINE_DESC>{}(p.pDesc);
  }
};
} // namespace std


namespace dxmt {
static std::string format_desc(const MTL_GRAPHICS_PIPELINE_DESC& d) {
  std::string s;
  s += "VS="       + (d.VertexShader   ? d.VertexShader->sha1().string().substr(0,8)   : "null");
  s += " PS="      + (d.PixelShader    ? d.PixelShader->sha1().string().substr(0,8)    : "null");
  s += " NCA="     + std::to_string(d.NumColorAttachments);
  s += " CAF=[";
  for (uint8_t i = 0; i < 8; i++) {
      if (i) s += ",";
      s += std::to_string(static_cast<uint32_t>(d.ColorAttachmentFormats[i]));
  }
  s += "] DSF="    + std::to_string(static_cast<uint32_t>(d.DepthStencilFormat));
  s += " TOPO="    + std::to_string(static_cast<uint32_t>(d.TopologyClass));
  s += " RASTER="  + std::to_string(d.RasterizationEnabled);
  s += " SC="      + std::to_string(d.SampleCount);
  s += " GSSTrip=" + std::to_string(d.GSStripTopology);
  s += " IBF="     + std::to_string(static_cast<uint32_t>(d.IndexBufferFormat));
  s += " SMASK="   + std::to_string(d.SampleMask);
  s += " GSPass="  + std::to_string(d.GSPassthrough);
  s += " BLEND=" + std::to_string(reinterpret_cast<uintptr_t>(d.BlendState));
  s += " IL="    + std::to_string(reinterpret_cast<uintptr_t>(d.InputLayout));
  s += " SO="    + std::to_string(reinterpret_cast<uintptr_t>(d.SOLayout));
  s += " HS="    + std::to_string(reinterpret_cast<uintptr_t>(d.HullShader));
  s += " GS="    + std::to_string(reinterpret_cast<uintptr_t>(d.GeometryShader));
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
PSColorOutputs ExtractPSColorOutputs(const void* dxbc, size_t size) {
  PSColorOutputs out{};
  out.classes.fill(ScalarClass::None);

  microsoft::CDXBCParser container;
  if (FAILED(container.ReadDXBC(dxbc, size))) return out;
  UINT idx = container.FindNextMatchingBlob(microsoft::DXBC_OutputSignature, 0);
  if (idx == DXBC_BLOB_NOT_FOUND) {
    Logger::info("PSColorOutputs: OSGN blob not found");
    return out;
  }

  microsoft::CSignatureParser parser;
  if (FAILED(parser.ReadSignature4(container.GetBlob(idx),
                                  container.GetBlobSize(idx)))) {
    Logger::info("PSColorOutputs: ReadSignature4 failed");
    return out;
  }

  const microsoft::D3D11_SIGNATURE_PARAMETER* params = nullptr;
  UINT count = parser.GetParameters(&params);

  int max_slot = -1;
  for (UINT i = 0; i < count; ++i) {
    const auto& p = params[i];
    bool is_target =
        p.SystemValue == D3D_NAME_TARGET ||
        (p.SystemValue == D3D_NAME_UNDEFINED &&
        p.SemanticName != nullptr &&
        _stricmp(p.SemanticName, "SV_TARGET") == 0);
    if (!is_target) continue;

    int slot = static_cast<int>(p.SemanticIndex);
    if (slot < 0 || slot >= 8) continue;

    // A given SV_TargetN normally appears as one signature entry, but
    // OR the mask defensively in case fxc emits split-register entries.
    out.classes[slot] = to_scalar_class(p.ComponentType);
    out.masks[slot]   = static_cast<uint8_t>(out.masks[slot] | (p.Mask & 0xF));
    if (slot > max_slot) max_slot = slot;
  }

  out.count = static_cast<uint8_t>(max_slot + 1);
  return out;
}

VSInputRequirement ExtractVSInputRequirement(const void* dxbc, size_t size) {
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
  if (FAILED(parser.ReadSignature4(container.GetBlob(idx),
                                  container.GetBlobSize(idx)))) {

    Logger::info("ExtractVSInputRequirement: ReadSignature4 failed");
    return req;
  }

  const microsoft::D3D11_SIGNATURE_PARAMETER* params = nullptr;
  UINT count = parser.GetParameters(&params);

  for (UINT i = 0; i < count; ++i) {
    const auto& p = params[i];
    if (p.SystemValue != D3D_NAME_UNDEFINED) continue; // SV_VertexID/InstanceID aren't IA inputs

    VSInputRequirement::Element e{};
    const char* s = p.SemanticName ? p.SemanticName : "";
    for (size_t n = 0; s[n] && n < sizeof(e.semantic) - 1; ++n)
      e.semantic[n] = (char)toupper((unsigned char)s[n]);
    e.semantic_index  = p.SemanticIndex;
    e.reg             = p.Register;
    e.mask            = p.Mask & 0xF;
    e.component_class = static_cast<uint8_t>(to_scalar_class(p.ComponentType));  // your existing helper
    req.elements.push_back(e);
  }
  return req;
}

VSInputRequirement ExtractVSInputRequirementNoSize(const void* dxbc) {
  VSInputRequirement req;
  microsoft::CDXBCParser container;
  // Trusts the total-size field in the DXBC header instead of an outside length.
  if (FAILED(container.ReadDXBCAssumingValidSize(dxbc))) return req;
  UINT idx = container.FindNextMatchingBlob(microsoft::DXBC_InputSignature, 0);
  if (idx == DXBC_BLOB_NOT_FOUND) return req;

  microsoft::CSignatureParser parser;
  if (FAILED(parser.ReadSignature4(container.GetBlob(idx),
                                  container.GetBlobSize(idx))))
    return req;

  const microsoft::D3D11_SIGNATURE_PARAMETER* params = nullptr;
  UINT count = parser.GetParameters(&params);
  for (UINT i = 0; i < count; ++i) {
    const auto& p = params[i];
    if (p.SystemValue != D3D_NAME_UNDEFINED) continue;
    VSInputRequirement::Element e{};
    const char* s = p.SemanticName ? p.SemanticName : "";
    for (size_t n = 0; s[n] && n < sizeof(e.semantic) - 1; ++n)
      e.semantic[n] = (char)toupper((unsigned char)s[n]);
    e.semantic_index = p.SemanticIndex;
    e.reg            = p.Register;
    e.mask           = p.Mask & 0xF;
    e.component_class = static_cast<uint8_t>(to_scalar_class(p.ComponentType));
    req.elements.push_back(e);
  }
  return req;
}

Sha1Digest HashVSInputRequirement(const VSInputRequirement& req) {
  if (req.elements.empty()) {
    Logger::info("HashVSInputRequirement: no IA inputs.");
    return {};  // no IA inputs => associates with null layout
  }
  Sha1HashState h;
  for (const auto& e : req.elements) h.update(e);  // Element is trivially copyable
  h.update(req.elements.size());
  return h.final();
}

// Does this CachedInputLayout satisfy the VS's input signature?
bool LayoutCoversVS(InputLayout* il, const VSInputRequirement& req) {
  if (req.elements.empty())
    return il == nullptr;   // VS reads nothing => only a null layout is "correct"
  if (!il) return false;

  MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC* els = nullptr;
  uint32_t n = il->input_layout_element(&els);

  for (const auto& need : req.elements) {
    bool found = false;
    for (uint32_t j = 0; j < n; ++j) {
      // DXMT's IA element keys vertex-fetch by input register; match on that.
      // (Adjust the field name to whatever MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC
      //  exposes — it's the register/attribute index the VS reads.)
      if (els[j].Index == need.reg) { found = true; break; }
    }
    if (!found) return false;  // VS reads a register the layout doesn't feed
  }
  return true;
}

void lock_shader_deterministic_fields(MTL_GRAPHICS_PIPELINE_DESC *pDesc) {
  pDesc->GSStripTopology = false;
  pDesc->GSPassthrough = ~0u;
  pDesc->RasterizationEnabled = pDesc->PixelShader != nullptr;
  pDesc->SampleMask = 0xFFFFFFFFu;
  pDesc->SOLayout = nullptr;
}

void set_shader_defaults(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                         std::unique_ptr<ManagedDeviceChild<IMTLD3D11BlendState>>& default_blend_state_ptr) {
  // pDesc->IndexBufferFormat = SM50_INDEX_BUFFER_FORMAT_UINT16;
  // pDesc->DepthStencilFormat = WMTPixelFormatDepth32Float_Stencil8;
  pDesc->TopologyClass = WMTPrimitiveTopologyClassTriangle;
  pDesc->BlendState = default_blend_state_ptr.get();
}

std::vector<Prediction> predictor_reflection_and_default(ManagedShader vs, ManagedShader ps,
                                                          std::unique_ptr<ManagedDeviceChild<IMTLD3D11BlendState>>& default_blend_state_ptr,
                                                          std::unordered_map<Sha1Digest, std::unordered_set<Sha1Digest>>& input_requirement_to_layouts,
                                                          std::unordered_map<Sha1Digest, InputLayout*>& layout_by_sha1,
                                                          std::unordered_set<Prediction>& previous_predictions
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
  std::vector<InputLayout*> valid_ils;

  // Get list of valid input layouts
  for (const auto input_req_digest: input_requirement_to_layouts[HashVSInputRequirement(vs->vs_input_req)]) {
    valid_ils.push_back(layout_by_sha1[input_req_digest]);
  }

  // Sample count cross product
  for (uint8_t i = 0; i < 2; i++) {
    MTL_GRAPHICS_PIPELINE_DESC pDesc{};
    pDesc.SampleCount = i + 1;
    pDesc.VertexShader = vs;
    pDesc.PixelShader = ps;

    for (int dsf_selector = 0; dsf_selector < 2; ++dsf_selector) {
      switch (dsf_selector)
      {
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
        pDesc.IndexBufferFormat = ibf_selector % 2 == 0 ? SM50_INDEX_BUFFER_FORMAT_UINT16 : SM50_INDEX_BUFFER_FORMAT_NONE;
        
        for (const auto input_layout: valid_ils) {
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
                pDesc.ColorAttachmentFormats[i] = j%2 == 0 ? WMTPixelFormatRG11B10Float : WMTPixelFormatRGBA8Unorm_sRGB;
                break;

                case ScalarClass::SInt :
                pDesc.ColorAttachmentFormats[i] = WMTPixelFormatR32Sint;
                break;

                case ScalarClass::UInt :
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
std::vector<Prediction> predictor_most_frequent_global(
    ManagedShader vs,
    ManagedShader ps,
    std::unordered_map<MTL_GRAPHICS_PIPELINE_DESC, uint32_t>& freq_table,
    std::unordered_set<Prediction>& previous_predictions,
    uint32_t top_n = 5)
{
    // Build a sorted list of (count, desc) pairs
    std::vector<std::pair<uint32_t, MTL_GRAPHICS_PIPELINE_DESC>> ranked;
    ranked.reserve(freq_table.size());
    for (const auto& [desc, count] : freq_table)
        ranked.push_back({count, desc});

    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<Prediction> predictions;
    for (uint32_t i = 0; i < std::min(top_n, (uint32_t)ranked.size()); i++) {
        MTL_GRAPHICS_PIPELINE_DESC desc = ranked[i].second;

        // Substitute the new pair's shaders
        desc.VertexShader   = vs;
        desc.PixelShader    = ps;
        desc.HullShader     = nullptr;
        desc.DomainShader   = nullptr;
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

} // namespace dxmt