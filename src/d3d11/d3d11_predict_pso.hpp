#pragma once
#include "d3d11_shader.hpp"
#include "d3d11_pipeline.hpp"
#include "sha1/sha1_util.hpp"
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include "DXBCParser/ShaderBinary.h"
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

static std::string format_blend_desc(IMTLD3D11BlendState* bs) {
    if (!bs) return "null";
    D3D11_BLEND_DESC1 bd;
    bs->GetDesc1(&bd);
    
    std::string s = str::format(
        "AlphaToCoverage=", (uint32_t)bd.AlphaToCoverageEnable,
        " IndepBlend=", (uint32_t)bd.IndependentBlendEnable,
        "\n"
    );
    
    for (uint8_t i = 0; i < 8; i++) {
        const auto& rt = bd.RenderTarget[i];
        s += str::format(
            "  RT[", (uint32_t)i, "]:",
            " BlendEnable=",      (uint32_t)rt.BlendEnable,
            " LogicOpEnable=",    (uint32_t)rt.LogicOpEnable,
            " SrcBlend=",         (uint32_t)rt.SrcBlend,
            " DestBlend=",        (uint32_t)rt.DestBlend,
            " BlendOp=",          (uint32_t)rt.BlendOp,
            " SrcBlendAlpha=",    (uint32_t)rt.SrcBlendAlpha,
            " DestBlendAlpha=",   (uint32_t)rt.DestBlendAlpha,
            " BlendOpAlpha=",     (uint32_t)rt.BlendOpAlpha,
            " LogicOp=",          (uint32_t)rt.LogicOp,
            " WriteMask=",        (uint32_t)rt.RenderTargetWriteMask,
            "\n"
        );
    }
    
    return s;
}

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
    uint32_t ps_valid_render_targets,
    const std::unordered_map<IMTLD3D11BlendState*, uint8_t>& blend_min_ps_outs)
{
    if (!bs) return true;

    // If we've observed this blend before, the minimum ps_outs.count
    // seen with it is a hard lower bound — reject anything below it.
    auto it = blend_min_ps_outs.find(bs);
    if (it != blend_min_ps_outs.end()) {
        if (ps_valid_render_targets < it->second)
          return false;
    }

    
    // D3D11_BLEND_DESC1 bd;
    // bs->GetDesc1(&bd);

    // if (!bd.IndependentBlendEnable) {
    //     // Only RT[0] applies to slot 0, all other slots are irrelevant
    //     const auto& rt = bd.RenderTarget[0];
    //     bool slot0_active = rt.BlendEnable || rt.RenderTargetWriteMask != 0;
    //     if (!slot0_active) return true;
    //     bool slot0_bound  = nca > 0;
    //     bool ps_writes_0  = ps_valid_render_targets > 0;
    //     if (slot0_active && slot0_bound && !ps_writes_0) return false;
    //     if (slot0_active && !slot0_bound)                return false;
    //     return true;
    // }

    // // IndependentBlendEnable=true: check each slot individually
    // for (uint8_t slot = 0; slot < 8; slot++) {
    //     const auto& rt = bd.RenderTarget[slot];
    //     bool slot_active = rt.BlendEnable || rt.RenderTargetWriteMask != 0;
    //     if (!slot_active) continue;
    //     bool slot_bound  = slot < nca;
    //     bool ps_writes   = (ps_valid_render_targets >> slot) & 1;
    //     if (slot_active && slot_bound && !ps_writes) return false;
    //     if (slot_active && !slot_bound)              return false;
    // }
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

// Raw DXBC opcode token — just the type, stripped of length/flags
using OpcodeSeq = std::vector<uint32_t>;

static const char* opcode_name(uint32_t op) {
    using namespace microsoft;
    using T = D3D10_SB_OPCODE_TYPE;
    switch (static_cast<T>(op)) {
    case D3D10_SB_OPCODE_ADD:                                   return "ADD";
    case D3D10_SB_OPCODE_AND:                                   return "AND";
    case D3D10_SB_OPCODE_BREAK:                                 return "BREAK";
    case D3D10_SB_OPCODE_BREAKC:                                return "BREAKC";
    case D3D10_SB_OPCODE_CALL:                                  return "CALL";
    case D3D10_SB_OPCODE_CALLC:                                 return "CALLC";
    case D3D10_SB_OPCODE_CASE:                                  return "CASE";
    case D3D10_SB_OPCODE_CONTINUE:                              return "CONTINUE";
    case D3D10_SB_OPCODE_CONTINUEC:                             return "CONTINUEC";
    case D3D10_SB_OPCODE_CUT:                                   return "CUT";
    case D3D10_SB_OPCODE_DEFAULT:                               return "DEFAULT";
    case D3D10_SB_OPCODE_DERIV_RTX:                             return "DERIV_RTX";
    case D3D10_SB_OPCODE_DERIV_RTY:                             return "DERIV_RTY";
    case D3D10_SB_OPCODE_DISCARD:                               return "DISCARD";
    case D3D10_SB_OPCODE_DIV:                                   return "DIV";
    case D3D10_SB_OPCODE_DP2:                                   return "DP2";
    case D3D10_SB_OPCODE_DP3:                                   return "DP3";
    case D3D10_SB_OPCODE_DP4:                                   return "DP4";
    case D3D10_SB_OPCODE_ELSE:                                  return "ELSE";
    case D3D10_SB_OPCODE_EMIT:                                  return "EMIT";
    case D3D10_SB_OPCODE_EMITTHENCUT:                           return "EMITTHENCUT";
    case D3D10_SB_OPCODE_ENDIF:                                 return "ENDIF";
    case D3D10_SB_OPCODE_ENDLOOP:                               return "ENDLOOP";
    case D3D10_SB_OPCODE_ENDSWITCH:                             return "ENDSWITCH";
    case D3D10_SB_OPCODE_EQ:                                    return "EQ";
    case D3D10_SB_OPCODE_EXP:                                   return "EXP";
    case D3D10_SB_OPCODE_FRC:                                   return "FRC";
    case D3D10_SB_OPCODE_FTOI:                                  return "FTOI";
    case D3D10_SB_OPCODE_FTOU:                                  return "FTOU";
    case D3D10_SB_OPCODE_GE:                                    return "GE";
    case D3D10_SB_OPCODE_IADD:                                  return "IADD";
    case D3D10_SB_OPCODE_IF:                                    return "IF";
    case D3D10_SB_OPCODE_IEQ:                                   return "IEQ";
    case D3D10_SB_OPCODE_IGE:                                   return "IGE";
    case D3D10_SB_OPCODE_ILT:                                   return "ILT";
    case D3D10_SB_OPCODE_IMAD:                                  return "IMAD";
    case D3D10_SB_OPCODE_IMAX:                                  return "IMAX";
    case D3D10_SB_OPCODE_IMIN:                                  return "IMIN";
    case D3D10_SB_OPCODE_IMUL:                                  return "IMUL";
    case D3D10_SB_OPCODE_INE:                                   return "INE";
    case D3D10_SB_OPCODE_INEG:                                  return "INEG";
    case D3D10_SB_OPCODE_ISHL:                                  return "ISHL";
    case D3D10_SB_OPCODE_ISHR:                                  return "ISHR";
    case D3D10_SB_OPCODE_ITOF:                                  return "ITOF";
    case D3D10_SB_OPCODE_LABEL:                                 return "LABEL";
    case D3D10_SB_OPCODE_LD:                                    return "LD";
    case D3D10_SB_OPCODE_LD_MS:                                 return "LD_MS";
    case D3D10_SB_OPCODE_LOG:                                   return "LOG";
    case D3D10_SB_OPCODE_LOOP:                                  return "LOOP";
    case D3D10_SB_OPCODE_LT:                                    return "LT";
    case D3D10_SB_OPCODE_MAD:                                   return "MAD";
    case D3D10_SB_OPCODE_MIN:                                   return "MIN";
    case D3D10_SB_OPCODE_MAX:                                   return "MAX";
    case D3D10_SB_OPCODE_CUSTOMDATA:                            return "CUSTOMDATA";
    case D3D10_SB_OPCODE_MOV:                                   return "MOV";
    case D3D10_SB_OPCODE_MOVC:                                  return "MOVC";
    case D3D10_SB_OPCODE_MUL:                                   return "MUL";
    case D3D10_SB_OPCODE_NE:                                    return "NE";
    case D3D10_SB_OPCODE_NOP:                                   return "NOP";
    case D3D10_SB_OPCODE_NOT:                                   return "NOT";
    case D3D10_SB_OPCODE_OR:                                    return "OR";
    case D3D10_SB_OPCODE_RESINFO:                               return "RESINFO";
    case D3D10_SB_OPCODE_RET:                                   return "RET";
    case D3D10_SB_OPCODE_RETC:                                  return "RETC";
    case D3D10_SB_OPCODE_ROUND_NE:                              return "ROUND_NE";
    case D3D10_SB_OPCODE_ROUND_NI:                              return "ROUND_NI";
    case D3D10_SB_OPCODE_ROUND_PI:                              return "ROUND_PI";
    case D3D10_SB_OPCODE_ROUND_Z:                               return "ROUND_Z";
    case D3D10_SB_OPCODE_RSQ:                                   return "RSQ";
    case D3D10_SB_OPCODE_SAMPLE:                                return "SAMPLE";
    case D3D10_SB_OPCODE_SAMPLE_C:                              return "SAMPLE_C";
    case D3D10_SB_OPCODE_SAMPLE_C_LZ:                          return "SAMPLE_C_LZ";
    case D3D10_SB_OPCODE_SAMPLE_L:                              return "SAMPLE_L";
    case D3D10_SB_OPCODE_SAMPLE_D:                              return "SAMPLE_D";
    case D3D10_SB_OPCODE_SAMPLE_B:                              return "SAMPLE_B";
    case D3D10_SB_OPCODE_SQRT:                                  return "SQRT";
    case D3D10_SB_OPCODE_SWITCH:                                return "SWITCH";
    case D3D10_SB_OPCODE_SINCOS:                                return "SINCOS";
    case D3D10_SB_OPCODE_UDIV:                                  return "UDIV";
    case D3D10_SB_OPCODE_ULT:                                   return "ULT";
    case D3D10_SB_OPCODE_UGE:                                   return "UGE";
    case D3D10_SB_OPCODE_UMUL:                                  return "UMUL";
    case D3D10_SB_OPCODE_UMAD:                                  return "UMAD";
    case D3D10_SB_OPCODE_UMAX:                                  return "UMAX";
    case D3D10_SB_OPCODE_UMIN:                                  return "UMIN";
    case D3D10_SB_OPCODE_USHR:                                  return "USHR";
    case D3D10_SB_OPCODE_UTOF:                                  return "UTOF";
    case D3D10_SB_OPCODE_XOR:                                   return "XOR";
    case D3D10_SB_OPCODE_DCL_RESOURCE:                         return "DCL_RESOURCE";
    case D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER:                  return "DCL_CONSTANT_BUFFER";
    case D3D10_SB_OPCODE_DCL_SAMPLER:                          return "DCL_SAMPLER";
    case D3D10_SB_OPCODE_DCL_INDEX_RANGE:                      return "DCL_INDEX_RANGE";
    case D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY:     return "DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY";
    case D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE:               return "DCL_GS_INPUT_PRIMITIVE";
    case D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT:          return "DCL_MAX_OUTPUT_VERTEX_COUNT";
    case D3D10_SB_OPCODE_DCL_INPUT:                            return "DCL_INPUT";
    case D3D10_SB_OPCODE_DCL_INPUT_SGV:                        return "DCL_INPUT_SGV";
    case D3D10_SB_OPCODE_DCL_INPUT_SIV:                        return "DCL_INPUT_SIV";
    case D3D10_SB_OPCODE_DCL_INPUT_PS:                         return "DCL_INPUT_PS";
    case D3D10_SB_OPCODE_DCL_INPUT_PS_SGV:                     return "DCL_INPUT_PS_SGV";
    case D3D10_SB_OPCODE_DCL_INPUT_PS_SIV:                     return "DCL_INPUT_PS_SIV";
    case D3D10_SB_OPCODE_DCL_OUTPUT:                           return "DCL_OUTPUT";
    case D3D10_SB_OPCODE_DCL_OUTPUT_SGV:                       return "DCL_OUTPUT_SGV";
    case D3D10_SB_OPCODE_DCL_OUTPUT_SIV:                       return "DCL_OUTPUT_SIV";
    case D3D10_SB_OPCODE_DCL_TEMPS:                            return "DCL_TEMPS";
    case D3D10_SB_OPCODE_DCL_INDEXABLE_TEMP:                   return "DCL_INDEXABLE_TEMP";
    case D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS:                     return "DCL_GLOBAL_FLAGS";
    case D3D11_SB_OPCODE_EMIT_STREAM:                          return "EMIT_STREAM";
    case D3D11_SB_OPCODE_CUT_STREAM:                           return "CUT_STREAM";
    case D3D11_SB_OPCODE_EMITTHENCUT_STREAM:                   return "EMITTHENCUT_STREAM";
    case D3D11_SB_OPCODE_INTERFACE_CALL:                       return "INTERFACE_CALL";
    case D3D11_SB_OPCODE_BUFINFO:                              return "BUFINFO";
    case D3D11_SB_OPCODE_DERIV_RTX_COARSE:                     return "DERIV_RTX_COARSE";
    case D3D11_SB_OPCODE_DERIV_RTX_FINE:                       return "DERIV_RTX_FINE";
    case D3D11_SB_OPCODE_DERIV_RTY_COARSE:                     return "DERIV_RTY_COARSE";
    case D3D11_SB_OPCODE_DERIV_RTY_FINE:                       return "DERIV_RTY_FINE";
    case D3D11_SB_OPCODE_GATHER4_C:                            return "GATHER4_C";
    case D3D11_SB_OPCODE_GATHER4_PO:                           return "GATHER4_PO";
    case D3D11_SB_OPCODE_GATHER4_PO_C:                         return "GATHER4_PO_C";
    case D3D11_SB_OPCODE_RCP:                                  return "RCP";
    case D3D11_SB_OPCODE_F32TOF16:                             return "F32TOF16";
    case D3D11_SB_OPCODE_F16TOF32:                             return "F16TOF32";
    case D3D11_SB_OPCODE_UADDC:                                return "UADDC";
    case D3D11_SB_OPCODE_USUBB:                                return "USUBB";
    case D3D11_SB_OPCODE_COUNTBITS:                            return "COUNTBITS";
    case D3D11_SB_OPCODE_FIRSTBIT_HI:                          return "FIRSTBIT_HI";
    case D3D11_SB_OPCODE_FIRSTBIT_LO:                          return "FIRSTBIT_LO";
    case D3D11_SB_OPCODE_FIRSTBIT_SHI:                         return "FIRSTBIT_SHI";
    case D3D11_SB_OPCODE_UBFE:                                 return "UBFE";
    case D3D11_SB_OPCODE_IBFE:                                 return "IBFE";
    case D3D11_SB_OPCODE_BFI:                                  return "BFI";
    case D3D11_SB_OPCODE_BFREV:                                return "BFREV";
    case D3D11_SB_OPCODE_SWAPC:                                return "SWAPC";
    case D3D11_SB_OPCODE_DCL_STREAM:                           return "DCL_STREAM";
    case D3D11_SB_OPCODE_DCL_FUNCTION_BODY:                    return "DCL_FUNCTION_BODY";
    case D3D11_SB_OPCODE_DCL_FUNCTION_TABLE:                   return "DCL_FUNCTION_TABLE";
    case D3D11_SB_OPCODE_DCL_INTERFACE:                        return "DCL_INTERFACE";
    case D3D11_SB_OPCODE_DCL_INPUT_CONTROL_POINT_COUNT:        return "DCL_INPUT_CONTROL_POINT_COUNT";
    case D3D11_SB_OPCODE_DCL_OUTPUT_CONTROL_POINT_COUNT:       return "DCL_OUTPUT_CONTROL_POINT_COUNT";
    case D3D11_SB_OPCODE_DCL_TESS_DOMAIN:                      return "DCL_TESS_DOMAIN";
    case D3D11_SB_OPCODE_DCL_TESS_PARTITIONING:                return "DCL_TESS_PARTITIONING";
    case D3D11_SB_OPCODE_DCL_TESS_OUTPUT_PRIMITIVE:            return "DCL_TESS_OUTPUT_PRIMITIVE";
    case D3D11_SB_OPCODE_DCL_HS_MAX_TESSFACTOR:                return "DCL_HS_MAX_TESSFACTOR";
    case D3D11_SB_OPCODE_DCL_HS_FORK_PHASE_INSTANCE_COUNT:     return "DCL_HS_FORK_PHASE_INSTANCE_COUNT";
    case D3D11_SB_OPCODE_DCL_HS_JOIN_PHASE_INSTANCE_COUNT:     return "DCL_HS_JOIN_PHASE_INSTANCE_COUNT";
    case D3D11_SB_OPCODE_DCL_THREAD_GROUP:                     return "DCL_THREAD_GROUP";
    case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED:      return "DCL_UAV_TYPED";
    case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_RAW:        return "DCL_UAV_RAW";
    case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED: return "DCL_UAV_STRUCTURED";
    case D3D11_SB_OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_RAW:   return "DCL_TGSM_RAW";
    case D3D11_SB_OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_STRUCTURED: return "DCL_TGSM_STRUCTURED";
    case D3D11_SB_OPCODE_DCL_RESOURCE_RAW:                     return "DCL_RESOURCE_RAW";
    case D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED:              return "DCL_RESOURCE_STRUCTURED";
    case D3D11_SB_OPCODE_LD_UAV_TYPED:                         return "LD_UAV_TYPED";
    case D3D11_SB_OPCODE_STORE_UAV_TYPED:                      return "STORE_UAV_TYPED";
    case D3D11_SB_OPCODE_LD_RAW:                               return "LD_RAW";
    case D3D11_SB_OPCODE_STORE_RAW:                            return "STORE_RAW";
    case D3D11_SB_OPCODE_LD_STRUCTURED:                        return "LD_STRUCTURED";
    case D3D11_SB_OPCODE_STORE_STRUCTURED:                     return "STORE_STRUCTURED";
    case D3D11_SB_OPCODE_ATOMIC_AND:                           return "ATOMIC_AND";
    case D3D11_SB_OPCODE_ATOMIC_OR:                            return "ATOMIC_OR";
    case D3D11_SB_OPCODE_ATOMIC_XOR:                           return "ATOMIC_XOR";
    case D3D11_SB_OPCODE_ATOMIC_CMP_STORE:                     return "ATOMIC_CMP_STORE";
    case D3D11_SB_OPCODE_ATOMIC_IADD:                          return "ATOMIC_IADD";
    case D3D11_SB_OPCODE_ATOMIC_IMAX:                          return "ATOMIC_IMAX";
    case D3D11_SB_OPCODE_ATOMIC_IMIN:                          return "ATOMIC_IMIN";
    case D3D11_SB_OPCODE_ATOMIC_UMAX:                          return "ATOMIC_UMAX";
    case D3D11_SB_OPCODE_ATOMIC_UMIN:                          return "ATOMIC_UMIN";
    case D3D11_SB_OPCODE_IMM_ATOMIC_ALLOC:                     return "IMM_ATOMIC_ALLOC";
    case D3D11_SB_OPCODE_IMM_ATOMIC_CONSUME:                   return "IMM_ATOMIC_CONSUME";
    case D3D11_SB_OPCODE_IMM_ATOMIC_IADD:                      return "IMM_ATOMIC_IADD";
    case D3D11_SB_OPCODE_IMM_ATOMIC_AND:                       return "IMM_ATOMIC_AND";
    case D3D11_SB_OPCODE_IMM_ATOMIC_OR:                        return "IMM_ATOMIC_OR";
    case D3D11_SB_OPCODE_IMM_ATOMIC_XOR:                       return "IMM_ATOMIC_XOR";
    case D3D11_SB_OPCODE_IMM_ATOMIC_EXCH:                      return "IMM_ATOMIC_EXCH";
    case D3D11_SB_OPCODE_IMM_ATOMIC_CMP_EXCH:                  return "IMM_ATOMIC_CMP_EXCH";
    case D3D11_SB_OPCODE_IMM_ATOMIC_IMAX:                      return "IMM_ATOMIC_IMAX";
    case D3D11_SB_OPCODE_IMM_ATOMIC_IMIN:                      return "IMM_ATOMIC_IMIN";
    case D3D11_SB_OPCODE_IMM_ATOMIC_UMAX:                      return "IMM_ATOMIC_UMAX";
    case D3D11_SB_OPCODE_IMM_ATOMIC_UMIN:                      return "IMM_ATOMIC_UMIN";
    case D3D11_SB_OPCODE_SYNC:                                 return "SYNC";
    case D3D11_SB_OPCODE_DADD:                                 return "DADD";
    case D3D11_SB_OPCODE_DMAX:                                 return "DMAX";
    case D3D11_SB_OPCODE_DMIN:                                 return "DMIN";
    case D3D11_SB_OPCODE_DMUL:                                 return "DMUL";
    case D3D11_SB_OPCODE_DEQ:                                  return "DEQ";
    case D3D11_SB_OPCODE_DGE:                                  return "DGE";
    case D3D11_SB_OPCODE_DLT:                                  return "DLT";
    case D3D11_SB_OPCODE_DNE:                                  return "DNE";
    case D3D11_SB_OPCODE_DMOV:                                 return "DMOV";
    case D3D11_SB_OPCODE_DMOVC:                                return "DMOVC";
    case D3D11_SB_OPCODE_DTOF:                                 return "DTOF";
    case D3D11_SB_OPCODE_FTOD:                                 return "FTOD";
    case D3D11_SB_OPCODE_EVAL_SNAPPED:                         return "EVAL_SNAPPED";
    case D3D11_SB_OPCODE_EVAL_SAMPLE_INDEX:                    return "EVAL_SAMPLE_INDEX";
    case D3D11_SB_OPCODE_EVAL_CENTROID:                        return "EVAL_CENTROID";
    case D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT:                return "DCL_GS_INSTANCE_COUNT";
    case D3D11_1_SB_OPCODE_DDIV:                               return "DDIV";
    case D3D11_1_SB_OPCODE_DFMA:                               return "DFMA";
    case D3D11_1_SB_OPCODE_DRCP:                               return "DRCP";
    case D3D11_1_SB_OPCODE_MSAD:                               return "MSAD";
    case D3D11_1_SB_OPCODE_DTOI:                               return "DTOI";
    case D3D11_1_SB_OPCODE_DTOU:                               return "DTOU";
    case D3D11_1_SB_OPCODE_ITOD:                               return "ITOD";
    case D3D11_1_SB_OPCODE_UTOD:                               return "UTOD";
    default: return "UNKNOWN";
    }
}

static OpcodeSeq extract_opcode_sequence(const void* dxbc, size_t size) {
    OpcodeSeq opcodes;

    microsoft::CDXBCParser container;
    if (FAILED(container.ReadDXBC(dxbc, size)))
        return opcodes;

    // GetBlob() strips the DXBCBlobHeader (FourCC + BlobSize), so the pointer
    // lands directly on the version token — exactly what SetShader() expects.
    UINT idx = container.FindNextMatchingBlob(microsoft::DXBC_GenericShaderEx, 0);
    if (idx == DXBC_BLOB_NOT_FOUND)
        idx = container.FindNextMatchingBlob(microsoft::DXBC_GenericShader, 0);
    if (idx == DXBC_BLOB_NOT_FOUND)
        return opcodes;

    const auto* tokens = reinterpret_cast<const CShaderToken*>(container.GetBlob(idx));
    if (!tokens)
        return opcodes;

    microsoft::D3D10ShaderBinary::CShaderCodeParser parser(tokens);
    while (!parser.EndOfShader()) {
        microsoft::D3D10ShaderBinary::CInstruction inst;
        parser.ParseInstruction(&inst);
        opcodes.push_back(static_cast<uint32_t>(inst.OpCode()));
    }

    constexpr size_t kLogMax = 50;
    std::string seq;
    for (size_t i = 0; i < std::min(opcodes.size(), kLogMax); ++i) {
        if (i) seq += ' ';
        seq += opcode_name(opcodes[i]);
    }

    return opcodes;
}

static constexpr uint32_t kMinHashSize = 128;
static constexpr uint32_t kShingleK    = 4;

using MinHashSig = std::array<uint32_t, kMinHashSize>;

// FNV-1a mix for combining 4 opcodes into a shingle hash
static uint32_t shingle_hash(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t h = 2166136261u;
    h = (h ^ a) * 16777619u;
    h = (h ^ b) * 16777619u;
    h = (h ^ c) * 16777619u;
    h = (h ^ d) * 16777619u;
    return h;
}

static MinHashSig compute_minhash(const OpcodeSeq& ops) {
    MinHashSig sig;
    sig.fill(UINT32_MAX);

    if (ops.size() < kShingleK) {
        // Shader too short for 4-grams — use individual opcodes as shingles
        for (uint32_t op : ops) {
            for (uint32_t i = 0; i < kMinHashSize; i++) {
                // Universal hash: (a*x + b) mod p, different (a,b) per band
                uint32_t h = (1000003u * i + 1234567u) ^ op;
                h ^= h >> 16; h *= 0x45d9f3b; h ^= h >> 16;
                if (h < sig[i]) sig[i] = h;
            }
        }
        return sig;
    }

    for (size_t s = 0; s + kShingleK <= ops.size(); s++) {
        uint32_t sh = shingle_hash(ops[s], ops[s+1], ops[s+2], ops[s+3]);
        for (uint32_t i = 0; i < kMinHashSize; i++) {
            // Per-hash-function permutation of the shingle hash
            uint32_t h = sh ^ (i * 2654435761u);
            h ^= h >> 16; h *= 0x45d9f3b; h ^= h >> 16;
            if (h < sig[i]) sig[i] = h;
        }
    }
    return sig;
}

static float jaccard_estimate(const MinHashSig& a, const MinHashSig& b) {
    uint32_t matches = 0;
    for (uint32_t i = 0; i < kMinHashSize; i++)
        if (a[i] == b[i]) matches++;
    return static_cast<float>(matches) / kMinHashSize;
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
    const std::unordered_map<IMTLD3D11BlendState*, uint8_t>& blend_min_ps_outs,
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
      if (candidates[i].second.PixelShader && !blend_compatible_with_ps(candidates[i].second.BlendState, candidates[i].second.PixelShader->ps_outs.count, candidates[i].second.PixelShader->reflection().PSValidRenderTargets, blend_min_ps_outs))
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

    auto ps_it = ps_rps_blend_table.find(ps->sha1());
    if (ps_it == ps_rps_blend_table.end())
        return predictions;

    const auto& rps_blend_map = ps_it->second;

    std::vector<std::pair<uint32_t, std::pair<RenderPassSignature, IMTLD3D11BlendState*>>> ranked;
    ranked.reserve(rps_blend_map.size());
    for (const auto& [key, count] : rps_blend_map)
        ranked.push_back({count, key});
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b){ return a.first > b.first; });

    uint32_t limit = std::min(top_k, (uint32_t)ranked.size());

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
            for (auto ibf : {SM50_INDEX_BUFFER_FORMAT_UINT16}) {

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

std::vector<Prediction> predictor_global_rps_blend_compatible(
    ManagedShader vs,
    ManagedShader ps,
    const std::unordered_map<RenderPassSignature,
        std::unordered_map<IMTLD3D11BlendState*, uint32_t>>& rps_blend_table,
    std::unordered_map<Sha1Digest, std::unordered_set<Sha1Digest>>& input_req_to_layouts,
    std::unordered_map<Sha1Digest, InputLayout*>& layout_by_sha1,
    std::unordered_set<Prediction>& previous_predictions,
    const std::unordered_map<IMTLD3D11BlendState*, uint8_t>& blend_min_ps_outs,
    uint32_t top_k = 3)
{
    std::vector<Prediction> predictions;
    if (!ps) return predictions;

    // Collect all (RPS, blend) pairs compatible with this PS, ranked by count
    std::vector<std::pair<uint32_t, std::pair<RenderPassSignature, IMTLD3D11BlendState*>>> ranked;
    uint32_t ps_vrt = ps->ps_outs.count;

    for (const auto& [rps, blend_map] : rps_blend_table) {
        // if (!rps_compatible_with_ps(rps, ps))
        //     continue;
        for (const auto& [blend, count] : blend_map) {
            if (!blend_compatible_with_ps(blend, rps.num_color_attachments, ps_vrt, blend_min_ps_outs))
                continue;
            ranked.push_back({count, {rps, blend}});
        }
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b){ return a.first > b.first; });

    // VS-compatible input layouts
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
            return predictions;
    }

    uint32_t limit = std::min(top_k, (uint32_t)ranked.size());

    for (uint32_t i = 0; i < limit; i++) {
        const auto& [rps, blend] = ranked[i].second;

        for (auto* il : valid_ils) {
            for (auto ibf : {SM50_INDEX_BUFFER_FORMAT_NONE,
                             SM50_INDEX_BUFFER_FORMAT_UINT16}) {
                MTL_GRAPHICS_PIPELINE_DESC desc{};
                desc.VertexShader    = vs;
                desc.PixelShader     = ps;
                desc.HullShader      = nullptr;
                desc.DomainShader    = nullptr;
                desc.GeometryShader  = nullptr;

                desc.NumColorAttachments = rps.num_color_attachments;
                for (uint8_t s = 0; s < 8; s++)
                    desc.ColorAttachmentFormats[s] =
                        (s < rps.num_color_attachments)
                        ? rps.color_formats[s]
                        : WMTPixelFormatInvalid;
                desc.DepthStencilFormat  = rps.depth_stencil_format;
                desc.SampleCount         = rps.sample_count;
                desc.BlendState          = blend;
                desc.InputLayout         = il;
                desc.IndexBufferFormat   = ibf;
                desc.TopologyClass       = WMTPrimitiveTopologyClassTriangle;
                desc.RasterizationEnabled = true;
                desc.SampleMask          = 0xFFFFFFFF;

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

std::vector<Prediction> predictor_nn_rps_blend(
    ManagedShader vs,
    ManagedShader ps,
    const std::unordered_map<Sha1Digest, MinHashSig>& ps_minhash,
    const std::unordered_map<Sha1Digest,
        std::unordered_map<std::pair<RenderPassSignature, IMTLD3D11BlendState*>, uint32_t,
            PairHash<RenderPassSignature, IMTLD3D11BlendState*>>>& ps_rps_blend_table,
    const std::unordered_map<RenderPassSignature,
        std::unordered_map<IMTLD3D11BlendState*, uint32_t>>& rps_blend_table,
    std::unordered_map<Sha1Digest, std::unordered_set<Sha1Digest>>& input_req_to_layouts,
    std::unordered_map<Sha1Digest, InputLayout*>& layout_by_sha1,
    std::unordered_set<Prediction>& previous_predictions,
    const std::unordered_map<IMTLD3D11BlendState*, uint8_t>& blend_min_ps_outs,
    float sim_threshold = 0.5f)
{
    std::vector<Prediction> predictions;
    if (!ps) return predictions;

    // Step 1 — find nearest neighbour by MinHash similarity
    auto query_it = ps_minhash.find(ps->sha1());
    if (query_it == ps_minhash.end())
        return predictions; // no signature yet for this PS

    const auto& query_sig = query_it->second;

    float best_sim = 0.0f;
    Sha1Digest best_sha1{};

    for (const auto& [known_sha1, known_sig] : ps_minhash) {
        if (known_sha1 == ps->sha1()) continue;
        if (ps_rps_blend_table.count(known_sha1) == 0) continue;

        float sim = jaccard_estimate(query_sig, known_sig);
        if (sim > best_sim) {
            best_sim = sim;
            best_sha1 = known_sha1;
        }
    }

    // No useful neighbour found
    if (best_sim < sim_threshold)
        return predictions;

    // Step 2 — collect RPS candidates from neighbour's history
    std::unordered_set<RenderPassSignature> rps_candidates;
    const auto& neighbour_history = ps_rps_blend_table.at(best_sha1);
    for (const auto& [key, count] : neighbour_history)
        rps_candidates.insert(key.first);

    // Step 3 — for each candidate RPS, get all known blends from rps_blend_table
    // Filter by PS compatibility
    uint8_t nca     = ps->ps_outs.count;
    uint32_t ps_vrt = ps->reflection().PSValidRenderTargets;

    std::vector<std::pair<uint32_t, std::pair<RenderPassSignature, IMTLD3D11BlendState*>>> ranked;

    for (const auto& rps : rps_candidates) {
        if (!rps_compatible_with_ps(rps, ps))
            continue;

        // Look up all blends seen with this RPS globally
        auto rps_it = rps_blend_table.find(rps);
        if (rps_it == rps_blend_table.end()) {
            // RPS not in global table — use whatever the neighbour had
            for (const auto& [key, count] : neighbour_history) {
                if (!(key.first == rps)) continue;
                if (!blend_compatible_with_ps(key.second, nca, ps_vrt, blend_min_ps_outs))
                    continue;
                ranked.push_back({count, key});
            }
            continue;
        }

        for (const auto& [blend, count] : rps_it->second) {
            if (!blend_compatible_with_ps(blend, nca, ps_vrt, blend_min_ps_outs))
                continue;
            ranked.push_back({count, {rps, blend}});
        }
    }

    // Sort by frequency descending
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b){ return a.first > b.first; });

    // Step 4 — VS-compatible input layouts
    std::vector<InputLayout*> valid_ils;
    auto vs_sig = HashVSInputRequirement(vs->vs_input_req);
    auto il_it  = input_req_to_layouts.find(vs_sig);
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
            return predictions;
    }

    // Step 5 — emit predictions
    static constexpr SM50_INDEX_BUFFER_FORAMT kIBFs[] = {
        SM50_INDEX_BUFFER_FORMAT_NONE,
        SM50_INDEX_BUFFER_FORMAT_UINT16,
    };

    for (const auto& [score, pair] : ranked) {
        const auto& [rps, blend] = pair;

        for (auto* il : valid_ils) {
            for (auto ibf : kIBFs) {
                MTL_GRAPHICS_PIPELINE_DESC desc{};
                desc.VertexShader    = vs;
                desc.PixelShader     = ps;
                desc.HullShader      = nullptr;
                desc.DomainShader    = nullptr;
                desc.GeometryShader  = nullptr;

                desc.NumColorAttachments = rps.num_color_attachments;
                for (uint8_t s = 0; s < 8; s++)
                    desc.ColorAttachmentFormats[s] =
                        s < rps.num_color_attachments
                        ? rps.color_formats[s]
                        : WMTPixelFormatInvalid;
                desc.DepthStencilFormat  = rps.depth_stencil_format;
                desc.SampleCount         = rps.sample_count;
                desc.BlendState          = blend;
                desc.InputLayout         = il;
                desc.IndexBufferFormat   = ibf;
                desc.TopologyClass       = WMTPrimitiveTopologyClassTriangle;
                desc.SampleMask          = 0xFFFFFFFF;

                lock_shader_deterministic_fields(&desc);

                Prediction pred{desc};
                if (previous_predictions.count(pred) == 0) {
                    predictions.push_back(pred);
                    previous_predictions.insert(pred);
                }
            }
        }
    }

    return predictions;
}

