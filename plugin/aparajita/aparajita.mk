# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# RocksDB reads this file for both the make and the cmake build. Note that the
# cmake path parses SOURCES out of here *and* reads any ${plugin}_SOURCES set by
# CMakeLists.txt, appending both, so the sources are declared here only.
aparajita_SOURCES = aparajita_memtable.cc
aparajita_HEADERS = aparajita_memtable.h
aparajita_FUNC = register_AparajitaMemTable
