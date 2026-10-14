The examples/ rule (DENG-327): files under examples/ include only public headers. Own table:
two modules and two [[example_internal]] entries, one of them for a file that does not exist.
examples/demo/main.c includes public headers, its own header, the C library and a third-party
style header (allowed), plus an unknown public header, an internal header through the include
root and one through a relative path (errors). glue.c includes an internal header and is exempt.
Expected: exit 1, 4 violations.
