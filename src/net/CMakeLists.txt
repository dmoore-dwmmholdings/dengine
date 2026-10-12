# src/net: module `net` (target de_net), layer gameplay. SPEC §16.
#
# Transport (DENG-51): net_internal.h is the socket interface every backend implements;
# socket.c the portable part, socket_posix.c / socket_win32.c the OS UDP sockets,
# socket_loopback.c the in-process backend, net_address.c the address type.
if(WIN32)
    set(_de_net_os win32)
else()
    set(_de_net_os posix)
endif()
set(_de_net_libs "")
if(WIN32)
    list(APPEND _de_net_libs ws2_32) # Winsock 2 (socket_win32.c)
endif()

de_add_module(
    NAME net
    LAYER gameplay
    SOURCES
        net_internal.h
        net_address.c
        socket.c
        socket_${_de_net_os}.c
        socket_loopback.c
    DEPS platform ecs reflect
    OPTIONAL_DEPS physics
    OPTIONAL_OPTION DE_WITH_NET
    LIBS ${_de_net_libs}
    DETERMINISTIC
)
