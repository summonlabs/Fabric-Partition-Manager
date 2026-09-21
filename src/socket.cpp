// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "socket.hpp"

#include <algorithm>
#include <cstring>

#include "fabric_partition_manager/strong_id.hpp"

namespace fabric_partition_manager::detail {
namespace {

#if defined(_WIN32)
using native_socket = SOCKET;
constexpr native_socket kNativeInvalid = INVALID_SOCKET;

native_socket to_native(std::uintptr_t handle) noexcept {
  return static_cast<native_socket>(handle);
}

std::uintptr_t from_native(native_socket socket) noexcept {
  return static_cast<std::uintptr_t>(socket);
}
#else
using native_socket = int;
constexpr native_socket kNativeInvalid = -1;

native_socket to_native(std::uintptr_t handle) noexcept {
  return static_cast<native_socket>(handle);
}

std::uintptr_t from_native(native_socket socket) noexcept {
  return static_cast<std::uintptr_t>(socket);
}
#endif

void close_native(native_socket socket) noexcept {
#if defined(_WIN32)
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

}  // namespace

const char* SocketRuntime::last_error_text() noexcept {
#if defined(_WIN32)
  static thread_local char buffer[64] = {};
  const int code = ::WSAGetLastError();
  std::snprintf(buffer, sizeof(buffer), "winsock error %d", code);
  return buffer;
#else
  return std::strerror(errno);
#endif
}

SocketRuntime::SocketRuntime() noexcept {
#if defined(_WIN32)
  WSADATA data{};
  ok_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  ok_ = true;
#endif
}

SocketRuntime::~SocketRuntime() {
#if defined(_WIN32)
  if (ok_) {
    ::WSACleanup();
  }
#endif
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalid; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalid;
  }
  return *this;
}

bool Socket::listen_on(const std::string& address, std::uint16_t port, std::uint16_t& bound_port,
                       std::string& error) {
  ensure_socket_runtime();
  close();
  native_socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kNativeInvalid) {
    error = SocketRuntime::last_error_text();
    return false;
  }
  int reuse = 1;
  ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               static_cast<int>(sizeof(reuse)));

  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = ::htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
    error = "the bind address is not a valid IPv4 literal";
    close_native(socket);
    return false;
  }
  if (::bind(socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
    error = SocketRuntime::last_error_text();
    close_native(socket);
    return false;
  }
  if (::listen(socket, static_cast<int>(SOMAXCONN)) != 0) {
    error = SocketRuntime::last_error_text();
    close_native(socket);
    return false;
  }
  sockaddr_in actual{};
#if defined(_WIN32)
  int actual_length = static_cast<int>(sizeof(actual));
#else
  socklen_t actual_length = static_cast<socklen_t>(sizeof(actual));
#endif
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&actual), &actual_length) != 0) {
    error = SocketRuntime::last_error_text();
    close_native(socket);
    return false;
  }
  bound_port = ::ntohs(actual.sin_port);
  handle_ = from_native(socket);
  return true;
}

bool Socket::accept_from(Socket& out, std::string& error) {
  if (!valid()) {
    error = "the listening socket is closed";
    return false;
  }
  sockaddr_in peer{};
#if defined(_WIN32)
  int peer_length = static_cast<int>(sizeof(peer));
#else
  socklen_t peer_length = static_cast<socklen_t>(sizeof(peer));
#endif
  const native_socket accepted =
      ::accept(to_native(handle_), reinterpret_cast<sockaddr*>(&peer), &peer_length);
  if (accepted == kNativeInvalid) {
    error = SocketRuntime::last_error_text();
    return false;
  }
  out.close();
  out.handle_ = from_native(accepted);
  return true;
}

bool Socket::connect_to(const std::string& address, std::uint16_t port, std::string& error) {
  ensure_socket_runtime();
  close();
  native_socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kNativeInvalid) {
    error = SocketRuntime::last_error_text();
    return false;
  }
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = ::htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
    error = "the connect address is not a valid IPv4 literal";
    close_native(socket);
    return false;
  }
  if (::connect(socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
    error = SocketRuntime::last_error_text();
    close_native(socket);
    return false;
  }
  handle_ = from_native(socket);
  return true;
}

bool Socket::wait_readable(std::uint64_t timeout_millis, std::string& error) {
  if (!valid()) {
    error = "the socket is closed";
    return false;
  }
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(to_native(handle_), &readable);
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(timeout_millis / 1000);
  timeout.tv_usec = static_cast<long>((timeout_millis % 1000) * 1000);
#if defined(_WIN32)
  const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
  const int ready = ::select(static_cast<int>(handle_) + 1, &readable, nullptr, nullptr, &timeout);
#endif
  if (ready < 0) {
    error = SocketRuntime::last_error_text();
    return false;
  }
  return ready > 0;
}

bool Socket::send_all(const std::byte* data, std::size_t size, std::string& error) {
  if (!valid()) {
    error = "the socket is closed";
    return false;
  }
  std::size_t sent = 0;
  while (sent < size) {
    const std::size_t remaining = size - sent;
    const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 1u << 20));
#if defined(MSG_NOSIGNAL)
    const int written = ::send(to_native(handle_), reinterpret_cast<const char*>(data + sent), chunk,
                               MSG_NOSIGNAL);
#else
    const int written =
        ::send(to_native(handle_), reinterpret_cast<const char*>(data + sent), chunk, 0);
#endif
    if (written <= 0) {
      error = SocketRuntime::last_error_text();
      return false;
    }
    sent += static_cast<std::size_t>(written);
  }
  return true;
}

int Socket::recv_some(std::byte* data, std::size_t size, std::string& error) {
  if (!valid()) {
    error = "the socket is closed";
    return -1;
  }
  const int chunk = static_cast<int>(std::min<std::size_t>(size, 1u << 20));
  const int read = ::recv(to_native(handle_), reinterpret_cast<char*>(data), chunk, 0);
  if (read < 0) {
    error = SocketRuntime::last_error_text();
    return -1;
  }
  return read;
}

void Socket::shutdown_both() noexcept {
  if (!valid()) {
    return;
  }
#if defined(_WIN32)
  ::shutdown(to_native(handle_), SD_BOTH);
#else
  ::shutdown(to_native(handle_), SHUT_RDWR);
#endif
}

void ensure_socket_runtime() noexcept {
  static const SocketRuntime runtime;
  (void)runtime;
}

void fill_session_handle(std::array<std::byte, 16>& handle) noexcept {
  std::byte* raw = handle.data();
  fill_random_bytes(raw, handle.size());
}

void Socket::close() noexcept {
  if (!valid()) {
    return;
  }
  close_native(to_native(handle_));
  handle_ = kInvalid;
}

}  // namespace fabric_partition_manager::detail
