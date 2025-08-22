#include "ui_helpers.h"

ViewDispatcher* g_vd = NULL;
Submenu* g_submenu = NULL;
VariableItemList* g_vil = NULL;

void ui_go_view(ViewId v){
    view_dispatcher_switch_to_view(g_vd, v);
}
