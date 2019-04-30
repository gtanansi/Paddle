// Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>
#include <queue>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "paddle/fluid/framework/details/computation_op_handle.h"
#include "paddle/fluid/framework/details/eager_deletion_op_handle.h"
#include "paddle/fluid/framework/details/multi_devices_helper.h"
#include "paddle/fluid/framework/details/op_graph_view.h"
#include "paddle/fluid/framework/details/reference_count_pass.h"
#include "paddle/fluid/framework/details/reference_count_pass_helper.h"
#include "paddle/fluid/framework/ir/graph_helper.h"

namespace paddle {
namespace framework {
namespace details {

// A functor to shrink/remove operators who depend on other operators in a set
class ShrinkDepsOpFunctor {
 private:
  enum RelationShip { kSame = 0, kNoDeps = 1, kBefore = 2, kAfter = 3 };

 public:
  explicit ShrinkDepsOpFunctor(const std::vector<OpHandleBase *> &all_ops)
      : graph_(all_ops) {}

  template <typename OpSet>
  OpSet operator()(const OpSet &op_set) const {
    using KeyType = typename OpSet::key_type;
    static_assert(
        std::is_base_of<OpHandleBase,
                        typename std::remove_pointer<KeyType>::type>::value,
        "Key type of OpSet must be OpHandleBase, or derived of OpHandleBase");

    if (op_set.size() <= 1) return op_set;
    std::vector<OpHandleBase *> ops(op_set.begin(), op_set.end());
    OpSet ret;
    auto rels = GetRelations(ops);
    auto not_before = [](RelationShip r) { return r != kBefore; };
    for (size_t i = 0; i < rels.size(); ++i) {
      if (std::all_of(rels[i].begin(), rels[i].end(), not_before)) {
        ret.emplace(static_cast<KeyType>(ops[i]));
      }
    }
    return ret;
  }

 private:
  std::vector<std::vector<RelationShip>> GetRelations(
      const std::vector<OpHandleBase *> &ops) const {
    std::unordered_map<OpHandleBase *, size_t> op_to_idx;
    for (size_t i = 0; i < ops.size(); ++i) {
      PADDLE_ENFORCE(graph_.HasOp(ops[i]), "Op does not exist in graph");
      op_to_idx[ops[i]] = i;
    }

    PADDLE_ENFORCE(op_to_idx.size() == ops.size(), "Duplicate ops");

    std::vector<std::vector<RelationShip>> ret(ops.size());
    for (auto &e : ret) {
      e.assign(ops.size(), kSame);
    }

    size_t found_num = ops.size();
    size_t total_num = ops.size() * ops.size();
    auto visitor = [&](OpHandleBase *op, size_t i) {
      auto it = op_to_idx.find(op);
      if (it != op_to_idx.end()) {
        size_t j = it->second;
        if (i != j && ret[i][j] == kSame) {
          ret[i][j] = kBefore;
          ret[j][i] = kAfter;
          found_num += 2;
          if (found_num == total_num) {
            return false;
          }
        }
      }
      return true;
    };

    for (size_t i = 0; i < ops.size(); ++i) {
      auto sub_visitor = [&, i](OpHandleBase *op) { return visitor(op, i); };
      if (!graph_.VisitAllPendingOps(ops[i], sub_visitor)) {
        break;
      }
    }

    for (size_t i = 0; i < ops.size(); ++i) {
      for (size_t j = i + 1; j < ops.size(); ++j) {
        if (ret[i][j] != kSame) continue;
        ret[i][j] = kNoDeps;
        ret[j][i] = kNoDeps;
      }
    }

    return ret;
  }

