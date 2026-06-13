# Downloads a pre-built FFmpeg SDK for Windows from BtbN/FFmpeg-Builds.
# Uses CMake's built-in file(DOWNLOAD) which handles HTTPS natively.

function(download_ffmpeg RESULT_DIR)
    set(install_dir "${CMAKE_BINARY_DIR}/_deps/ffmpeg")
    if(EXISTS "${install_dir}/include/libavcodec/avcodec.h")
        set(${RESULT_DIR} "${install_dir}" PARENT_SCOPE)
        return()
    endif()

    # Ensure download directory exists
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")

    set(zip_path "${CMAKE_BINARY_DIR}/_deps/ffmpeg.zip")
    file(REMOVE "${zip_path}")

    # Use the latest shared build from BtbN (includes DLLs + headers + .lib)
    set(URL
        "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip"
    )

    message(STATUS "Downloading FFmpeg SDK (CMake file(DOWNLOAD))...")

    # Try CMake's built-in download first (most reliable)
    set(download_ok FALSE)

    file(DOWNLOAD "${URL}" "${zip_path}"
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
        TIMEOUT 120
    )

    list(GET download_status 0 status_code)
    if(status_code EQUAL 0 AND EXISTS "${zip_path}")
        set(download_ok TRUE)
        message(STATUS "FFmpeg SDK downloaded via CMake")
    endif()

    # Fallback: try curl
    if(NOT download_ok)
        message(STATUS "CMake download failed, trying curl...")
        file(REMOVE "${zip_path}")
        execute_process(
            COMMAND curl -L -o "${zip_path}" "${URL}"
            RESULT_VARIABLE curl_result
            ERROR_VARIABLE curl_err
            TIMEOUT 120
        )
        if(curl_result EQUAL 0 AND EXISTS "${zip_path}")
            set(download_ok TRUE)
            message(STATUS "FFmpeg SDK downloaded via curl")
        endif()
    endif()

    # Fallback: try PowerShell
    if(NOT download_ok)
        message(STATUS "curl failed, trying PowerShell...")
        file(REMOVE "${zip_path}")
        execute_process(
            COMMAND powershell -NoProfile -Command "
                [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12;
                Invoke-WebRequest -Uri '${URL}' -OutFile '${zip_path}' -UserAgent 'CMake'
            "
            RESULT_VARIABLE ps_result
            ERROR_VARIABLE ps_err
            TIMEOUT 120
        )
        if(ps_result EQUAL 0 AND EXISTS "${zip_path}")
            set(download_ok TRUE)
            message(STATUS "FFmpeg SDK downloaded via PowerShell")
        endif()
    endif()

    if(NOT download_ok)
        message(FATAL_ERROR
            "Failed to download FFmpeg SDK.\n"
            "Download manually from:\n"
            "  ${URL}\n"
            "Extract and run:\n"
            "  cmake -B build -DFFMPEG_DIR=\"path/to/extracted\"")
    endif()

    # Extract
    set(extract_dir "${CMAKE_BINARY_DIR}/_deps/ffmpeg-tmp")
    file(MAKE_DIRECTORY "${extract_dir}")
    file(ARCHIVE_EXTRACT INPUT "${zip_path}" DESTINATION "${extract_dir}")
    file(REMOVE "${zip_path}")

    # BtbN zip has a nested directory: extract_dir/ffmpeg-master-latest-win64-gpl-shared/...
    # Find the root that has include/libavcodec/avcodec.h
    file(GLOB_RECURSE hdrs "${extract_dir}/*/include/libavcodec/avcodec.h")
    if(NOT hdrs)
        # Try without nested folder (just in case)
        file(GLOB_RECURSE hdrs "${extract_dir}/include/libavcodec/avcodec.h")
    endif()
    if(NOT hdrs)
        message(FATAL_ERROR "Extracted FFmpeg archive has unexpected structure at ${extract_dir}")
    endif()

    list(GET hdrs 0 hdr_path)
    get_filename_component(inc_dir "${hdr_path}" DIRECTORY)   # .../include/libavcodec
    get_filename_component(inc_dir "${inc_dir}" DIRECTORY)     # .../include
    get_filename_component(ffmpeg_root "${inc_dir}" DIRECTORY) # root

    # Remove the root and replace with install_dir
    file(REMOVE_RECURSE "${install_dir}")
    file(RENAME "${ffmpeg_root}" "${install_dir}")
    file(REMOVE_RECURSE "${extract_dir}")

    set(${RESULT_DIR} "${install_dir}" PARENT_SCOPE)
    message(STATUS "FFmpeg SDK ready at ${install_dir}")
endfunction()
