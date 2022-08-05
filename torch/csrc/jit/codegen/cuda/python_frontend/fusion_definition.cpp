#ifdef USE_CUDA
#include <torch/csrc/jit/codegen/cuda/instrumentation.h>
#include <torch/csrc/jit/codegen/cuda/python_frontend/fusion_definition.h>
#include <torch/csrc/jit/codegen/cuda/python_frontend/fusion_manager.h>
#include <torch/csrc/jit/codegen/cuda/utils.h>

// Require namespace for perf scope instrumentation
using namespace torch::jit::fuser::cuda::inst;

namespace nvfuser {

FusionDefinition::FusionDefinition(FusionManager* fusion_manager)
    : fusion_manager_(fusion_manager),
      end_record_(new EndRecord()),
      recording_(),
      recording_state_(),
      fusion_state_(),
      ops(this) {}

void FusionDefinition::buildFusionIr() {
  FUSER_PERF_SCOPE("FusionDefinition::buildFusionIr");
  Nvf::FusionGuard::setCurFusion(fusion_manager_->fusionPtr());
  fusion_state_.resize(recording_state_.size(), nullptr);
  for (auto& record : recording_) {
    auto functor = record.get();
    (*functor)(*this);
  }
  Nvf::FusionGuard::setCurFusion(nullptr);
}

FusionDefinition* FusionDefinition::enter() {
  Trace::instance()->beginEvent("FusionDefinition Context Manager");
  FUSER_PERF_SCOPE("FusionDefinition::enter");
  fusion_manager_->resetFusionCachePtr();
  return this;
}
void FusionDefinition::exit() {
  FUSER_PERF_SCOPE("FusionDefinition::exit");
  auto cache_entry = fusion_manager_->lookupFusionCacheEntry(end_record_.get());
  if (!cache_entry.has_value()) {
    fusion_manager_->createTerminalFusionCacheEntry(end_record_);
    fusion_manager_->traverseFusionCache(end_record_);

    buildFusionIr();
  } else {
    fusion_manager_->traverseFusionCache(end_record_);
  }
  Trace::instance()->endEvent(nullptr);
}

Scalar* FusionDefinition::defineScalar() {
  FUSER_PERF_SCOPE("FusionDefinition::defineScalar");
  Scalar* out = new nvfuser::Scalar(recording_state_.size());
  recording_state_.emplace_back(out);
  return out;
}
Tensor* FusionDefinition::defineTensor() {
  FUSER_PERF_SCOPE("FusionDefinition::defineTensor");
  Tensor* out = new nvfuser::Tensor(recording_state_.size());
  recording_state_.emplace_back(out);
  return out;
}
void FusionDefinition::defineRecord(RecordFunctor* record) {
  FUSER_PERF_SCOPE("FusionDefinition::defineRecord");
  auto cache_entry = fusion_manager_->lookupFusionCacheEntry(record);
  if (cache_entry.has_value()) {
    if (Nvf::isDebugDumpEnabled(Nvf::DebugDumpOption::PythonFrontend)) {
      std::cout << "\nFusionDefinition: Record hit in Fusion Cache.\n";
    }
    recording_.emplace_back(cache_entry.value()->record);
  } else {
    if (Nvf::isDebugDumpEnabled(Nvf::DebugDumpOption::PythonFrontend)) {
      std::cout << "\nFusionDefinition: Record mised in Fusion Cache.\n";
    }
    recording_.emplace_back(record);
    fusion_manager_->createFusionCacheEntry(recording_.back());
  }
  fusion_manager_->traverseFusionCache(recording_.back());
}

void FusionDefinition::addInput(Nvf::Val* input) {
  fusion_manager_->fusionPtr()->addInput(input);
}
void FusionDefinition::addOutput(Nvf::Val* output) {
  fusion_manager_->fusionPtr()->addOutput(output);
}

Nvf::Val* FusionDefinition::getFusionState(size_t index) const {
  return fusion_state_.at(index);
}
void FusionDefinition::setFusionState(size_t index, Nvf::Val* val) {
  fusion_state_.at(index) = val;
}

} // namespace nvfuser

#endif // USE_CUDA
