# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Release")
  file(REMOVE_RECURSE
  "app\\CMakeFiles\\jade_gui_autogen.dir\\AutogenUsed.txt"
  "app\\CMakeFiles\\jade_gui_autogen.dir\\ParseCache.txt"
  "app\\CMakeFiles\\projectdoc_cli_autogen.dir\\AutogenUsed.txt"
  "app\\CMakeFiles\\projectdoc_cli_autogen.dir\\ParseCache.txt"
  "app\\jade_gui_autogen"
  "app\\projectdoc_cli_autogen"
  )
endif()