  const OpGraphView graph_;
};

/**
 * Find the nearest downstream computation op handle. If the op is a
 * computation op, just return itself.
 */
static ComputationOpHandle *FindNextComputationOpHandleOrReturnItself(
    OpHandleBase *op, size_t scope_idx) {
  std::queue<OpHandleBase *> q;
  std::unordered_set<OpHandleBase *> visited;
  q.push(op);
  do {
    auto *op = q.front();
    q.pop();
    auto *compute_op = dynamic_cast<ComputationOpHandle *>(op);
    if (compute_op != nullptr && compute_op->GetScopeIdx() == scope_idx) {
      return compute_op;
    }
    for (auto *out_var : op->Outputs()) {
      for (auto *pending_op : out_var->PendingOps()) {
        if (visited.count(pending_op)) continue;
        visited.insert(pending_op);
        q.push(pending_op);
      }
    }
  } while (!q.empty());
  return nullptr;
}

static std::unordered_set<ComputationOpHandle *>
ExtractComputationOpFromLastLivedVar(VarHandle *var, size_t scope_idx,
                                     const ShrinkDepsOpFunctor &shrink_func,
                                     bool *ok) {
  // stage one. Get last op for variable.
  std::unordered_set<OpHandleBase *> candidates;
  {
    if (var->PendingOps().empty() && var->GeneratedOp()) {
      // No operator depends on this variable. So the last operator is the op
      // who generates this variable.
      candidates.emplace(var->GeneratedOp());
    } else {
      candidates = var->PendingOps();
    }

    // No pending ops or generated op is nullptr
    if (candidates.empty()) {
      *ok = false;
      return {};
    }
  }

  // stage two. Try to cast them to computation op.
  // return (*ok=false) when failed.
  //
  // The reason why we cannot make any types of op handle to be the last lived
  // op is:
  //    some op handle may operate on many DeviceContext, however, our garbage
  //    collector can only wait one DeviceContext for now. So currently, we wait
  //    the nearest compute op.
  std::unordered_set<ComputationOpHandle *> computation_op;
  {
    for (auto *op : candidates) {
      auto *compute_op =
          FindNextComputationOpHandleOrReturnItself(op, scope_idx);
      if (compute_op == nullptr) {
        *ok = false;
        return {};
      }
      computation_op.emplace(compute_op);
    }
  }

  // stage three. Try to shrink computation op if they depend on each other.
  // Get the smallest set of the most ops.
  *ok = true;
  return shrink_func(computation_op);
}

/**
 * Shrink op dependencies according to no need buffer vars.
 *
 * If some ops do not need Tensor buffer of any input,
 * just remove the dependency of this op, i.e, decrease reference count.
 *
 * For example, input Y of elementwise_add_grad op is only used to infer shape
 * and lod of Y@GRAD, we do not need the buffer of input Y. Data buffer of
 * input Y can be collected before elementwise_add_grad op runs.
 *
 * This method returns whether the dependency count decreases to 0, and
 * shrinks op dependency if possible.
 */
static bool ShrinkNoNeedBufferVarOpDependency(
    const std::string &var_name,
    std::unordered_set<ComputationOpHandle *> *op_handles) {
  std::vector<ComputationOpHandle *> skip_ops;
  for (auto *op_handle : *op_handles) {
    auto *op_base = op_handle->GetOp();
    auto &inferer = op_base->Info().NoNeedBufferVarsInferer();
    if (!inferer) {
      continue;
    }

    std::unordered_set<std::string> no_need_buffer_vars =
        inferer(op_base->Inputs(), op_base->Outputs(), op_base->Attrs());

    // Check whether var_name occurs in other inputs or outputs of the op
    // If it occurs, we cannot decrease the dependency number.
    bool occurred_in_other_vars = false;
    for (auto &in_pair : op_base->Inputs()) {
      if (no_need_buffer_vars.count(in_pair.first) > 0) {
        continue;
      }

      auto &args = in_pair.second;
      auto iter = std::find(args.begin(), args.end(), var_name);
      if (iter != args.end()) {
        occurred_in_other_vars = true;
        break;
      }
    }

    if (occurred_in_other_vars) {
      continue;
    }

    for (auto &out_pair : op_base->Outputs()) {
      auto &args = out_pair.second;
      auto iter = std::find(args.begin(), args.end(), var_name);
      if (iter != args.end()) {
        occurred_in_other_vars = true;
        break;
      }
    }

    if (!occurred_in_other_vars) {
      VLOG(2) << "Shrink var " << var_name << " in op " << op_handle->Name();
      skip_ops.emplace_back(op_handle);
    }
  }

  if (skip_ops.size() == op_handles->size()) {
    op_handles->clear();
    return true;
  } else {
    for (auto *skip_op : skip_ops) {
      op_handles->erase(skip_op);
    }
    return false;
  }
}

void ReferenceCountPass::ApplyImpl(ir::Graph *graph) const {
  auto &ref_cnts = Get<std::vector<ReferenceCountMap>>(kGlobalReferenceCount);
  auto &last_live_ops_of_vars =
      Get<std::vector<LastLiveOpsOfVars>>(kLastLiveOpsOfVars);

  PADDLE_ENFORCE(last_live_ops_of_vars.empty() && ref_cnts.empty(),
                 "Last Live Ops and Reference Counts of vars should be "
                 "initialized at here.");

  const auto &vars = graph->Get<GraphVars>(kGraphVars);

  last_live_ops_of_vars.resize(vars.size());
  ref_cnts.resize(vars.size());

  ShrinkDepsOpFunctor shrink_func(
      ir::FilterByNodeWrapper<OpHandleBase>(*graph));

  for (size_t i = 0; i < vars.size(); ++i) {
    for (auto &name_var_pair : vars[i]) {
      // Whether this variable can be reused or deleted? If not, we do not
      // compute reference counts and dependencies.
      VarDesc *var_desc = TryGetLatestVarDesc(name_var_pair.second);

      if (var_desc == nullptr || var_desc->Persistable()) {
        continue;
      }

      auto var_type = var_desc->Proto()->type().type();
      if (var_type != proto::VarType::LOD_TENSOR &&
          var_type != proto::VarType::SELECTED_ROWS &&
          var_type != proto::VarType::LOD_TENSOR_ARRAY) {
        // Var type cannot be deleted
        continue;
      }

      auto &var_name = name_var_pair.first;
      auto &var_handles = name_var_pair.second;

      for (auto iter = var_handles.rbegin(); iter != var_handles.rend();
           ++iter) {
        bool ok;
        auto result =
            ExtractComputationOpFromLastLivedVar(*iter, i, shrink_func, &ok);

        // Seldomly, some vars may have no pending or preceding computation ops
        // Just break;
        if (!ok) break;
        VLOG(10) << "Extract " << result.size() << " ops of var " << var_name;

        size_t original_op_deps = result.size();
        // If all ops do not need buffer of var_name, calculate reference count
        // of the previous version of var_name.
        if (ShrinkNoNeedBufferVarOpDependency(var_name, &result)) {
          VLOG(10) << "Try to precede reference count computing at var "
                   << var_name;
          continue;
        }

        size_t final_op_deps = result.size();
        if (final_op_deps < original_op_deps) {
          VLOG(5) << "Shrink op deps from " << original_op_deps << " to "
                  << final_op_deps;
        }

        PADDLE_ENFORCE(!result.empty(), "Last living ops of %s cannot be empty",
                       var_name);
        ref_cnts[i].emplace(var_name, result.size());
        last_live_ops_of_vars[i].emplace(var_name, std::move(result));
        break;
      }

      // Seldomly, all preceding trying failed.
      // Just skip this corner case
    }
  }
}

}  // namespace details
}  // namespace framework
}  // namespace paddle

REGISTER_PASS(reference_count_pass,
              paddle::framework::details::ReferenceCountPass)
    .RequirePassAttr(paddle::framework::details::kGlobalReferenceCount)
    .RequirePassAttr(paddle::framework::details::kLastLiveOpsOfVars);