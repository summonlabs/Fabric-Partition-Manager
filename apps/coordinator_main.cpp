// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// fabric-partition-coordinator: one coordinator incarnation.
//
// The process owns exactly one PartitionRuntime and exactly one
// PartitionServer. When a durable store directory is named the runtime is
// durable, and a store that cannot be loaded is a fail-closed refusal rather
// than a silent amnesia. The bound port and the readiness of the listener are
// published through the files named on the command line, so a supervisor never
// has to guess either one.
//
// The runtime's logical clock is advanced by the elapsed wall-clock time of
// every loop iteration. Evidence freshness, isolation expiry and revalidation
// therefore behave sensibly over a long run instead of standing still.
//
// Exit codes
//   0  the requested runtime duration elapsed and the coordinator stopped
//   2  the coordinator could not start, could not publish its readiness, or
//      could not advance its logical clock
//
// Standard output
//   coordinator ready epoch=<n> port=<n>
//   coordinator stopped epoch=<n> port=<n> sessions=<n> frames=<n>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "arguments.hpp"
#include "fabric_partition_manager/fabric_partition_manager.hpp"

namespace {

namespace fpm = fabric_partition_manager;

constexpr const char* kUsage =
    "usage: fabric-partition-coordinator [--store=DIR] [--bind=ADDR] [--port=N] "
    "[--accept-port-file=FILE] [--ready-file=FILE] [--run-seconds=N]";

[[nodiscard]] std::vector<std::string> known_options() {
  return {"store", "bind", "port", "accept-port-file", "ready-file", "run-seconds", "help"};
}

// The authority policy this coordinator incarnation installs. A single
// connected fabric of three or more components satisfies the full quorum; two
// such component sets are a split-brain CONFLICT. No weight table is required,
// because an unlisted component carries zero weight and is not a voter.
[[nodiscard]] fpm::PartitionPolicy coordinator_policy() {
  fpm::PartitionPolicy policy =
      fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  policy.id = fpm::PolicyId::from_validated("fabric.partition.coordinator");
  policy.full_authority.min_components = 3;
  policy.full_authority.min_voters = 0;
  policy.full_authority.min_weight = 0;
  policy.degraded_authority.min_components = 1;
  policy.max_evidence_age_ticks = 1'000'000;
  return policy;
}

// Writes text to a file, creating the parent directory when one is named.
[[nodiscard]] bool write_text_file(const std::string& path, const std::string& text,
                                   std::string& error) {
  const std::filesystem::path target(path);
  if (target.has_parent_path() && !target.parent_path().empty()) {
    std::error_code code;
    (void)std::filesystem::create_directories(target.parent_path(), code);
  }
  std::ofstream stream(target, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    error = "the file could not be opened for writing: " + target.string();
    return false;
  }
  stream << text;
  stream.flush();
  const bool healthy = stream.good();
  stream.close();
  if (!healthy) {
    error = "the file could not be written: " + target.string();
    return false;
  }
  return true;
}

[[nodiscard]] bool parse_u16(const fpm_apps::Arguments& arguments, const std::string& name,
                             std::uint16_t& value) {
  if (!arguments.has(name)) {
    return true;
  }
  const std::optional<std::uint64_t> parsed = arguments.get_u64(name);
  if (!parsed.has_value() || *parsed > 65535u) {
    return false;
  }
  value = static_cast<std::uint16_t>(*parsed);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const fpm_apps::Arguments arguments = fpm_apps::parse_arguments(argc, argv);
  std::string unknown;
  if (!fpm_apps::options_known(arguments, known_options(), unknown)) {
    std::cerr << "coordinator failed: unknown option --" << unknown << "\n" << kUsage << "\n";
    return 2;
  }
  if (!arguments.positional.empty()) {
    std::cerr << "coordinator failed: unexpected argument '" << arguments.positional.front() << "'\n"
              << kUsage << "\n";
    return 2;
  }
  if (arguments.has("help")) {
    std::cout << kUsage << "\n";
    return 0;
  }

  std::uint16_t port = 0;
  if (!parse_u16(arguments, "port", port)) {
    std::cerr << "coordinator failed: --port must be an unsigned 16-bit port number\n" << kUsage
              << "\n";
    return 2;
  }
  std::uint64_t run_seconds = 0;
  if (arguments.has("run-seconds")) {
    const std::optional<std::uint64_t> parsed = arguments.get_u64("run-seconds");
    if (!parsed.has_value()) {
      std::cerr << "coordinator failed: --run-seconds must be an unsigned integer\n" << kUsage
                << "\n";
      return 2;
    }
    run_seconds = *parsed;
  }

  fpm::RuntimeOptions runtime_options;
  runtime_options.provenance = fpm::Provenance::from_validated("coordinator");
  runtime_options.limits = fpm::default_limits();
  runtime_options.policy = coordinator_policy();
  runtime_options.durable = arguments.has("store");
  if (runtime_options.durable) {
    runtime_options.store_directory = std::filesystem::path(arguments.get("store"));
  }

  std::unique_ptr<fpm::PartitionRuntime> runtime;
  try {
    runtime = std::make_unique<fpm::PartitionRuntime>(runtime_options);
  } catch (const fpm::PartitionError& error) {
    std::cerr << "coordinator failed: " << error.render() << "\n";
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "coordinator failed: " << error.what() << "\n";
    return 2;
  }

  fpm::ServerOptions server_options;
  server_options.bind_address = arguments.get("bind", "127.0.0.1");
  server_options.port = port;
  server_options.limits = runtime_options.limits;
  server_options.provenance = fpm::Provenance::from_validated("coordinator");

  fpm::PartitionServer server(*runtime, server_options);
  std::string error;
  if (!server.start(error)) {
    std::cerr << "coordinator failed: the listener could not be started: " << error << "\n";
    runtime->close();
    return 2;
  }
  const std::uint16_t bound_port = server.port();

  if (arguments.has("accept-port-file")) {
    std::string file_error;
    if (!write_text_file(arguments.get("accept-port-file"), std::to_string(bound_port) + "\n",
                         file_error)) {
      std::cerr << "coordinator failed: " << file_error << "\n";
      server.stop();
      runtime->close();
      return 2;
    }
  }
  if (arguments.has("ready-file")) {
    std::string file_error;
    if (!write_text_file(arguments.get("ready-file"), "", file_error)) {
      std::cerr << "coordinator failed: " << file_error << "\n";
      server.stop();
      runtime->close();
      return 2;
    }
  }

  std::cout << "coordinator ready epoch=" << runtime->epoch().value() << " port=" << bound_port
            << "\n"
            << std::flush;

  // The coordinator runs until its requested duration elapses. A duration of
  // zero means "until the process is killed", which is the shape a supervisor
  // uses when it owns the lifetime itself.
  const auto started = std::chrono::steady_clock::now();
  auto last_clock_sample = started;
  bool clock_failed = false;
  for (;;) {
    if (run_seconds != 0) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
      if (static_cast<std::uint64_t>(elapsed) >= run_seconds) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto now = std::chrono::steady_clock::now();
    const auto delta_millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_clock_sample).count();
    last_clock_sample = now;
    if (delta_millis <= 0) {
      continue;
    }
    if (!runtime->advance_ticks(static_cast<std::uint64_t>(delta_millis))) {
      clock_failed = true;
      break;
    }
  }

  server.stop();
  const fpm::ServerStats stats = server.stats();
  const std::uint64_t stopped_epoch = runtime->epoch().value();
  const std::uint64_t stopped_tick = runtime->tick();
  runtime->close();
  if (clock_failed) {
    std::cerr << "coordinator failed: the logical clock could not be advanced\n";
    return 2;
  }
  std::cout << "coordinator stopped epoch=" << stopped_epoch << " port=" << bound_port
            << " tick=" << stopped_tick << " sessions=" << stats.accepted_sessions
            << " frames=" << stats.frames_received << "\n"
            << std::flush;
  return 0;
}
