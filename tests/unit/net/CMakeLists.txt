# Unit tests of the net module (src/net/): addresses, OS UDP sockets and the loopback transport
# (DENG-51). One executable, so CTest names are net.<test>. The tests use the internal header
# src/net/net_internal.h and the alloc guard (udp_send_recv_no_alloc). Skipped with the module
# (DE_WITH_NET=OFF).
if(TARGET de_net)
    de_add_test(NAME net SOURCES test_address.c test_socket.c
        test_crypto.c test_connect_token.c test_handshake.c
        LIBS de_net de_test_support)
    if(TARGET test_net)
        target_include_directories(test_net PRIVATE "${DE_ROOT_DIR}/src")
        # Monocypher's own test vectors (tis-ci-vectors.h) for test_crypto.c (DENG-52).
        target_include_directories(test_net SYSTEM PRIVATE
            "${DE_ROOT_DIR}/third_party/monocypher/tests")
        target_link_libraries(test_net PRIVATE de_tp_monocypher)
    endif()
endif()
