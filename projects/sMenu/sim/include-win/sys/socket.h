// BSD sockets -> Winsock, for the Windows cross-build only.
//
// The chat widget in IWidget.hpp is a plain BSD-socket client written straight
// into the header. Winsock spells almost all of it identically, so this maps
// the handful of names that differ rather than touching the widget.
//
// This directory is on the include path for the Windows build ONLY, so the
// Linux build keeps the real system headers.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

// Winsock spells an IPv4 address as unsigned long and has no in_addr_t.
#ifndef _IN_ADDR_T_DECLARED
typedef unsigned long in_addr_t;
#define _IN_ADDR_T_DECLARED
#endif

// Winsock has no socklen_t before some SDKs, and uses int throughout.
#ifndef _SOCKLEN_T_DECLARED
typedef int socklen_t;
#define _SOCKLEN_T_DECLARED
#endif

// Winsock sockets are closed with closesocket(), not close(). Safe to map
// wholesale here: close() appears in these sources only on sockets (file
// handles go through fclose, and directories through closedir).
#ifndef close
#define close(s) closesocket(s)
#endif

// Likewise ioctl() on a socket is ioctlsocket(); FIONBIO already comes from
// winsock2.h with the same meaning.
#ifndef ioctl
#define ioctl(s, cmd, argp) ioctlsocket((s), (cmd), (u_long *)(argp))
#endif
