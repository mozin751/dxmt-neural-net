#include "d3d11_pipeline_cache.hpp"
#include "airconv_public.h"
#include "d3d11_device.hpp"
#include "d3d11_shader.hpp"
#include "d3d11_pipeline.hpp"
#include "d3d11_predict_pso.hpp"
#include "dxmt_shader_cache.hpp"
#include "dxmt_tasks.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"
#include "util_env.hpp"
#include "../d3d10/d3d10_shader.hpp"
#include "../d3d10/d3d10_input_layout.hpp"
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include <cstring>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <optional>

namespace dxmt {

class PipelineCache;
static PipelineCache* g_pipeline_cache_instance = nullptr;

static std::string format_ps_color_outputs(const PSColorOutputs& outs) {
  std::string s = "PSColorOutputs { count=" + std::to_string((uint32_t)outs.count);
  s += " classes=[";
  for (uint8_t i = 0; i < 8; i++) {
      if (i) s += ",";
      switch (outs.classes[i]) {
      case ScalarClass::None:  s += "None";  break;
      case ScalarClass::Float: s += "Float"; break;
      case ScalarClass::UInt:  s += "UInt";  break;
      case ScalarClass::SInt:  s += "SInt";  break;
      }
  }
  s += "] masks=[";
  for (uint8_t i = 0; i < 8; i++) {
      if (i) s += ",";
      s += std::to_string((uint32_t)outs.masks[i]);
  }
  s += "] }";
  return s;
}

constexpr uint32_t kShaderPairMagic   = 0x53485052;
constexpr size_t kHashStringLen = 8;
constexpr uint32_t kCacheMagic   = 0x44584D43;
constexpr uint32_t kCacheVersion = 1;
constexpr size_t kDescKeyLen = 17;
constexpr const char* kDefaultPSName = "________";
constexpr bool kPredict = true;

class MTLD3D11InputLayout final
    : public MTLD3D11DeviceChild<IMTLD3D11InputLayout> {
public:
  MTLD3D11InputLayout(MTLD3D11Device *device, ManagedInputLayout input_layout)
      : MTLD3D11DeviceChild<IMTLD3D11InputLayout>(device),
        input_layout(input_layout), d3d10(this) {}

  ~MTLD3D11InputLayout() {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11InputLayout) ||
        riid == __uuidof(IMTLD3D11InputLayout)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10DeviceChild) ||
        riid == __uuidof(ID3D10InputLayout)) {
      *ppvObject = ref(&d3d10);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11InputLayout), riid)) {
      WARN("D3D311InputLayout: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  ManagedInputLayout GetManagedInputLayout() override { return input_layout; }

private:
  ManagedInputLayout input_layout;
  MTLD3D10InputLayout d3d10;
};

template <> struct task_trait<ThreadpoolWork *> {
  ThreadpoolWork *run_task(ThreadpoolWork *task) {
    return task->RunThreadpoolWork();
  }
  bool get_done(ThreadpoolWork *task) { return task->GetIsDone(); }
  void set_done(ThreadpoolWork *task) { task->SetIsDone(true); }
};

class PipelineCache : public MTLD3D11PipelineCacheBase {

  class CachedSM50Shader final : public Shader {
    PipelineCache *cache;
    sm50_shader_t shader = nullptr;
    Sha1Digest sha1_;
    MTL_SHADER_REFLECTION reflection_;
    MTL_SM50_SHADER_ARGUMENT *arguments_info_buffer;
    std::unordered_map<ShaderVariant, std::unique_ptr<CompiledShader>> variants;

  public:
    CachedSM50Shader(PipelineCache *cache, sm50_shader_t shader_transfered,
                     const Sha1Digest &hash, MTL_SHADER_REFLECTION &reflection)
        : cache(cache), shader(shader_transfered), sha1_(hash),
          reflection_(reflection) {
      if (reflection_.NumConstantBuffers + reflection_.NumArguments) {
        arguments_info_buffer = (MTL_SM50_SHADER_ARGUMENT *)malloc(
            sizeof(MTL_SM50_SHADER_ARGUMENT) *
            (reflection_.NumConstantBuffers + reflection_.NumArguments));
        SM50GetArgumentsInfo(shader, arguments_info_buffer,
                             arguments_info_buffer +
                                 reflection_.NumConstantBuffers);
      } else {
        arguments_info_buffer = nullptr;
      }
    }

    ~CachedSM50Shader() {
      if (shader) {
        SM50Destroy(shader);
        if (arguments_info_buffer)
          free(arguments_info_buffer);
        shader = nullptr;
      }
    };

    CachedSM50Shader(CachedSM50Shader &&moved) = delete;
    CachedSM50Shader(const CachedSM50Shader &copy) = delete;

    virtual sm50_shader_t handle() { return shader; };
    virtual MTL_SHADER_REFLECTION &reflection() { return reflection_; }
    virtual MTL_SM50_SHADER_ARGUMENT *constant_buffers_info() {
      return arguments_info_buffer;
    };
    virtual MTL_SM50_SHADER_ARGUMENT *arguments_info() {
      return arguments_info_buffer + reflection_.NumConstantBuffers;
    };
    virtual CompiledShader *get_shader(ShaderVariant variant) {
      auto c = variants.insert({variant, nullptr});
      if (c.second) {
        c.first->second = std::visit(
            [=, this](auto var) {
              return CreateVariantShader(cache->device, this, var);
            },
            variant);
        cache->scheduler_.submit(c.first->second.get());
      }
      return c.first->second.get();
    }
    virtual const Sha1Digest &sha1() { return sha1_; };

#ifdef DXMT_DEBUG
    void *bytecode;
    size_t bytecode_length;

    virtual void dump() {
      std::fstream dump_out;
      dump_out.open("shader_dump_" + sha1_.string() + ".cso",
                    std::ios::out | std::ios::binary);
      if (dump_out) {
        dump_out.write((char *)bytecode, bytecode_length);
      }
      dump_out.close();
      WARN("shader dumped to ./shader_dump_" + sha1_.string() + ".cso");
    }
#else
    virtual void dump() {}
#endif

    virtual WMT::Reference<WMT::DispatchData> find_cached_variant(Sha1Digest &variant_digest) final {
      auto reader = cache->scache_.getReader();
      if (reader)
        return reader->get(std::make_pair(sha1_, variant_digest));
      return {};
    };
    virtual void update_cached_variant(Sha1Digest &variant_digest, WMT::DispatchData data) final {
      auto writer = cache->scache_.getWriter();
      if (writer)
        writer->set(std::make_pair(sha1_, variant_digest), data);
    }
  };

  class CachedInputLayout final : public InputLayout {
  private:
  public:
    CachedInputLayout(
        std::vector<MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC> &&attributes,
        uint32_t input_slot_mask)
        : attributes_(attributes), input_slot_mask_(input_slot_mask) {
      Sha1HashState h;
      h.update(input_slot_mask);
      h.update(attributes_.size());
      for (auto &el : attributes_) {
        h.update(el);
      }
      sha1_ = h.final();
    }

    virtual uint32_t input_slot_mask() final { return input_slot_mask_; }

    virtual uint32_t input_layout_element(
        MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC **ppElements) final {
      *ppElements = attributes_.data();
      return attributes_.size();
    }

    virtual Sha1Digest &sha1() final { return sha1_; }

    std::vector<MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC> attributes_;
    Sha1Digest sha1_;
    uint32_t input_slot_mask_;
  };

  ShaderCache& scache_;

  std::unordered_map<size_t, WMT::Reference<WMT::BinaryArchive>> pso_cache_;
  std::mutex pso_cache_mutex_;
  size_t original_pso_cache_size_;

  std::unordered_map<std::string, std::unordered_set<MTL_GRAPHICS_PIPELINE_DESC>> descriptor_shader_map_;
  std::unordered_map<std::string, std::unordered_set<std::optional<std::string>>> vs_to_ps_map_;
  std::unordered_map<std::string, std::unordered_set<std::string>> ps_to_vs_map_;

  bool dirty_maps_;

  task_scheduler<ThreadpoolWork *> scheduler_;

  MTLD3D11Device *device;
  StateObjectCache<D3D11_BLEND_DESC1, IMTLD3D11BlendState> blend_states;

  std::unordered_map<MTL_INPUT_LAYOUT_DESC, std::unique_ptr<CachedInputLayout>> input_layouts;
  dxmt::mutex mutex_ia_;

  StateObjectCache<MTL_STREAM_OUTPUT_DESC, IMTLD3D11StreamOutputLayout>
      so_layouts;
  dxmt::mutex mutex_so_;

  std::unordered_map<Sha1Digest, std::unique_ptr<CachedSM50Shader>> shaders_;
  std::shared_mutex mutex_shares;

  std::unordered_set<std::string> encountered_pairs_;
  std::unordered_map<std::string, Sha1Digest> vertex_shaders_;
  std::unordered_map<std::string, Sha1Digest> pixel_shaders_;
  dxmt::mutex mutex_add_;

  std::unordered_map<MTL_GRAPHICS_PIPELINE_DESC, std::unique_ptr<MTLCompiledGraphicsPipeline>> pipelines_;
  dxmt::mutex mutex_;

  std::unordered_map<MTL_GRAPHICS_PIPELINE_DESC, std::unique_ptr<MTLCompiledGeometryPipeline>> pipelines_gs_;
  dxmt::mutex mutex_gs_;

  std::unordered_map<MTL_GRAPHICS_PIPELINE_DESC, std::unique_ptr<MTLCompiledTessellationMeshPipeline>> pipelines_ts_;
  dxmt::mutex mutex_ts_;

  std::unordered_map<ManagedShader, std::unique_ptr<MTLCompiledComputePipeline>> pipelines_cs_;
  dxmt::mutex mutex_cs_;

  std::unordered_map<Sha1Digest, std::unordered_set<Sha1Digest>> input_requirement_to_layouts_;
  std::unordered_map<Sha1Digest, std::unordered_set<std::string>> input_requirement_to_vs_;
  std::unordered_map<Sha1Digest, InputLayout*> layout_by_sha1_;

  std::unordered_set<Prediction> previous_predictions_;
  std::unordered_set<MTL_GRAPHICS_PIPELINE_DESC> missed_predictions_;
  std::unordered_map<MTL_GRAPHICS_PIPELINE_DESC, uint32_t> descriptor_freq_table_;
  std::unordered_map<RenderPassSignature, uint32_t> rps_freq_table_;
  std::unordered_map<IMTLD3D11BlendState*, uint32_t> blend_freq_table_;
  std::unordered_map<WMTPixelFormat, uint32_t>          dsf_freq_table_;
  std::array<std::unordered_map<WMTPixelFormat, uint32_t>, 8> caf_freq_table_;
  std::unordered_map<uint8_t, uint32_t>                 sc_freq_table_;
  std::unordered_map<SM50_INDEX_BUFFER_FORAMT, uint32_t> ibf_freq_table_;
  std::unordered_map<RenderPassSignature, 
  std::unordered_map<IMTLD3D11BlendState*, uint32_t>> rps_blend_table_;
  std::unordered_map<Sha1Digest,
  std::unordered_map<std::pair<RenderPassSignature, IMTLD3D11BlendState*>, uint32_t,
    PairHash<RenderPassSignature, IMTLD3D11BlendState*>>> ps_rps_blend_table_;
  std::unordered_map<IMTLD3D11BlendState*, uint8_t> blend_min_ps_outs_;
  std::unordered_map<Sha1Digest, MinHashSig> ps_minhash_;
  int misses_;

  CachedSM50Shader *CreateShader(const void *pBytecode,
                                 uint32_t BytecodeLength) {
    auto sha1 = Sha1HashState::compute(pBytecode, BytecodeLength);
    {
      std::shared_lock<std::shared_mutex> lock(mutex_shares);
      auto result = shaders_.find(sha1);
      if (result != shaders_.end()) {
        return shaders_.at(sha1).get();
      }
    }
    sm50_error_t err;
    sm50_shader_t sm50;
    MTL_SHADER_REFLECTION reflection;
    if (SM50Initialize(pBytecode, BytecodeLength, &sm50, &reflection, &err)) {
      ERR("Failed to initialize shader: ", SM50GetErrorMessageString(err));
      SM50FreeError(err);
      return nullptr;
    }
    auto shader = std::make_unique<CachedSM50Shader>(this, sm50, sha1, reflection);
    {
      std::unique_lock<std::shared_mutex> lock(mutex_shares);
      auto result = shaders_.find(sha1);
      if (result != shaders_.end()) {
        return shaders_.at(sha1).get();
      }
#ifdef DXMT_DEBUG
      shader->bytecode = malloc(BytecodeLength);
      shader->bytecode_length = BytecodeLength;
      memcpy(shader->bytecode, pBytecode, BytecodeLength);
#endif
      return shaders_.emplace(sha1, std::move(shader)).first->second.get();
    }
  }

  virtual HRESULT AddVertexShader(const void *pBytecode,
                                  uint32_t BytecodeLength,
                                  ID3D11VertexShader **ppShader) override {
    std::lock_guard<dxmt::mutex> lock(mutex_add_);
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_FAIL;
    }

    managed_shader->vs_input_req = ExtractVSInputRequirement(pBytecode, BytecodeLength);
    
    const auto vs_name = managed_shader->sha1().string().substr(0, 8);
    input_requirement_to_vs_[HashVSInputRequirement(managed_shader->vs_input_req)].insert(vs_name);
    // for (const auto& ps_name: vs_to_ps_map_[vs_name]) {
    //   if (ps_name.has_value() && pixel_shaders_.count(ps_name.value()) == 0) continue;
    //   ManagedShader ps = ps_name.has_value() ? shaders_[pixel_shaders_[ps_name.value()]].get() : nullptr;
    //   PrecompileGraphicsPipelines(vs_name, ps_name, managed_shader, ps);
    // }

    vertex_shaders_.emplace(vs_name, managed_shader->sha1());

    // Predictions:
    if (kPredict) {
      for (const auto& ps_name: vs_to_ps_map_[vs_name]) {
        if (ps_name.has_value() && pixel_shaders_.count(ps_name.value()) == 0) continue;
        ManagedShader ps = ps_name.has_value() ? shaders_[pixel_shaders_[ps_name.value()]].get() : nullptr;
        auto predictions =  predictor_composed_nearest_neighbour(
          managed_shader, ps,
          ps_rps_blend_table_,
          rps_blend_table_,
          ps_minhash_,
          input_requirement_to_layouts_,
          layout_by_sha1_,
          previous_predictions_,
          blend_min_ps_outs_,
          /*top_k=*/5);
        // Logger::info(str::format("Is default blend desc nullptr? ", blend_states.cache[kDefaultBlendDesc].get() == nullptr));

        for (auto prediction: predictions) {
          MTLCompiledGraphicsPipeline *dummyPipeline;
          GetGraphicsPipeline(&prediction.pDesc, &dummyPipeline);
        }
      }
    }

    *ppShader =
        ref(new TShaderBase<ID3D11VertexShader, MTLD3D10VertexShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddPixelShader(const void *pBytecode, uint32_t BytecodeLength,
                                 ID3D11PixelShader **ppShader) override {
    std::lock_guard<dxmt::mutex> lock(mutex_add_);
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_FAIL;
    }
    auto ops = extract_opcode_sequence(pBytecode, BytecodeLength);
    ps_minhash_[managed_shader->sha1()] = compute_minhash(ops);

    managed_shader->ps_outs = ExtractPSColorOutputs(pBytecode, BytecodeLength);
    
    const auto ps_name = managed_shader->sha1().string().substr(0, 8);
    // for (const auto& vs_name: ps_to_vs_map_[ps_name]) {
    //   if (vertex_shaders_.count(vs_name) == 0) continue;
    //   ManagedShader vs = shaders_[vertex_shaders_[vs_name]].get();
    //   PrecompileGraphicsPipelines(vs_name, std::optional<std::string>(ps_name), vs, managed_shader);
    // }

    pixel_shaders_.emplace(ps_name, managed_shader->sha1());

    // Predictions
    if (kPredict) {
      for (const auto& vs_name: ps_to_vs_map_[ps_name]) {
        if (vertex_shaders_.count(vs_name) == 0) continue;
        ManagedShader vs = shaders_[vertex_shaders_[vs_name]].get();
        auto predictions =  predictor_composed_nearest_neighbour(
          vs, managed_shader,
          ps_rps_blend_table_,
          rps_blend_table_,
          ps_minhash_,
          input_requirement_to_layouts_,
          layout_by_sha1_,
          previous_predictions_,
          blend_min_ps_outs_,
          /*top_k=*/5);
        // Logger::info(str::format("Is default blend desc nullptr? ", blend_states.cache[kDefaultBlendDesc].get() == nullptr));
        
        for (auto prediction: predictions) {
          MTLCompiledGraphicsPipeline *dummyPipeline;
          GetGraphicsPipeline(&prediction.pDesc, &dummyPipeline);
        }
      }
    }

    *ppShader = ref(new TShaderBase<ID3D11PixelShader, MTLD3D10PixelShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddHullShader(const void *pBytecode, uint32_t BytecodeLength,
                                ID3D11HullShader **ppShader) override {
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_FAIL;
    }
    *ppShader = ref(new TShaderBase<ID3D11HullShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddDomainShader(const void *pBytecode,
                                  uint32_t BytecodeLength,
                                  ID3D11DomainShader **ppShader) override {
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_FAIL;
    }
    *ppShader =
        ref(new TShaderBase<ID3D11DomainShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddGeometryShader(const void *pBytecode,
                                    uint32_t BytecodeLength,
                                    ID3D11GeometryShader **ppShader) override {
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_FAIL;
    }
    *ppShader =
        ref(new TShaderBase<ID3D11GeometryShader, MTLD3D10GeometryShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddComputeShader(const void *pBytecode,
                                   uint32_t BytecodeLength,
                                   ID3D11ComputeShader **ppShader) override {
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_FAIL;
    }
    *ppShader =
        ref(new TShaderBase<ID3D11ComputeShader>(device, managed_shader));
    return S_OK;
  }

  HRESULT AddInputLayout(const void *pShaderBytecodeWithInputSignature,
                         const D3D11_INPUT_ELEMENT_DESC *pInputElementDesc,
                         UINT NumElements,
                         IMTLD3D11InputLayout **ppInputLayout) override {
    std::lock_guard<dxmt::mutex> lock(mutex_ia_);
    std::vector<MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC> buffer(NumElements);
    uint32_t num_metal_ia_elements;
    HRESULT hr;
    if (FAILED(hr = ExtractMTLInputLayoutElements(
            device, pShaderBytecodeWithInputSignature, pInputElementDesc,
            NumElements, buffer.data(), &num_metal_ia_elements))) {
      return hr;
    }
    buffer.resize(num_metal_ia_elements);
    if (!input_layouts.contains(buffer)) {
      uint32_t input_slot_mask = 0;
      for (auto &element : buffer) {
        input_slot_mask |= (1 << element.Slot);
      }
      input_layouts.emplace(buffer, std::make_unique<CachedInputLayout>(
                                        std::move(buffer), input_slot_mask));
    }
    auto* cached = input_layouts.at(buffer).get();
    *ppInputLayout =
        ref(new MTLD3D11InputLayout(device, input_layouts.at(buffer).get()));

    auto req = ExtractVSInputRequirementNoSize(pShaderBytecodeWithInputSignature);
    auto sig = HashVSInputRequirement(req);
    auto il_sha1 = cached->sha1();
    input_requirement_to_layouts_[sig].insert(il_sha1);
    layout_by_sha1_.emplace(il_sha1, cached);

    // Predictions
    if (kPredict) {
      for (const auto vs_name: input_requirement_to_vs_[sig]) {
        for (const auto& ps_name: vs_to_ps_map_[vs_name]) {
          if (ps_name.has_value() && pixel_shaders_.count(ps_name.value()) == 0) continue;
          ManagedShader ps = ps_name.has_value() ? shaders_[pixel_shaders_[ps_name.value()]].get() : nullptr;
          auto predictions =  predictor_composed_nearest_neighbour(
            shaders_[vertex_shaders_[vs_name]].get(), ps,
            ps_rps_blend_table_,
            rps_blend_table_,
            ps_minhash_,
            input_requirement_to_layouts_,
            layout_by_sha1_,
            previous_predictions_,
            blend_min_ps_outs_,
            /*top_k=*/5);

          for (auto prediction: predictions) {
            MTLCompiledGraphicsPipeline *dummyPipeline;
            GetGraphicsPipeline(&prediction.pDesc, &dummyPipeline);
          }
        }
      }
    }
    return hr;
  }

  HRESULT
  AddStreamOutputLayout(const void *pShaderBytecode, UINT NumEntries,
                        const D3D11_SO_DECLARATION_ENTRY *pEntries,
                        UINT NumStrides, const UINT *pStrides,
                        UINT RasterizedStream,
                        IMTLD3D11StreamOutputLayout **ppSOLayout) override {
    std::lock_guard<dxmt::mutex> lock(mutex_so_);
    std::vector<MTL_SHADER_STREAM_OUTPUT_ELEMENT_DESC> buffer(NumEntries * 4);
    std::array<uint32_t, 4> strides = {{}};
    uint32_t num_metal_so_elements;
    if (FAILED(ExtractMTLStreamOutputElements(
            device, pShaderBytecode, NumEntries, pEntries, buffer.data(),
            &num_metal_so_elements))) {
      return E_FAIL;
    }
    buffer.resize(num_metal_so_elements);
    for (unsigned i = 0; i < NumStrides; i++) {
      strides[i] = pStrides[i];
    }
    MTL_STREAM_OUTPUT_DESC desc;
    memcpy(desc.Strides, strides.data(), sizeof(strides));
    desc.Elements = std::move(buffer);
    desc.RasterizedStream = RasterizedStream;
    return so_layouts.CreateStateObject(&desc, ppSOLayout);
  };

  HRESULT AddBlendState(const D3D11_BLEND_DESC1 *pBlendDesc,
                        IMTLD3D11BlendState **ppBlendState) override {
    return blend_states.CreateStateObject(pBlendDesc, ppBlendState);
  }

  void PrecompileGraphicsPipelines(const std::string& vs_name, const std::optional<std::string>& ps_name, ManagedShader vs, ManagedShader ps) {
    auto name = str::format(vs_name, "/", ps_name.value_or(kDefaultPSName));
    if (encountered_pairs_.count(name) > 0) return;
    encountered_pairs_.emplace(name);

    for (auto pDesc: descriptor_shader_map_[name]) {
      pDesc.VertexShader = vs;
      pDesc.PixelShader = ps;
      MTLCompiledGraphicsPipeline *dummyPipeline;
      GetGraphicsPipeline(&pDesc, &dummyPipeline, nullptr, true);
    }
  }

  void GetGraphicsPipeline(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                           MTLCompiledGraphicsPipeline **ppPipeline,
                           bool* pWasCached = nullptr, bool fromCache = false) override {
    std::lock_guard<dxmt::mutex> lock(mutex_);

    if (auto iter = pipelines_.find(*pDesc); iter != pipelines_.end()) {
      if (pWasCached) *pWasCached = true;
      *ppPipeline = iter->second.get();

      auto it = previous_predictions_.find(Prediction{*pDesc});
      if (it != previous_predictions_.end() && missed_predictions_.count(*pDesc) == 0) {
        auto pred = const_cast<Prediction&>(*it);

        if (!pred.hit) {
          const_cast<Prediction&>(*it).hit = true;
          Logger::info(str::format("Prediction hit on: ", format_desc(pred.pDesc)));
          auto desc = pred.pDesc;
          if (desc.PixelShader) {
            auto key = std::make_pair(RenderPassSignature::from_desc(desc), desc.BlendState);
            ps_rps_blend_table_[desc.PixelShader->sha1()][key]++;
          }

          auto rps = RenderPassSignature::from_desc(desc);
          rps_freq_table_[rps]++;

          // Blend frequency — pointer is stable (deduped by StateObjectCache)
          if (desc.BlendState)
              blend_freq_table_[desc.BlendState]++;

          if (desc.BlendState)
              rps_blend_table_[rps][desc.BlendState]++;

          // Existing full-descriptor frequency table
          MTL_GRAPHICS_PIPELINE_DESC anon = desc;
          anon.VertexShader = anon.PixelShader = anon.HullShader
                            = anon.DomainShader = anon.GeometryShader = nullptr;
          descriptor_freq_table_[anon]++;
          dsf_freq_table_[desc.DepthStencilFormat]++;
          sc_freq_table_[desc.SampleCount]++;
          ibf_freq_table_[desc.IndexBufferFormat]++;
          for (uint8_t i = 0; i < desc.NumColorAttachments; i++)
              caf_freq_table_[i][desc.ColorAttachmentFormats[i]]++; 
        }
      }
      return;
    }
    if (pWasCached) *pWasCached = fromCache;

    // TEST CODE

    // if (pDesc->PixelShader) {
    //    auto ps_sha1 = pDesc->PixelShader->sha1();
    
    //   if (ps_rps_blend_table_.find(ps_sha1) == ps_rps_blend_table_.end()) {
    //     auto rps = RenderPassSignature::from_desc(*pDesc);
    //     IMTLD3D11BlendState* blend = pDesc->BlendState;

    //     bool rps_seen        = rps_freq_table_.count(rps) > 0;
    //     bool blend_seen      = blend_freq_table_.count(blend) > 0;
    //     bool pair_seen       = rps_blend_table_.count(rps) > 0 &&
    //                            rps_blend_table_.at(rps).count(blend) > 0;

    //     Logger::info(str::format(
    //         "New PS=", ps_sha1.string().substr(0, 8),
    //         " rps_seen=", (uint32_t)rps_seen,
    //         " blend_seen=", (uint32_t)blend_seen,
    //         " pair_seen=", (uint32_t)pair_seen,
    //         " scenario=",
    //             (!rps_seen && !blend_seen)              ? "C" :
    //             (rps_seen  && !blend_seen)              ? "B_rps" :
    //             (!rps_seen && blend_seen)               ? "B_blend" :
    //             (rps_seen  && blend_seen && pair_seen)  ? "A_pair" :
    //                                                       "A_unpaired"
    //     ));
    // }
    // }

    // if (pDesc->PixelShader)
      // Logger::info(str::format("Is blend compatible with this ps? ", blend_compatible_with_ps(pDesc->BlendState, pDesc->PixelShader->ps_outs.count, pDesc->PixelShader->reflection().PSValidRenderTargets)));
    // Logger::info(str::format("Is rps compatible with this ps? ", rps_compatible_with_ps(
    // RenderPassSignature::from_desc(*pDesc),
    // pDesc->PixelShader)));
    // Logger::info(str::format("PSValidRenderTargets: ", 
    //     pDesc->PixelShader->reflection().PSValidRenderTargets,
    //     " ps_outs.count: ", pDesc->PixelShader->ps_outs.count,
    //     " NCA: ", pDesc->NumColorAttachments
    // ));
    // for (const auto& [bs, count] : blend_freq_table_) {
    // if (!blend_compatible_with_ps(bs, nca, ps->reflection().PSValidRenderTargets))
    //     continue;
    // // ... emit prediction with this blend state
    // }

    // END OF TEST CODE


    // Updating frequency tables

    if (previous_predictions_.count(Prediction{*pDesc}) == 0) {
      if (pDesc->PixelShader) {
        if (pDesc->PixelShader) {
          auto ps_sha1 = pDesc->PixelShader->sha1();
          auto rps = RenderPassSignature::from_desc(*pDesc);
          IMTLD3D11BlendState* blend = pDesc->BlendState;

          bool ps_known    = ps_rps_blend_table_.count(ps_sha1) > 0;
          bool rps_seen    = rps_freq_table_.count(rps) > 0;
          bool blend_seen  = blend_freq_table_.count(blend) > 0;
          bool pair_seen   = ps_known &&
                            ps_rps_blend_table_.at(ps_sha1).count(
                                std::make_pair(rps, blend)) > 0;

          std::string scenario;
          if (pair_seen) {
              scenario = "A_pair";
          } else if (ps_known && rps_seen && blend_seen) {
              scenario = "A_unpaired";
          } else if (!ps_known && rps_seen && blend_seen) {
              scenario = "B_both";
          } else if (!ps_known && rps_seen && !blend_seen) {
              scenario = "B_rps";
          } else if (!ps_known && !rps_seen && blend_seen) {
              scenario = "B_blend";
          } else {
              scenario = "C";
          }

          Logger::info(str::format(
              "MISS scenario=", scenario,
              " ps_known=", (uint32_t)ps_known,
              " rps_seen=", (uint32_t)rps_seen,
              " blend_seen=", (uint32_t)blend_seen,
              " pair_seen=", (uint32_t)pair_seen,
              " PS=", ps_sha1.string().substr(0, 8)
          ));

          if (scenario == "B_both" || scenario == "B_rps" || scenario == "B_blend" || scenario == "C") {
              auto ps_sha1 = pDesc->PixelShader->sha1();
              
              auto query_it = ps_minhash_.find(ps_sha1);
              if (query_it != ps_minhash_.end()) {
                  const auto& query_sig = query_it->second;
                  
                  float best_sim = 0.0f;
                  Sha1Digest best_sha1{};
                  bool best_has_history = false;
                  
                  for (const auto& [known_sha1, known_sig] : ps_minhash_) {
                      if (known_sha1 == ps_sha1) continue;
                      
                      float sim = jaccard_estimate(query_sig, known_sig);
                      bool has_history = ps_rps_blend_table_.count(known_sha1) > 0;
                      
                      if (sim > best_sim && has_history) {
                          best_sim = sim;
                          best_sha1 = known_sha1;
                          best_has_history = true;
                      }
                  }
                  
                  Logger::info(str::format(
                      "NN search PS=", ps_sha1.string().substr(0, 8),
                      " scenario=", scenario,
                      " best_neighbour=", best_sha1.string().substr(0, 8),
                      " similarity=", (uint32_t)(best_sim * 100),
                      "% has_history=", (uint32_t)best_has_history
                  ));

                  if (best_has_history && best_sim > 0.0f) {
                      auto& neighbour_history = ps_rps_blend_table_.at(best_sha1);
                      
                      auto actual_rps = RenderPassSignature::from_desc(*pDesc);
                      IMTLD3D11BlendState* actual_blend = pDesc->BlendState;
                      
                      // Check if actual pair is in neighbour history
                      auto key = std::make_pair(actual_rps, actual_blend);
                      bool neighbour_has_pair = neighbour_history.count(key) > 0;

                      // Check: if we emitted ALL pairs from neighbour that share the actual RPS,
                      // would we have hit?
                      bool same_rps_covers_actual = false;
                      uint32_t same_rps_pair_count = 0;
                      for (const auto& [k, count] : neighbour_history) {
                          if (k.first == actual_rps) {
                              same_rps_pair_count++;
                              if (k.second == actual_blend)
                                  same_rps_covers_actual = true;
                          }
                      }

                      // Check: if we emitted ALL pairs from neighbour history entirely,
                      // would we have hit?
                      bool full_history_covers_actual = neighbour_has_pair;

                      // Log top-3 pairs from neighbour history
                      std::vector<std::pair<uint32_t, std::pair<RenderPassSignature, IMTLD3D11BlendState*>>> ranked;
                      for (const auto& [k, count] : neighbour_history)
                          ranked.push_back({count, k});
                      std::sort(ranked.begin(), ranked.end(),
                                [](const auto& a, const auto& b){ return a.first > b.first; });

                      std::string hist_str;
                      for (uint32_t i = 0; i < std::min((uint32_t)ranked.size(), 3u); i++) {
                          const auto& [rps, bs] = ranked[i].second;
                          hist_str += str::format(
                              " [", rps.to_string(),
                              " BLEND=", reinterpret_cast<uintptr_t>(bs),
                              " cnt=", ranked[i].first, "]"
                          );
                      }

                      Logger::info(str::format(
                          "NN detail: PS=", ps_sha1.string().substr(0, 8),
                          " neighbour=", best_sha1.string().substr(0, 8),
                          " sim=", (uint32_t)(best_sim * 100), "%",
                          " has_pair=", (uint32_t)neighbour_has_pair,
                          " same_rps_count=", same_rps_pair_count,
                          " same_rps_covers=", (uint32_t)same_rps_covers_actual,
                          " full_history_covers=", (uint32_t)full_history_covers_actual,
                          " actual_rps=", actual_rps.to_string(),
                          " actual_blend=", reinterpret_cast<uintptr_t>(actual_blend),
                          " neighbour_top3:", hist_str
                      ));
                  }
              }
          }
        }
        auto key = std::make_pair(RenderPassSignature::from_desc(*pDesc), pDesc->BlendState);
        ps_rps_blend_table_[pDesc->PixelShader->sha1()][key]++;
      }

      if (previous_predictions_.count(Prediction{*pDesc}) == 0) {
        Logger::info(str::format("REAL DRAW (no prediction match): ", format_desc(*pDesc)));
        ++misses_;
        missed_predictions_.insert(*pDesc);
      }

      auto rps = RenderPassSignature::from_desc(*pDesc);
      rps_freq_table_[rps]++;

      // Blend frequency — pointer is stable (deduped by StateObjectCache)
      if (pDesc->BlendState) {
        blend_freq_table_[pDesc->BlendState]++;
        rps_blend_table_[rps][pDesc->BlendState]++;
        if (pDesc->PixelShader) {
          uint8_t count = pDesc->PixelShader->ps_outs.count;
          auto it = blend_min_ps_outs_.find(pDesc->BlendState);
          if (it == blend_min_ps_outs_.end())
              blend_min_ps_outs_[pDesc->BlendState] = count;
          else
              it->second = std::min(it->second, count);
        }
      }

      // Existing full-descriptor frequency table
      MTL_GRAPHICS_PIPELINE_DESC anon = *pDesc;
      anon.VertexShader = anon.PixelShader = anon.HullShader
                        = anon.DomainShader = anon.GeometryShader = nullptr;
      descriptor_freq_table_[anon]++;
      dsf_freq_table_[pDesc->DepthStencilFormat]++;
      sc_freq_table_[pDesc->SampleCount]++;
      ibf_freq_table_[pDesc->IndexBufferFormat]++;
      for (uint8_t i = 0; i < pDesc->NumColorAttachments; i++)
          caf_freq_table_[i][pDesc->ColorAttachmentFormats[i]]++; 
    }

    if (!fromCache) {
      auto vs_name = pDesc->VertexShader->sha1().string().substr(0, 8);
      std::string ps_name = kDefaultPSName;
      if (pDesc->PixelShader) {
        ps_name = pDesc->PixelShader->sha1().string().substr(0, 8);
        if (ps_to_vs_map_[ps_name].count(vs_name) == 0) Logger::info(str::format("Adding pair: ", vs_name, "/", ps_name));
        ps_to_vs_map_[ps_name].insert(vs_name);
      }
      if (vs_to_ps_map_[vs_name].count(ps_name) == 0) Logger::info(str::format("Adding pair: ", vs_name, "/", ps_name));
      vs_to_ps_map_[vs_name].insert(pDesc->PixelShader ? std::optional<std::string>(ps_name) : std::nullopt);
      descriptor_shader_map_[str::format(vs_name, "/", ps_name)].insert(*pDesc);
      dirty_maps_ = true;
    }

    auto [iter, inserted] = pipelines_.insert({*pDesc, CreateGraphicsPipeline(device, pDesc, pso_cache_, pso_cache_mutex_)});
    if (!inserted) {
      D3D11_ASSERT(0 && "duplicated graphics pipeline");
    } else {
      scheduler_.submit(iter->second.get());
    }
    *ppPipeline = iter->second.get();
  }

  void GetGeometryPipeline(
      MTL_GRAPHICS_PIPELINE_DESC *pDesc,
      MTLCompiledGeometryPipeline **ppPipeline) override {
    std::lock_guard<dxmt::mutex> lock(mutex_gs_);

    if (auto iter = pipelines_gs_.find(*pDesc); iter != pipelines_gs_.end()) {
      *ppPipeline = iter->second.get();
      return;
    }
    auto [iter, inserted] = pipelines_gs_.insert({*pDesc, CreateGeometryPipeline(device, pDesc)});
    if (!inserted) {
      D3D11_ASSERT(0 && "duplicated geometry pipeline");
    } else {
      scheduler_.submit(iter->second.get());
    }
    *ppPipeline = iter->second.get();
  }

  void GetTessellationPipeline(MTL_GRAPHICS_PIPELINE_DESC * pDesc,
                                   MTLCompiledTessellationMeshPipeline *
                                       *ppPipeline) override {
    std::lock_guard<dxmt::mutex> lock(mutex_ts_);

    if (auto iter = pipelines_ts_.find(*pDesc); iter != pipelines_ts_.end()) {
      *ppPipeline = iter->second.get();
      return;
    }
    auto [iter, inserted] = pipelines_ts_.insert({*pDesc, CreateTessellationMeshPipeline(device, pDesc)});
    if (!inserted) {
      D3D11_ASSERT(0 && "duplicated tessellation pipeline");
    } else {
      scheduler_.submit(iter->second.get());
    }
    *ppPipeline = iter->second.get();
  }

  void GetComputePipeline(MTL_COMPUTE_PIPELINE_DESC *pDesc,
                                  MTLCompiledComputePipeline **ppPipeline) override {
   std::lock_guard<dxmt::mutex> lock(mutex_cs_);

    if (auto iter = pipelines_cs_.find(pDesc->ComputeShader); iter != pipelines_cs_.end()) {
      *ppPipeline = iter->second.get();
      return;
    }
    auto [iter, inserted] = pipelines_cs_.insert({pDesc->ComputeShader, CreateComputePipeline(device, pDesc->ComputeShader)});
    if (!inserted) {
      D3D11_ASSERT(0 && "duplicated compute pipeline");
    } else {
      scheduler_.submit(iter->second.get());
    }
    *ppPipeline = iter->second.get();
  }

  void save_cache(std::unordered_map<size_t, WMT::Reference<WMT::BinaryArchive>>& cache, const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;

    uint32_t count = cache.size();
    f.write(reinterpret_cast<const char*>(&count), sizeof(count));

    WMT::Reference<WMT::Error> err;
    for (auto& [hash, archive] : cache) {
      f.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
      archive.serialize((WMT::GetCacheDir() + "/metal_bin_archives/" + std::to_string(hash) + ".bin").c_str(), err);
    }
  }

  template <typename T>
  static void write_pod(std::ostream& os, const T& v) {
      static_assert(std::is_trivially_copyable_v<T>);
      os.write(reinterpret_cast<const char*>(&v), sizeof(T));
  }

  template <typename T>
  static bool read_pod(std::istream& is, T& v) {
      static_assert(std::is_trivially_copyable_v<T>);
      is.read(reinterpret_cast<char*>(&v), sizeof(T));
      return is.good();
  }

  static void serialise_desc(std::ostream& os,
                           const MTL_GRAPHICS_PIPELINE_DESC& desc) {
    write_pod(os, desc.NumColorAttachments);
    write_pod(os, desc.ColorAttachmentFormats);
    write_pod(os, desc.DepthStencilFormat);
    write_pod(os, desc.TopologyClass);
    write_pod(os, desc.RasterizationEnabled);
    write_pod(os, desc.SampleCount);
    write_pod(os, desc.GSStripTopology);
    write_pod(os, desc.IndexBufferFormat);
    write_pod(os, desc.SampleMask);
    write_pod(os, desc.GSPassthrough);

    const bool has_blend = (desc.BlendState != nullptr);
    write_pod(os, has_blend);
    if (has_blend) {
      D3D11_BLEND_DESC1 bd;
      desc.BlendState->GetDesc1(&bd);
      write_pod(os, bd);
    }

    const bool has_il = (desc.InputLayout != nullptr);
    write_pod(os, has_il);
    if (has_il) {
      MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC* elements = nullptr;
      const uint32_t count = desc.InputLayout->input_layout_element(&elements);
      const uint32_t mask  = desc.InputLayout->input_slot_mask();

      write_pod(os, mask);
      write_pod(os, count);
      os.write(reinterpret_cast<const char*>(elements),
              sizeof(MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC) * count);

      Sha1Digest vs_sig{};
      if (desc.VertexShader)
        vs_sig = HashVSInputRequirement(desc.VertexShader->vs_input_req);
      write_pod(os, vs_sig);
    }

    const bool has_so = (desc.SOLayout != nullptr);
    write_pod(os, has_so);
    if (has_so) {
        MTL_SHADER_STREAM_OUTPUT_ELEMENT_DESC* elements = nullptr;
        uint32_t strides[4] = {};
        const uint32_t count = desc.SOLayout->GetStreamOutputElements(&elements, strides);
        const uint32_t rasterized_stream = desc.SOLayout->RasterizedStream();

        write_pod(os, strides);
        write_pod(os, rasterized_stream);
        write_pod(os, count);
        os.write(reinterpret_cast<const char*>(elements),
                sizeof(MTL_SHADER_STREAM_OUTPUT_ELEMENT_DESC) * count);
    }
  }

  void serialise_map(std::ostream& os,
                 const std::unordered_map<std::string, std::unordered_set<MTL_GRAPHICS_PIPELINE_DESC>>& map) {
    write_pod(os, kCacheMagic);
    write_pod(os, kCacheVersion);
    write_pod(os, static_cast<uint32_t>(map.size()));
    for (const auto& [key, descs] : map) {
      if (key.size() != kDescKeyLen) continue;
      os.write(key.data(), kDescKeyLen);
      write_pod(os, static_cast<uint32_t>(descs.size()));
      for (const auto& desc : descs) {
        serialise_desc(os, desc);
      }
    }
  }

  void writeCacheToDisk(const std::string& path) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        ERR("Failed to write to pipeline disk cache");
        return;
    }

    std::unordered_map<std::string, std::unordered_set<MTL_GRAPHICS_PIPELINE_DESC>> snapshot;
    {
      // TODO: Lock
      snapshot = descriptor_shader_map_;
    }

    serialise_map(ofs, snapshot);
  }

  template <typename T>
  bool writeShaderPairMap(
      const std::unordered_map<std::string, std::unordered_set<T>>& map,
      const std::string& path)
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
      return false;

    auto write = [&](const void* data, size_t size) {
      f.write(reinterpret_cast<const char*>(data), size);
    };

    auto writeStr = [&](const std::string& s) {
      write(s.data(), kHashStringLen);
    };

    write(&kShaderPairMagic, sizeof(kShaderPairMagic));

    uint32_t mapSize = static_cast<uint32_t>(map.size());
    write(&mapSize, sizeof(mapSize));

    for (const auto& [vs, psSet] : map) {
      writeStr(vs);

      uint32_t setSize = static_cast<uint32_t>(psSet.size());
      write(&setSize, sizeof(setSize));

      for (const auto& ps : psSet) {
        if constexpr (std::is_same_v<T, std::optional<std::string>>) {
          uint8_t hasValue = ps.has_value() ? 1 : 0;
          write(&hasValue, sizeof(hasValue));

          if (ps.has_value())
              writeStr(*ps);
        } else {
          uint8_t hasValue = 1;
          write(&hasValue, sizeof(hasValue));
          writeStr(ps);
        }
      }
    }

    return f.good();
  }

  bool deserialise_desc(std::istream& is, MTL_GRAPHICS_PIPELINE_DESC& desc) {
    if (!read_pod(is, desc.NumColorAttachments))   return false;
    if (!read_pod(is, desc.ColorAttachmentFormats)) return false;
    if (!read_pod(is, desc.DepthStencilFormat))    return false;
    if (!read_pod(is, desc.TopologyClass))         return false;
    if (!read_pod(is, desc.RasterizationEnabled))  return false;
    if (!read_pod(is, desc.SampleCount))           return false;
    if (!read_pod(is, desc.GSStripTopology))       return false;
    if (!read_pod(is, desc.IndexBufferFormat))     return false;
    if (!read_pod(is, desc.SampleMask))            return false;
    if (!read_pod(is, desc.GSPassthrough))         return false;

    // --- BlendState ---
    bool has_blend = false;
    if (!read_pod(is, has_blend)) return false;
    if (has_blend) {
      D3D11_BLEND_DESC1 bd;
      if (!read_pod(is, bd)) return false;

      IMTLD3D11BlendState* bs = nullptr;
      if (FAILED(blend_states.CreateStateObject(&bd, &bs))) return false;
      desc.BlendState = bs;
      bs->Release();
    }

    // --- InputLayout ---
    bool has_il = false;
    if (!read_pod(is, has_il)) return false;
    if (has_il) {
      uint32_t mask = 0;
      uint32_t count = 0;
      if (!read_pod(is, mask))  return false;
      if (!read_pod(is, count)) return false;

      constexpr uint32_t kMaxElements = 64;
      if (count > kMaxElements) return false;

      std::vector<MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC> elements(count);
      if (count > 0) {
        is.read(reinterpret_cast<char*>(elements.data()),
                sizeof(MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC) * count);
        if (!is.good()) return false;
      }

      std::lock_guard<dxmt::mutex> lock(mutex_ia_);

      auto it = input_layouts.find(elements);
      if (it == input_layouts.end()) {
        auto key = elements;  // separate copy for the key
        it = input_layouts.emplace(
            std::move(key),
            std::make_unique<CachedInputLayout>(std::move(elements), mask)
        ).first;
      }
      desc.InputLayout = it->second.get();

      Sha1Digest vs_sig{};
      if (!read_pod(is, vs_sig)) return false;

      auto* cached = static_cast<CachedInputLayout*>(it->second.get());
      if (!(vs_sig == Sha1Digest{})) {
        input_requirement_to_layouts_[vs_sig].insert(cached->sha1());
        layout_by_sha1_.emplace(cached->sha1(), cached);
      }
    }

    // --- SOLayout ---
    bool has_so = false;
    if (!read_pod(is, has_so)) return false;
    if (has_so) {
      uint32_t strides[4] = {};
      uint32_t rasterized_stream = 0;
      uint32_t count = 0;
      if (!read_pod(is, strides))           return false;
      if (!read_pod(is, rasterized_stream)) return false;
      if (!read_pod(is, count))             return false;

      constexpr uint32_t kMaxElements = 128;
      if (count > kMaxElements) return false;

      std::vector<MTL_SHADER_STREAM_OUTPUT_ELEMENT_DESC> elements(count);
      if (count > 0) {
        is.read(reinterpret_cast<char*>(elements.data()),
                sizeof(MTL_SHADER_STREAM_OUTPUT_ELEMENT_DESC) * count);
        if (!is.good()) return false;
      }

      MTL_STREAM_OUTPUT_DESC so_desc;
      std::memcpy(so_desc.Strides, strides, sizeof(strides));
      so_desc.Elements = std::move(elements);
      so_desc.RasterizedStream = rasterized_stream;

      std::lock_guard<dxmt::mutex> lock(mutex_so_);

      IMTLD3D11StreamOutputLayout* so = nullptr;
      if (FAILED(so_layouts.CreateStateObject(&so_desc, &so))) return false;
      desc.SOLayout = so;
      so->Release();   // cache owns the lifetime; same pattern as blend state
    }

    return true;
  }

  void loadCacheFromDisk(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
      return; // no cache yet, first run — not an error
    }

    uint32_t magic = 0, version = 0, count = 0;
    if (!read_pod(ifs, magic)   || magic   != kCacheMagic)   return;
    if (!read_pod(ifs, version) || version != kCacheVersion) return;
    if (!read_pod(ifs, count))                                return;

    std::unordered_map<std::string, std::unordered_set<MTL_GRAPHICS_PIPELINE_DESC>> loaded;
    loaded.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
      std::string key(kDescKeyLen, '\0');
      ifs.read(key.data(), kDescKeyLen);
      if (!ifs.good()) return;

      uint32_t desc_count = 0;
      if (!read_pod(ifs, desc_count)) return;

      constexpr uint32_t kMaxDescsPerKey = 4096;
      if (desc_count > kMaxDescsPerKey) return;

      auto& set = loaded[std::move(key)];
      set.reserve(desc_count);
      for (uint32_t j = 0; j < desc_count; ++j) {
        MTL_GRAPHICS_PIPELINE_DESC desc{};
        if (!deserialise_desc(ifs, desc)) return;
        set.insert(std::move(desc));
      }
    }

    // TODO: Lock
    descriptor_shader_map_ = std::move(loaded);
  }

  template <typename T>
  bool readShaderPairMap(
      std::unordered_map<std::string, std::unordered_set<T>>& map,
      const std::string& path)
  {
    std::ifstream f(path, std::ios::binary);
    if (!f)
      return false;

    auto read = [&](void* data, size_t size) -> bool {
      f.read(reinterpret_cast<char*>(data), size);
      return f.good();
    };

    auto readStr = [&](std::string& s) -> bool {
      s.resize(kHashStringLen);
      return read(s.data(), kHashStringLen);
    };

    uint32_t magic;
    if (!read(&magic, sizeof(magic)) ||
      magic != kShaderPairMagic)
      return false;

    uint32_t mapSize;
    if (!read(&mapSize, sizeof(mapSize)))
      return false;

    map.clear();
    map.reserve(mapSize);

    for (uint32_t i = 0; i < mapSize; i++) {
      std::string vs;
      if (!readStr(vs))
        return false;

      uint32_t setSize;
      if (!read(&setSize, sizeof(setSize)))
        return false;

      auto& psSet = map[vs];
      psSet.reserve(setSize);

      for (uint32_t j = 0; j < setSize; j++) {
        uint8_t hasValue;
        if (!read(&hasValue, sizeof(hasValue)))
          return false;

        if constexpr (std::is_same_v<T, std::optional<std::string>>) {
          if (hasValue) {
              std::string ps;
              if (!readStr(ps))
                  return false;

              psSet.insert(std::move(ps));
          } else {
              psSet.insert(std::nullopt);
          }
        } else {
          if (!hasValue)
            return false;

          std::string ps;
          if (!readStr(ps))
            return false;

          psSet.insert(std::move(ps));
        }
      }
    }

    return true;
  }

  void load_cache(std::unordered_map<size_t, WMT::Reference<WMT::BinaryArchive>>& cache, const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;

    uint32_t count = 0;
    f.read(reinterpret_cast<char*>(&count), sizeof(count));
    cache.reserve(count * 2 < 100 ? 100 : count * 2);

    for (uint32_t i = 0; i < count; i++) {
        size_t hash = 0;
        f.read(reinterpret_cast<char*>(&hash), sizeof(hash));

        if (!f.good()) {
            break;
        }

        std::string bin_path = WMT::GetCacheDir() + "/metal_bin_archives/" + std::to_string(hash) + ".bin";
        WMT::Reference<WMT::Error> err;
        WMT::Reference<WMT::BinaryArchive> archive = device->GetMTLDevice().newBinaryArchive(bin_path.c_str(), err);

        if (archive) {
          cache[hash] = archive;
        }
    }
  }

  void savePredictorState(const std::string& path) {
      std::ofstream f(path, std::ios::binary | std::ios::trunc);
      if (!f) { ERR("Failed to open predictor state for write: ", path); return; }

      constexpr uint32_t kMagic   = 0x50524544; // "PRED"
      constexpr uint32_t kVersion = 2;
      write_pod(f, kMagic);
      write_pod(f, kVersion);

      // --- ps_rps_blend_table_ ---
      write_pod(f, (uint32_t)ps_rps_blend_table_.size());
      for (const auto& [ps_sha1, pair_map] : ps_rps_blend_table_) {
          write_pod(f, ps_sha1);
          write_pod(f, (uint32_t)pair_map.size());
          for (const auto& [key, count] : pair_map) {
              const auto& [rps, bs] = key;
              // RPS
              write_pod(f, rps.num_color_attachments);
              write_pod(f, rps.color_formats);
              write_pod(f, rps.depth_stencil_format);
              write_pod(f, rps.sample_count);
              // Blend state — serialise by content, not pointer
              bool has_blend = (bs != nullptr);
              write_pod(f, has_blend);
              if (has_blend) {
                  D3D11_BLEND_DESC1 bd;
                  bs->GetDesc1(&bd);
                  write_pod(f, bd);
              }
              write_pod(f, count);
          }
      }

      // --- rps_blend_table_ ---
      write_pod(f, (uint32_t)rps_blend_table_.size());
      for (const auto& [rps, blend_map] : rps_blend_table_) {
          write_pod(f, rps.num_color_attachments);
          write_pod(f, rps.color_formats);
          write_pod(f, rps.depth_stencil_format);
          write_pod(f, rps.sample_count);
          write_pod(f, (uint32_t)blend_map.size());
          for (const auto& [bs, count] : blend_map) {
              bool has_blend = (bs != nullptr);
              write_pod(f, has_blend);
              if (has_blend) {
                  D3D11_BLEND_DESC1 bd;
                  bs->GetDesc1(&bd);
                  write_pod(f, bd);
              }
              write_pod(f, count);
          }
      }

      // --- blend_min_ps_outs_ ---
      write_pod(f, (uint32_t)blend_min_ps_outs_.size());
      for (const auto& [bs, min_count] : blend_min_ps_outs_) {
          bool has_blend = (bs != nullptr);
          write_pod(f, has_blend);
          if (has_blend) {
              D3D11_BLEND_DESC1 bd;
              bs->GetDesc1(&bd);
              write_pod(f, bd);
          }
          write_pod(f, min_count);
      }
  }

  void loadPredictorState(const std::string& path) {
      std::ifstream f(path, std::ios::binary);
      if (!f) return; // first run

      constexpr uint32_t kMagic   = 0x50524544;
      constexpr uint32_t kVersion = 2;

      uint32_t magic = 0, version = 0;
      if (!read_pod(f, magic)   || magic   != kMagic)   return;
      if (!read_pod(f, version) || version != kVersion) return;

      // Helper: deserialise a blend state pointer from stored D3D11_BLEND_DESC1
      auto read_blend = [&](IMTLD3D11BlendState*& bs) -> bool {
          bool has_blend = false;
          if (!read_pod(f, has_blend)) return false;
          bs = nullptr;
          if (has_blend) {
              D3D11_BLEND_DESC1 bd;
              if (!read_pod(f, bd)) return false;
              if (FAILED(blend_states.CreateStateObject(&bd, &bs))) return false;
              bs->Release(); // cache owns lifetime
          }
          return true;
      };

      auto read_rps = [&](RenderPassSignature& rps) -> bool {
          if (!read_pod(f, rps.num_color_attachments)) return false;
          if (!read_pod(f, rps.color_formats))         return false;
          if (!read_pod(f, rps.depth_stencil_format))  return false;
          if (!read_pod(f, rps.sample_count))          return false;
          return true;
      };

      // --- ps_rps_blend_table_ ---
      uint32_t ps_count = 0;
      if (!read_pod(f, ps_count)) return;
      for (uint32_t i = 0; i < ps_count; i++) {
          Sha1Digest ps_sha1;
          if (!read_pod(f, ps_sha1)) return;
          uint32_t pair_count = 0;
          if (!read_pod(f, pair_count)) return;
          auto& pair_map = ps_rps_blend_table_[ps_sha1];
          for (uint32_t j = 0; j < pair_count; j++) {
              RenderPassSignature rps{};
              if (!read_rps(rps)) return;
              IMTLD3D11BlendState* bs = nullptr;
              if (!read_blend(bs)) return;
              uint32_t count = 0;
              if (!read_pod(f, count)) return;
              auto key = std::make_pair(rps, bs);
              pair_map[key] += count;
          }
      }

      // --- rps_blend_table_ ---
      uint32_t rps_count = 0;
      if (!read_pod(f, rps_count)) return;
      for (uint32_t i = 0; i < rps_count; i++) {
          RenderPassSignature rps{};
          if (!read_rps(rps)) return;
          uint32_t blend_count = 0;
          if (!read_pod(f, blend_count)) return;
          auto& blend_map = rps_blend_table_[rps];
          for (uint32_t j = 0; j < blend_count; j++) {
              IMTLD3D11BlendState* bs = nullptr;
              if (!read_blend(bs)) return;
              uint32_t count = 0;
              if (!read_pod(f, count)) return;
              blend_map[bs] += count;
          }
      }

      // --- blend_min_ps_outs_ ---
      uint32_t bmp_count = 0;
      if (!read_pod(f, bmp_count)) return;
      for (uint32_t i = 0; i < bmp_count; i++) {
          IMTLD3D11BlendState* bs = nullptr;
          if (!read_blend(bs)) return;
          uint8_t min_count = 0;
          if (!read_pod(f, min_count)) return;
          if (bs) {
              auto it = blend_min_ps_outs_.find(bs);
              if (it == blend_min_ps_outs_.end())
                  blend_min_ps_outs_[bs] = min_count;
              else
                  it->second = std::min(it->second, min_count);
          }
      }
  }

  public:
  // void OnRenderPassTransition(const RenderPassSignature& new_rps,
  //                             IMTLD3D11BlendState* blend) override {
  //   Logger::info("RPS transition (OMSetRenderTargets): " + new_rps.to_string()
  //               + " BLEND=" + std::to_string(reinterpret_cast<uintptr_t>(blend)));
  // }

  PipelineCache(MTLD3D11Device *pDevice) :
      scache_(ShaderCache::getInstance(pDevice->GetDXMTDevice().metalVersion())),
      dirty_maps_(false),
      device(pDevice),
      blend_states(pDevice),
      misses_(0),
      so_layouts(pDevice) {
    load_cache(pso_cache_, WMT::GetCacheDir() + "cache_map.bin");
    readShaderPairMap(vs_to_ps_map_, WMT::GetCacheDir() + "vs_to_ps_map.bin");
    readShaderPairMap(ps_to_vs_map_, WMT::GetCacheDir() + "ps_to_vs_map.bin");
    loadCacheFromDisk(WMT::GetCacheDir() + "shader_descriptor_map.bin");
    loadPredictorState(WMT::GetCacheDir() + "predictor_state.bin");
    g_pipeline_cache_instance = this;
  };

  void Flush() {
    // if (original_pso_cache_size_ != pso_cache_.size()) {
    //   save_cache(pso_cache_, WMT::GetCacheDir() + "cache_map.bin");
    // }

    if (dirty_maps_) {
      writeShaderPairMap(vs_to_ps_map_, WMT::GetCacheDir() + "vs_to_ps_map.bin");
      writeShaderPairMap(ps_to_vs_map_, WMT::GetCacheDir() + "ps_to_vs_map.bin");
    //   writeCacheToDisk(WMT::GetCacheDir() + "shader_descriptor_map.bin");
    }
    // savePredictorState(WMT::GetCacheDir() + "predictor_state.bin");

    Logger::info(str::format("Num predictions: ", previous_predictions_.size()));
    int hits = 0;
    for (const auto prediction: previous_predictions_) {
      if (prediction.hit) {
        Logger::info(str::format("Prediction hit on: ", format_desc(prediction.pDesc)));
        ++hits;
      }
    }

    Logger::info(str::format("Hits: ", hits));
    Logger::info(str::format("Mispredictions: ", (previous_predictions_.size() - hits)));
    Logger::info(str::format("Misses: ", misses_));
    Logger::info(str::format("Num pipelines: ", pipelines_.size()));

  //   Logger::info(str::format("Num rps: ", rps_blend_table_.size()));
  //   for (auto [rps, item]: rps_blend_table_) {
  //     Logger::info(str::format("RPS: ", rps.to_string(), "count: ", rps_freq_table_[rps]));

  //     for (auto [blend, count]: item) {
  //       Logger::info(str::format("\t ", blend, ". count: ", count));
  //     }
  //   }

  //   Logger::info("=== Per-PS (RPS, Blend) frequencies ===");
  //   for (const auto& [ps_sha1, rps_blend_map] : ps_rps_blend_table_) {
  //       Logger::info(str::format("PS=", ps_sha1.string().substr(0, 8),
  //                               " distinct (RPS,blend) pairs: ", rps_blend_map.size()));
  //       for (const auto& [key, count] : rps_blend_map) {
  //           const auto& [rps, blend] = key;
  //           Logger::info(str::format("  ", rps.to_string(),
  //                                   " BLEND=", reinterpret_cast<uintptr_t>(blend),
  //                                   " count=", count));
  //       }
  //   }

  //   Logger::info("=== Observed (RPS, blend) pairs ===");
  //   for (const auto& [rps, blend_map] : rps_blend_table_) {
  //       for (const auto& [blend, count] : blend_map) {
  //           Logger::info(str::format(
  //               "  ", rps.to_string(),
  //               " BLEND=", reinterpret_cast<uintptr_t>(blend),
  //               " count=", count
  //           ));
  //       }
  //   }
  //   Logger::info(str::format("Total distinct (RPS, blend) pairs: ",
  //       [&]() {
  //           size_t total = 0;
  //           for (const auto& [rps, bm] : rps_blend_table_) total += bm.size();
  //           return total;
  //       }()
  //   ));
  }

  ~PipelineCache() {
    Flush();
    g_pipeline_cache_instance = nullptr;
  }

  __attribute__((destructor))
  static void OnDylibUnload() {
      if (g_pipeline_cache_instance) {
          g_pipeline_cache_instance->Flush();
          g_pipeline_cache_instance = nullptr;
      }
  }
};

std::unique_ptr<MTLD3D11PipelineCacheBase>
InitializePipelineCache(MTLD3D11Device *device) {
  return std::make_unique<PipelineCache>(device);
}

}; // namespace dxmt