# tests/unit/: one directory per module (tests/unit/<module>/), each calling de_add_test()
# (cmake/DeTesting.cmake). Directories are added automatically in sorted order; never edit this
# file to add a module. de_test.h / de_test.c here are the framework (target de_test_framework).
de_add_subdirectories()