std::vector<Prediction> predictor_nn_rps_blend_v2(
    ManagedShader vs,
    ManagedShader ps,
    const std::unordered_map<Sha1Digest, MinHashSig>& ps_minhash,
    const std::unordered_map<Sha1Digest,
        std::unordered_map<std::pair<RenderPassSignature, IMTLD3D11BlendState*>, uint32_t,
            PairHash<RenderPassSignature, IMTLD3D11BlendState*>>>& ps_rps_blend_table,
    const std::unordered_map<RenderPassSignature,
        std::unordered_map<IMTLD3D11BlendState*, uint32_t>>& rps_blend_table,
    std::unordered_map<Sha1Digest, std::unordered_set<Sha1Digest>>& input_req_to_layouts,
    std::unordered_map<Sha1Digest, InputLayout*>& layout_by_sha1,
    std::unordered_set<Prediction>& previous_predictions,
    const std::unordered_map<IMTLD3D11BlendState*, uint8_t>& blend_min_ps_outs,
    bool* found_similar,
    float sim_threshold = 0.5f,
    uint32_t extra_blends_per_rps = 2)
{
    std::vector<Prediction> predictions;
    if (!ps) return predictions;

    // Step 1 — find nearest neighbour
    auto query_it = ps_minhash.find(ps->sha1());
    if (query_it == ps_minhash.end())
        return predictions;

    const auto& query_sig = query_it->second;

    float best_sim = 0.0f;
    Sha1Digest best_sha1{};

    for (const auto& [known_sha1, known_sig] : ps_minhash) {
        if (known_sha1 == ps->sha1()) continue;
        if (ps_rps_blend_table.count(known_sha1) == 0) continue;
        float sim = jaccard_estimate(query_sig, known_sig);
        if (sim > best_sim) {
            best_sim = sim;
            best_sha1 = known_sha1;
        }
    }

    *found_similar = best_sim >= sim_threshold;
    if (!(*found_similar))
      return predictions;

    uint8_t  nca    = ps->ps_outs.count;
    uint32_t ps_vrt = ps->reflection().PSValidRenderTargets;

    // Step 2 — use neighbour's history directly as the base candidate set
    const auto& neighbour_history = ps_rps_blend_table.at(best_sha1);

    // Collect (RPS, blend) pairs from neighbour history, ranked by count
    std::vector<std::pair<uint32_t, std::pair<RenderPassSignature, IMTLD3D11BlendState*>>> ranked;

    for (const auto& [key, count] : neighbour_history) {
        const auto& [rps, blend] = key;
        if (!rps_compatible_with_ps(rps, ps))   continue;
        if (!blend_compatible_with_ps(blend, nca, ps_vrt, blend_min_ps_outs)) continue;
        ranked.push_back({count, key});
    }

    // Step 3 — for each RPS in neighbour history, supplement with top
    // extra_blends_per_rps additional blends from rps_blend_table,
    // ranked by global frequency, that aren't already in the list
    std::unordered_set<RenderPassSignature> seen_rps;
    for (const auto& [cnt, key] : ranked)
        seen_rps.insert(key.first);

    for (const auto& rps : seen_rps) {
        auto rps_it = rps_blend_table.find(rps);
        if (rps_it == rps_blend_table.end()) continue;

        // Rank blends for this RPS by global frequency
        std::vector<std::pair<uint32_t, IMTLD3D11BlendState*>> blends_ranked;
        for (const auto& [blend, count] : rps_it->second)
            blends_ranked.push_back({count, blend});
        std::sort(blends_ranked.begin(), blends_ranked.end(),
                  [](const auto& a, const auto& b){ return a.first > b.first; });

        // Add top extra_blends_per_rps that aren't already covered
        uint32_t added = 0;
        for (const auto& [count, blend] : blends_ranked) {
            if (added >= extra_blends_per_rps) break;
            if (!blend_compatible_with_ps(blend, nca, ps_vrt, blend_min_ps_outs)) continue;

            // Check if this (rps, blend) pair is already in ranked
            auto key = std::make_pair(rps, blend);
            bool already_present = false;
            for (const auto& [cnt, existing_key] : ranked) {
                if (existing_key == key) { already_present = true; break; }
            }
            if (already_present) continue;

            ranked.push_back({count, key});
            added++;
        }
    }

    // Sort final list by count descending
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b){ return a.first > b.first; });

    // Step 4 — VS-compatible input layouts
    std::vector<InputLayout*> valid_ils;
    auto vs_sig = HashVSInputRequirement(vs->vs_input_req);
    auto il_it  = input_req_to_layouts.find(vs_sig);
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
            return predictions;
    }

    // Step 5 — emit predictions
    static constexpr SM50_INDEX_BUFFER_FORAMT kIBFs[] = {
        SM50_INDEX_BUFFER_FORMAT_UINT16,
    };

    for (const auto& [score, pair] : ranked) {
        const auto& [rps, blend] = pair;

        for (auto* il : valid_ils) {
            for (auto ibf : kIBFs) {
                MTL_GRAPHICS_PIPELINE_DESC desc{};
                desc.VertexShader    = vs;
                desc.PixelShader     = ps;
                desc.HullShader      = nullptr;
                desc.DomainShader    = nullptr;
                desc.GeometryShader  = nullptr;

                desc.NumColorAttachments = rps.num_color_attachments;
                for (uint8_t s = 0; s < 8; s++)
                    desc.ColorAttachmentFormats[s] =
                        s < rps.num_color_attachments
                        ? rps.color_formats[s]
                        : WMTPixelFormatInvalid;
                desc.DepthStencilFormat  = rps.depth_stencil_format;
                desc.SampleCount         = rps.sample_count;
                desc.BlendState          = blend;
                desc.InputLayout         = il;
                desc.IndexBufferFormat   = ibf;
                desc.TopologyClass       = WMTPrimitiveTopologyClassTriangle;
                desc.SampleMask          = 0xFFFFFFFF;

                lock_shader_deterministic_fields(&desc);

                Prediction pred{desc};
                if (previous_predictions.count(pred) == 0) {
                    predictions.push_back(pred);
                    previous_predictions.insert(pred);
                }
            }
        }
    }

    return predictions;
}

