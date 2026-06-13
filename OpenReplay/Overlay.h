#pragma once
#include <windows.h>

void overlayCreate();
void overlayDestroy();
void overlayShow(bool show);
void overlaySetCorner(int corner);
void overlaySetSize(int size);
void overlaySetAlpha(int alpha);
void overlaySetColor(COLORREF color);
void overlaySetShowFps(bool show);
int overlayCorner();
int overlaySize();
int overlayAlpha();
COLORREF overlayColor();
bool overlayShowFps();
