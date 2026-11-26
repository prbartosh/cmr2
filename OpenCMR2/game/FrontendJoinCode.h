#ifndef _FRONTENDJOINCODE_H
#define _FRONTENDJOINCODE_H

#include "Menu.h"

void FrontendJoinCode_BuildMenu(void);
void FrontendJoinCode_Open(Menu *pMenu, MenuItem *pItem);
char *FrontendJoinCode_BrowserLabel(void);
void FrontendJoinCode_LabelBrowserItem(Menu *pMenu);
void FrontendJoinCode_DrawHostCode(void);

#endif
