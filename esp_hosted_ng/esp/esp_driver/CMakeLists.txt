cmake_minimum_required(VERSION 3.0)

project(ESP-hosted)

# Reset Git repository
execute_process(
    COMMAND git reset --hard
)

# Initialize submodules for esp-idf
execute_process(
    COMMAND git submodule update --init --depth=1
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# Initialize submodule for esp-idf in esp-idf directory
execute_process(
    COMMAND git submodule update --init --depth=1 -f
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf
)

# Determine which shell is compatible and execute the corresponding install script
if(UNIX AND NOT APPLE)
    execute_process(
        COMMAND which bash
        OUTPUT_VARIABLE BASH_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${BASH_PATH} install.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf
    )
elseif(APPLE)
    execute_process(
        COMMAND ./install.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf
    )
elseif(WIN32)
    if(${CMAKE_GENERATOR} MATCHES ".*Visual Studio.*")
        execute_process(
            COMMAND powershell.exe -File install.ps1
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf
        )
    else()
        execute_process(
            COMMAND cmd.exe /C install.bat
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf
        )
    endif()
elseif(UNIX AND NOT WIN32)
    execute_process(
        COMMAND install.fish
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf
    )
else()
    message(FATAL_ERROR "Unsupported shell")
endif()

# Replace wireless libraries
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/lib/esp32/ DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf/components/esp_wifi/lib/esp32/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/lib/esp32c3/ DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf/components/esp_wifi/lib/esp32c3/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/lib/esp32s2/ DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf/components/esp_wifi/lib/esp32s2/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/lib/esp32s3/ DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf/components/esp_wifi/lib/esp32s3/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/lib/esp32c2/ DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf/components/esp_wifi/lib/esp32c2/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/lib/esp32c6/ DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf/components/esp_wifi/lib/esp32c6/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/lib/esp32c5/ DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/esp-idf/components/esp_wifi/lib/esp32c5/)

# Output message
message("###### Setup Done ######")
