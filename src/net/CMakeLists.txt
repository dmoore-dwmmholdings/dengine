# src/net: module `net` (target de_net), layer gameplay. SPEC §16.
#
# Transport (DENG-51): net_internal.h is the socket interface every backend implements;
# socket.c the portable part, socket_posix.c / socket_win32.c the OS UDP sockets,
# socket_loopback.c the in-process backend, net_address.c the address type.
#
# Handshake and crypto (DENG-52): crypto.c wraps Monocypher (de_tp_monocypher), connect_token.c
# the connect tokens, handshake.c the packet codec, connection.c the client/server state machines.
if(WIN32)
    set(_de_net_os win32)
else()
    set(_de_net_os posix)
endif()
set(_de_net_libs "")
if(WIN32)
    list(APPEND _de_net_libs ws2_32) # Winsock 2 (socket_win32.c)
endif()
list(APPEND _de_net_libs de_tp_monocypher) # packet crypto (crypto.c, DENG-52)

de_add_module(
    NAME net
    LAYER gameplay
    SOURCES
        net_internal.h
        net_address.c
        socket.c
        socket_${_de_net_os}.c
        socket_loopback.c
        crypto.h
        crypto.c
        connect_token.c
        handshake.c
        connection.h
        connection.c
    DEPS platform ecs reflect
    OPTIONAL_DEPS physics
    OPTIONAL_OPTION DE_WITH_NET
    LIBS ${_de_net_libs}
    DETERMINISTIC
)

# DENG-56: preallocated development-only datagram condition simulator; no-op public setter in
# Release/Shipping until S32's session implementation supplies the Dev entry point.
if(TARGET de_net)
    target_sources(de_net PRIVATE net_sim.c net_sim.h)
endif()

# DENG-53: delivery semantics above authenticated PAYLOAD; DENG-54/55 replace the two hooks.
if(TARGET de_net)
    target_sources(de_net PRIVATE packet.c packet.h ack.c rtt.c channel.c channel.h
        mtu.c congestion.c)
endif()

# DENG-55: bounded adaptive send pacing and rolling peer/channel statistics.
if(TARGET de_net)
    target_sources(de_net PRIVATE congestion.h net_stats.c net_stats.h)
endif()
