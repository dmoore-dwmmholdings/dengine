# Unit tests of the platform module (src/platform/): threads, sync, TLS, atomics, time.
de_add_test(NAME platform SOURCES test_thread.c test_time.c LIBS de_platform)
if(TARGET test_platform)
    # The atomics test uses the internal header src/platform/de_atomic.h.
    target_include_directories(test_platform PRIVATE "${DE_ROOT_DIR}/src")
endif()
