cmake_minimum_required(VERSION 3.2)

target_sources(opencaptainclaw
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ScreenElementVideo.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScreenElementVideo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VideoProcess.h
    ${CMAKE_CURRENT_SOURCE_DIR}/VideoProcess.cpp
)
