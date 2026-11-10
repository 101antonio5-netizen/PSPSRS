cmake_minimum_required(VERSION 3.5)
project(SRSGui C)

add_executable(srsgui main.c)

# intrafont must come before pspgu (it uses it); m = math library
target_link_libraries(srsgui PRIVATE intrafont pspgu pspdisplay pspge pspctrl psprtc m)

create_pbp_file(
    TARGET srsgui
    ICON_PATH NULL
    BACKGROUND_PATH NULL
    PREVIEW_PATH NULL
    TITLE "SRS Decks"
)
