// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/lineage.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

#include "fabric_partition_manager/encoding.hpp"

namespace fabric_partition_manager {
namespace {

[[nodiscard]] std::string hex_identifier(std::string_view prefix, const Digest& digest) {
  const std::string text = digest.hex().substr(0, 32);
  std::string result(prefix);
  result.append(text);
  return result;
}

}  // namespace

std::string_view lineage_event_kind_name(LineageEventKind value) noexcept {
  switch (value) {
    case LineageEventKind::Genesis: return "GENESIS";
    case LineageEventKind::Split: return "SPLIT";
    case LineageEventKind::Merge: return "MERGE";
    case LineageEventKind::Revalidate: return "REVALIDATE";
    case LineageEventKind::Retire: return "RETIRE";
  }
  return "UNRECOGNIZED";
}

std::string_view lineage_relation_name(LineageRelation value) noexcept {
  switch (value) {
    case LineageRelation::Identical: return "IDENTICAL";
    case LineageRelation::AncestorOf: return "ANCESTOR_OF";
    case LineageRelation::DescendantOf: return "DESCENDANT_OF";
    case LineageRelation::Divergent: return "DIVERGENT";
    case LineageRelation::Unrelated: return "UNRELATED";
    case LineageRelation::Indeterminate: return "INDETERMINATE";
  }
  return "UNRECOGNIZED";
}

LineageDigest compute_lineage_digest(const LineageRecord& record) noexcept {
  Sha256 hasher;
  hasher.update_text("fabric-partition-manager/lineage-record/v1");
  hasher.update_be64(record.sequence.value());
  hasher.update_text(record.lineage.view());
  hasher.update_be64(record.generation.value());
  hasher.update(record.membership.value().bytes.data(), digest_bytes);
  hasher.update_be64(record.member_count());
  for (const ComponentId& member : record.members) {
    hasher.update_text(member.view());
  }
  hasher.update_u8(static_cast<std::uint8_t>(record.event));
  hasher.update_text(record.parent_left.view());
  hasher.update_text(record.parent_right.view());
  hasher.update_be64(record.epoch.value());
  hasher.update(record.boot.bytes().data(), record.boot.bytes().size());
  hasher.update_text(record.decision.view());
  hasher.update_be64(record.tick);
  return LineageDigest::from_digest(hasher.finish());
}

LineageId derive_lineage_id(std::string_view domain, const LineageId& left,
                            const LineageId& right, const MembershipDigest& membership) noexcept {
  Sha256 hasher;
  hasher.update_text("fabric-partition-manager/lineage-identity/v1");
  hasher.update_text(domain);
  hasher.update_text(left.view());
  hasher.update_text(right.view());
  hasher.update(membership.value().bytes.data(), digest_bytes);
  const std::string text = hex_identifier("l", hasher.finish());
  return LineageId::from_validated(text);
}

LineageId genesis_lineage_id(const MembershipDigest& membership) noexcept {
  return derive_lineage_id("genesis", LineageId{}, LineageId{}, membership);
}

LineageId split_lineage_id(const LineageId& parent, const MembershipDigest& membership) noexcept {
  return derive_lineage_id("split", parent, LineageId{}, membership);
}

LineageId merge_lineage_id(const LineageId& left, const LineageId& right) noexcept {
  // Ordered canonically so that the identity of a merge does not depend on
  // which side was presented first.
  const LineageId& first = left < right ? left : right;
  const LineageId& second = left < right ? right : left;
  return derive_lineage_id("merge", first, second, MembershipDigest{});
}

LineageStore::LineageStore(const Limits& limits) : limits_(limits) {}

bool LineageStore::contains(const LineageId& lineage) const {
  return slots_.find(lineage) != slots_.end();
}

const LineageRecord* LineageStore::latest(const LineageId& lineage) const {
  const auto found = slots_.find(lineage);
  if (found == slots_.end()) {
    return nullptr;
  }
  return &records_[found->second.latest];
}

const LineageRecord* LineageStore::earliest(const LineageId& lineage) const {
  const auto found = slots_.find(lineage);
  if (found == slots_.end()) {
    return nullptr;
  }
  return &records_[found->second.earliest];
}

bool LineageStore::append(const LineageRecord& record) {
  if (records_.size() >= limits_.max_lineage_records) {
    return false;
  }
  if (record.lineage.is_nil() || record.sequence.is_zero()) {
    return false;
  }
  if (!records_.empty() && record.sequence.value() <= highest_sequence_.value()) {
    return false;
  }
  // A retirement is a terminal event on an existing generation, not a new
  // generation of it, so it legitimately closes the pair it names. Every other
  // event must introduce a generation the store has not seen.
  const auto key = std::make_pair(record.lineage, record.generation.value());
  const bool known_generation = generations_.find(key) != generations_.end();
  if (record.event == LineageEventKind::Retire) {
    if (!known_generation) {
      return false;
    }
    for (const LineageRecord& existing : records_) {
      if (existing.lineage != record.lineage) {
        continue;
      }
      if (existing.event == LineageEventKind::Retire) {
        return false;
      }
      break;
    }
  } else if (known_generation) {
    return false;
  }
  if (record.event == LineageEventKind::Genesis || record.event == LineageEventKind::Retire) {
    // A genesis has no parents by definition, and a retirement closes the
    // lineage it names rather than descending from another one.
    if (!record.parent_left.is_nil() || !record.parent_right.is_nil()) {
      return false;
    }
  } else {
    if (record.parent_left.is_nil() || !contains(record.parent_left)) {
      return false;
    }
    if (!record.parent_right.is_nil() && !contains(record.parent_right)) {
      return false;
    }
    if (record.event == LineageEventKind::Merge && record.parent_right.is_nil()) {
      return false;
    }
  }
  if (record.members.empty() || record.members.size() > limits_.max_components) {
    return false;
  }
  for (std::size_t index = 0; index < record.members.size(); ++index) {
    if (record.members[index].is_nil()) {
      return false;
    }
    if (index > 0 && !(record.members[index - 1] < record.members[index])) {
      return false;
    }
  }
  if (!(compute_membership_digest(record.members) == record.membership)) {
    return false;
  }
  const auto total = checked_size_add(retained_member_entries_, record.members.size());
  if (!total.has_value() || *total > limits_.max_lineage_member_entries) {
    return false;
  }

  LineageRecord stored = record;
  stored.digest = compute_lineage_digest(stored);
  const std::size_t index = records_.size();
  records_.push_back(std::move(stored));
  const auto slot = slots_.find(record.lineage);
  if (slot == slots_.end()) {
    slots_.emplace(record.lineage, LineageSlot{index, index});
  } else {
    slot->second.latest = index;
  }
  generations_.insert(key);
  highest_sequence_ = record.sequence;
  retained_member_entries_ = *total;
  return true;
}

std::vector<LineageId> LineageStore::live_lineages() const {
  std::vector<LineageId> superseded;
  for (const LineageRecord& record : records_) {
    if (!record.parent_left.is_nil()) {
      superseded.push_back(record.parent_left);
    }
    if (!record.parent_right.is_nil()) {
      superseded.push_back(record.parent_right);
    }
  }
  std::sort(superseded.begin(), superseded.end());
  superseded.erase(std::unique(superseded.begin(), superseded.end()), superseded.end());

  std::vector<LineageId> result;
  for (const LineageRecord& record : records_) {
    const LineageRecord* newest = latest(record.lineage);
    if (newest == nullptr || newest->event == LineageEventKind::Retire) {
      continue;
    }
    if (std::binary_search(superseded.begin(), superseded.end(), record.lineage)) {
      continue;
    }
    result.push_back(record.lineage);
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

namespace {

struct AncestryWalk {
  std::vector<LineageId> visited;
  bool truncated = false;
};

void collect_ancestors(const LineageStore& store, const LineageId& start,
                       const Limits& limits, AncestryWalk& walk) {
  std::vector<std::pair<LineageId, std::size_t>> frontier;
  const LineageRecord* first = store.earliest(start);
  if (first == nullptr) {
    walk.truncated = true;
    return;
  }
  if (!first->parent_left.is_nil()) {
    frontier.emplace_back(first->parent_left, 1);
  }
  if (!first->parent_right.is_nil()) {
    frontier.emplace_back(first->parent_right, 1);
  }
  // The frontier is expanded in canonical lineage order so that hitting the
  // node budget always truncates at the same, reproducible point.
  std::sort(frontier.begin(), frontier.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

  while (!frontier.empty()) {
    const auto current = frontier.front();
    frontier.erase(frontier.begin());
    if (std::find(walk.visited.begin(), walk.visited.end(), current.first) != walk.visited.end()) {
      continue;
    }
    if (walk.visited.size() >= limits.max_lineage_walk_nodes ||
        current.second > limits.max_lineage_walk_depth) {
      walk.truncated = true;
      return;
    }
    walk.visited.push_back(current.first);
    const LineageRecord* record = store.earliest(current.first);
    if (record == nullptr) {
      continue;
    }
    if (!record->parent_left.is_nil()) {
      frontier.emplace_back(record->parent_left, current.second + 1);
    }
    if (!record->parent_right.is_nil()) {
      frontier.emplace_back(record->parent_right, current.second + 1);
    }
    std::sort(frontier.begin(), frontier.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  }
}

[[nodiscard]] bool list_contains(const std::vector<LineageId>& values, const LineageId& key) {
  return std::find(values.begin(), values.end(), key) != values.end();
}

}  // namespace

LineageRelation LineageStore::relate(const LineageId& left, const LineageId& right,
                                     bool& limit_reached) const {
  limit_reached = false;
  if (left == right) {
    return LineageRelation::Identical;
  }
  if (!contains(left) || !contains(right)) {
    // An identity that was never recorded cannot be related to anything. That
    // is an unresolved question, not a negative answer.
    return LineageRelation::Indeterminate;
  }
  AncestryWalk left_walk;
  AncestryWalk right_walk;
  collect_ancestors(*this, left, limits_, left_walk);
  collect_ancestors(*this, right, limits_, right_walk);

  if (list_contains(right_walk.visited, left)) {
    return LineageRelation::AncestorOf;
  }
  if (list_contains(left_walk.visited, right)) {
    return LineageRelation::DescendantOf;
  }
  const bool common = has_common_ancestor(left, right);
  if (left_walk.truncated || right_walk.truncated) {
    if (common) {
      return LineageRelation::Divergent;
    }
    limit_reached = true;
    return LineageRelation::Indeterminate;
  }
  if (common) {
    return LineageRelation::Divergent;
  }
  return LineageRelation::Unrelated;
}

bool LineageStore::has_common_ancestor(const LineageId& left, const LineageId& right) const {
  AncestryWalk left_walk;
  AncestryWalk right_walk;
  collect_ancestors(*this, left, limits_, left_walk);
  collect_ancestors(*this, right, limits_, right_walk);
  for (const LineageId& value : left_walk.visited) {
    if (list_contains(right_walk.visited, value)) {
      return true;
    }
  }
  return false;
}

void LineageStore::clear() noexcept {
  records_.clear();
  slots_.clear();
  generations_.clear();
  highest_sequence_ = LineageSequence{};
  retained_member_entries_ = 0;
}

}  // namespace fabric_partition_manager
