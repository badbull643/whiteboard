#pragma once
#include <windows.h>

// Every user action has a command id. Menu items, toolbar buttons, keyboard shortcuts
// and WM_COMMAND messages all go through App::HandleCommand.
enum CommandId : UINT {
    ID_GRID_LINES = 100,
    ID_GRID_DOTS,
    ID_GRID_OFF,
    ID_GRID_SMALLER,
    ID_GRID_LARGER,
    ID_RESET_VIEW,
    ID_FULLSCREEN,
    ID_STATS,
    ID_UNDO,
    ID_REDO,
    ID_DARK,
    ID_TOOL_PEN,
    ID_TOOL_ERASER,
    ID_CLEAR,
    ID_GRID_MENU,       // toolbar: opens the grid popup
    ID_COLOR_CUSTOM,    // opens the color picker
    ID_WIDTH_SMALLER,
    ID_WIDTH_LARGER,
    ID_FILE_NEW,
    ID_FILE_OPEN,
    ID_FILE_SAVE,
    ID_FILE_SAVE_AS,
    ID_HELP,
    ID_GRID_SIZE_BASE = 200,  // + grid preset index
    ID_COLOR_BASE = 300,      // + palette index
    ID_WIDTH_BASE = 400,      // + pen width index
};
