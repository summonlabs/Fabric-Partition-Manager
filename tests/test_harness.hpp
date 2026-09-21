// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Minimal test harness. No test in this repository is given a timeout: a
// hanging test is a defect and must surface as a hang.
#ifndef FABRIC_PARTITION_MANAGER_TESTS_TEST_HARNESS_HPP
#define FABRIC_PARTITION_MANAGER_TESTS_TEST_HARNESS_HPP

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace fpm_test {

class Failure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct TestCase {
  const char* suite;
  const char* name;
  std::function<void()> body;
};

[[nodiscard]] std::vector<TestCase>& registry();

class Registrar {
 public:
  Registrar(const char* suite, const char* name, std::function<void()> body);
};

void fail(const char* file, int line, const std::string& expression, const std::string& detail);

[[nodiscard]] int run_all(const std::string& filter, const std::string& skip);

// A private directory that is removed when the object is destroyed.
class TempDirectory {
 public:
  TempDirectory();
  ~TempDirectory();
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;
  TempDirectory(TempDirectory&&) = delete;
  TempDirectory& operator=(TempDirectory&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path file(const std::string& name) const;

 private:
  std::filesystem::path path_;
};

struct ProcessResult {
  bool started = false;
  int exit_code = -1;
  std::string output;
  std::string error;
};

// Runs a process to completion, capturing its combined output. Standard output
// and standard error are redirected to a file rather than a pipe, so the test
// binary never depends on pipe semantics.
[[nodiscard]] ProcessResult run_process(const std::vector<std::string>& arguments,
                                        const std::filesystem::path& working_directory);

// A process that can be hard-killed at a chosen point. Threads are not a
// substitute for this.
class BackgroundProcess {
 public:
  BackgroundProcess() = default;
  ~BackgroundProcess();
  BackgroundProcess(const BackgroundProcess&) = delete;
  BackgroundProcess& operator=(const BackgroundProcess&) = delete;

  [[nodiscard]] bool start(const std::vector<std::string>& arguments,
                           const std::filesystem::path& working_directory);
  [[nodiscard]] bool running();
  // Immediately and unconditionally terminates the process (SIGKILL semantics).
  [[nodiscard]] bool hard_kill();
  // Waits for natural termination. Returns the exit code, or -1 on failure.
  [[nodiscard]] int wait();
  [[nodiscard]] std::string output() const;
  [[nodiscard]] std::uint64_t identifier() const noexcept { return identifier_; }

 private:
  void close_handles();

  void* process_ = nullptr;
  void* thread_ = nullptr;
  std::uint64_t identifier_ = 0;
  bool retained_ = false;
  std::filesystem::path output_path_;
};

// Reads a whole file as text. Returns an empty string when the file is absent.
[[nodiscard]] std::string read_text_file(const std::filesystem::path& path);

// Writes raw bytes, replacing the file.
[[nodiscard]] bool write_binary_file(const std::filesystem::path& path,
                                     const std::vector<std::byte>& bytes);

// Reads raw bytes.
[[nodiscard]] std::vector<std::byte> read_binary_file(const std::filesystem::path& path);

// A deterministic latch used to make concurrency tests reproducible rather than
// dependent on scheduling luck.
class Latch {
 public:
  explicit Latch(std::size_t target) : target_(target) {}
  void arrive_and_wait();
  void arrive();

 private:
  std::size_t target_;
  std::size_t arrived_ = 0;
  std::mutex mutex_;
  std::condition_variable condition_;
};

}  // namespace fpm_test

#define FPM_TEST(suite, name)                                                          \
  static void suite##_##name##_body();                                                 \
  static const ::fpm_test::Registrar suite##_##name##_registrar(#suite, #name,         \
                                                               suite##_##name##_body); \
  static void suite##_##name##_body()

#define FPM_CHECK(expression)                                                     \
  do {                                                                            \
    if (!(expression)) {                                                          \
      ::fpm_test::fail(__FILE__, __LINE__, #expression, std::string());           \
    }                                                                             \
  } while (0)

#define FPM_CHECK_MSG(expression, detail)                                         \
  do {                                                                            \
    if (!(expression)) {                                                          \
      ::fpm_test::fail(__FILE__, __LINE__, #expression, (detail));                \
    }                                                                             \
  } while (0)

#define FPM_EQ(lhs, rhs)                                                          \
  do {                                                                            \
    if (!((lhs) == (rhs))) {                                                      \
      ::fpm_test::fail(__FILE__, __LINE__, #lhs " == " #rhs, std::string());      \
    }                                                                             \
  } while (0)

#define FPM_NE(lhs, rhs)                                                          \
  do {                                                                            \
    if ((lhs) == (rhs)) {                                                         \
      ::fpm_test::fail(__FILE__, __LINE__, #lhs " != " #rhs, std::string());      \
    }                                                                             \
  } while (0)

#endif  // FABRIC_PARTITION_MANAGER_TESTS_TEST_HARNESS_HPP
