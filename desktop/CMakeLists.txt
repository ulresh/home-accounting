cmake_minimum_required(VERSION 3.20)
project(home-accounting LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(BOOST_ROOT /usr/local)
find_package(Boost CONFIG REQUIRED COMPONENTS json)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

set(HA_SOURCES
    src/model/Paths.cpp
    src/model/Jsonl.cpp
    src/model/Store.cpp
    src/sync/Crypto.cpp
    src/sync/QrCode.cpp
    src/sync/SyncService.cpp
    src/ui/MainWindow.cpp
    src/ui/EventDialog.cpp
    src/ui/SyncDialog.cpp
    src/ui/SettingsDialog.cpp
    src/ui/CatalogDialog.cpp
    src/ui/MainWindow.h
    src/ui/EventDialog.h
    src/ui/SyncDialog.h
    src/ui/SettingsDialog.h
    src/ui/CatalogDialog.h
    resources/app.qrc
)

add_executable(home-accounting src/main.cpp ${HA_SOURCES})
add_executable(guitest tools/guitest.cpp ${HA_SOURCES})

foreach(tgt home-accounting guitest)
    target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src /usr/local/include)
    target_link_libraries(${tgt} PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets
        Boost::json
        OpenSSL::SSL OpenSSL::Crypto
        Threads::Threads
    )
endforeach()
