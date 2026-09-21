// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "test_harness.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>
#include <thread>

#include "fabric_partition_manager/strong_id.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fpm_test {
namespace {

std::string quote_argument(const std::string& text) {
  std::string result = "\"";
  for (const char value : text) {
    if (value == '\\') {
      result += "\\\\";
    } else if (value == '"') {
      result += "\\\"";
    } else {
      result += value;
    }
  }
  result += '"';
  return result;
}

std::filesystem::path unique_suffix_path(const std::filesystem::path& base,
                                         const std::string& tag) {
  std::array<std::byte, 16> entropy{};
  fabric_partition_manager::detail::fill_random_bytes(entropy.data(), entropy.size());
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string suffix;
  for (const std::byte value : entropy) {
    const auto raw = static_cast<unsigned>(value);
    suffix.push_back(kDigits[(raw >> 4) & 0xFu]);
    suffix.push_back(kDigits[raw & 0xFu]);
  }
  return base / (tag + "_" + suffix);
}

}  // namespace

void fail(const char* file, int line, const std::string& expression, const std::string& detail) {
  std::ostringstream message;
  message << file << ':' << line << ": assertion failed: " << expression;
  if (!detail.empty()) {
    message << " [" << detail << ']';
  }
  throw Failure(message.str());
}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registrar::Registrar(const char* suite, const char* name, std::function<void()> body) {
  registry().push_back(TestCase{suite, name, std::move(body)});
}

int run_all(const std::string& filter, const std::string& skip) {
  std::vector<TestCase> cases = registry();
  std::stable_sort(cases.begin(), cases.end(), [](const TestCase& lhs, const TestCase& rhs) {
    if (std::strcmp(lhs.suite, rhs.suite) != 0) {
      return std::strcmp(lhs.suite, rhs.suite) < 0;
    }
    return std::strcmp(lhs.name, rhs.name) < 0;
  });

  int executed = 0;
  int failures = 0;
  for (const TestCase& test : cases) {
    const std::string full = std::string(test.suite) + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    if (!skip.empty() && full.find(skip) != std::string::npos) {
      continue;
    }
    ++executed;
    try {
      test.body();
      std::cout << "[ PASS ] " << full << std::endl;
    } catch (const Failure& error) {
      ++failures;
      std::cout << "[ FAIL ] " << full << ": " << error.what() << std::endl;
    } catch (const std::exception& error) {
      ++failures;
      std::cout << "[ FAIL ] " << full << ": unexpected exception " << error.what() << std::endl;
    }
  }
  std::cout << "executed=" << executed << " failures=" << failures << std::endl;
  if (executed == 0) {
    std::cout << "[ FAIL ] no test cases were executed" << std::endl;
    return 1;
  }
  return failures;
}

TempDirectory::TempDirectory() {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  path_ = unique_suffix_path(error ? std::filesystem::path(".") : base, "fpm_test");
  std::filesystem::create_directories(path_, error);
}

TempDirectory::~TempDirectory() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::filesystem::path TempDirectory::file(const std::string& name) const {
  return path_ / name;
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

bool write_binary_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  if (!bytes.empty()) {
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  return stream.good();
}

std::vector<std::byte> read_binary_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  std::vector<std::byte> bytes;
  // Heap allocated so the harness does not commit a 64 KiB stack frame.
  std::vector<char> chunk(64 * 1024, 0);
  while (stream) {
    stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize read = stream.gcount();
    for (std::streamsize index = 0; index < read; ++index) {
      bytes.push_back(static_cast<std::byte>(
          static_cast<unsigned char>(chunk[static_cast<std::size_t>(index)])));
    }
  }
  return bytes;
}

void Latch::arrive_and_wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  ++arrived_;
  if (arrived_ >= target_) {
    condition_.notify_all();
    return;
  }
  condition_.wait(lock, [this]() { return arrived_ >= target_; });
}

void Latch::arrive() {
  std::unique_lock<std::mutex> lock(mutex_);
  ++arrived_;
  if (arrived_ >= target_) {
    condition_.notify_all();
  }
}

#if defined(_WIN32)
namespace {

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size);
  return result;
}

}  // namespace
#endif

