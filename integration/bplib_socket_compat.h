#ifndef BPLIB_SOCKET_COMPAT_H
#define BPLIB_SOCKET_COMPAT_H

/* Keep the host CLA independent of the native socket ABI. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <BaseTsd.h>
typedef SSIZE_T bplib_socket_count_t;
typedef SOCKET bplib_socket_t;
#define BPLIB_INVALID_SOCKET INVALID_SOCKET
#define BPLIB_SOCKET_ERROR SOCKET_ERROR
#define bplib_socket_close(s) closesocket((SOCKET)(s))
#define bplib_socket_last_error() WSAGetLastError()
#define bplib_socket_would_block(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
#define bplib_socket_interrupted(e) ((e) == WSAEINTR)
#else
#include <errno.h>
#include <sys/types.h>
typedef ssize_t bplib_socket_count_t;
typedef int bplib_socket_t;
#define BPLIB_INVALID_SOCKET (-1)
#define BPLIB_SOCKET_ERROR (-1)
#define bplib_socket_close(s) close((s))
#define bplib_socket_last_error() errno
#define bplib_socket_would_block(e) ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINPROGRESS)
#define bplib_socket_interrupted(e) ((e) == EINTR)
#endif

#endif
