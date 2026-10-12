/* project_manager module: shared between its files only. */
#ifndef CWED_PM_H
#define CWED_PM_H

#include "cwed/editor.h"

/* the engine checkout the native editor runs tools from ("" in a cart) */
const char *pm_engine_dir(void);
/* the project file's value for key in the CURRENT project ("" when absent) */
const char *pm_get(const char *key);

void pm_settings_init(void);  /* settings.c: project.set/get/settings + the Project Settings panel */
void pm_settings_frame(void);

#endif
