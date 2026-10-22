# 3.25: FetchContent_Declare(... SYSTEM) below.
cmake_minimum_required(VERSION 3.25)
project(soccer_display_sim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)
endif()

include(FetchContent)

# --- Dependencies (pinned) ---------------------------------------------------
# LVGL reads our lv_conf.h from this directory.
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(LV_CONF_SKIP OFF CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
# doctest 2.4.12 still declares an old cmake_minimum_required; keep output clean.
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)

FetchContent_Declare(lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG v9.2.2
  GIT_SHALLOW TRUE
  SYSTEM)
FetchContent_Declare(ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
  GIT_SHALLOW TRUE
  SYSTEM)
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.12
  GIT_SHALLOW TRUE
  SYSTEM)
FetchContent_MakeAvailable(lvgl ArduinoJson doctest)

find_package(SDL2 REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# LVGL's own warnings are not ours to fix.
target_compile_options(lvgl PRIVATE -w)
target_link_libraries(lvgl PUBLIC SDL2::SDL2)
if(NOT TARGET SDL2::SDL2)
  message(FATAL_ERROR "SDL2 found without an SDL2::SDL2 target")
endif()

# --- UI library: shared with the ESP32 firmware, no SDL/desktop dependencies --
set(UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ui")
file(GLOB UI_FONT_SOURCES CONFIGURE_DEPENDS "${UI_DIR}/fonts/*.c")
add_library(ui STATIC "${UI_DIR}/src/app.cpp" "${UI_DIR}/src/theme.cpp"
  "${UI_DIR}/src/json_parse.cpp" "${UI_DIR}/src/store.cpp" "${UI_DIR}/src/goal_detector.cpp" "${UI_DIR}/src/image_provider.cpp"
  "${UI_DIR}/src/screens/live.cpp" "${UI_DIR}/src/format.cpp" "${UI_DIR}/src/screens/detail.cpp"
  "${UI_DIR}/src/screens/widgets.cpp" "${UI_DIR}/src/screens/matches.cpp" "${UI_DIR}/src/screens/news.cpp"
  "${UI_DIR}/src/screens/goal.cpp" "${UI_DIR}/src/ray_fan.cpp"
  "${UI_DIR}/src/prefs.cpp" "${UI_DIR}/src/settings.cpp" "${UI_DIR}/src/timezone.cpp" "${UI_DIR}/src/dim_schedule.cpp"
  "${UI_DIR}/src/screens/settings.cpp" "${UI_DIR}/src/screens/zone_list.cpp" "${UI_DIR}/src/screens/team_picker.cpp"
  "${UI_DIR}/src/screens/wifi.cpp" "${UI_DIR}/src/wifi_store.cpp" "${UI_DIR}/src/wifi_controller.cpp" "${UI_DIR}/src/serial_commands.cpp" "${UI_DIR}/src/helper_config.cpp" "${UI_DIR}/src/request_deadline.cpp"
  "${UI_DIR}/src/refresh_policy.cpp" "${UI_DIR}/src/http_source.cpp" "${UI_DIR}/src/poller.cpp"
  "${UI_DIR}/src/host_loop.cpp" "${UI_DIR}/src/demo_data.cpp" "${UI_DIR}/src/mock_source.cpp"
  "${UI_DIR}/src/http_image_provider.cpp" "${UI_DIR}/src/lvgl_image_decoder.cpp" "${UI_DIR}/src/battery_model.cpp" "${UI_DIR}/src/screens/battery_banner.cpp"
  "${UI_DIR}/src/sound_synth.cpp" "${UI_DIR}/src/sound_policy.cpp" "${UI_DIR}/src/sound_voice.cpp" "${UI_DIR}/src/power_policy.cpp" "${UI_DIR}/src/screens/power.cpp" "${UI_DIR}/src/battery_cal.cpp"
  "${UI_DIR}/src/update_check.cpp"
  ${UI_FONT_SOURCES})
target_include_directories(ui PUBLIC "${UI_DIR}/src" "${UI_DIR}/fonts")
target_link_libraries(ui PUBLIC lvgl ArduinoJson)
set_source_files_properties("${UI_DIR}/src/app.cpp" "${UI_DIR}/src/theme.cpp" "${UI_DIR}/src/json_parse.cpp"
  "${UI_DIR}/src/store.cpp" "${UI_DIR}/src/goal_detector.cpp" "${UI_DIR}/src/image_provider.cpp"
  "${UI_DIR}/src/screens/live.cpp" "${UI_DIR}/src/format.cpp" "${UI_DIR}/src/screens/detail.cpp"
  "${UI_DIR}/src/screens/widgets.cpp" "${UI_DIR}/src/screens/matches.cpp" "${UI_DIR}/src/screens/news.cpp"
  "${UI_DIR}/src/screens/goal.cpp" "${UI_DIR}/src/ray_fan.cpp" "${UI_DIR}/src/prefs.cpp" "${UI_DIR}/src/settings.cpp"
  "${UI_DIR}/src/timezone.cpp" "${UI_DIR}/src/dim_schedule.cpp" "${UI_DIR}/src/screens/settings.cpp"
  "${UI_DIR}/src/screens/zone_list.cpp" "${UI_DIR}/src/screens/team_picker.cpp"
  "${UI_DIR}/src/screens/wifi.cpp" "${UI_DIR}/src/wifi_store.cpp" "${UI_DIR}/src/wifi_controller.cpp" "${UI_DIR}/src/serial_commands.cpp" "${UI_DIR}/src/helper_config.cpp" "${UI_DIR}/src/request_deadline.cpp"
  "${UI_DIR}/src/refresh_policy.cpp" "${UI_DIR}/src/http_source.cpp" "${UI_DIR}/src/poller.cpp"
  "${UI_DIR}/src/host_loop.cpp" "${UI_DIR}/src/demo_data.cpp" "${UI_DIR}/src/mock_source.cpp"
  "${UI_DIR}/src/http_image_provider.cpp" "${UI_DIR}/src/lvgl_image_decoder.cpp" "${UI_DIR}/src/battery_model.cpp" "${UI_DIR}/src/screens/battery_banner.cpp"
  "${UI_DIR}/src/sound_synth.cpp" "${UI_DIR}/src/sound_policy.cpp" "${UI_DIR}/src/sound_voice.cpp" "${UI_DIR}/src/power_policy.cpp" "${UI_DIR}/src/screens/power.cpp" "${UI_DIR}/src/battery_cal.cpp"
  "${UI_DIR}/src/update_check.cpp"
  PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# --- Simulator executable ----------------------------------------------------
add_executable(sim main.cpp sim_alloc.cpp fixture_source.cpp mock_image_provider.cpp snapshot.cpp
  file_prefs.cpp sim_backlight.cpp mock_wifi.cpp curl_transport.cpp mock_power.cpp)
# Defaults for the simulator's --assets and --fixtures flags.
target_compile_definitions(sim PRIVATE
  SIM_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
  CONTRACT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../contract/fixtures")
target_link_libraries(sim PRIVATE ui lvgl SDL2::SDL2 CURL::libcurl Threads::Threads)
target_compile_options(sim PRIVATE -Wall -Wextra)

# Writes the sounds to WAV files for checking off the board: dump_wav OUTDIR.
add_executable(dump_wav dump_wav.cpp)
target_link_libraries(dump_wav PRIVATE ui)
target_compile_options(dump_wav PRIVATE -Wall -Wextra)

# --- Tests (headless: never opens an SDL window) -----------------------------
enable_testing()
add_executable(sim_tests sim_alloc.cpp tests/test_main.cpp tests/smoke_test.cpp tests/theme_test.cpp tests/parse_test.cpp
  tests/goal_test.cpp tests/store_test.cpp tests/mock_source_test.cpp fixture_source.cpp
  tests/lru_test.cpp tests/image_provider_test.cpp mock_image_provider.cpp
  tests/image_decoder_test.cpp tests/http_image_provider_test.cpp
  tests/live_test.cpp snapshot.cpp tests/format_test.cpp tests/detail_test.cpp tests/matches_test.cpp tests/news_test.cpp
  tests/goal_screen_test.cpp tests/prefs_test.cpp tests/settings_test.cpp tests/timezone_test.cpp tests/dim_test.cpp
  tests/settings_screen_test.cpp tests/team_picker_test.cpp tests/app_settings_test.cpp
  tests/file_prefs_test.cpp file_prefs.cpp tests/wifi_screen_test.cpp mock_wifi.cpp
  tests/refresh_test.cpp tests/http_source_test.cpp tests/poller_test.cpp tests/offline_test.cpp
  tests/curl_transport_test.cpp curl_transport.cpp
  tests/battery_model_test.cpp tests/battery_ui_test.cpp
  tests/mock_power_test.cpp mock_power.cpp tests/host_loop_test.cpp tests/demo_data_test.cpp tests/ray_fan_test.cpp tests/wifi_store_test.cpp tests/wifi_controller_test.cpp tests/serial_commands_test.cpp tests/wifi_secrets_static_test.cpp tests/helper_config_test.cpp tests/helper_secrets_static_test.cpp tests/request_deadline_test.cpp
  tests/sound_synth_test.cpp tests/sound_policy_test.cpp tests/sound_app_test.cpp tests/sound_settings_test.cpp tests/sound_voice_test.cpp
  tests/battery_choice_test.cpp tests/power_policy_test.cpp tests/power_ui_test.cpp tests/battery_cal_test.cpp
  tests/update_check_test.cpp)
target_include_directories(sim_tests PRIVATE tests "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(sim_tests PRIVATE
  REPO_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
  CONTRACT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../contract/fixtures")
target_link_libraries(sim_tests PRIVATE ui doctest::doctest CURL::libcurl Threads::Threads)
target_compile_options(sim_tests PRIVATE -Wall -Wextra)
add_test(NAME sim_tests COMMAND sim_tests)
