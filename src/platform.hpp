// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Internal platform shim. Never installed.
#ifndef FABRIC_PARTITION_MANAGER_SRC_PLATFORM_HPP
#define FABRIC_PARTITION_MANAGER_SRC_PLATFORM_HPP

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// /analyze reports annotated-argument findings inside the Windows SDK headers
// themselves. The suppression is scoped to these three includes; no first-party
// warning number is affected by it.
#pragma warning(push)
#pragma warning(disable : 6101)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma warning(pop)
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#endif  // FABRIC_PARTITION_MANAGER_SRC_PLATFORM_HPP
