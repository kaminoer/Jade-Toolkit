# jade_gui — native port of jade_explorer's PySide6 GUI (app.py + gui/*.py).
# One .cpp/.hpp pair per Python module, same names, so the two trees stay
# diffable file-by-file.

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)

qt_add_executable(jade_gui
  main.cpp                # app.py
  Theme.cpp               # (new) phthalo-green application theme
  Tooltips.cpp            # gui/tooltips.py
  ProjectDoc.cpp          # Qt wrapper around jade::project::ModProject
  MainWindow.cpp          # gui/main_window.py
  AssetBrowserTab.cpp     # gui/asset_browser_tab.py
  PlacementTab.cpp        # gui/placement_tab.py
  MeshSwapTab.cpp         # gui/mesh_swap_tab.py
  TextureSwapTab.cpp      # gui/texture_swap_tab.py
  AddAssetsTab.cpp        # gui/add_assets_tab.py
  LevelBlenderTab.cpp     # gui/level_blender_tab.py
  BakeLightmapsTab.cpp    # gui/bake_lightmaps_tab.py
  ZonesTab.cpp            # gui/zones_tab.py
  CreateZoneTab.cpp       # gui/create_zone_tab.py
  EditZoneDepsTab.cpp     # gui/edit_zone_deps_tab.py
  ProjectPanel.cpp        # gui/project_panel.py
  ElementThumb.cpp        # gui/element_thumb.py
  BinPickerDialog.cpp     # gui/bin_picker_dialog.py
  MaterialFlagsDialog.cpp # gui/material_flags_dialog.py
  ExePatchDialog.cpp      # gui/exe_patch_dialog.py
  JtmodDialog.cpp         # gui/jtmod_dialog.py
  MaterialPickerDialog.cpp # gui/material_picker_dialog.py
  TexturePickerDialog.cpp # gui/texture_picker_dialog.py
  BoneMapView.cpp         # gui/bone_map_view.py
  BoneRemapDialog.cpp     # gui/bone_remap_dialog.py
  PreviewPanel.cpp        # gui/preview_panel.py
  MeshPreview.cpp         # gui/mesh_preview.py (VTK → QOpenGLWidget)
  Viewer3D.cpp            # gui/viewer_3d.py
  PlacementViewport.cpp   # gui/placement_viewport.py (pygfx → QOpenGLWidget)
  app_icon.rc
)

target_link_libraries(jade_gui PRIVATE jade Qt6::Widgets Qt6::OpenGLWidgets)

# Direct oracle harness for project/project.py's document mutation/history
# behavior. It links only QtCore even though the GUI itself uses Widgets.
add_executable(projectdoc_cli projectdoc_cli.cpp ProjectDoc.cpp)
target_link_libraries(projectdoc_cli PRIVATE jade Qt6::Core)
set_target_properties(projectdoc_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/app)
if(PYTHON_EXE)
  add_test(NAME projectdoc_history_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../tests/projectdoc_history_check.py
            $<TARGET_FILE:projectdoc_cli>)
  add_test(NAME projectdoc_save_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../tests/projectdoc_save_check.py
            $<TARGET_FILE:projectdoc_cli>)
endif()

# A GUI built from a Qt SDK does not run outside the developer shell unless
# its Qt DLLs, platform plugin, and MinGW runtime are deployed beside it.
# Resolve windeployqt from the same Qt installation CMake selected, then run
# it after every relink so build-gui/app is directly launchable.
if(WIN32)
  get_target_property(_jade_qmake_executable Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(_jade_qt_bin_dir
                         "${_jade_qmake_executable}" DIRECTORY)
  find_program(JADE_WINDEPLOYQT_EXECUTABLE
    NAMES windeployqt
    HINTS "${_jade_qt_bin_dir}"
    NO_DEFAULT_PATH)
  if(NOT JADE_WINDEPLOYQT_EXECUTABLE)
    message(FATAL_ERROR
      "Qt6 was found but windeployqt is missing from ${_jade_qt_bin_dir}")
  endif()
  add_custom_command(TARGET jade_gui POST_BUILD
    COMMAND "${JADE_WINDEPLOYQT_EXECUTABLE}"
            --release
            --no-translations
            --no-opengl-sw
            "$<TARGET_FILE:jade_gui>"
    COMMENT "Deploying Qt and MinGW runtime dependencies for jade_gui")
endif()

if(TARGET jade_bytepatch_stub)
  add_dependencies(jade_gui jade_bytepatch_stub)
endif()

set_target_properties(jade_gui PROPERTIES
  WIN32_EXECUTABLE TRUE
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/app)

# The .png fallback icon is loaded from disk next to the exe (matches the
# Python assets/ folder layout).
add_custom_command(TARGET jade_gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          ${CMAKE_BINARY_DIR}/app/assets)
