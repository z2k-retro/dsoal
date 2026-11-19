# A3D 1.x (a3d.dll) and A3D 2.0 and 3.0 (a3dapi.dll) on top of DirectSound/DSOAL.
set(A3D_COMMON common.cpp)
# MSVC's .def files name stdcall exports undecorated; MinGW's carry the @n.
if(MSVC)
    set(A3D_DEF_SUFFIX _msvc)
endif()

add_library(a3dapi SHARED a3dapi_main.cpp engine.cpp source.cpp source3.cpp reverb.cpp ${A3D_COMMON} a3dapi${A3D_DEF_SUFFIX}.def)
set_target_properties(a3dapi PROPERTIES PREFIX "" OUTPUT_NAME a3dapi)
target_link_libraries(a3dapi PRIVATE dxguid uuid ole32 winmm msacm32)

add_library(a3d SHARED a3d1_main.cpp ${A3D_COMMON} a3d${A3D_DEF_SUFFIX}.def)
set_target_properties(a3d PROPERTIES PREFIX "" OUTPUT_NAME a3d)
target_link_libraries(a3d PRIVATE dxguid uuid ole32)

foreach(t a3dapi a3d)
    # windows.h's min and max macros break std::min and std::max.
    target_compile_definitions(${t} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endforeach()
if(MSVC)
    foreach(t a3dapi a3d)
        target_compile_options(${t} PRIVATE /utf-8)
        # Static runtime, so the DLLs need no Visual C++ redistributable.
        set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()

if(MINGW)
    foreach(t a3dapi a3d)
        target_link_options(${t} PRIVATE -static -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
    endforeach()
endif()

install(TARGETS a3dapi a3d
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
