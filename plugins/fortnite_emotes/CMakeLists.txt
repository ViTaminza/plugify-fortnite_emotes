# fortnite_emotes — Plugify C++ port of the Metamod:Source plugin ref/src/cs2-fortnite_emotes
# (Fortnite emotes & dances: chat commands/menu play an animation on an invisible rig prop cloned
# onto the player, with a third-person camera, hidden weapons and a sound loop).
#
#   fortnite_emotes_core     config.cpp only: settings/emotes/phrases and the name lookup, so the
#                            tests can drive it without any engine (test/fe_core_tests.cpp)
#   plugify-fortnite_emotes  the plugin shared library (entry "bin/fortnite_emotes")

set(FORTNITE_EMOTES_VERSION "2.0.0")

# --- core (config parsing + name lookup) --------------------------------------
add_library(fortnite_emotes_core STATIC src/config.cpp)
target_include_directories(fortnite_emotes_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(fortnite_emotes_core PUBLIC pisexcompat)
target_compile_options(fortnite_emotes_core PRIVATE -Wall -Wextra -Werror)
# SYSTEM: warnings from the generated s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(fortnite_emotes_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_definitions(fortnite_emotes_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- generated consumer bindings of the optional cores -------------------------
# admin_system (HasPermission) and vip (VIP_IsClientVIP, features) are consumed exactly like the
# as_* modules consume the admin core; both keep working when the plugin is not installed (the
# wrapper checks its binding slots).
if(NOT TARGET admin_system_bindings_obj)
    add_library(admin_system_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include/admin_system/admin_system.cpp")
    target_include_directories(admin_system_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(admin_system_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

# plugins/fortnite_emotes is added before plugins/vip (alphabetical order), so the vip_bindings
# target of that directory does not exist yet: this plugin always compiles its own copy of the
# binding slots from the checked-in generated file (each shared library carries its own copy of the
# __vip_* globals, exactly like the as_* modules do with admin_system's).
set(FORTNITE_EMOTES_VIP_CPP "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include/vip/vip.cpp")
set(FORTNITE_EMOTES_EXTRA_BINDINGS "")
if(EXISTS "${FORTNITE_EMOTES_VIP_CPP}")
    add_library(fortnite_emotes_vip_bindings OBJECT "${FORTNITE_EMOTES_VIP_CPP}")
    target_include_directories(fortnite_emotes_vip_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(fortnite_emotes_vip_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    list(APPEND FORTNITE_EMOTES_EXTRA_BINDINGS $<TARGET_OBJECTS:fortnite_emotes_vip_bindings>)
    set(FORTNITE_EMOTES_HAS_VIP 1)
else()
    message(STATUS "fortnite_emotes: no vip bindings found, building without the VIP integration")
    set(FORTNITE_EMOTES_HAS_VIP 0)
endif()

# --- plugin -------------------------------------------------------------------
add_library(plugify-fortnite_emotes SHARED
        src/plugin.cpp
        src/emote.cpp
        src/camera.cpp
        src/weapons_gloves.cpp
        src/hooks.cpp
        src/commands.cpp
        src/fe_entity.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:admin_system_bindings_obj>
        ${FORTNITE_EMOTES_EXTRA_BINDINGS}
)
target_include_directories(plugify-fortnite_emotes PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/plg/pisexcompat/admin_system/vip headers are not ours.
target_include_directories(plugify-fortnite_emotes SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include")
target_link_libraries(plugify-fortnite_emotes PRIVATE
        fortnite_emotes_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-fortnite_emotes PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-fortnite_emotes PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="fortnite_emotes"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
        FE_HAS_VIP=${FORTNITE_EMOTES_HAS_VIP}
)
set_target_properties(plugify-fortnite_emotes PROPERTIES OUTPUT_NAME fortnite_emotes)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-fortnite_emotes PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/fortnite_emotes.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/fortnite_emotes.pplugin"
)

# --- tests ---------------------------------------------------------------------
# Config parsing + name lookup against the live server's settings.ini / emotes.ini / phrases
# (ref/prod-27016); no engine involved.
add_executable(fortnite_emotes_tests test/fe_core_tests.cpp)
target_link_libraries(fortnite_emotes_tests PRIVATE fortnite_emotes_core -static-libstdc++ -static-libgcc)
target_compile_options(fortnite_emotes_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(fortnite_emotes_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_definitions(fortnite_emotes_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        FE_TEST_CONFIGS_DIR="${CMAKE_SOURCE_DIR}/ref/prod-27016/addons/configs/fortnite_emotes"
        FE_TEST_TRANSLATIONS_DIR="${CMAKE_SOURCE_DIR}/ref/prod-27016/addons/translations")
set_target_properties(fortnite_emotes_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
