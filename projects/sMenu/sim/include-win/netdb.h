// Folded into <sys/socket.h> for the Windows build; Winsock declares all of
// these in winsock2.h/ws2tcpip.h. Present only so the widget header, which
// includes the POSIX names, compiles unchanged.
#pragma once
#include <sys/socket.h>
