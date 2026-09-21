// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/decision.hpp"

#include <algorithm>

namespace fabric_partition_manager {
namespace {

[[nodiscard]] ScopeId make_scope_id(std::string_view prefix, std::string_view value) {
  std::string text(prefix);
  text.append(value);
  return ScopeId::from_validated(text);
}

}  // namespace

std::string_view scope_kind_name(ScopeKind value) noexcept {
  switch (value) {
    case ScopeKind::Fabric: return "FABRIC";
    case ScopeKind::Region: return "REGION";
    case ScopeKind::Domain: return "DOMAIN";
    case ScopeKind::Partition: return "PARTITION";
    case ScopeKind::Component: return "COMPONENT";
  }
  return "UNRECOGNIZED";
}

Scope fabric_scope() {
  Scope scope;
  scope.kind = ScopeKind::Fabric;
  scope.id = ScopeId::from_validated("fabric");
  return scope;
}

Scope partition_scope(const PartitionId& partition) {
  Scope scope;
  scope.kind = ScopeKind::Partition;
  scope.id = make_scope_id("partition:", partition.view());
  return scope;
}

Scope component_scope(const ComponentId& component) {
  Scope scope;
  scope.kind = ScopeKind::Component;
  scope.id = make_scope_id("component:", component.view());
  return scope;
}

Scope named_scope(ScopeKind kind, std::string_view name) {
  Scope scope;
  scope.kind = kind;
  scope.id = ScopeId::from_validated(name);
  return scope;
}

std::string_view decision_kind_name(DecisionKind value) noexcept {
  switch (value) {
    case DecisionKind::AdoptTopology: return "ADOPT_TOPOLOGY";
    case DecisionKind::SetPolicy: return "SET_POLICY";
    case DecisionKind::IngestEvidence: return "INGEST_EVIDENCE";
    case DecisionKind::AssessFabric: return "ASSESS_FABRIC";
    case DecisionKind::GrantAuthority: return "GRANT_AUTHORITY";
    case DecisionKind::DegradeAuthority: return "DEGRADE_AUTHORITY";
    case DecisionKind::IsolateComponents: return "ISOLATE_COMPONENTS";
    case DecisionKind::MergePartitions: return "MERGE_PARTITIONS";
    case DecisionKind::Revalidate: return "REVALIDATE";
    case DecisionKind::Fence: return "FENCE";
    case DecisionKind::Retire: return "RETIRE";
    case DecisionKind::AcknowledgeAuthority: return "ACKNOWLEDGE_AUTHORITY";
    case DecisionKind::VerifyEffect: return "VERIFY_EFFECT";
  }
  return "UNRECOGNIZED";
}

std::string_view decision_verdict_name(DecisionVerdict value) noexcept {
  switch (value) {
    case DecisionVerdict::Observed: return "OBSERVED";
    case DecisionVerdict::Granted: return "GRANTED";
    case DecisionVerdict::Denied: return "DENIED";
    case DecisionVerdict::Degraded: return "DEGRADED";
    case DecisionVerdict::Isolated: return "ISOLATED";
    case DecisionVerdict::Indeterminate: return "INDETERMINATE";
    case DecisionVerdict::Unsupported: return "UNSUPPORTED";
    case DecisionVerdict::Invalid: return "INVALID";
    case DecisionVerdict::Conflict: return "CONFLICT";
  }
  return "UNRECOGNIZED";
}

DecisionVerdict derive_verdict(bool invalid, bool unsupported, bool indeterminate, bool conflict,
                              bool granted, bool degraded, bool isolated) noexcept {
  if (invalid) {
    return DecisionVerdict::Invalid;
  }
  if (unsupported) {
    return DecisionVerdict::Unsupported;
  }
  if (conflict) {
    return DecisionVerdict::Conflict;
  }
  if (indeterminate) {
    return DecisionVerdict::Indeterminate;
  }
  if (isolated) {
    return DecisionVerdict::Isolated;
  }
  if (degraded) {
    return DecisionVerdict::Degraded;
  }
  if (granted) {
    return DecisionVerdict::Granted;
  }
  return DecisionVerdict::Denied;
}

bool decision_verdict_is_success(DecisionVerdict value) noexcept {
  return value == DecisionVerdict::Granted;
}

bool decision_verdict_needs_attention(DecisionVerdict value) noexcept {
  switch (value) {
    case DecisionVerdict::Granted:
    case DecisionVerdict::Observed:
      return false;
    case DecisionVerdict::Denied:
    case DecisionVerdict::Degraded:
    case DecisionVerdict::Isolated:
    case DecisionVerdict::Indeterminate:
    case DecisionVerdict::Unsupported:
    case DecisionVerdict::Invalid:
    case DecisionVerdict::Conflict:
      return true;
  }
  return true;
}

std::string Decision::render() const {
  std::string result;
  result.append(decision_kind_name(kind));
  result.push_back(' ');
  result.append(decision_verdict_name(verdict));
  result.append(" id=");
  result.append(id.view());
  result.append(" scope=");
  result.append(scope_kind_name(scope.kind));
  result.push_back(':');
  result.append(scope.id.view());
  result.append(" epoch=");
  result.append(bindings.epoch.render());
  result.append(" topology=");
  result.append(bindings.topology_generation.render());
  result.append(" reachability=");
  result.append(bindings.reachability_generation.render());
  result.append(" policy=");
  result.append(bindings.policy_generation.render());
  result.append(" partition=");
  result.append(bindings.partition_generation.render());
  result.append(" authority-seq=");
  result.append(bindings.authority_sequence.render());
  result.append(" authority=");
  result.append(authority.render());
  return result;
}

DecisionLog::DecisionLog(const Limits& limits) : limits_(limits) {}

void DecisionLog::append(Decision decision) {
  entries_.push_back(std::move(decision));
  while (entries_.size() > limits_.max_decision_history) {
    entries_.erase(entries_.begin());
    ++dropped_;
  }
}

const Decision* DecisionLog::find(const DecisionId& id) const {
  for (const Decision& entry : entries_) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

void DecisionLog::clear() noexcept {
  entries_.clear();
  dropped_ = 0;
}

}  // namespace fabric_partition_manager