ProcessResult run_process(const std::vector<std::string>& arguments,
                          const std::filesystem::path& working_directory) {
  ProcessResult result;
  if (arguments.empty()) {
    result.error = "no executable was supplied";
    return result;
  }
  const std::filesystem::path output_path = unique_suffix_path(working_directory, "fpm_out");
  std::string command_line;
  for (const std::string& argument : arguments) {
    if (!command_line.empty()) {
      command_line.push_back(' ');
    }
    command_line += quote_argument(argument);
  }
#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE output = ::CreateFileW(output_path.wstring().c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    result.error = "the capture file could not be created";
    return result;
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION information{};
  std::wstring mutable_command = widen(command_line);
  const std::wstring directory = working_directory.wstring();
  const BOOL created =
      ::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, directory.c_str(), &startup, &information);
  ::CloseHandle(output);
  if (!created) {
    result.error = "CreateProcess failed";
    return result;
  }
  result.started = true;
  ::WaitForSingleObject(information.hProcess, INFINITE);
  DWORD exit_code = 0;
  ::GetExitCodeProcess(information.hProcess, &exit_code);
  result.exit_code = static_cast<int>(exit_code);
  ::CloseHandle(information.hThread);
  ::CloseHandle(information.hProcess);
#else
  const std::string output_text = output_path.string();
  const pid_t child = ::fork();
  if (child < 0) {
    result.error = "fork failed";
    return result;
  }
  if (child == 0) {
    const int descriptor =
        ::open(output_text.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor >= 0) {
      ::dup2(descriptor, STDOUT_FILENO);
      ::dup2(descriptor, STDERR_FILENO);
      ::close(descriptor);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    if (::chdir(working_directory.string().c_str()) != 0) {
      ::_exit(127);
    }
    ::execv(argv[0], argv.data());
    ::_exit(127);
  }
  result.started = true;
  int status = 0;
  ::waitpid(child, &status, 0);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  result.output = read_text_file(output_path);
  std::error_code error;
  std::filesystem::remove(output_path, error);
  return result;
}

BackgroundProcess::~BackgroundProcess() {
  if (process_ != nullptr) {
    (void)hard_kill();
    (void)wait();
  }
}

bool BackgroundProcess::start(const std::vector<std::string>& arguments,
                              const std::filesystem::path& working_directory) {
  if (arguments.empty() || process_ != nullptr) {
    return false;
  }
  output_path_ = unique_suffix_path(working_directory, "fpm_bg");
  std::string command_line;
  for (const std::string& argument : arguments) {
    if (!command_line.empty()) {
      command_line.push_back(' ');
    }
    command_line += quote_argument(argument);
  }
#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE output = ::CreateFileW(output_path_.wstring().c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    return false;
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION information{};
  std::wstring mutable_command = widen(command_line);
  const std::wstring directory = working_directory.wstring();
  const BOOL created =
      ::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, directory.c_str(), &startup, &information);
  ::CloseHandle(output);
  if (!created) {
    return false;
  }
  ::CloseHandle(information.hThread);
  process_ = information.hProcess;
  identifier_ = static_cast<std::uint64_t>(information.dwProcessId);
  return true;
#else
  const std::string output_text = output_path_.string();
  const pid_t child = ::fork();
  if (child < 0) {
    return false;
  }
  if (child == 0) {
    const int descriptor = ::open(output_text.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor >= 0) {
      ::dup2(descriptor, STDOUT_FILENO);
      ::dup2(descriptor, STDERR_FILENO);
      ::close(descriptor);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    if (::chdir(working_directory.string().c_str()) != 0) {
      ::_exit(127);
    }
    ::execv(argv[0], argv.data());
    ::_exit(127);
  }
  process_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(child));
  identifier_ = static_cast<std::uint64_t>(child);
  return true;
#endif
}

bool BackgroundProcess::running() {
  if (process_ == nullptr) {
    return false;
  }
#if defined(_WIN32)
  DWORD code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(process_), &code) == 0) {
    return false;
  }
  return code == STILL_ACTIVE;
#else
  const pid_t child = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  int status = 0;
  const pid_t outcome = ::waitpid(child, &status, WNOHANG);
  return outcome == 0;
#endif
}

bool BackgroundProcess::hard_kill() {
  if (process_ == nullptr) {
    return false;
  }
#if defined(_WIN32)
  const BOOL killed = ::TerminateProcess(static_cast<HANDLE>(process_), 137);
  return killed != 0;
#else
  const pid_t child = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  return ::kill(child, SIGKILL) == 0;
#endif
}

int BackgroundProcess::wait() {
  if (process_ == nullptr) {
    return -1;
  }
#if defined(_WIN32)
  ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  close_handles();
  return static_cast<int>(code);
#else
  const pid_t child = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  int status = 0;
  ::waitpid(child, &status, 0);
  close_handles();
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::string BackgroundProcess::output() const { return read_text_file(output_path_); }

void BackgroundProcess::close_handles() {
  if (process_ != nullptr) {
#if defined(_WIN32)
    ::CloseHandle(static_cast<HANDLE>(process_));
#endif
    process_ = nullptr;
  }
  if (!output_path_.empty()) {
    std::error_code error;
    std::filesystem::remove(output_path_, error);
    output_path_.clear();
  }
}

}  // namespace fpm_test
