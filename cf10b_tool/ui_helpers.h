#pragma once
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <input/input.h>

typedef enum { VIEW_STATUS=0, VIEW_MODEL_MENU=1, VIEW_EDIT_MODEL=2 } ViewId;

extern ViewDispatcher* g_vd;
extern Submenu* g_submenu;
extern VariableItemList* g_vil;

void ui_go_view(ViewId v);
