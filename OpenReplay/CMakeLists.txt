# ── FFmpeg SDK ──
#   Specify with: -DFFMPEG_DIR="path/to/ffmpeg-dev"
#   or use vcpkg: -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
#   or auto-download: -DAUTO_DOWNLOAD_FFMPEG=ON

set(FFMPEG_DIR "" CACHE PATH "Path to FFmpeg dev SDK (include/, lib/)")
set(AUTO_DOWNLOAD_FFMPEG ON CACHE BOOL "Auto-download FFmpeg SDK if not found")

if(NOT FFMPEG_DIR)
    find_package(FFmpeg QUIET)
    if(FFmpeg_FOUND)
        message(STATUS "FFmpeg found via find_package")
    elseif(AUTO_DOWNLOAD_FFMPEG)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FFmpegDownload.cmake")
        download_ffmpeg(FFMPEG_DIR)
        set(FFMPEG_DIR "${FFMPEG_DIR}" CACHE PATH "Path to FFmpeg dev SDK" FORCE)
        message(STATUS "FFmpeg auto-downloaded to: ${FFMPEG_DIR}")
    else()
        message(WARNING
            "FFmpeg not found.\n"
            "Options:\n"
            "  1. Set -DFFMPEG_DIR=\"path/to/ffmpeg-dev\"\n"
            "  2. Use vcpkg: -DCMAKE_TOOLCHAIN_FILE=\"C:/vcpkg/scripts/buildsystems/vcpkg.cmake\"\n"
            "  3. Auto-download: -DAUTO_DOWNLOAD_FFMPEG=ON")
    endif()
else()
    if(NOT EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h")
        message(FATAL_ERROR "FFMPEG_DIR set to '${FFMPEG_DIR}' but avcodec.h not found there.")
    endif()
    message(STATUS "FFmpeg SDK: ${FFMPEG_DIR}")
endif()

add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)

set(SOURCES
    main.cpp
    UI.cpp                 UI.h
    Locale.cpp             Locale.h
    Config.cpp             Config.h
    ClipManager.cpp        ClipManager.h
    OpenReplay.cpp         OpenReplay.h
    DiskBackedBuffer.cpp   DiskBackedBuffer.h
    VideoEncoder.cpp       VideoEncoder.h
    AudioCapture.cpp       AudioCapture.h
    Muxer.cpp              Muxer.h
    ScreenCapture.cpp      ScreenCapture.h
    Overlay.cpp            Overlay.h
    ImGuiLayer.cpp         ImGuiLayer.h
)

add_executable(OpenReplay ${SOURCES})

target_include_directories(OpenReplay PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/external"
)

if(FFmpeg_FOUND)
    target_link_libraries(OpenReplay PRIVATE
        FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil
        FFmpeg::swscale FFmpeg::swresample
    )
elseif(FFMPEG_DIR)
    target_include_directories(OpenReplay PRIVATE "${FFMPEG_DIR}/include")
    target_link_directories(OpenReplay PRIVATE "${FFMPEG_DIR}/lib")
    target_link_libraries(OpenReplay PRIVATE
        avcodec avformat avutil swscale swresample
    )
endif()

target_compile_features(OpenReplay PRIVATE cxx_std_20)

target_link_libraries(OpenReplay PRIVATE
    ole32 oleaut32 d3d11 dxgi dxguid dwmapi shell32
)

# ── imgui (Dear ImGui) ──
find_package(imgui CONFIG QUIET)
if(imgui_FOUND)
    target_link_libraries(OpenReplay PRIVATE imgui::imgui)
    target_compile_definitions(OpenReplay PRIVATE HAS_IMGUI)
else()
    set(IMGUI_VERSION "1.91.6")
    set(IMGUI_URL "https://github.com/ocornut/imgui/archive/refs/tags/v${IMGUI_VERSION}.tar.gz")
    set(IMGUI_DST "${CMAKE_BINARY_DIR}/_deps/imgui")

    if(NOT EXISTS "${IMGUI_DST}/imgui.h")
        message(STATUS "Downloading imgui ${IMGUI_VERSION}...")
        file(DOWNLOAD "${IMGUI_URL}" "${IMGUI_DST}.tar.gz" SHOW_PROGRESS TIMEOUT 60)
        file(MAKE_DIRECTORY "${IMGUI_DST}-tmp")
        file(ARCHIVE_EXTRACT INPUT "${IMGUI_DST}.tar.gz" DESTINATION "${IMGUI_DST}-tmp")
        file(REMOVE "${IMGUI_DST}.tar.gz")
        file(GLOB imgui_extracted "${IMGUI_DST}-tmp/*")
        list(LENGTH imgui_extracted ec)
        if(ec EQUAL 1)
            file(RENAME "${imgui_extracted}" "${IMGUI_DST}")
        else()
            file(RENAME "${IMGUI_DST}-tmp" "${IMGUI_DST}")
        endif()
        file(REMOVE_RECURSE "${IMGUI_DST}-tmp")
        message(STATUS "imgui extracted to ${IMGUI_DST}")
    endif()

    target_include_directories(OpenReplay PRIVATE "${IMGUI_DST}" "${IMGUI_DST}/backends")
    target_sources(OpenReplay PRIVATE
        "${IMGUI_DST}/imgui.cpp"
        "${IMGUI_DST}/imgui_draw.cpp"
        "${IMGUI_DST}/imgui_widgets.cpp"
        "${IMGUI_DST}/imgui_tables.cpp"
        "${IMGUI_DST}/backends/imgui_impl_win32.cpp"
        "${IMGUI_DST}/backends/imgui_impl_dx11.cpp"
    )
    target_compile_definitions(OpenReplay PRIVATE HAS_IMGUI)
endif()

if(FFMPEG_DIR)
    set(FFMPEG_DLL_DIR "${FFMPEG_DIR}/bin")
    set(TARGET_DIR $<TARGET_FILE_DIR:OpenReplay>)
    # Auto-detect FFmpeg DLLs by globbing the bin directory
    file(GLOB FFMPEG_DLLS "${FFMPEG_DLL_DIR}/*.dll")
    if(NOT FFMPEG_DLLS)
        # Fallback: try versioned filenames (BtbN shared builds)
        foreach(PFX avcodec;avformat;avutil;swscale;swresample;avdevice;avfilter;postproc)
            file(GLOB MATCHED "${FFMPEG_DLL_DIR}/${PFX}-*.dll")
            list(APPEND FFMPEG_DLLS ${MATCHED})
        endforeach()
    endif()
    foreach(DLL_PATH ${FFMPEG_DLLS})
        get_filename_component(DLL_NAME ${DLL_PATH} NAME)
        add_custom_command(TARGET OpenReplay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DLL_PATH}" "${TARGET_DIR}/${DLL_NAME}"
            COMMENT "Copying ${DLL_NAME}"
        )
    endforeach()
    if(NOT FFMPEG_DLLS)
        message(WARNING "No FFmpeg DLLs found in ${FFMPEG_DLL_DIR}")
    endif()
endif()
add_custom_command(TARGET OpenReplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.json" $<TARGET_FILE_DIR:OpenReplay>
    COMMENT "Copying config.json"
)

set_target_properties(OpenReplay PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:OpenReplay>"
)
