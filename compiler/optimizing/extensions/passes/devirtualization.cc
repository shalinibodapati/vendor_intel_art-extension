/*
 * Copyright (C) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "devirtualization.h"
#include "driver/compiler_driver-inl.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "ext_utility.h"
#include "utils/dex_cache_arrays_layout-inl.h"
#include "scoped_thread_state_change.h"

namespace art {

bool HDevirtualization::Gate() {
  if (graph_->GetArtMethod() == nullptr) {
    // We need to have art_method_ to work.
    return false;
  }
  return HSpeculationPass::Gate();
}

HDevirtualization::~HDevirtualization() {
  if (kIsDebugBuild) {
    for (auto it : imprecise_predictions_) {
      // We must have at least one prediction if in list of predictions.
      DCHECK_GT(it.second.size(), 0u);
      // It must be the case that our imprecise predictions are not considered precise also.
      DCHECK(precise_prediction_.find(it.first) == precise_prediction_.end());
    }
  }
}

bool HDevirtualization::IsCandidate(HInstruction* instr) {
  if (instr->IsInvokeVirtual() || instr->IsInvokeInterface()) {
    // If the invoke is already marked as intrinsic, we do not need to sharpen it.
    return !instr->AsInvoke()->IsIntrinsic();
  }

  return false;
}

bool HDevirtualization::HasPrediction(HInstruction* instr, bool update) {
  DCHECK(IsCandidate(instr));
  // First check if we have already checked this prediction before.
  if (precise_prediction_.find(instr) != precise_prediction_.end()) {
    return true;
  } else if (imprecise_predictions_.find(instr) != imprecise_predictions_.end()) {
    return true;
  }

  // Try resolving the target of this call.
  HInvoke* invoke = instr->AsInvoke();
  uint32_t method_index = invoke->GetDexMethodIndex();
  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* class_linker = compilation_unit_.GetClassLinker();
  ArtMethod* resolved_method =
      compilation_unit_.GetDexCache().Get()->GetResolvedMethod(method_index,
                                                               class_linker->GetImagePointerSize());

  if (resolved_method != nullptr) {
    // Now that we have a resolved method, try to find a potential target if the type
    // is provably precise.
    ArtMethod* actual_method = FindVirtualOrInterfaceTarget(invoke, resolved_method);

    if (actual_method != nullptr) {
      if (update) {
        TypeHandle type;
        if (IsMethodOrDeclaringClassFinal(actual_method)) {
          type = handles_->NewHandle(actual_method->GetDeclaringClass());
        } else {
          // Type must have been recovered from RTI.
          HInstruction* receiver = invoke->InputAt(0);
          if (receiver->IsNullCheck()) {
            // RTP might have not propagated through null check - let us go one more level.
            receiver = receiver->InputAt(0);
          }
          type = receiver->GetReferenceTypeInfo().GetTypeHandle();
        }
        DCHECK(type.GetReference() != nullptr);

        precise_prediction_.Put(invoke, type);
        PRINT_PASS_OSTREAM_MESSAGE(this, "Found precise type " << PrettyClass(type.Get()) <<
                                   " for " << invoke);
      }
      return true;
    } else {
      // We do not have a precise type based on analysis - well what about from profile?
      std::vector<TypeHandle> possible_targets = FindTypesFromProfile(invoke, graph_->GetArtMethod());

      if (possible_targets.size() != 0u) {
        // Seems that we have potential targets - record this if needed.
        if (update) {
          imprecise_predictions_.Put(invoke, possible_targets);
          if (this->IsVerbose()) {
            std::string potential_types = "";
            for (auto it : possible_targets) {
              potential_types.append(PrettyClass(it.Get()));
              potential_types.append(",");
            }
            PRINT_PASS_OSTREAM_MESSAGE(this, "Found imprecise types " <<
                                       potential_types << " for " << invoke);
          }
        }
        return true;
      }
    }
  }

  // Could not figure out a prediction for this.
  return false;
}

uint64_t HDevirtualization::GetMaxCost() {
  return kCostOfLoadClass + kCostOfDevirtCheck;
}

uint64_t HDevirtualization::GetCost(HInstruction* instr) {
  DCHECK(HasPrediction(instr, false));
  if (precise_prediction_.find(instr) != precise_prediction_.end()) {
    return 0u;
  } else {
    DCHECK_NE(imprecise_predictions_.count(instr), 0u);
    std::vector<TypeHandle>& types = imprecise_predictions_.find(instr)->second;
    ScopedObjectAccess soa(Thread::Current());
    mirror::Class* referrer_class = graph_->GetArtMethod()->GetDeclaringClass();
    uint64_t cost = 0u;
    for (auto it : types) {
      if (it.Get() == referrer_class) {
        cost += (kCostOfLoadReferrerClass + kCostOfDevirtCheck);
      } else {
        cost += GetMaxCost();
      }
    }
    return cost;
  }
}

std::pair<uint64_t, uint64_t> HDevirtualization::GetMispredictRate(HInstruction* instr) {
  DCHECK(HasPrediction(instr, false));

  if (precise_prediction_.find(instr) != precise_prediction_.end()) {
    return std::make_pair (0, 10);
  } else {
    DCHECK(imprecise_predictions_.find(instr) != imprecise_predictions_.end());
    size_t count = imprecise_predictions_.count(instr);
    // TODO Use the actual profile information to determine the likelihood of mispredict.
    if (count == 1) {
      // Since target is imprecise, we do not want to return that prediction is always true.
      return std::make_pair(1, 10);
    }
    return std::make_pair(count - 1, count);
  }
}

uint64_t HDevirtualization::GetProfit(HInstruction* instr) {
  DCHECK(HasPrediction(instr, false));
  // Since direct invokes have a bigger path length than virtual invokes,
  // we do not get profit simply from the replacement. We get the profit from
  // potentially inlining.

  // Since we potentially save on the copying, include the number of arguments
  // plus one extra for the return.
  const uint32_t num_arguments = instr->AsInvoke()->GetNumberOfArguments() + 1;

  return kCostOfVirtualInvokes + num_arguments;
}

std::vector<HDevirtualization::TypeHandle> HDevirtualization::FindTypesFromProfile(
    HInvoke* invoke ATTRIBUTE_UNUSED, ArtMethod* caller_method ATTRIBUTE_UNUSED) {
  // TODO Implement either ability to look up type profiles or ability to look
  // up type via Class Hierarchy Analysis.
  // TODO When getting a new handle - do not forget to update "handles_".
  return std::vector<TypeHandle>();
}

HDevirtualization::TypeHandle HDevirtualization::GetPrimaryType(HInvoke* invoke) const {
  if (precise_prediction_.find(invoke) != precise_prediction_.end()) {
    return precise_prediction_.Get(invoke);
  } else {
    auto it = imprecise_predictions_.find(invoke);
    DCHECK(it != imprecise_predictions_.end());
    DCHECK_NE(it->second.size(), 0u);
    return it->second[0];
  }
}

bool HDevirtualization::IsPredictionSame(HInstruction* instr, HInstruction* instr2) {
  DCHECK(HasPrediction(instr, false));
  DCHECK(HasPrediction(instr2, false));
  HInvoke* invoke1 = instr->AsInvoke();
  HInvoke* invoke2 = instr2->AsInvoke();

  // Same instance means it should use the same prediction.
  bool same_instance = (invoke1->InputAt(0) == invoke2->InputAt(0));
  if (same_instance) {
    return true;
  }

  // They are not same instance - but are they at least the same type?
  TypeHandle type1 = GetPrimaryType(invoke1);
  TypeHandle type2 = GetPrimaryType(invoke2);
  ScopedObjectAccess soa(Thread::Current());
  return (type1.Get() == type2.Get());
}

HSpeculationGuard* HDevirtualization::InsertSpeculationGuard(HInstruction* instr_guarded,
                                                             HInstruction* instr_cursor) {
  HInvoke* invoke = instr_guarded->AsInvoke();
  // The object is always the first argument of instance invoke.
  HInstruction* object = invoke->InputAt(0);

  // Check that the type is accessible from current dex cache.
  ScopedObjectAccess soa(Thread::Current());
  TypeHandle type = GetPrimaryType(invoke);
  const DexFile& caller_dex_file = *compilation_unit_.GetDexFile();
  uint32_t class_index = FindClassIndexIn(type.Get(), caller_dex_file, compilation_unit_.GetDexCache());
  if (class_index == DexFile::kDexNoIndex) {
    // Seems we cannot find current type in the dex cache.
    PRINT_PASS_OSTREAM_MESSAGE(this, "Guard insertion failed because we cannot find " <<
                               PrettyClass(type.Get()) << " in the dex cache for " <<
                               invoke);
    return nullptr;
  }

  // Load the class from the object.
  constexpr Primitive::Type loaded_type = Primitive::kPrimNot;
  ArtField* field =
      compilation_unit_.GetClassLinker()->GetClassRoot(ClassLinker::kJavaLangObject)->GetInstanceField(0);
  DCHECK_EQ(std::string(field->GetName()), "shadow$_klass_");
  HInstanceFieldGet* class_getter = new (graph_->GetArena()) HInstanceFieldGet(
      object,
      loaded_type,
      field->GetOffset(),
      field->IsVolatile(),
      field->GetDexFieldIndex(),
      field->GetDeclaringClass()->GetDexClassDefIndex(),
      *field->GetDexFile(),
      handles_->NewHandle(field->GetDexCache()),
      instr_guarded->GetDexPc());
  // The class field is essentially a final field.
  class_getter->SetSideEffects(SideEffects::None());

  // Now create a load class for the prediction.
  bool is_referrer = (type.Get() == graph_->GetArtMethod()->GetDeclaringClass());
  HLoadClass* prediction = new (graph_->GetArena()) HLoadClass(graph_->GetCurrentMethod(),
                                                               class_index,
                                                               *compilation_unit_.GetDexFile(),
                                                               is_referrer,
                                                               instr_guarded->GetDexPc(),
                                                               /* needs_access_check */ false,
                                                               /* is_in_dex_cache */ true);

  HDevirtGuard* guard = new (graph_->GetArena()) HDevirtGuard(prediction, class_getter, invoke->GetDexPc());

  // Handle the insertion.
  HBasicBlock* insertion_bb = instr_cursor->GetBlock();
  DCHECK(insertion_bb != nullptr);
  insertion_bb->InsertInstructionBefore(prediction, instr_cursor);
  insertion_bb->InsertInstructionAfter(class_getter, prediction);
  insertion_bb->InsertInstructionAfter(guard, class_getter);

  return guard;
}

