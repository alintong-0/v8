// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/revectorizer.h"

#include "src/base/cpu.h"
#include "src/base/logging.h"
#include "src/compiler/all-nodes.h"
#include "src/compiler/machine-operator.h"
#include "src/compiler/verifier.h"

namespace v8 {
namespace internal {
namespace compiler {

#define TRACE(...)                         \
  do {                                     \
    if (v8_flags.trace_wasm_revectorize) { \
      PrintF("Revec: ");                   \
      PrintF(__VA_ARGS__);                 \
    }                                      \
  } while (false)

namespace {

#ifdef DEBUG
// Currently, only Load/ProtectedLoad/LoadTransfrom are supported.
// TODO(jiepan): add support for UnalignedLoad, LoadLane
bool IsSupportedLoad(const Node* node) {
  if (node->opcode() == IrOpcode::kProtectedLoad ||
      node->opcode() == IrOpcode::kLoad ||
      node->opcode() == IrOpcode::kLoadTransform) {
    return true;
  }
  return false;
}
bool IsSupportedLoad(const ZoneVector<Node*>& node_group) {
  for (auto node : node_group) {
    if (!IsSupportedLoad(node)) return false;
  }
  return true;
}
#endif

int64_t GetConstantValue(const Node* node) {
  int64_t value = -1;
  if (node->opcode() == IrOpcode::kInt64Constant) {
    value = OpParameter<int64_t>(node->op());
  }
  return value;
}

int64_t GetMemoryOffsetValue(const Node* node) {
  DCHECK(node->opcode() == IrOpcode::kProtectedLoad ||
         node->opcode() == IrOpcode::kStore ||
         node->opcode() == IrOpcode::kProtectedStore);

  Node* offset = node->InputAt(0);
  if (offset->opcode() == IrOpcode::kLoadFromObject ||
      offset->opcode() == IrOpcode::kLoad) {
    return 0;
  }

  int64_t offset_value = -1;
  if (offset->opcode() == IrOpcode::kInt64Add) {
    if (NodeProperties::IsConstant(offset->InputAt(0))) {
      offset_value = GetConstantValue(offset->InputAt(0));
    } else if (NodeProperties::IsConstant(offset->InputAt(1))) {
      offset_value = GetConstantValue(offset->InputAt(1));
    }
  }
  return offset_value;
}

// We want to combine load/store nodes with continuous memory address,
// for load/store node, input(0) is memory_start + offset,  input(1) is index,
// we currently use index as the address of the node, nodes with same index and
// continuous offset can be combined together.
Node* GetNodeAddress(const Node* node) {
  Node* address = node->InputAt(1);
  // The index is changed to Uint64 for memory32
  if (address->opcode() == IrOpcode::kChangeUint32ToUint64) {
    address = address->InputAt(0);
  }
  return address;
}

bool IsContinuousAccess(const ZoneVector<Node*>& node_group) {
  DCHECK_GT(node_group.size(), 0);
  int64_t previous_offset = GetMemoryOffsetValue(node_group[0]);
  for (size_t i = 1; i < node_group.size(); ++i) {
    int64_t current_offset = GetMemoryOffsetValue(node_group[i]);
    int64_t diff = current_offset - previous_offset;
    if (diff != kSimd128Size) {
      TRACE("Non-continuous store!");
      return false;
    }
    previous_offset = current_offset;
  }
  return true;
}

// Returns true if all of the nodes in node_group are constants.
bool AllConstant(const ZoneVector<Node*>& node_group) {
  for (Node* node : node_group) {
    if (!NodeProperties::IsConstant(node)) {
      return false;
    }
  }
  return true;
}

// Returns true if all the addresses of the nodes in node_group are identical.
bool AllSameAddress(const ZoneVector<Node*>& nodes) {
  Node* address = GetNodeAddress(nodes[0]);
  for (size_t i = 1; i < nodes.size(); i++) {
    if (GetNodeAddress(nodes[i]) != address) {
      TRACE("Diff address #%d,#%d!\n", address->id(),
            GetNodeAddress(nodes[i])->id());
      return false;
    }
  }
  return true;
}

// Returns true if all of the nodes in node_group are identical.
// Splat opcode in WASM SIMD is used to create vector with identical lanes.
bool IsSplat(const ZoneVector<Node*>& node_group) {
  for (ZoneVector<Node*>::size_type i = 1; i < node_group.size(); ++i) {
    if (node_group[i] != node_group[0]) {
      return false;
    }
  }
  return true;
}

// Returns true if all of the nodes in node_group have the same type.
bool AllSameOperator(const ZoneVector<Node*>& node_group) {
  auto op = node_group[0]->op();
  for (ZoneVector<Node*>::size_type i = 1; i < node_group.size(); i++) {
    if (node_group[i]->op() != op) {
      return false;
    }
  }
  return true;
}

}  // anonymous namespace

// Sort load/store node by offset
bool MemoryOffsetComparer::operator()(const Node* lhs, const Node* rhs) const {
  return GetMemoryOffsetValue(lhs) < GetMemoryOffsetValue(rhs);
}

void PackNode::Print() const {
  if (revectorized_node_ != nullptr) {
    TRACE("0x%p #%d:%s(%d %d, %s)\n", this, revectorized_node_->id(),
          revectorized_node_->op()->mnemonic(), nodes_[0]->id(),
          nodes_[1]->id(), nodes_[0]->op()->mnemonic());
  } else {
    TRACE("0x%p null(%d %d, %s)\n", this, nodes_[0]->id(), nodes_[1]->id(),
          nodes_[0]->op()->mnemonic());
  }
}

bool SLPTree::CanBePacked(const ZoneVector<Node*>& node_group) {
  DCHECK_EQ(node_group.size(), 2);
  if (!SameBasicBlock(node_group[0], node_group[1])) {
    TRACE("%s(#%d, #%d) not in same BB!\n", node_group[0]->op()->mnemonic(),
          node_group[0]->id(), node_group[1]->id());
    return false;
  }
  if (!AllSameOperator(node_group)) {
    TRACE("%s(#%d, #%d) have different operator!\n",
          node_group[0]->op()->mnemonic(), node_group[0]->id(),
          node_group[1]->id());
    return false;
  }
  // TODO(jiepan): add support for Constant
  if (AllConstant(node_group)) {
    TRACE("%s(#%d, #%d) are constantant, not supported yet!\n",
          node_group[0]->op()->mnemonic(), node_group[0]->id(),
          node_group[1]->id());
    return false;
  }

  // Only Support simd128 operators or common operators with simd128
  // MachineRepresentation. The MachineRepresentation of root had been checked,
  // and the leaf node will be checked later. here we omit the check of
  // MachineRepresentation, only check the opcode itself.
  IrOpcode::Value op = node_group[0]->opcode();
  if (NodeProperties::IsSimd128Operation(node_group[0]) ||
      (op == IrOpcode::kStore) || (op == IrOpcode::kProtectedStore) ||
      (op == IrOpcode::kLoad) || (op == IrOpcode::kProtectedLoad) ||
      (op == IrOpcode::kPhi) || (op == IrOpcode::kLoopExitValue) ||
      (op == IrOpcode::kExtractF128)) {
    return true;
  }
  return false;
}

PackNode* SLPTree::NewPackNode(const ZoneVector<Node*>& node_group) {
  TRACE("PackNode %s(#%d:, #%d)\n", node_group[0]->op()->mnemonic(),
        node_group[0]->id(), node_group[1]->id());
  PackNode* pnode = zone_->New<PackNode>(zone_, node_group);
  for (Node* node : node_group) {
    node_to_packnode_[node] = pnode;
  }
  return pnode;
}

PackNode* SLPTree::NewPackNodeAndRecurs(const ZoneVector<Node*>& node_group,
                                        int start_index, int count,
                                        unsigned recursion_depth) {
  PackNode* pnode = NewPackNode(node_group);
  for (int i = start_index; i < start_index + count; ++i) {
    ZoneVector<Node*> operands(zone_);
    // Prepare the operand vector.
    for (size_t j = 0; j < node_group.size(); j++) {
      Node* node = node_group[j];
      operands.push_back(NodeProperties::GetValueInput(node, i));
    }

    PackNode* child = BuildTreeRec(operands, recursion_depth + 1);
    if (child) {
      pnode->SetOperand(i, child);
    } else {
      return nullptr;
    }
  }
  return pnode;
}

PackNode* SLPTree::GetPackNode(Node* node) {
  auto I = node_to_packnode_.find(node);
  if (I != node_to_packnode_.end()) {
    return I->second;
  }
  return nullptr;
}

void SLPTree::PushStack(const ZoneVector<Node*>& node_group) {
  TRACE("Stack Push (%d %s, %d %s)\n", node_group[0]->id(),
        node_group[0]->op()->mnemonic(), node_group[1]->id(),
        node_group[1]->op()->mnemonic());
  for (auto node : node_group) {
    on_stack_.insert(node);
  }
  stack_.push({node_group});
}

void SLPTree::PopStack() {
  const ZoneVector<Node*>& node_group = stack_.top();
  DCHECK_EQ(node_group.size(), 2);
  TRACE("Stack Pop (%d %s, %d %s)\n", node_group[0]->id(),
        node_group[0]->op()->mnemonic(), node_group[1]->id(),
        node_group[1]->op()->mnemonic());
  for (auto node : node_group) {
    on_stack_.erase(node);
  }
  stack_.pop();
}

bool SLPTree::OnStack(Node* node) {
  return on_stack_.find(node) != on_stack_.end();
}

bool SLPTree::AllOnStack(const ZoneVector<Node*>& node_group) {
  for (auto node : node_group) {
    if (OnStack(node)) return true;
  }
  return false;
}

bool SLPTree::StackTopIsPhi() {
  const ZoneVector<Node*>& node_group = stack_.top();
  DCHECK_EQ(node_group.size(), 2);
  return NodeProperties::IsPhi(node_group[0]);
}

void SLPTree::ClearStack() {
  stack_ = ZoneStack<ZoneVector<Node*>>(zone_);
  on_stack_.clear();
}

bool SLPTree::IsSideEffectFreeLoad(const ZoneVector<Node*>& node_group) {
  DCHECK(IsSupportedLoad(node_group));
  DCHECK_EQ(node_group.size(), 2);
  TRACE("Enter IsSideEffectFreeLoad (%d %s, %d %s)\n", node_group[0]->id(),
        node_group[0]->op()->mnemonic(), node_group[1]->id(),
        node_group[1]->op()->mnemonic());

  std::stack<Node*> to_visit;
  std::unordered_set<Node*> visited;
  // Visit all the inputs (except for control inputs) of Loads.
  for (size_t i = 0, e = node_group.size(); i < e; i++) {
    Node* load = node_group[i];
    for (int j = 0; j < NodeProperties::FirstControlIndex(load); ++j) {
      Node* input = load->InputAt(j);
      if (std::find(node_group.begin(), node_group.end(), input) ==
          node_group.end()) {
        to_visit.push(input);
      }
    }
  }

  // Check the inputs of Loads and find if they are connected to existing nodes
  // in SLPTree. If there is, then there will be side effect and we can not
  // merge such Loads.
  while (!to_visit.empty()) {
    Node* input = to_visit.top();
    to_visit.pop();
    TRACE("IsSideEffectFreeLoad visit (%d %s)\n", input->id(),
          input->op()->mnemonic());
    if (visited.find(input) == visited.end()) {
      visited.insert(input);

      if (OnStack(input)) {
        TRACE("Has internal dependency because (%d %s) on stack\n", input->id(),
              input->op()->mnemonic());
        return false;
      }

      // If the input is not in same basic block as Loads, it must not be in
      // SLPTree. Otherwise recursively visit all input's edges and find if they
      // are connected to SLPTree.
      if (SameBasicBlock(input, node_group[0])) {
        for (int i = 0; i < NodeProperties::FirstControlIndex(input); ++i) {
          to_visit.push(input->InputAt(i));
        }
      }
    }
  }
  return true;
}

PackNode* SLPTree::BuildTree(const ZoneVector<Node*>& roots) {
  TRACE("Enter %s\n", __func__);

  DeleteTree();

  root_ = BuildTreeRec(roots, 0);
  return root_;
}

PackNode* SLPTree::BuildTreeRec(const ZoneVector<Node*>& node_group,
                                unsigned recursion_depth) {
  TRACE("Enter %s\n", __func__);
  DCHECK_EQ(node_group.size(), 2);

  Node* node0 = node_group[0];
  Node* node1 = node_group[1];

  if (recursion_depth == RecursionMaxDepth) {
    TRACE("Failed due to max recursion depth!\n");
    return nullptr;
  }

  if (AllOnStack(node_group)) {
    if (!StackTopIsPhi()) {
      TRACE("Failed due to (%d %s, %d %s) on stack!\n", node0->id(),
            node0->op()->mnemonic(), node1->id(), node1->op()->mnemonic());
      return nullptr;
    }
  }
  PushStack(node_group);

  if (!CanBePacked(node_group)) {
    return nullptr;
  }

  DCHECK(AllConstant(node_group) || AllSameOperator(node_group));

  // Check if this is a duplicate of another entry.
  for (Node* node : node_group) {
    if (PackNode* p = GetPackNode(node)) {
      if (!p->IsSame(node_group)) {
        // TODO(jiepan): Gathering due to partial overlap
        TRACE("Failed due to partial overlap at #%d,%s!\n", node->id(),
              node->op()->mnemonic());
        return nullptr;
      }

      PopStack();
      TRACE("Perfect diamond merge at #%d,%s\n", node->id(),
            node->op()->mnemonic());
      return p;
    }
  }

  if (node0->opcode() == IrOpcode::kExtractF128) {
    Node* source = node0->InputAt(0);
    TRACE("Extract leaf node from #%d,%s!\n", source->id(),
          source->op()->mnemonic());
    // For 256 only, check whether they are from the same source
    if (node0->InputAt(0) == node1->InputAt(0) &&
        (node0->InputAt(0)->opcode() == IrOpcode::kLoadTransform
             ? node0 == node1
             : OpParameter<int32_t>(node0->op()) + 1 ==
                   OpParameter<int32_t>(node1->op()))) {
      TRACE("Added a pair of Extract.\n");
      PackNode* pnode = NewPackNode(node_group);
      PopStack();
      return pnode;
    }
    TRACE("Failed due to ExtractF128!\n");
    return nullptr;
  }

  if (node0->opcode() == IrOpcode::kProtectedLoad ||
      node0->opcode() == IrOpcode::kLoadTransform) {
    TRACE("Load leaf node\n");
    if (!AllSameAddress(node_group)) {
      TRACE("Failed due to different load addr!\n");
      return nullptr;
    }
    if (node0->opcode() == IrOpcode::kProtectedLoad) {
      MachineRepresentation rep =
          LoadRepresentationOf(node0->op()).representation();
      if (rep != MachineRepresentation::kSimd128) {
        return nullptr;
      }
      // Sort loads by offset
      ZoneVector<Node*> sorted_node_group(node_group.size(), zone_);
      partial_sort_copy(begin(node_group), end(node_group),
                        begin(sorted_node_group), end(sorted_node_group),
                        MemoryOffsetComparer());
      if (!IsContinuousAccess(sorted_node_group)) {
        TRACE("Failed due to non-continuous load!\n");
        return nullptr;
      }
    }

    if (node0->opcode() == IrOpcode::kLoadTransform) {
      if (!IsSplat(node_group)) {
        TRACE("LoadTransform Failed due to IsSplat!\n");
        return nullptr;
      }
      LoadTransformParameters params = LoadTransformParametersOf(node0->op());
      // TODO(jiepan): Support more LoadTransformation types
      if (params.transformation != LoadTransformation::kS128Load32Splat &&
          params.transformation != LoadTransformation::kS128Load64Splat) {
        TRACE("LoadTransform failed due to unsupported type #%d!\n",
              node0->id());
        return nullptr;
      }
    }

    if (!IsSideEffectFreeLoad(node_group)) {
      TRACE("Failed due to dependency check\n");
      return nullptr;
    }
    PackNode* p = NewPackNode(node_group);
    PopStack();
    return p;
  }

  int value_in_count = node0->op()->ValueInputCount();
  switch (node0->opcode()) {
    case IrOpcode::kPhi: {
      TRACE("Added a vector of PHI nodes.\n");
      MachineRepresentation rep = PhiRepresentationOf(node0->op());
      if (rep != MachineRepresentation::kSimd128) {
        return nullptr;
      }
      PackNode* pnode =
          NewPackNodeAndRecurs(node_group, 0, value_in_count, recursion_depth);
      PopStack();
      return pnode;
    }
    case IrOpcode::kLoopExitValue: {
      MachineRepresentation rep = LoopExitValueRepresentationOf(node0->op());
      if (rep != MachineRepresentation::kSimd128) {
        return nullptr;
      }
      PackNode* pnode =
          NewPackNodeAndRecurs(node_group, 0, value_in_count, recursion_depth);
      PopStack();
      return pnode;
    }
    case IrOpcode::kF32x4Add:
    case IrOpcode::kF32x4Mul: {
      TRACE("Added a vector of un/bin/ter op.\n");
      PackNode* pnode =
          NewPackNodeAndRecurs(node_group, 0, value_in_count, recursion_depth);
      PopStack();
      return pnode;
    }

    // TODO(jiepan): UnalignedStore,
    case IrOpcode::kStore:
    case IrOpcode::kProtectedStore: {
      TRACE("Added a vector of stores.\n");
      if (!AllSameAddress(node_group)) {
        TRACE("Failed due to different store addr!\n");
        return nullptr;
      }
      PackNode* pnode = NewPackNodeAndRecurs(node_group, 2, 1, recursion_depth);
      PopStack();
      return pnode;
    }
    default:
      TRACE("Default branch #%d:%s\n", node0->id(), node0->op()->mnemonic());
      break;
  }
  return nullptr;
}

void SLPTree::DeleteTree() {
  ClearStack();
  node_to_packnode_.clear();
}

void SLPTree::Print(const char* info) {
  TRACE("%s, Packed node:\n", info);
  if (!v8_flags.trace_wasm_revectorize) {
    return;
  }
  std::unordered_set<PackNode const*> visited;

  for (auto& entry : node_to_packnode_) {
    PackNode const* pnode = entry.second;
    if (!pnode || visited.find(pnode) != visited.end()) {
      continue;
    }
    pnode->Print();
    visited.insert(pnode);
  }
}

//////////////////////////////////////////////////////
void Revectorizer::DetectCPUFeatures() {
  base::CPU cpu;
  if (cpu.has_avx2()) {
    support_simd256_ = true;
  }
}

bool Revectorizer::TryRevectorize(const char* function) {
  bool success = false;
  if (support_simd256_ && graph_->GetSimdStoreNodes().size()) {
    TRACE("TryRevectorize %s\n", function);
    CollectSeeds();
    for (auto entry : group_of_stores_) {
      ZoneMap<Node*, StoreNodeSet>* store_chains = entry.second;
      if (store_chains != nullptr) {
        PrintStores(store_chains);
        if (ReduceStoreChains(store_chains)) {
          TRACE("Successful revectorize %s\n", function);
          success = true;
        }
      }
    }
    TRACE("Finish revectorize %s\n", function);
  }
  return success;
}

void Revectorizer::CollectSeeds() {
  for (auto it = graph_->GetSimdStoreNodes().begin();
       it != graph_->GetSimdStoreNodes().end(); ++it) {
    Node* node = *it;
    Node* dominator = slp_tree_->GetEarlySchedulePosition(node);

    if ((GetMemoryOffsetValue(node) % kSimd128Size) != 0) {
      continue;
    }
    Node* address = GetNodeAddress(node);
    ZoneMap<Node*, StoreNodeSet>* store_nodes;
    auto first_level_iter = group_of_stores_.find(dominator);
    if (first_level_iter == group_of_stores_.end()) {
      store_nodes = zone_->New<ZoneMap<Node*, StoreNodeSet>>(zone_);
      group_of_stores_[dominator] = store_nodes;
    } else {
      store_nodes = first_level_iter->second;
    }
    auto second_level_iter = store_nodes->find(address);
    if (second_level_iter == store_nodes->end()) {
      second_level_iter =
          store_nodes->insert({address, StoreNodeSet(zone())}).first;
    }
    second_level_iter->second.insert(node);
  }
}

bool Revectorizer::ReduceStoreChains(
    ZoneMap<Node*, StoreNodeSet>* store_chains) {
  TRACE("Enter %s\n", __func__);
  bool changed = false;
  for (auto chain_iter = store_chains->cbegin();
       chain_iter != store_chains->cend(); ++chain_iter) {
    if (chain_iter->second.size() >= 2 && chain_iter->second.size() % 2 == 0) {
      ZoneVector<Node*> store_chain(chain_iter->second.begin(),
                                    chain_iter->second.end(), zone_);
      for (auto it = store_chain.begin(); it < store_chain.end(); it = it + 2) {
        ZoneVector<Node*> stores_unit(it, it + 2, zone_);
        if (ReduceStoreChain(stores_unit)) {
          changed = true;
        }
      }
    }
  }

  return changed;
}

bool Revectorizer::ReduceStoreChain(const ZoneVector<Node*>& Stores) {
  TRACE("Enter %s, root@ (#%d,#%d)\n", __func__, Stores[0]->id(),
        Stores[1]->id());
  if (!IsContinuousAccess(Stores)) {
    return false;
  }

  PackNode* root = slp_tree_->BuildTree(Stores);
  if (!root) {
    TRACE("Build tree failed!\n");
    return false;
  }

  slp_tree_->Print("After build tree");
  TRACE("\n");
  return true;
}

void Revectorizer::PrintStores(ZoneMap<Node*, StoreNodeSet>* store_chains) {
  if (!v8_flags.trace_wasm_revectorize) {
    return;
  }
  TRACE("Enter %s\n", __func__);
  for (auto it = store_chains->cbegin(); it != store_chains->cend(); ++it) {
    if (it->second.size() > 0) {
      TRACE("address = #%d:%s \n", it->first->id(),
            it->first->op()->mnemonic());

      for (auto node : it->second) {
        TRACE("#%d:%s, ", node->id(), node->op()->mnemonic());
      }

      TRACE("\n");
    }
  }
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8