cmake_minimum_required(VERSION 3.24)
project(AMExclusive VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "AMExclusive supports Windows x64 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
  minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG v1.3.4
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(minhook)

set(APPLE_MUSIC_PACKAGE_DIR
    "C:/Program Files/WindowsApps/AppleInc.AppleMusicWin_1.1540.23042.0_x64__nzyj5cx40ttqa"
    CACHE PATH "Supported Apple Music package used to generate WinUI projections")
find_program(CPPWINRT_EXECUTABLE cppwinrt.exe)
if(NOT CPPWINRT_EXECUTABLE)
  file(GLOB CPPWINRT_CANDIDATES
    "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/cppwinrt.exe")
  if(CPPWINRT_CANDIDATES)
    list(SORT CPPWINRT_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
    list(GET CPPWINRT_CANDIDATES 0 CPPWINRT_EXECUTABLE)
  endif()
endif()
if(NOT CPPWINRT_EXECUTABLE)
  message(FATAL_ERROR "cppwinrt.exe was not found in the Windows SDK")
endif()

set(WINRT_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
  OUTPUT "${WINRT_GENERATED_DIR}/winrt/Microsoft.UI.Xaml.h"
  COMMAND "${CPPWINRT_EXECUTABLE}"
          -input "${APPLE_MUSIC_PACKAGE_DIR}"
          -reference sdk
          -output "${WINRT_GENERATED_DIR}"
          -overwrite -base
  DEPENDS "${APPLE_MUSIC_PACKAGE_DIR}/Microsoft.UI.Xaml.winmd"
  VERBATIM)
add_custom_target(generate_winui_projection
  DEPENDS "${WINRT_GENERATED_DIR}/winrt/Microsoft.UI.Xaml.h")

add_library(am_exclusive_hook SHARED
  src/HookDll.cpp
  src/IpcTransport.cpp
  src/LosslessQualityLock.cpp
  src/audio_v2/AudioCoreCoordinator.cpp
  src/audio_v2/AudioCoreRuntime.cpp
  src/audio_v2/NativeRenderGateProxy.cpp
  src/audio_v2/WasapiExclusiveSink.cpp)
target_include_directories(am_exclusive_hook PRIVATE
  src "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rigtorp-SPSCQueue/include")
target_compile_definitions(am_exclusive_hook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(am_exclusive_hook PRIVATE minhook ole32 uuid advapi32 avrt)
set_target_properties(am_exclusive_hook PROPERTIES OUTPUT_NAME "am-exclusive-hook")

add_library(am_exclusive_ui SHARED src/UiAdapter.cpp src/IpcTransport.cpp)
add_dependencies(am_exclusive_ui generate_winui_projection)
target_include_directories(am_exclusive_ui PRIVATE src "${WINRT_GENERATED_DIR}")
target_compile_definitions(am_exclusive_ui PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(am_exclusive_ui PRIVATE advapi32 ole32 runtimeobject windowsapp)
set_target_properties(am_exclusive_ui PROPERTIES OUTPUT_NAME "am-exclusive-ui")

add_executable(am_exclusive_media_control src/MediaSessionControl.cpp)
target_compile_definitions(am_exclusive_media_control PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(am_exclusive_media_control PRIVATE ole32 runtimeobject windowsapp)

add_executable(am_exclusive_broker src/Broker.cpp src/IpcTransport.cpp)
target_include_directories(am_exclusive_broker PRIVATE src)
target_compile_definitions(am_exclusive_broker PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(am_exclusive_broker PRIVATE advapi32 ole32 comctl32)
set_target_properties(am_exclusive_broker PROPERTIES OUTPUT_NAME "am-exclusive-broker")
add_custom_command(TARGET am_exclusive_broker POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/config/am-exclusive.ini
          $<TARGET_FILE_DIR:am_exclusive_broker>/am-exclusive.ini)

if(MSVC)
  target_compile_options(am_exclusive_hook PRIVATE /W4 /permissive- /EHsc /guard:cf /wd4324)
  target_compile_options(am_exclusive_ui PRIVATE /W4 /permissive- /EHsc /guard:cf /utf-8)
  target_compile_options(am_exclusive_media_control PRIVATE /W4 /permissive- /EHsc /guard:cf)
  target_compile_options(am_exclusive_broker PRIVATE /W4 /permissive- /EHsc /guard:cf)
  target_link_options(am_exclusive_hook PRIVATE /guard:cf)
  target_link_options(am_exclusive_ui PRIVATE /guard:cf)
  target_link_options(am_exclusive_media_control PRIVATE /guard:cf)
  target_link_options(am_exclusive_broker PRIVATE
    /guard:cf /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
endif()

# Single-file setup. Only the fixed production payload and installer support
# files listed by Generate-SetupResources.ps1 can be embedded.
find_program(POWERSHELL_EXECUTABLE powershell.exe REQUIRED)
set(SETUP_RESOURCE_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/setup/AMExclusive-Setup.rc")
add_custom_command(
  OUTPUT "${SETUP_RESOURCE_FILE}"
  COMMAND "${POWERSHELL_EXECUTABLE}"
          -NoLogo -NoProfile -ExecutionPolicy Bypass
          -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Generate-SetupResources.ps1"
          -OutputFile "${SETUP_RESOURCE_FILE}"
          -PayloadDirectory "$<TARGET_FILE_DIR:am_exclusive_broker>"
          -ScriptDirectory "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
          -RepositoryRoot "${CMAKE_CURRENT_SOURCE_DIR}/.."
  DEPENDS
    am_exclusive_broker
    am_exclusive_media_control
    am_exclusive_hook
    am_exclusive_ui
    "${CMAKE_CURRENT_SOURCE_DIR}/config/am-exclusive.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Generate-SetupResources.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Install-Mod.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Uninstall-Mod.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Register-Companion.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Uninstall.cmd"
    "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_NOTICES.md"
  VERBATIM)
set_source_files_properties("${SETUP_RESOURCE_FILE}" PROPERTIES GENERATED TRUE)
add_custom_target(am_exclusive_setup_resources DEPENDS "${SETUP_RESOURCE_FILE}")

add_executable(am_exclusive_setup src/SetupInstaller.cpp "${SETUP_RESOURCE_FILE}")
add_dependencies(am_exclusive_setup
  am_exclusive_setup_resources
  am_exclusive_broker
  am_exclusive_media_control
  am_exclusive_hook
  am_exclusive_ui)
target_include_directories(am_exclusive_setup PRIVATE src)
target_compile_definitions(am_exclusive_setup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(am_exclusive_setup PROPERTIES OUTPUT_NAME "AMExclusive-Setup")
if(MSVC)
  target_compile_options(am_exclusive_setup PRIVATE /W4 /permissive- /EHsc /guard:cf /utf-8)
  target_link_options(am_exclusive_setup PRIVATE /guard:cf)
endif()

install(TARGETS
  am_exclusive_hook
  am_exclusive_ui
  am_exclusive_media_control
  am_exclusive_broker
  am_exclusive_setup
  RUNTIME DESTINATION .
  LIBRARY DESTINATION .)
install(FILES config/am-exclusive.ini DESTINATION .)
