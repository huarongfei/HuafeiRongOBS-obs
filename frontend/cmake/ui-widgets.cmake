if(NOT TARGET OBS::qt-vertical-scroll-area)
  add_subdirectory(
    "${CMAKE_SOURCE_DIR}/shared/qt/vertical-scroll-area"
    "${CMAKE_BINARY_DIR}/shared/qt/vertical-scroll-area"
  )
endif()

target_link_libraries(obs-studio PRIVATE OBS::qt-vertical-scroll-area)

target_sources(
  obs-studio
  PRIVATE
    widgets/AudioMixer.cpp
    widgets/AudioMixer.hpp
    widgets/ColorSelect.cpp
    widgets/ColorSelect.hpp
    widgets/OBSBasic.cpp
    widgets/OBSBasic.hpp
    widgets/OBSBasic_Browser.cpp
    widgets/OBSBasic_Canvases.cpp
    widgets/OBSBasic_Clipboard.cpp
    widgets/OBSBasic_ContextToolbar.cpp
    widgets/OBSBasic_Docks.cpp
    widgets/OBSBasic_Dropfiles.cpp
    widgets/OBSBasic_Hotkeys.cpp
    widgets/OBSBasic_Icons.cpp
    widgets/OBSBasic_MainControls.cpp
    widgets/OBSBasic_OutputHandler.cpp
    widgets/OBSBasic_Preview.cpp
    widgets/OBSBasic_Profiles.cpp
    widgets/OBSBasic_Projectors.cpp
    widgets/OBSBasic_Recording.cpp
    widgets/OBSBasic_ReplayBuffer.cpp
    widgets/OBSBasic_SceneCollections.cpp
    widgets/OBSBasic_SceneItems.cpp
    widgets/OBSBasic_Scenes.cpp
    widgets/OBSBasic_Screenshots.cpp
    widgets/OBSBasic_Service.cpp
    widgets/OBSBasic_StatusBar.cpp
    widgets/OBSBasic_Streaming.cpp
    widgets/OBSBasic_StudioMode.cpp
    widgets/OBSBasic_SysTray.cpp
    widgets/OBSBasic_Transitions.cpp
    widgets/OBSBasic_Updater.cpp
    widgets/OBSBasic_VirtualCam.cpp
    widgets/OBSBasic_YouTube.cpp
    widgets/OBSBasicControls.cpp
    widgets/OBSBasicControls.hpp
    widgets/OBSBasicPreview.cpp
    widgets/OBSBasicPreview.hpp
    widgets/OBSBasicStats.cpp
    widgets/OBSBasicStats.hpp
    widgets/OBSBasicStatusBar.cpp
    widgets/OBSBasicStatusBar.hpp
    widgets/OBSMainWindow.hpp
    widgets/OBSProjector.cpp
    widgets/OBSProjector.hpp
    widgets/OBSQTDisplay.cpp
    widgets/OBSQTDisplay.hpp
    widgets/HFRConsole.cpp
    widgets/HFRConsole.hpp
    widgets/HFRPpt.cpp
    widgets/HFRPpt.hpp
    widgets/HFRPptSource.cpp
    widgets/HFRPptSource.hpp
    widgets/HFRPresenter.cpp
    widgets/HFRPresenter.hpp
    widgets/StatusBarWidget.cpp
    widgets/StatusBarWidget.hpp
)

# ---------------------------------------------------------------------------
# HuafeiRongOBS: 可选 PPT 支持（同进程 LibreOfficeKit，无 IPC 桥接）
#   启用示例：
#     cmake --preset windows-x64 -DENABLE_BROWSER=OFF -DHFR_ENABLE_PPT=ON \
#           -DHFR_LO_SDK_DIR="D:/HuafeirongOBS/tools/LibreOfficeSDK/sdk/include"
#   运行期需要 LibreOffice 解包目录（默认 D:/HuafeirongOBS/tools/LibreOffice，
#   可用环境变量 HFR_LO_PATH 覆盖）。
# ---------------------------------------------------------------------------
option(HFR_ENABLE_PPT "Enable LibreOfficeKit-based PPT rendering (in-process)" OFF)
set(HFR_LO_SDK_DIR "D:/HuafeirongOBS/tools/LibreOfficeSDK/sdk/include" CACHE PATH "LibreOfficeKit SDK include dir")

if(HFR_ENABLE_PPT)
  if(NOT EXISTS "${HFR_LO_SDK_DIR}/LibreOfficeKit/LibreOfficeKit.h")
    message(FATAL_ERROR "HFR_ENABLE_PPT=ON but LibreOfficeKit.h not found under ${HFR_LO_SDK_DIR}")
  endif()
  target_include_directories(obs-studio PRIVATE "${HFR_LO_SDK_DIR}")
  target_compile_definitions(obs-studio PRIVATE HFR_ENABLE_PPT)
  message(STATUS "HuafeiRongOBS: PPT(LOK) enabled, SDK=${HFR_LO_SDK_DIR}")
endif()

# ---------------------------------------------------------------------------
# HuafeiRongOBS: 将 LibreOffice 运行时"嵌入"到程序目录（自包含分发）
#   - 目标位置：<binary>/rundir/<Config>/bin/64bit/lo
#   - HFRPpt 优先从"obs64.exe 同目录/lo"加载（见 HFRPpt::DefaultLoPath）
#   - 新机器/新克隆：先运行 scripts/fetch-libreoffice-runtime.cmd 获取运行时
#   关闭：-DHFR_EMBED_LO=OFF
# ---------------------------------------------------------------------------
option(HFR_EMBED_LO "Embed LibreOffice runtime next to obs64.exe (bin/64bit/lo)" ON)
if(HFR_EMBED_LO)
  set(HFR_LO_RUNTIME_DIR "D:/HuafeirongOBS/tools/LibreOffice" CACHE PATH "LibreOffice runtime to embed (contains program/ and share/)")
  if(EXISTS "${HFR_LO_RUNTIME_DIR}/program/mergedlo.dll")
    add_custom_command(
      TARGET obs-studio POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E echo "HuafeiRongOBS: embedding LibreOffice runtime -> bin/64bit/lo"
      COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
              "${HFR_LO_RUNTIME_DIR}"
              "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>/bin/64bit/lo"
      COMMENT "Embed LibreOffice runtime (only changed files are copied)"
      VERBATIM
    )
    message(STATUS "HuafeiRongOBS: LO runtime will be embedded from ${HFR_LO_RUNTIME_DIR}")
  else()
    message(STATUS "HuafeiRongOBS: HFR_EMBED_LO=ON but runtime not found at ${HFR_LO_RUNTIME_DIR} (skip embedding)")
  endif()
endif()