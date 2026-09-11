# Isolated probe for realm_mesh_select_net_backend: resolves one
# (platform, requested) pair and records the outcome, or exits non-zero with the
# function's FATAL_ERROR. Driven by tests/cmake/net_backend_test.cmake.
cmake_minimum_required(VERSION 3.20)

include("${REALM_MESH_PROBE_MODULE}")

realm_mesh_select_net_backend(
    "${REALM_MESH_PROBE_SYSTEM}"
    "${REALM_MESH_PROBE_REQUESTED}"
    REALM_MESH_PROBE_BACKEND
)

file(WRITE "${REALM_MESH_PROBE_RESULT}" "${REALM_MESH_PROBE_BACKEND}\n")
