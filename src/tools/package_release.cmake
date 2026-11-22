# Assembles the Windows release folder: what a player downloads. It contains no game code; the setup tool builds the
# game module from the player's own copy.
#
#   cmake -DBUILD=<runtime build dir> -DRECOMP=<dt_recomp build dir> -DLLVM=<LLVM install dir> -DOUT=<folder>
#         -DSOURCE=<repository root> -P src/tools/package_release.cmake
#
# LLVM: an LLVM for Windows install (its bin/clang-cl.exe, bin/lld-link.exe and lib/clang/<version>/include are used).
#
# Layout:
#   dt_port.exe, DevilsThirdSetup.exe, README.md, LICENSE, NOTICE.md
#   tools/dt_recomp.exe, tools/sdk/ (ppc_context.h, module_abi.h, dt_module.cpp, hooks.txt)
#   tools/llvm/bin/clang-cl.exe, tools/llvm/bin/lld-link.exe, tools/llvm/lib/clang/<version>/include/
#   licenses/
foreach(v BUILD RECOMP LLVM OUT SOURCE)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "package_release.cmake: -D${v}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/tools/sdk" "${OUT}/tools/llvm/bin" "${OUT}/licenses")

file(COPY "${BUILD}/dt_port.exe" "${BUILD}/DevilsThirdSetup.exe" DESTINATION "${OUT}")
foreach(f LICENSE NOTICE.md README.md)
    if(EXISTS "${SOURCE}/${f}")
        file(COPY "${SOURCE}/${f}" DESTINATION "${OUT}")
    endif()
endforeach()
file(COPY "${RECOMP}/dt_recomp.exe" DESTINATION "${OUT}/tools")
file(GLOB sdk "${BUILD}/sdk/*")
file(COPY ${sdk} DESTINATION "${OUT}/tools/sdk")

# The compiler and linker, and clang's own headers (the module uses only <stdint.h> and <stddef.h>, no system SDK).
file(COPY "${LLVM}/bin/clang-cl.exe" "${LLVM}/bin/lld-link.exe" DESTINATION "${OUT}/tools/llvm/bin")
file(GLOB clang_versions LIST_DIRECTORIES true "${LLVM}/lib/clang/*")
list(GET clang_versions 0 clang_version_dir)
get_filename_component(clang_version "${clang_version_dir}" NAME)
file(COPY "${clang_version_dir}/include" DESTINATION "${OUT}/tools/llvm/lib/clang/${clang_version}")

# Licenses of everything shipped (NOTICE.md lists them).
set(deps "${BUILD}/_deps")
function(license name)
    foreach(candidate ${ARGN})
        if(EXISTS "${candidate}")
            file(COPY_FILE "${candidate}" "${OUT}/licenses/${name}.txt")
            return()
        endif()
    endforeach()
    message(WARNING "no license file found for ${name}")
endfunction()
license(dt-port "${SOURCE}/LICENSE")
license(XenonRecomp "${SOURCE}/src/recomp/LICENSE.XenonRecomp")
license(SDL3 "${deps}/sdl3-src/LICENSE.txt")
license(zlib "${deps}/zlib-src/LICENSE")
license(imgui "${deps}/imgui-src/LICENSE.txt")
license(Roboto "${SOURCE}/src/tools/licenses/Apache-2.0.txt")
license(addrlib "${deps}/addrlib-src/LICENSE" "${deps}/addrlib-src/LICENSE.txt")
license(Vulkan-Headers "${deps}/vulkanheaders-src/LICENSE.md")
license(volk "${deps}/volk-src/LICENSE.md")
license(zstd "${deps}/zstd-src/LICENSE")
license(ZArchive "${deps}/zarchive-src/LICENSE")
license(LLVM "${LLVM}/LICENSE.TXT" "${LLVM}/share/doc/LICENSE.TXT" "${SOURCE}/src/tools/licenses/LLVM-LICENSE.txt")
message(STATUS "release folder: ${OUT}")
