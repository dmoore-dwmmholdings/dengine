# src/services/crash: module `crash` (target de_crash), layer host.
# The crash report bundle (SPEC §18.5, DENG-96): the log tail and the cvar dump that a chained
# crash handler writes beside the core report of src/platform/crash_*.c (DENG-251). The app host
# (src/host/host_crash.c) installs it. Shared file: later tasks append their own blocks.
de_add_module(
    NAME crash
    LAYER host
    SOURCES
        bundle_internal.h
        bundle.c
        log_ring.c
        cvar_snapshot.c
    DEPS core platform
)