std::vector<Prediction> predictor_composed(
    ManagedShader vs,
    ManagedShader ps,
    const std::unordered_map<Sha1Digest,
        std::unordered_map<std::pair<RenderPassSignature, IMTLD3D11BlendState*>, uint32_t,
            PairHash<RenderPassSignature, IMTLD3D11BlendState*>>>& ps_rps_blend_table,
    const std::unordered_map<RenderPassSignature,
        std::unordered_map<IMTLD3D11BlendState*, uint32_t>>& rps_blend_table,
    std::unordered_map<Sha1Digest, std::unordered_set<Sha1Digest>>& input_req_to_layouts,
    std::unordered_map<Sha1Digest, InputLayout*>& layout_by_sha1,
    std::unordered_set<Prediction>& previous_predictions,
    const std::unordered_map<IMTLD3D11BlendState*, uint8_t>& blend_min_ps_outs,
    uint32_t top_k = 3)
{
    if (!ps) return {};

    bool ps_known = ps_rps_blend_table.count(ps->sha1()) > 0;

    if (ps_known) {
        return predictor_ps_rps_blend_history(
            vs, ps,
            ps_rps_blend_table,
            input_req_to_layouts, layout_by_sha1,
            previous_predictions, top_k);
    } else {
        return predictor_global_rps_blend_compatible(
            vs, ps,
            rps_blend_table,
            input_req_to_layouts, layout_by_sha1,
            previous_predictions,
            blend_min_ps_outs,
            top_k);
    }
}

