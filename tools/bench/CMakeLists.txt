# SPDX-License-Identifier: GPL-3.0-or-later
# Development benchmarks (not run by CTest).
add_executable(bench_blocker bench_blocker.c)
target_link_libraries(bench_blocker PRIVATE hk_net)
hk_copy_runtime(bench_blocker)
