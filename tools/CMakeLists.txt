# Do NOT call project() here; top-level CMakeLists already did that.

# Tools share the core library's public includes; nothing extra needed
# unless tools have their own local headers.

# usrl-ctl tool
add_executable(usrl-ctl
    usrl_ctl.c
)
target_link_libraries(usrl-ctl PRIVATE usrl_core)

# usrl-top tool
add_executable(usrl-top
    usrl_top.c
)

target_link_libraries(usrl-top PRIVATE usrl_core)

# core_loader tool
add_executable(core_loader
    core_loader.c
)
target_link_libraries(core_loader PRIVATE usrl_core)

configure_file(
    ${CMAKE_SOURCE_DIR}/config.json
    ${CMAKE_CURRENT_BINARY_DIR}/config.json
    COPYONLY
)