std::vector<Prediction> predictor_composed_nearest_neighbour(
    ManagedShader vs,
    ManagedShader ps,
    const std::unordered_map<Sha1Digest,
        std::unordered_map<std::pair<RenderPassSignature, IMTLD3D11BlendState*>, uint32_t,
            PairHash<RenderPassSignature, IMTLD3D11BlendState*>>>& ps_rps_blend_table,
    const std::unordered_map<RenderPassSignature,
        std::unordered_map<IMTLD3D11BlendState*, uint32_t>>& rps_blend_table,
    const std::unordered_map<Sha1Digest, MinHashSig>& ps_minhash,
    std::unordered_map<Sha1Digest, std::unordered_set<Sha1Digest>>& input_req_to_layouts,
    std::unordered_map<Sha1Digest, InputLayout*>& layout_by_sha1,
    std::unordered_set<Prediction>& previous_predictions,
    const std::unordered_map<IMTLD3D11BlendState*, uint8_t>& blend_min_ps_outs,
    uint32_t top_k = 3)
{
    if (!ps) return {};

    bool ps_known = ps_rps_blend_table.count(ps->sha1()) > 0;

    if (ps_known) {
        return predictor_ps_rps_blend_history(
            vs, ps,
            ps_rps_blend_table,
            input_req_to_layouts, layout_by_sha1,
            previous_predictions, top_k);
    } else {
        bool dummy;
        return predictor_nn_rps_blend_v2(
            vs, ps,
            ps_minhash,
            ps_rps_blend_table,
            rps_blend_table,
            input_req_to_layouts, layout_by_sha1,
            previous_predictions,
            blend_min_ps_outs,
            &dummy);
    }
}

