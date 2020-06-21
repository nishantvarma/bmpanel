/*
 * Copyright (C) 2008 nsf
 */

#ifndef BMPANEL_RENDER_H
#define BMPANEL_RENDER_H

#include "bmpanel.h"
#include "common.h"
#include "theme.h"
#include <Imlib2.h>
#include <X11/Xlib.h>

void init_render(struct xinfo *X, struct panel *P);
void shutdown_render();

void render_update_panel_positions(struct panel *p);
void render_switcher(struct desktop *d);
void render_taskbar(struct task *t, struct desktop *d);
int render_clock();
void render_panel(struct panel *p);
void render_present();

#endif
