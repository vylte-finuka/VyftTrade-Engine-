# CMakeList.txt : moteur Vyft Trade™ (bibliothèque d'ordres VEZ + exécutable en ligne de commande)

# Compilable seul (Docker, cmake -S .) ou comme sous-projet du CMakeLists.txt parent
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required (VERSION 3.14)
  project ("VyftTradeEngine" CXX)
endif ()

# Bibliothèque : construction, validation et envoi des ordres (API vyft-trade.vyft-one.com)
add_library (VyftTradeEngine STATIC "VyftTrade_motor.cpp" "VyftTrade_motor.h")
target_include_directories (VyftTradeEngine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if (WIN32)
  # Client HTTPS inclus dans Windows
  target_link_libraries (VyftTradeEngine PRIVATE winhttp)
else ()
  find_package (CURL)
  if (CURL_FOUND)
    target_compile_definitions (VyftTradeEngine PRIVATE VYFTTRADE_WITH_CURL)
    target_link_libraries (VyftTradeEngine PRIVATE CURL::libcurl)
  else ()
    message (WARNING "libcurl introuvable : l'envoi des ordres sera désactivé (--dry-run uniquement)")
  endif ()
endif ()

# Exécutable : VyftTrade <buy|sell> <execute_order|cancel_order> <quantité> <wallet> [devise] [prix] [orderid]
add_executable (VyftTrade "VyftTrade_execution.cpp")
target_link_libraries (VyftTrade PRIVATE VyftTradeEngine)

# Serveur API (vyft-trade.vyft-one.com) : cpp-httplib + nlohmann/json, récupérés à la configuration
option (VYFTTRADE_BUILD_SERVER "Construire le serveur API Vyft Trade" ON)
if (VYFTTRADE_BUILD_SERVER)
  include (FetchContent)
  FetchContent_Declare (httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3)
  FetchContent_Declare (nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable (httplib nlohmann_json)

  add_executable (VyftTradeServer "VyftTrade_server.cpp" "VyftTrade_market.cpp")
  target_link_libraries (VyftTradeServer PRIVATE VyftTradeEngine httplib::httplib nlohmann_json::nlohmann_json)
  if (WIN32)
    target_link_libraries (VyftTradeServer PRIVATE ws2_32)
  endif ()
  if (CMAKE_VERSION VERSION_GREATER 3.12)
    set_property (TARGET VyftTradeServer PROPERTY CXX_STANDARD 20)
  endif ()
endif ()

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property (TARGET VyftTradeEngine PROPERTY CXX_STANDARD 20)
  set_property (TARGET VyftTrade PROPERTY CXX_STANDARD 20)
endif ()