std::vector<Prediction> predictor_composed_nn_and_global(
    ManagedShader vs,
    ManagedShader ps,
    const std::unordered_map<Sha1Digest,
        std::unordered_map<std::pair<RenderPassSignature, IMTLD3D11BlendState*>, uint32_t,
            PairHash<RenderPassSignature, IMTLD3D11BlendState*>>>& ps_rps_blend_table,
    const std::unordered_map<RenderPassSignature,
        std::unordered_map<IMTLD3D11BlendState*, uint32_t>>& rps_blend_table,
    const std::unordered_map<Sha1Digest, MinHashSig>& ps_minhash,
    std::unordered_map<Sha1Digest, std::unordered_set<Sha1Digest>>& input_req_to_layouts,
    std::unordered_map<Sha1Digest, InputLayout*>& layout_by_sha1,
    std::unordered_set<Prediction>& previous_predictions,
    const std::unordered_map<IMTLD3D11BlendState*, uint8_t>& blend_min_ps_outs,
    uint32_t top_k = 3)
{
  if (!ps) return {};

  bool ps_known = ps_rps_blend_table.count(ps->sha1()) > 0;

  if (ps_known) {
    return predictor_ps_rps_blend_history(
      vs, ps,
      ps_rps_blend_table,
      input_req_to_layouts, layout_by_sha1,
      previous_predictions, top_k);
  } else {
    bool found_similar;
    auto preds = predictor_nn_rps_blend_v2(
      vs, ps,
      ps_minhash,
      ps_rps_blend_table,
      rps_blend_table,
      input_req_to_layouts, layout_by_sha1,
      previous_predictions,
      blend_min_ps_outs,
      &found_similar);

    if (!found_similar) {
      preds = predictor_global_rps_blend_compatible(
        vs, ps,
        rps_blend_table,
        input_req_to_layouts, layout_by_sha1,
        previous_predictions,
        blend_min_ps_outs,
        top_k);
    }

    return preds;
        
  }
}

} // namespace dxmt