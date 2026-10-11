# tests/fuzz/: one directory per module (tests/fuzz/<module>/), each calling
# de_add_fuzz_target() (cmake/DeTesting.cmake) for its parsers. Directories are added
# automatically in sorted order; never edit this file to add a module.
de_add_subdirectories()
