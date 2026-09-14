# Stages everything the app loads at runtime beside a built target, so a copy
# of the output folder runs on a machine without OBS Studio or Visual Studio:
# the OBS runtime DLLs and helper executables, the plugins with their data,
# the libobs shader effects, and the MSVC runtime. Invoked post-build with
# -DOBS_ROOT, -DDEST, -DDLLS, -DEXES, -DPLUGINS, and -DCRT.
foreach(name IN LISTS DLLS)
  file(COPY "${OBS_ROOT}/bin/64bit/${name}.dll" DESTINATION "${DEST}")
endforeach()
foreach(name IN LISTS EXES)
  file(COPY "${OBS_ROOT}/bin/64bit/${name}.exe" DESTINATION "${DEST}")
endforeach()
foreach(name IN LISTS PLUGINS)
  file(COPY "${OBS_ROOT}/obs-plugins/64bit/${name}.dll" DESTINATION "${DEST}/obs-plugins")
  if(IS_DIRECTORY "${OBS_ROOT}/data/obs-plugins/${name}")
    file(COPY "${OBS_ROOT}/data/obs-plugins/${name}" DESTINATION "${DEST}/data/obs-plugins" PATTERN "*.pdb" EXCLUDE)
  endif()
endforeach()
file(COPY "${OBS_ROOT}/data/libobs" DESTINATION "${DEST}/data")
foreach(library IN LISTS CRT)
  file(COPY "${library}" DESTINATION "${DEST}")
endforeach()
