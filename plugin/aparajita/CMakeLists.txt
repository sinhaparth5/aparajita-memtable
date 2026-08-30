# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# Deliberately does not set aparajita_SOURCES. RocksDB's CMakeLists appends both
# the sources named here and the ones it parses out of aparajita.mk, so declaring
# them in both places compiles the file twice and fails the link on duplicate
# symbols. The .mk is the single declaration; this file supplies the include path
# for the header-only core.
#
# CMAKE_CURRENT_LIST_FILE is resolved through symlinks on purpose: the supported
# way to build this is to link the repository's plugin directory into a RocksDB
# checkout, and the include path has to point back at the real repository rather
# than at the RocksDB tree the link lives in.
get_filename_component(_aparajita_list_file "${CMAKE_CURRENT_LIST_FILE}" REALPATH)
get_filename_component(_aparajita_plugin_dir "${_aparajita_list_file}" DIRECTORY)
get_filename_component(_aparajita_root "${_aparajita_plugin_dir}/../.." ABSOLUTE)

if(NOT EXISTS "${_aparajita_root}/include/aparajita/memtable.hpp")
  message(FATAL_ERROR
    "aparajita headers not found under ${_aparajita_root}/include. "
    "Link the repository's plugin/aparajita directory into plugin/ rather than copying it.")
endif()

# PARENT_SCOPE is required, not stylistic: RocksDB pulls this file in with
# add_subdirectory(), which gives it its own variable scope, and reads
# ${plugin}_INCLUDE_PATHS back in the parent. Without the qualifier the include
# path is set in a scope nobody reads and the plugin fails to find its headers.
set(aparajita_INCLUDE_PATHS "${_aparajita_root}/include" PARENT_SCOPE)

# Tests are declared here rather than in the .mk: RocksDB's cmake takes
# ${plugin}_TESTS from this file only, and turns each one into a gtest target
# named after the file.
set(aparajita_TESTS aparajita_memtable_test.cc PARENT_SCOPE)
