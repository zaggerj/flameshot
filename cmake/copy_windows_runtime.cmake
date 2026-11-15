# Copies the full DLL dependency closure of a Windows install tree.
#
# windeployqt --compiler-runtime only knows the MSVC redistributable. A MinGW
# build of Qt also needs zlib1.dll, libharfbuzz-0.dll, libpcre2-16-0.dll and
# friends, and windeployqt silently skips them when it cannot resolve them.
# This script walks the import tables with objdump and pulls every missing
# dependency from the toolchain directory.
#
# Usage:
#   cmake -DOBJDUMP=<objdump.exe> -DBIN_DIR=<dir to fix>
#         -DEXTRA_DIRS=<dir1;dir2;...> -P copy_windows_runtime.cmake

if (NOT OBJDUMP)
    message(FATAL_ERROR "OBJDUMP is required")
endif ()
if (NOT BIN_DIR)
    message(FATAL_ERROR "BIN_DIR is required")
endif ()

file(GLOB_RECURSE _queue "${BIN_DIR}/*.dll" "${BIN_DIR}/*.exe")
set(_seen "")
set(_copied 0)

while (_queue)
    list(POP_FRONT _queue _file)
    list(FIND _seen "${_file}" _index)
    if (_index GREATER -1)
        continue()
    endif ()
    list(APPEND _seen "${_file}")

    execute_process(COMMAND "${OBJDUMP}" -p "${_file}"
                    OUTPUT_VARIABLE _dump
                    ERROR_QUIET)
    string(REGEX MATCHALL "DLL Name: [^\r\n]+" _lines "${_dump}")
    foreach (_line ${_lines})
        string(REGEX REPLACE "DLL Name: " "" _name "${_line}")
        string(STRIP "${_name}" _name)
        if (EXISTS "${BIN_DIR}/${_name}")
            list(APPEND _queue "${BIN_DIR}/${_name}")
            continue()
        endif ()

        foreach (_dir ${EXTRA_DIRS})
            if (EXISTS "${_dir}/${_name}")
                file(COPY "${_dir}/${_name}" DESTINATION "${BIN_DIR}")
                message(STATUS "copied ${_name} from ${_dir}")
                list(APPEND _queue "${BIN_DIR}/${_name}")
                math(EXPR _copied "${_copied} + 1")
                break()
            endif ()
        endforeach ()
    endforeach ()
endwhile ()

message(STATUS "runtime dependency closure: copied ${_copied} files into ${BIN_DIR}")
