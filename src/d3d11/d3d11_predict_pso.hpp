#pragma once
#include "d3d11_shader.hpp"
#include "d3d11_pipeline.hpp"
#include "sha1/sha1_util.hpp"
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"


namespace dxmt {
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

}