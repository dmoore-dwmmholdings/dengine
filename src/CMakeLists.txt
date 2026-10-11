# src/: one directory per engine module (SPEC §3.4). Every child directory with a CMakeLists.txt
# is added in sorted order; group directories (gpu/, world/, gameplay/, services/) do the same
# for their children. Add a module by creating src/<mod>/CMakeLists.txt; never edit this file.
de_add_subdirectories()

# All modules are known now: check DEPS and build the aggregate `dengine` library.
de_finalize_modules()
