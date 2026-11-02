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

# The same public setter is compiled independently in both non-development configurations.
# No engine library/decorator may be linked into these tiny no-op executables.
if(DE_BUILD_TESTS AND TARGET de_net)
    foreach(_sim_config release shipping)
        add_executable(test_net_sim_${_sim_config} test_net_sim_noop.c
            "${DE_ROOT_DIR}/src/net/net_sim.c")
        target_link_libraries(test_net_sim_${_sim_config} PRIVATE de_build_flags)
        # The same deterministic source contract applies when compiled outside de_net.
        target_compile_options(test_net_sim_${_sim_config} PRIVATE ${DE_DETERMINISTIC_FLAGS})
        target_include_directories(test_net_sim_${_sim_config} PRIVATE
            "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/include")
        if(DE_COMPILER_FRONTEND STREQUAL "MSVC")
            target_compile_options(test_net_sim_${_sim_config} PRIVATE /UDE_DEV /UDE_DEBUG /UDE_SHIPPING)
            if(_sim_config STREQUAL "shipping")
                target_compile_options(test_net_sim_${_sim_config} PRIVATE /DDE_SHIPPING=1)
            endif()
        else()
            target_compile_options(test_net_sim_${_sim_config} PRIVATE -UDE_DEV -UDE_DEBUG -UDE_SHIPPING)
            if(_sim_config STREQUAL "shipping")
                target_compile_options(test_net_sim_${_sim_config} PRIVATE -DDE_SHIPPING=1)
            endif()
        endif()
        target_compile_definitions(test_net_sim_${_sim_config} PRIVATE
            DE_STATIC
            EXPECT_SHIPPING=$<IF:$<STREQUAL:${_sim_config},shipping>,1,0>)
        add_test(NAME net_sim_noop_${_sim_config} COMMAND "$<TARGET_FILE:test_net_sim_${_sim_config}>")
        set_tests_properties(net_sim_noop_${_sim_config} PROPERTIES LABELS "unit;net" TIMEOUT 10)
    endforeach()
endif()

# DENG-56: isolated simulator qualification, deterministic injected time and raw heap guard.
# Release/Shipping have no simulator test bodies to discover.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _de_net_test_config)
if(TARGET de_net AND (DE_MULTI_CONFIG OR _de_net_test_config MATCHES "^(DEBUG|DEV)$"))
    de_add_test(NAME net_sim SOURCES test_net_sim.c LIBS de_net de_test_support
        CONFIGURATIONS Debug Dev)
    if(TARGET test_net_sim)
        target_include_directories(test_net_sim PRIVATE "${DE_ROOT_DIR}/src")
        if(DE_MULTI_CONFIG AND CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
            set_target_properties(test_net_sim PROPERTIES
                EXCLUDE_FROM_ALL "$<NOT:$<CONFIG:Debug,Dev>>")
        elseif(DE_MULTI_CONFIG AND CMAKE_GENERATOR MATCHES "^Visual Studio")
            set_target_properties(test_net_sim PROPERTIES
                EXCLUDE_FROM_DEFAULT_BUILD_RELEASE TRUE
                EXCLUDE_FROM_DEFAULT_BUILD_SHIPPING TRUE)
        endif()
    endif()
endif()

# DENG-53: authenticated delivery, selective acknowledgements and resend qualification.
if(TARGET de_net)
    de_add_test(NAME net_channels SOURCES test_packet.c test_ack.c test_channels.c
        LIBS de_net de_test_support)
    if(TARGET test_net_channels)
        target_include_directories(test_net_channels PRIVATE "${DE_ROOT_DIR}/src")
    endif()
endif()

# DENG-55: adaptive rate control and allocation-free rolling telemetry.
if(TARGET de_net)
    de_add_test(NAME net_congestion SOURCES test_congestion.c test_net_stats.c
        LIBS de_net de_test_support)
    if(TARGET test_net_congestion)
        target_include_directories(test_net_congestion PRIVATE "${DE_ROOT_DIR}/src")
    endif()
endif()

# DENG-54: strict fragment codec, bounded pool and message lifetime.
if(TARGET de_net)
    de_add_test(NAME net_fragment SOURCES test_fragment.c LIBS de_net de_test_support)
    if(TARGET test_net_fragment)
        target_include_directories(test_net_fragment PRIVATE "${DE_ROOT_DIR}/src")
    endif()
endif()

# DENG-54: allocation-free MTU search, retries, fallback and periodic confirmation.
if(TARGET de_net)
    de_add_test(NAME net_mtu SOURCES test_mtu.c LIBS de_net de_test_support)
    if(TARGET test_net_mtu)
        target_include_directories(test_net_mtu PRIVATE "${DE_ROOT_DIR}/src")
    endif()
endif()

# DENG-719: verify real configure/build/discovery behavior, including multi-config generators.
if(DE_BUILD_TESTS AND TARGET de_net)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME net_sim_configurations
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_sim_test_configs.py"
            "${DE_ROOT_DIR}")
    set_tests_properties(net_sim_configurations PROPERTIES LABELS "check;net" TIMEOUT 120)
endif()
