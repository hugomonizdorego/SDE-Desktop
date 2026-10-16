# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(sde-tests test_main.cpp)
target_link_libraries(sde-tests PRIVATE sde-common)
add_test(NAME sde-tests COMMAND sde-tests)

# Test helper: a StatusNotifierItem with a menu (not installed).
add_executable(sde-sni-demo sni_demo.cpp)
target_link_libraries(sde-sni-demo PRIVATE sde-common)
