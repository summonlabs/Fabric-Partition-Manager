// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Internal blocking socket wrapper. Never installed.
//
// Shutdown design: a session thread blocked in recv() is released by
// shutdown_both(), which the owning server calls before joining. The listening
// socket is never used after the acceptor observes the stop flag, and it is
// closed only after the acceptor has been joined. No thread closes a descriptor
// another thread may still be using.
#ifndef FABRIC_PARTITION_MANAGER_SRC_SOCKET_HPP
#define FABRIC_PARTITION_MANAGER_SRC_SOCKET_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "platform.hpp"

namespace fabric_partition_manager::detail {

// Initialises and tears down the process-wide socket subsystem.
class SocketRuntime {
 public:
  SocketRuntime() noexcept;
  ~SocketRuntime();
  SocketRuntime(const SocketRuntime&) = delete;
  SocketRuntime& operator=(const SocketRuntime&) = delete;

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] static const char* last_error_text() noexcept;

 private:
  bool ok_ = false;
};

class Socket {
 public:
  static constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(~0ull);

  Socket() noexcept = default;
  ~Socket();
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalid; }

  [[nodiscard]] bool listen_on(const std::string& address, std::uint16_t port,
                               std::uint16_t& bound_port, std::string& error);
  [[nodiscard]] bool accept_from(Socket& out, std::string& error);
  [[nodiscard]] bool connect_to(const std::string& address, std::uint16_t port,
                                std::string& error);
  // Waits until the socket is readable or the timeout elapses. Returns true when
  // readable, false on timeout.
  [[nodiscard]] bool wait_readable(std::uint64_t timeout_millis, std::string& error);

  [[nodiscard]] bool send_all(const std::byte* data, std::size_t size, std::string& error);
  // Returns the number of bytes read, 0 when the peer closed, or -1 on error.
  [[nodiscard]] int recv_some(std::byte* data, std::size_t size, std::string& error);

  void shutdown_both() noexcept;
  void close() noexcept;

 private:
  std::uintptr_t handle_ = kInvalid;
};

// Fills a session handle with operating-system entropy. This provides
// uniqueness, not secrecy: the protocol is explicitly unauthenticated.
void fill_session_handle(std::array<std::byte, 16>& handle) noexcept;

// Initialises the process-wide socket subsystem on first use. Every socket
// entry point calls this, so a caller never has to sequence platform startup
// before using the library.
void ensure_socket_runtime() noexcept;

}  // namespace fabric_partition_manager::detail

#endif  // FABRIC_PARTITION_MANAGER_SRC_SOCKET_HPP
