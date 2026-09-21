// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Authority policy.
//
// Reachability is not authority. A component set that is internally connected
// has proven only that it is connected. Whether it may act - and how much of
// its capability it may exercise - is decided by this policy, evaluated against
// the evidence generations the decision is bound to.
//
// The policy object is the only place where "how much authority is retained"
// is expressed. Nothing in the partition computation can widen it.
#ifndef FABRIC_PARTITION_MANAGER_POLICY_HPP
#define FABRIC_PARTITION_MANAGER_POLICY_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fabric_partition_manager/limits.hpp"
#include "fabric_partition_manager/strong_id.hpp"

namespace fabric_partition_manager {

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

enum class AuthorityCapability : std::uint8_t {
  Observe = 0,
  ServeReads = 1,
  ServeWrites = 2,
  AdmitMutations = 3,
  CommitDurability = 4,
  IssueLeases = 5,
  PublishDecisions = 6,
};

inline constexpr std::size_t authority_capability_count = 7;

[[nodiscard]] std::string_view authority_capability_name(AuthorityCapability capability) noexcept;

// A typed bit set. Capability is not an integer: it cannot be compared with a
// count, added to a weight or accidentally used as an index.
class CapabilitySet {
 public:
  constexpr CapabilitySet() noexcept = default;
  explicit constexpr CapabilitySet(std::uint32_t bits) noexcept : bits_(bits) {}

  [[nodiscard]] static constexpr CapabilitySet of(AuthorityCapability capability) noexcept {
    return CapabilitySet(1u << static_cast<unsigned>(capability));
  }

  [[nodiscard]] static constexpr CapabilitySet all() noexcept {
    return CapabilitySet((1u << authority_capability_count) - 1u);
  }

  [[nodiscard]] static constexpr CapabilitySet none() noexcept { return CapabilitySet(0u); }

  [[nodiscard]] constexpr bool contains(AuthorityCapability capability) const noexcept {
    return (bits_ & (1u << static_cast<unsigned>(capability))) != 0u;
  }

  [[nodiscard]] constexpr bool contains_all(const CapabilitySet& other) const noexcept {
    return (bits_ & other.bits_) == other.bits_;
  }

  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0u; }
  [[nodiscard]] constexpr std::uint32_t bits() const noexcept { return bits_; }

  [[nodiscard]] constexpr std::size_t count() const noexcept {
    std::size_t total = 0;
    for (std::size_t index = 0; index < authority_capability_count; ++index) {
      if ((bits_ & (1u << index)) != 0u) {
        ++total;
      }
    }
    return total;
  }

  [[nodiscard]] constexpr CapabilitySet with(AuthorityCapability capability) const noexcept {
    return CapabilitySet(bits_ | (1u << static_cast<unsigned>(capability)));
  }

  [[nodiscard]] constexpr CapabilitySet with_all(const CapabilitySet& other) const noexcept {
    return CapabilitySet(bits_ | other.bits_);
  }

  [[nodiscard]] constexpr CapabilitySet without(const CapabilitySet& other) const noexcept {
    return CapabilitySet(bits_ & ~other.bits_);
  }

  [[nodiscard]] constexpr CapabilitySet intersect(const CapabilitySet& other) const noexcept {
    return CapabilitySet(bits_ & other.bits_);
  }

  // Canonical rendering: a stable, alphabetically ordered list. Used by
  // explanations and by the deterministic text form of a decision.
  [[nodiscard]] std::string render() const;

  friend constexpr bool operator==(const CapabilitySet&, const CapabilitySet&) noexcept = default;

 private:
  std::uint32_t bits_ = 0;
};

// ---------------------------------------------------------------------------
// Quorum
// ---------------------------------------------------------------------------

struct QuorumRequirement {
  // Minimum number of components the retained set must contain.
  std::uint32_t min_components = 0;
  // Minimum number of components designated as voters.
  std::uint32_t min_voters = 0;
  // Minimum absolute weight.
  std::uint64_t min_weight = 0;
  // Weight of the whole configured authority domain. When
  // require_strict_majority is set, the retained weight must satisfy
  // retained * 2 > domain_weight.
  std::uint64_t domain_weight = 0;
  bool require_strict_majority = false;

  [[nodiscard]] bool satisfied_by(std::uint32_t components, std::uint32_t voters,
                                  std::uint64_t weight) const noexcept;
};

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

struct ComponentWeight {
  ComponentId component;
  std::uint64_t weight = 1;
  bool voter = false;

  friend bool operator==(const ComponentWeight&, const ComponentWeight&) noexcept = default;
};

struct PartitionPolicy {
  PolicyId id;
  PolicyGeneration generation;
  std::uint16_t schema_version = 1;

  // The authority domain. Components absent from this table still belong to the
  // fabric but carry zero weight and are not voters.
  std::vector<ComponentWeight> weights;

  QuorumRequirement full_authority;
  QuorumRequirement degraded_authority;

  // Capabilities retained in each lifecycle state.
  CapabilitySet degraded_capabilities =
      CapabilitySet::of(AuthorityCapability::Observe)
          .with(AuthorityCapability::ServeReads)
          .with(AuthorityCapability::PublishDecisions);
  CapabilitySet readonly_capabilities =
      CapabilitySet::of(AuthorityCapability::Observe).with(AuthorityCapability::ServeReads);

  // Evidence older than this many ticks (relative to the evaluation tick) is
  // STALE. Stale evidence never participates in an affirmative determination.
  std::uint64_t max_evidence_age_ticks = 4096;

  // When true, an edge is only usable if both directions were observed
  // reachable. Default true: a one-way observation is not proven connectivity.
  bool require_symmetric_reachability = true;

  // Fail-closed switches.
  bool require_confirmed_partition_for_write = true;
  bool require_confirmed_partition_for_read = false;
  // When false (default), two or more partitions that each independently
  // satisfy full authority are a split-brain CONFLICT and every one of them is
  // reduced to observe-only.
  bool allow_multiple_authoritative_partitions = false;
  // When true (default), the existence of any UNKNOWN cross-link downgrades the
  // partition to at most degraded authority even if it satisfies the quorum.
  bool unknown_evidence_downgrades = true;

  [[nodiscard]] std::uint64_t weight_of(const ComponentId& component) const noexcept;
  [[nodiscard]] bool is_voter(const ComponentId& component) const noexcept;
  [[nodiscard]] std::uint64_t domain_weight() const noexcept;
  [[nodiscard]] std::uint32_t voter_capacity() const noexcept;

  // Canonicalises the weight table (sorted, unique by component).
  [[nodiscard]] std::optional<PartitionPolicy> canonicalised(const Limits& limits) const;
};

[[nodiscard]] PartitionPolicy default_policy(PolicyGeneration generation) noexcept;

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_POLICY_HPP
