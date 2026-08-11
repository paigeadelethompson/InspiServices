cmake_minimum_required(VERSION 3.25)

project(AnswerServices
  VERSION 0.1.0
  DESCRIPTION "AnswerServices (InspIRCd services & bridging)"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
set(SV_WARNINGS
  -Wall -Wextra -Wpedantic
  -Wconversion -Wsign-conversion
  -Wshadow -Wformat=2 -Wnull-dereference
  -Wno-unused-parameter)
add_compile_options(${SV_WARNINGS})

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)

# jsoncpp comes from thirdparty/json/ (amalgamated build).
set(JSONCPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty)

# ---------------------------------------------------------------------------
# The one daemon.
# ---------------------------------------------------------------------------
add_executable(anservices
  src/main.cpp
  src/util/util.cpp
  src/util/env.cpp
  src/util/crypto.cpp
  src/util/log.cpp
  src/db.cpp
  src/config.cpp
  src/net/eventloop.cpp
  src/net/socket.cpp
  src/net/tls.cpp
  src/net/http.cpp
  src/net/websocket.cpp
  src/irc/protocol.cpp
  src/irc/network.cpp
  src/irc/link.cpp
  src/irc/routing.cpp
  src/services/services.cpp
  src/services/syntax.cpp
  src/services/schema.cpp
  src/services/nickserv.cpp
  src/services/chanserv.cpp
  src/services/botserv.cpp
  src/services/operserv.cpp
  src/services/bridgeserv.cpp
  src/bridge/bridge.cpp
  src/bridge/discord.cpp
  src/bridge/signal.cpp
)

target_include_directories(anservices PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${JSONCPP_DIR})

target_sources(anservices PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/jsoncpp.cpp)

target_link_libraries(anservices
  PRIVATE
    Threads::Threads
    OpenSSL::SSL OpenSSL::Crypto
    SQLite::SQLite3)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS anservices
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES conf/environment.env.example
  DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/${PROJECT_NAME})