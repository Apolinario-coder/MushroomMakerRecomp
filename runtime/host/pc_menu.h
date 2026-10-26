#ifndef PC_MENU_H
#define PC_MENU_H
#include <windows.h>
int pc_menu_is_open(void);
int pc_menu_blocks_input(void);
void pc_menu_attach(HWND owner);
void pc_menu_toggle(void);
void pc_menu_pad_buttons(unsigned long long buttons);
int pc_menu_key(UINT msg, WPARAM key, LPARAM flags);
void pc_menu_wait(void);
float pc_menu_audio_gain(void);
const char *pc_menu_language(void);
int pc_menu_show_fps(void);
int pc_menu_upscaler(void);
int pc_menu_smaa(void);
int pc_menu_prompt_style(void);
int pc_menu_framegen(void);
float pc_menu_output_fps(void);
void pc_menu_note_present(void);
int pc_menu_dynamic_resolution(void);
int pc_menu_dynamic_min(void);
void pc_menu_viewport(int width, int height, RECT *out);
int pc_menu_mod_path(const char *relative, char *out, int capacity);
int pc_menu_mod_roots(const char *relative, char roots[][260], int capacity);
int pc_menu_selftest(void);
#endif