bool HDevirtualization::HandleSpeculation(HInstruction* instr, bool guard_inserted) {
  HInvoke* invoke = instr->AsInvoke();
  uint32_t method_index = 0u;

  // Find the target method - we know that the class is in dex cache
  // and therefore the method must be as well.
  {
    ScopedObjectAccess soa(Thread::Current());
    TypeHandle type = GetPrimaryType(invoke);
    ClassLinker* cl = Runtime::Current()->GetClassLinker();
    size_t pointer_size = cl->GetImagePointerSize();
    const DexFile& caller_dex_file = *compilation_unit_.GetDexFile();

    if (!guard_inserted || kIsDebugBuild) {
      // We repeat the same check as in "InsertSpeculationGuard" which checks that
      // the current class can be found in current dex file. The reason we do this
      // is that precise types do not require guard.
      uint32_t class_index = FindClassIndexIn(type.Get(), caller_dex_file,
                                              compilation_unit_.GetDexCache());
      if (kIsDebugBuild && guard_inserted) {
        CHECK_NE(class_index, DexFile::kDexNoIndex);
      } else if (class_index == DexFile::kDexNoIndex) {
        PRINT_PASS_OSTREAM_MESSAGE(this, "Sharpening failed because we cannot find " <<
                                   PrettyClass(type.Get()) << " in the dex cache for " <<
                                   invoke);
        return false;
      }
    }

    ArtMethod* resolved_method =
        compilation_unit_.GetDexCache().Get()->GetResolvedMethod(invoke->GetDexMethodIndex(),
                                                                 pointer_size);
    // We only sharpen for resolved invokes.
    DCHECK(resolved_method != nullptr);
    if (UNLIKELY(resolved_method == nullptr)) {
        PRINT_PASS_OSTREAM_MESSAGE(this, "Sharpening failed because resolved method is Null");
        return false;
    }

    ArtMethod* actual_method = resolved_method;
    if (!IsMethodOrDeclaringClassFinal(resolved_method)) {
      if (invoke->IsInvokeInterface()) {
        actual_method = type->FindVirtualMethodForInterface(resolved_method, pointer_size);
      } else {
        DCHECK(invoke->IsInvokeVirtual());
        actual_method = type->FindVirtualMethodForVirtual(resolved_method, pointer_size);
      }
    }

    if (actual_method == nullptr) {
      PRINT_PASS_OSTREAM_MESSAGE(this, "Sharpening failed because we cannot find " <<
                                 PrettyMethod(resolved_method) << " in the class " <<
                                 PrettyClass(type.Get()) <<  " for " << invoke);
      return false;
    }

    method_index = FindMethodIndexIn(actual_method,
                                     caller_dex_file,
                                     invoke->GetDexMethodIndex());
    if (method_index == DexFile::kDexNoIndex) {
      PRINT_PASS_OSTREAM_MESSAGE(this, "Sharpening failed because we cannot find " <<
                                 PrettyMethod(resolved_method) << " in the caller's dex file "
                                 "for " << invoke);
      return false;
    }
  }

  MethodReference target_method(compilation_unit_.GetDexFile(), method_index);
  HInvokeStaticOrDirect::DispatchInfo dispatch_info = {
      HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod,
      HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod,
      0u,
      0U
  };
  HInvokeStaticOrDirect* new_invoke =
      new (graph_->GetArena()) HInvokeStaticOrDirect(graph_->GetArena(),
                                                     invoke->GetNumberOfArguments(),
                                                     invoke->GetType(),
                                                     invoke->GetDexPc(),
                                                     method_index,
                                                     target_method,
                                                     dispatch_info,
                                                     invoke->GetOriginalInvokeType(),
                                                     InvokeType::kDirect,
                                                     HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);
  for (size_t i = 0, e = invoke->InputCount(); i < e; ++i) {
    HInstruction* input = invoke->InputAt(i);
    new_invoke->SetArgumentAt(i, input);
  }
  // We need to add current method as input so we can get access to dex cache.
  // This use might be removed during call sharpening phase.
  if (HInvokeStaticOrDirect::NeedsCurrentMethodInput(new_invoke->GetMethodLoadKind())) {
    new_invoke->SetArgumentAt(invoke->InputCount(), graph_->GetCurrentMethod());
  }
  // Keep the type information from previous invoke.
  if (invoke->GetType() == Primitive::kPrimNot) {
    new_invoke->SetReferenceTypeInfo(invoke->GetReferenceTypeInfo());
  }
  // No need to copy intrinsic information - these should not be candidates.
  DCHECK(!invoke->IsIntrinsic());

  invoke->GetBlock()->ReplaceAndRemoveInstructionWith(invoke, new_invoke);
  new_invoke->CopyEnvironmentFrom(invoke->GetEnvironment());
  return true;
}

SpeculationRecoveryApproach HDevirtualization::GetRecoveryMethod(HInstruction* instr) {
  if (precise_prediction_.find(instr) != precise_prediction_.end()) {
    return kRecoveryNotNeeded;
  }

  size_t prediction_count = imprecise_predictions_.find(instr)->second.size();
  if (prediction_count == 1) {
    // If the prediction is monomorphic call - we do not care about the overhead of deopt.
    return kRecoveryDeopt;
  }

  // For multiple predictions - use versioning.
  return kRecoveryCodeVersioning;
}

}  // namespace art