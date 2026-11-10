// A system action menu in the app's colours.
#pragma once

typedef struct {
  const char *label;
  void (*run)(void);
} MenuItem;

// Labels must stay valid while the menu is open.
void menu_open(const MenuItem *items, int count);
