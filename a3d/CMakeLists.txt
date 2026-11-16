# A3D 1.x (a3d.dll) and A3D 2.0 and 3.0 (a3dapi.dll) on top of DirectSound/DSOAL.
set(A3D_COMMON common.cpp)

add_library(a3dapi SHARED a3dapi_main.cpp engine.cpp source.cpp source3.cpp reverb.cpp ${A3D_COMMON} a3dapi.def)
set_target_properties(a3dapi PROPERTIES PREFIX "" OUTPUT_NAME a3dapi)
target_link_libraries(a3dapi PRIVATE dxguid uuid ole32 winmm msacm32)

add_library(a3d SHARED a3d1_main.cpp ${A3D_COMMON} a3d.def)
set_target_properties(a3d PROPERTIES PREFIX "" OUTPUT_NAME a3d)
target_link_libraries(a3d PRIVATE dxguid uuid ole32)

if(MINGW)
    foreach(t a3dapi a3d)
        target_link_options(${t} PRIVATE -static -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
    endforeach()
endif()

install(TARGETS a3dapi a3d
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
