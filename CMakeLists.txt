cmake_minimum_required(VERSION 3.20)
project(Whiteboard LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Whiteboard WIN32
    src/main.cpp
    src/App.cpp
    src/Renderer.cpp
    src/Grid.cpp
    src/StrokeBuilder.cpp
    src/SpatialIndex.cpp
    src/Stress.cpp
    src/StrokeMesh.cpp
    src/Board.cpp
    src/Toolbar.cpp
    src/FileIO.cpp
    src/res/app.rc
    src/Whiteboard.manifest
)

target_compile_definitions(Whiteboard PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(Whiteboard PRIVATE d2d1 d3d11 dxgi dwrite dwmapi d3dcompiler comdlg32 shell32 ole32)

if(MSVC)
    # Static CRT: single .exe, no runtime DLLs to ship.
    set_property(TARGET Whiteboard PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(Whiteboard PRIVATE /W4 /permissive- /utf-8
        $<$<CONFIG:Release>:/O2 /GL /Gw /arch:AVX2>)
    target_link_options(Whiteboard PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
endif()
