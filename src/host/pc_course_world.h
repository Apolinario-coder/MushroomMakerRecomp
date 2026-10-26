#ifndef PC_COURSE_WORLD_H
#define PC_COURSE_WORLD_H
#include "runtime.h"
int pc_course_world_dispatch(Ctx *c, uint64_t addr);
void pc_course_world_mouse_wheel(int delta);
void pc_course_world_mouse_cancel(void);
int pc_course_world_mouse_captured(void);
int pc_course_world_pause_open(void);
#endif
