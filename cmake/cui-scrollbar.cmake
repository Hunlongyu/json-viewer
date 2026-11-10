# The pinned CUI backend hardcodes the vertical thumb's visual width.
# Its independent 12-point pointer target stays available for dragging.
set(source "${CUI_SOURCE_DIR}/src/cui_widgets.c")
file(READ "${source}" contents)
set(original "float w = big ? 8 : 6;")
set(slim "float w = big ? 5 : 3;")
if(contents MATCHES "float w = big [?] 8 : 6;")
  string(REPLACE "${original}" "${slim}" contents "${contents}")
  file(WRITE "${source}" "${contents}")
elseif(NOT contents MATCHES "float w = big [?] 5 : 3;")
  message(FATAL_ERROR "CUI's vertical scrollbar implementation changed")
endif()
