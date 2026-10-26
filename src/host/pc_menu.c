/* PC options: an owned, double-buffered native window, independent of guest rendering. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pc_menu.h"
#include "gfx_options.h"
#include "nnpad.h"

enum { CREAM=0xf5fafc, INK=0x252525, YELLOW=0x32ceff, ORANGE=0x0059ef, WHITE=0xffffff, MUTED=0x6c6c6c };
enum { ASPECT, FULLSCREEN, FPS, RESOLUTION, UPSCALER, AA, VSYNC, ANISO, VOLUME, LANGUAGE, LAYOUT,
       RUMBLE, STRENGTH, DEADZONE, OUTER, TRIGGER, PLAYER, MOD_FOLDER, MOD_SCAN, RESTART, DESKTOP, DEVICE, OUTPUT_FPS, FRAMEGEN, DYNAMIC_RES, DYNAMIC_MIN, PROMPTS, BIND=100 };
typedef struct { int id; const wchar_t *name, *hint; } Row;
static const Row controls[] = {
 {LANGUAGE,L"Game language",L"Uses the languages included in your game files. Restart required."},
 {PROMPTS,L"Button icons",L"Choose automatic, keyboard and mouse, Xbox, PlayStation or Nintendo button prompts."},
 {LAYOUT,L"Controller layout",L"Use Nintendo button positions or the labels printed on your controller. Reloads in about a second."},
 {PLAYER,L"Keyboard player",L"Share player 1 or use the first free slot. Restart required."},
 {DEVICE,L"Edit bindings for",L"Choose a keyboard or controller, then scroll to the action you want to change."},
 {DEADZONE,L"Stick dead zone",L"Ignore small, unintended stick movements."},
 {OUTER,L"Outer stick range",L"Stick travel needed to reach full speed."},
 {TRIGGER,L"Trigger threshold",L"Trigger travel needed to press ZL and ZR."},
 {RUMBLE,L"Rumble",L"Enable rumble on supported controllers."},
 {STRENGTH,L"Rumble strength",L"Set the strength of controller vibration."}};
static const Row video[] = {
 {ASPECT,L"Aspect ratio",L"Original keeps 16:9 with bars. Fill stretches to the window. Applies immediately."},
 {FULLSCREEN,L"Fullscreen",L"Use borderless fullscreen. Shortcut: F11 or Alt+Enter."},
 {FPS,L"Show FPS",L"Show a green number over the game, including fullscreen and background windows. With frame generation, this shows output FPS."},
 {RESOLUTION,L"Internal resolution",L"Handheld mode: 100% is 720p, 200% is 1440p, 300% is 2160p/4K. Restart required."},
 {UPSCALER,L"Upscaling filter",L"Choose bilinear, FSR 1 or NIS. Upscaling applies when internal resolution is lower than the window."},
 {AA,L"Anti-aliasing",L"SMAA 1x smooths edges before upscaling. Applies immediately with Vulkan."},
 {VSYNC,L"Vertical sync",L"Synchronize presentation with your display. Simulation stays at 60 Hz. Restart required."},
 {ANISO,L"Anisotropic filtering",L"Improve textures viewed at an angle. Restart required."},
 {OUTPUT_FPS,L"Presentation limit",L"Original 60 or up to 120 with 2x interpolation. Simulation stays at 60 Hz."},
 {FRAMEGEN,L"Frame generation",L"Experimental Vulkan motion or blending. Adds latency and may produce visual artifacts."},
 {DYNAMIC_RES,L"Dynamic resolution",L"Vulkan adjusts internal targets using GPU time. CPU bottlenecks remain. Restart required."},
 {DYNAMIC_MIN,L"Minimum dynamic resolution",L"Set the lower limit. The internal resolution above is the upper limit. Restart required."}};
static const Row audio[] = {{VOLUME,L"Master volume",L"Game volume from 0% to 200%. Values above 100% can cause clipping."}};
static const Row mods[] = {
 {MOD_FOLDER,L"Open mods folder",L"Open the mods folder in File Explorer."},
 {MOD_SCAN,L"Refresh list",L"Find mod folders containing a romfs folder. Executable mods are not loaded."}};
static const Row quit[] = {
 {RESTART,L"Return to title screen",L"Restart the game. Unsaved progress will be lost."},
 {DESKTOP,L"Quit to desktop",L"Close the game. Unsaved progress will be lost."}};
static const wchar_t *const tab_names[] = {L"Controls", L"Video", L"Audio", L"Mods", L"Quit"};
static const char *const key_names[] = {"A","B","X","Y","STICK_L","STICK_R","L","R","ZL","ZR","PLUS","MINUS",
 "LEFT","UP","RIGHT","DOWN","LSTICK_LEFT","LSTICK_UP","LSTICK_RIGHT","LSTICK_DOWN","RSTICK_LEFT","RSTICK_UP","RSTICK_RIGHT","RSTICK_DOWN"};
static const wchar_t *const key_labels[] = {L"A · Confirm",L"B · Jump / back",L"X",L"Y · Run / hold",L"Left stick click",L"Right stick click",
 L"L",L"R",L"ZL",L"ZR",L"+ · Game menu",L"−",L"D-pad left",L"D-pad up",L"D-pad right",L"D-pad down",
 L"Move left",L"Move up",L"Move right",L"Move down",L"Right stick left",L"Right stick up",L"Right stick right",L"Right stick down"};
static const char *const languages[] = {"", "en-US","es","fr","de","it","ja","ko","nl","ru","zh-Hans","zh-Hant"};
static const wchar_t *const language_labels[] = {L"Automatic",L"English",L"Spanish",L"French",L"German",L"Italian",L"Japanese",L"Korean",L"Dutch",L"Russian",L"Simplified Chinese",L"Traditional Chinese"};
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static volatile LONG g_open, g_blocked;
static volatile LONG g_volume=100, g_aspect=1, g_fps, g_upscaler=1, g_aa=1, g_framegen, g_prompt_style;
static int g_dynamic_run, g_dynamic_min;
static HWND g_owner, g_window;
static int g_headless;
static ULONGLONG g_test_start;
static char g_settings[MAX_PATH], g_controllers[MAX_PATH], g_mods[MAX_PATH], g_language[16];
static int g_tab, g_selected, g_scroll, g_capture=-1, g_ask, g_yes, g_dirty, g_mod_count;
static int g_bind_pad, g_wait_pad;
typedef struct { char name[128], root[MAX_PATH]; int enabled, priority; } Mod;
static Mod g_mod_list[64], g_loaded_mods[64];
static int g_loaded_count;
static INIT_ONCE g_mod_once=INIT_ONCE_STATIC_INIT;
static ULONGLONG g_capture_until;
static wchar_t g_message[180];
static struct { RECT rect; int kind, value; } g_hits[48];
static int g_nhit;
static float g_scale=1;
static int g_origin_x,g_origin_y;

static BOOL CALLBACK settings_load(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once; (void)param; (void)context;
    const GfxOptions *go=gfx_options();
    snprintf(g_settings,sizeof g_settings,"%s",go->path);
    char *s=strrchr(g_settings,'\\'), *t=strrchr(g_settings,'/'); if(t && (!s || t>s))s=t;
    if(s)*s=0;
    snprintf(g_controllers,sizeof g_controllers,"%s/controllers.ini",g_settings);
    nnpad_config_path(g_controllers);
    char dir[MAX_PATH];snprintf(dir,sizeof dir,"%s",g_settings);
    snprintf(g_settings,sizeof g_settings,"%s/pc-menu.ini",dir);
    char exe[MAX_PATH];GetModuleFileNameA(NULL,exe,sizeof exe);s=strrchr(exe,'\\');if(s)*s=0;
    snprintf(g_mods,sizeof g_mods,"%s/../../mods",exe);
    const char *mods_env=getenv("SSBU_MODS");if(mods_env && mods_env[0])snprintf(g_mods,sizeof g_mods,"%s",mods_env);
    g_volume=GetPrivateProfileIntA("audio","volume",100,g_settings);if(g_volume<0 || g_volume>200)g_volume=100;
    g_prompt_style=GetPrivateProfileIntA("controls","prompt_style",0,g_settings);if(g_prompt_style<0||g_prompt_style>4)g_prompt_style=0;
    g_aspect=!!GetPrivateProfileIntA("video","original_aspect",1,g_settings);
    g_fps=!!GetPrivateProfileIntA("video","show_fps",go->overlay,g_settings);
    g_upscaler=GetPrivateProfileIntA("video","upscaler",1,g_settings);if(g_upscaler<0 || g_upscaler>2)g_upscaler=1;
    g_framegen=GetPrivateProfileIntA("video","frame_generation",1,g_settings);if(g_framegen<0 || g_framegen>2)g_framegen=0;
    g_dynamic_run=!!GetPrivateProfileIntA("video","dynamic_resolution",0,g_settings);
    g_dynamic_min=GetPrivateProfileIntA("video","dynamic_min_percent",50,g_settings);if(g_dynamic_min<25 || g_dynamic_min>100)g_dynamic_min=50;
    g_aa=!!GetPrivateProfileIntA("video","smaa",1,g_settings);
    GetPrivateProfileStringA("system","language","",g_language,sizeof g_language,g_settings);
    int valid=0;for(int i=0;i<12;i++)if(!strcmp(g_language,languages[i]))valid=1;
    if(!valid)g_language[0]=0;
    return TRUE;
}
static void init(void){InitOnceExecuteOnce(&g_once,settings_load,NULL,NULL);}
int pc_menu_is_open(void){return g_open!=0;}
int pc_menu_blocks_input(void){return g_blocked!=0;}
void pc_menu_wait(void){while(g_open)Sleep(5);}
float pc_menu_audio_gain(void){init();return (g_open)?0:(float)g_volume/100;}
const char *pc_menu_language(void){init();return g_language;}
int pc_menu_show_fps(void){init();return g_fps;}
int pc_menu_upscaler(void){init();return g_upscaler;}
int pc_menu_smaa(void){init();return g_aa;}
int pc_menu_prompt_style(void){init();return g_prompt_style;}
int pc_menu_framegen(void){init();return g_framegen;}
static volatile LONG g_output_fps;
float pc_menu_output_fps(void){return g_output_fps/10.0f;}
void pc_menu_note_present(void){
    static LARGE_INTEGER frequency;static LONGLONG start;static int frames;
    LARGE_INTEGER now;QueryPerformanceCounter(&now);if(!frequency.QuadPart)QueryPerformanceFrequency(&frequency);
    if(!start)start=now.QuadPart;frames++;
    if(now.QuadPart-start>=frequency.QuadPart){
        InterlockedExchange(&g_output_fps,(LONG)((double)frames*frequency.QuadPart*10/(now.QuadPart-start)));
        if(getenv("SSBU_PC_VIDEO_TEST"))printf("PC video output: %.1f fps, frame generation mode %ld\n",pc_menu_output_fps(),g_framegen);
        start=now.QuadPart;frames=0;
    }
}
int pc_menu_dynamic_resolution(void){init();return g_dynamic_run;}
int pc_menu_dynamic_min(void){init();return g_dynamic_min;}
void pc_menu_viewport(int w,int h,RECT *r){
    init();*r=(RECT){0,0,w,h};if(!g_aspect || w<=0 || h<=0)return;
    if((long long)w*9>(long long)h*16){int n=h*16/9;r->left=(w-n)/2;r->right=r->left+n;}
    else {int n=w*9/16;r->top=(h-n)/2;r->bottom=r->top+n;}
}
/* graphics.ini deliberately has no section headers; Windows profile APIs require them. */
static int flat_key(char *line,const char *key){
    while(*line==' '||*line=='\t')line++;size_t n=strlen(key);
    if(_strnicmp(line,key,n))return 0;line+=n;while(*line==' '||*line=='\t')line++;return *line=='=';
}
static void graphics_get(const char *key,const char *fallback,char *out,int cap){
    snprintf(out,cap,"%s",fallback);FILE *f=fopen(gfx_options()->path,"rb");if(!f)return;
    char line[2048];while(fgets(line,sizeof line,f))if(flat_key(line,key)){
        char *v=strchr(line,'=')+1;while(*v==' '||*v=='\t')v++;v[strcspn(v,";#\r\n")]=0;
        char *e=v+strlen(v);while(e>v && (e[-1]==' '||e[-1]=='\t'))*--e=0;snprintf(out,cap,"%s",v);
    }fclose(f);
}
static int graphics_save(const char *key,const char *value){
    const char *path=gfx_options()->path;char tmp[MAX_PATH+40];snprintf(tmp,sizeof tmp,"%s.%lu.tmp",path,GetCurrentProcessId());
    FILE *in=fopen(path,"rb");if(!in && GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES)return 0;
    FILE *out=fopen(tmp,"wbx");if(!out){if(in)fclose(in);return 0;}
    int found=0,ok=1,profile=0,custom=!strcmp(key,"resolution_scale")||!strcmp(key,"vsync")||!strcmp(key,"anisotropic_filtering");char line[2048];
    if(in){while(fgets(line,sizeof line,in)){if(flat_key(line,key)){if(fprintf(out,"%s = %s\r\n",key,value)<0)ok=0;found=1;}else if(custom && flat_key(line,"profile")){if(fputs("profile = custom\r\n",out)<0)ok=0;profile=1;}else if(fputs(line,out)<0)ok=0;}if(ferror(in))ok=0;fclose(in);}
    if(!found && fprintf(out,"\r\n%s = %s\r\n",key,value)<0)ok=0;
    if(custom && !profile && fputs("\r\nprofile = custom\r\n",out)<0)ok=0;
    if(fflush(out))ok=0;if(fclose(out))ok=0;
    if(ok)ok=MoveFileExA(tmp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok)DeleteFileA(tmp);return ok;
}
static int save(const char *path,const char *section,const char *key,const char *value){
    int ok=!section[0]?graphics_save(key,value):WritePrivateProfileStringA(section,key,value,path);
    if(!ok){swprintf(g_message,180,L"Couldn't save. Error %lu.",GetLastError());return 0;}
    wcscpy(g_message,L"Settings saved.");return 1;
}
static int save_int(const char *section,const char *key,int value){char b[24];snprintf(b,sizeof b,"%d",value);return save(g_settings,section,key,b);}
static int rows(const Row **out){
    switch(g_tab){case 0:*out=controls;return sizeof controls/sizeof *controls;case 1:*out=video;return sizeof video/sizeof *video;case 2:*out=audio;return 1;case 3:*out=mods;return 2;default:*out=quit;return 2;}
}
static int mod_compare(const void *a,const void *b){const Mod *x=a,*y=b;return x->priority!=y->priority?(x->priority>y->priority?1:-1):_stricmp(x->name,y->name);}
static int scan_mods(Mod *list){
    int count=0;char pattern[MAX_PATH];snprintf(pattern,sizeof pattern,"%s/*",g_mods);
    WIN32_FIND_DATAA data;HANDLE h=FindFirstFileA(pattern,&data);
    if(h!=INVALID_HANDLE_VALUE){do{if((data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) && data.cFileName[0]!='.' && !strpbrk(data.cFileName,"[]=;#") && strlen(data.cFileName)<128 && count<64){
        char path[MAX_PATH];snprintf(path,sizeof path,"%s/%s/romfs",g_mods,data.cFileName);
        DWORD attr=GetFileAttributesA(path);
        if(attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_DIRECTORY) && !(attr&FILE_ATTRIBUTE_REPARSE_POINT)){
            Mod *mod=&list[count++];snprintf(mod->name,sizeof mod->name,"%s",data.cFileName);snprintf(mod->root,sizeof mod->root,"%s",path);
            mod->enabled=!!GetPrivateProfileIntA("mods",mod->name,1,g_settings);mod->priority=GetPrivateProfileIntA("mod_priority",mod->name,1000,g_settings);
        }
    }}while(FindNextFileA(h,&data));FindClose(h);}
    qsort(list,count,sizeof *list,mod_compare);return count;
}
static void rescan(void){g_mod_count=scan_mods(g_mod_list);}
static BOOL CALLBACK load_mods(PINIT_ONCE once,PVOID param,PVOID *context){(void)once;(void)param;(void)context;init();g_loaded_count=scan_mods(g_loaded_mods);return TRUE;}
int pc_menu_mod_path(const char *relative,char *out,int capacity){
    InitOnceExecuteOnce(&g_mod_once,load_mods,NULL,NULL);
    for(int i=0;i<g_loaded_count;i++)if(g_loaded_mods[i].enabled){
        int n=snprintf(out,capacity,"%s/%s",g_loaded_mods[i].root,relative);
        if(n>0 && n<capacity && GetFileAttributesA(out)!=INVALID_FILE_ATTRIBUTES)return 1;
    }return 0;
}
int pc_menu_mod_roots(const char *relative,char roots[][260],int capacity){
    InitOnceExecuteOnce(&g_mod_once,load_mods,NULL,NULL);int n=0;
    for(int i=0;i<g_loaded_count && n<capacity;i++)if(g_loaded_mods[i].enabled){
        int size=snprintf(roots[n],260,"%s/%s",g_loaded_mods[i].root,relative);DWORD attr=size>0&&size<260?GetFileAttributesA(roots[n]):INVALID_FILE_ATTRIBUTES;
        if(attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_DIRECTORY))n++;
    }return n;
}
static float read_float(const char *path,const char *section,const char *key,float fallback){char b[32],d[32];snprintf(d,sizeof d,"%.2f",fallback);if(!section[0])graphics_get(key,d,b,sizeof b);else GetPrivateProfileStringA(section,key,d,b,sizeof b,path);return (float)atof(b);}
static void key_text(int vk,wchar_t *out,int cap){
    if(!vk){wcsncpy(out,L"Unbound",cap);return;}if(vk==VK_LBUTTON){wcsncpy(out,L"Left mouse",cap);return;}
    if(vk==VK_RBUTTON){wcsncpy(out,L"Right mouse",cap);return;}if(vk==VK_MBUTTON){wcsncpy(out,L"Middle mouse",cap);return;}
    LONG scan=MapVirtualKeyW(vk,MAPVK_VK_TO_VSC)<<16;
    if(vk==VK_LEFT||vk==VK_UP||vk==VK_RIGHT||vk==VK_DOWN)scan|=1<<24;
    if(!GetKeyNameTextW(scan,out,cap))swprintf(out,cap,L"0x%02X",vk);
}
static void value_text(int id,wchar_t *out,int cap){
    const GfxOptions *go=gfx_options();char b[80];out[0]=0;
    switch(id){
    case ASPECT:wcsncpy(out,g_aspect?L"Original · 16:9":L"Fill window",cap);break;
    case FULLSCREEN:wcsncpy(out,(g_owner?(GetWindowLongPtr(g_owner,GWL_STYLE)&WS_OVERLAPPEDWINDOW)!=0:!go->window_mode)?L"Off":L"On",cap);break;
    case FPS:wcsncpy(out,g_fps?L"On":L"Off",cap);break;
    case UPSCALER:wcsncpy(out,g_upscaler==0?L"Bilinear":g_upscaler==1?L"AMD FSR 1":L"NVIDIA NIS",cap);break;
    case OUTPUT_FPS:wcsncpy(out,g_framegen?L"120 · 2x interpolation":L"60 · original",cap);break;
    case FRAMEGEN:wcsncpy(out,g_framegen==1?L"2x · motion":g_framegen==2?L"2x · blend":L"Off",cap);break;
    case DYNAMIC_RES:wcsncpy(out,GetPrivateProfileIntA("video","dynamic_resolution",0,g_settings)?L"On · restart":L"Off",cap);break;
    case DYNAMIC_MIN:swprintf(out,cap,L"%u%%",GetPrivateProfileIntA("video","dynamic_min_percent",50,g_settings));break;
    case AA:wcsncpy(out,g_aa?L"SMAA 1x":L"Off",cap);break;
    case RESOLUTION:{float scale=read_float(go->path,"","resolution_scale",go->scale);int height=(int)((go->handheld?720:1080)*scale+.5f);swprintf(out,cap,L"%dp%ls · %.0f%%",height,height==2160?L" / 4K":L"",scale*100);break;}
    case VSYNC:graphics_get("vsync","auto",b,sizeof b);MultiByteToWideChar(CP_UTF8,0,b,-1,out,cap);break;
    case ANISO:graphics_get("anisotropic_filtering","0",b,sizeof b);if(atoi(b))swprintf(out,cap,L"%dx",atoi(b));else wcsncpy(out,L"Original",cap);break;
    case VOLUME:swprintf(out,cap,L"%ld%%",g_volume);break;
    case LANGUAGE:{int i=0;for(int k=0;k<12;k++)if(!strcmp(g_language,languages[k]))i=k;wcsncpy(out,language_labels[i],cap);break;}
    case PROMPTS:{const wchar_t *names[]={L"Automatic",L"Keyboard and mouse",L"Xbox",L"PlayStation",L"Nintendo"};wcsncpy(out,names[g_prompt_style],cap);break;}
    case LAYOUT:GetPrivateProfileStringA("general","layout","auto",b,sizeof b,g_controllers);wcsncpy(out,!strcmp(b,"labels")?L"Button labels":!strcmp(b,"positional")?L"Nintendo positions":L"Automatic",cap);break;
    case PLAYER:GetPrivateProfileStringA("keyboard","player","1",b,sizeof b,g_controllers);swprintf(out,cap,!strcmp(b,"auto")?L"Automatic":L"Player 1");break;
    case DEVICE:wcsncpy(out,g_bind_pad?L"Player 1 controller":L"Keyboard and mouse",cap);break;
    case RUMBLE:GetPrivateProfileStringA("general","rumble","on",b,sizeof b,g_controllers);wcsncpy(out,!strcmp(b,"off")?L"Off":L"On",cap);break;
    case STRENGTH:swprintf(out,cap,L"%.0f%%",read_float(g_controllers,"general","rumble_strength",1)*100);break;
    case DEADZONE:swprintf(out,cap,L"%.0f%%",read_float(g_controllers,"general","deadzone",0.1f)*100);break;
    case OUTER:swprintf(out,cap,L"%.0f%%",read_float(g_controllers,"general","outer_zone",0.98f)*100);break;
    case TRIGGER:swprintf(out,cap,L"%.0f%%",read_float(g_controllers,"general","trigger_threshold",0.33f)*100);break;
    default:if(id>=BIND){
        if(g_bind_pad){char name[80];nnpad_menu_binding_name(id-BIND,name,sizeof name);MultiByteToWideChar(CP_UTF8,0,name,-1,out,cap);}
        else {int used=0;for(int k=0;k<4;k++){int vk=nnpad_kb_vk(id-BIND,k);if(vk){wchar_t name[40];key_text(vk,name,40);used+=swprintf(out+used,cap-used,k?L" / %ls":L"%ls",name);if(used<0 || used>=cap-1)break;}}}
    }else wcsncpy(out,L"Open  ›",cap);break;
    }
}
static WINDOWPLACEMENT g_placement={sizeof(WINDOWPLACEMENT)};
static void fullscreen(void){
    if(!g_owner)return;InterlockedExchange(&g_blocked,1);
    DWORD style=(DWORD)GetWindowLongPtr(g_owner,GWL_STYLE);
    if(style&WS_OVERLAPPEDWINDOW){
        GetWindowPlacement(g_owner,&g_placement);MONITORINFO mi={sizeof mi};GetMonitorInfo(MonitorFromWindow(g_owner,MONITOR_DEFAULTTONEAREST),&mi);
        SetWindowLongPtr(g_owner,GWL_STYLE,style&~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_owner,NULL,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED|SWP_NOZORDER);
        save(gfx_options()->path,"","window","borderless");
    }else{SetWindowLongPtr(g_owner,GWL_STYLE,style|WS_OVERLAPPEDWINDOW);SetWindowPlacement(g_owner,&g_placement);SetWindowPos(g_owner,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_FRAMECHANGED);save(gfx_options()->path,"","window","windowed");}
}
static void change(int id,int direction){
    char b[80];int v;float f;
    if(id>=1000){
        int i=(id-1000)/2;if(i>=g_mod_count)return;Mod *mod=&g_mod_list[i];
        if((id-1000)%2){int other=i+(direction>0?1:-1);if(other>=0 && other<g_mod_count){Mod temp=g_mod_list[i];g_mod_list[i]=g_mod_list[other];g_mod_list[other]=temp;for(int k=0;k<g_mod_count;k++){char value[16];snprintf(value,sizeof value,"%d",k);if(!save(g_settings,"mod_priority",g_mod_list[k].name,value))return;}g_selected=2+other*2+1;g_dirty=1;}}
        else {if(save_int("mods",mod->name,!mod->enabled)){mod->enabled=!mod->enabled;g_dirty=1;}}
        return;
    }
    if(id>=BIND){
        if(g_bind_pad && (id-BIND>=16 || !nnpad_menu_capture_begin())){wcscpy(g_message,L"Connect a controller for player 1. Stick axes keep their physical mapping.");return;}
        g_capture=id-BIND;g_capture_until=GetTickCount64()+5000;return;
    }
    switch(id){
    case ASPECT:if(save_int("video","original_aspect",!g_aspect))InterlockedExchange(&g_aspect,!g_aspect);break;
    case FULLSCREEN:fullscreen();break;
    case FPS:if(save_int("video","show_fps",!g_fps))InterlockedExchange(&g_fps,!g_fps);break;
    case UPSCALER:v=(g_upscaler+direction+3)%3;if(save_int("video","upscaler",v))InterlockedExchange(&g_upscaler,v);break;
    case OUTPUT_FPS:v=g_framegen?0:1;if(save_int("video","frame_generation",v))InterlockedExchange(&g_framegen,v);break;
    case FRAMEGEN:v=(g_framegen+direction+3)%3;if(save_int("video","frame_generation",v))InterlockedExchange(&g_framegen,v);break;
    case DYNAMIC_RES:v=!GetPrivateProfileIntA("video","dynamic_resolution",0,g_settings);if(save_int("video","dynamic_resolution",v))g_dirty=1;break;
    case DYNAMIC_MIN:v=GetPrivateProfileIntA("video","dynamic_min_percent",50,g_settings)+direction*5;if(v<25)v=25;if(v>100)v=100;if(save_int("video","dynamic_min_percent",v))g_dirty=1;break;
    case AA:if(save_int("video","smaa",!g_aa))InterlockedExchange(&g_aa,!g_aa);break;
    case VOLUME:v=g_volume+direction*5;if(v<0)v=0;if(v>200)v=200;if(save_int("audio","volume",v))InterlockedExchange(&g_volume,v);break;
    case LANGUAGE:v=0;for(int i=0;i<12;i++)if(!strcmp(languages[i],g_language))v=i;v=(v+direction+12)%12;if(save(g_settings,"system","language",languages[v])){strcpy(g_language,languages[v]);g_dirty=1;}break;
    case RESOLUTION:{float choices[]={.25f,.5f,.75f,1,1.25f,1.5f,2,3};f=read_float(gfx_options()->path,"","resolution_scale",gfx_options()->scale);v=3;for(int i=0;i<8;i++)if(fabsf(f-choices[i])<.01f)v=i;v=(v+direction+8)%8;snprintf(b,sizeof b,"%.2f",choices[v]);if(save(gfx_options()->path,"","resolution_scale",b))g_dirty=1;break;}
    case VSYNC:{const char *choices[]={"auto","clock","on","off","mailbox"};graphics_get("vsync","auto",b,sizeof b);v=0;for(int i=0;i<5;i++)if(!strcmp(b,choices[i]))v=i;v=(v+direction+5)%5;if(save(gfx_options()->path,"","vsync",choices[v]))g_dirty=1;break;}
    case ANISO:{int a[]={0,2,4,8,16};v=0;graphics_get("anisotropic_filtering","0",b,sizeof b);int now=atoi(b);for(int i=0;i<5;i++)if(a[i]==now)v=i;snprintf(b,sizeof b,"%d",a[(v+direction+5)%5]);if(save(gfx_options()->path,"","anisotropic_filtering",b))g_dirty=1;break;}
    case PROMPTS:v=(g_prompt_style+direction+5)%5;if(save_int("controls","prompt_style",v))InterlockedExchange(&g_prompt_style,v);break;
    case LAYOUT:GetPrivateProfileStringA("general","layout","auto",b,sizeof b,g_controllers);save(g_controllers,"general","layout",!strcmp(b,"auto")?"positional":!strcmp(b,"positional")?"labels":"auto");break;
    case PLAYER:GetPrivateProfileStringA("keyboard","player","1",b,sizeof b,g_controllers);if(save(g_controllers,"keyboard","player",!strcmp(b,"1")?"auto":"1"))g_dirty=1;break;
    case DEVICE:g_bind_pad=!g_bind_pad;break;
    case RUMBLE:GetPrivateProfileStringA("general","rumble","on",b,sizeof b,g_controllers);save(g_controllers,"general","rumble",!strcmp(b,"off")?"on":"off");break;
    case DEADZONE:case OUTER:case TRIGGER:case STRENGTH:{const char *key=id==DEADZONE?"deadzone":id==OUTER?"outer_zone":id==TRIGGER?"trigger_threshold":"rumble_strength";float def=id==DEADZONE?.1f:id==OUTER?.98f:id==TRIGGER?.33f:1;f=read_float(g_controllers,"general",key,def)+direction*.05f;float low=id==OUTER?.5f:id==TRIGGER?.05f:0,high=id==DEADZONE?.4f:id==STRENGTH?2:1;if(f<low)f=low;if(f>high)f=high;snprintf(b,sizeof b,"%.2f",f);save(g_controllers,"general",key,b);break;}
    case MOD_FOLDER:CreateDirectoryA(g_mods,NULL);ShellExecuteA(g_window,"open",g_mods,NULL,NULL,SW_SHOWNORMAL);break;
    case MOD_SCAN:rescan();swprintf(g_message,180,L"Found %d folders containing romfs.",g_mod_count);break;
    case RESTART:case DESKTOP:g_ask=id;g_yes=0;break;
    }
    InvalidateRect(g_window,NULL,FALSE);
}
static void round_rect(HDC dc,RECT r,COLORREF fill,COLORREF border,int radius){
    HBRUSH br=CreateSolidBrush(fill);HPEN pen=CreatePen(PS_SOLID,1,border);
    HGDIOBJ ob=SelectObject(dc,br),op=SelectObject(dc,pen);RoundRect(dc,r.left,r.top,r.right,r.bottom,radius,radius);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(br);DeleteObject(pen);
}
static void text(HDC dc,RECT r,const wchar_t *s,int size,int weight,COLORREF color,UINT flags){
    HFONT font=CreateFontW(-(int)(size*g_scale),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HGDIOBJ old=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);DrawTextW(dc,s,-1,&r,flags);SelectObject(dc,old);DeleteObject(font);
}
static RECT box(int x,int y,int w,int h){return(RECT){g_origin_x+(LONG)(x*g_scale),g_origin_y+(LONG)(y*g_scale),g_origin_x+(LONG)((x+w)*g_scale),g_origin_y+(LONG)((y+h)*g_scale)};}
static void hit(RECT r,int kind,int value){if(g_nhit<48){g_hits[g_nhit].rect=r;g_hits[g_nhit].kind=kind;g_hits[g_nhit++].value=value;}}
static void paint(HDC dc,int w,int h){
    g_scale=fminf(w/1280.0f,h/720.0f);g_nhit=0;
    g_origin_x=(w-(int)(1280*g_scale))/2;g_origin_y=(h-(int)(720*g_scale))/2;
    RECT all={0,0,w,h};HBRUSH bg=CreateSolidBrush(YELLOW);FillRect(dc,&all,bg);DeleteObject(bg);
    HPEN grid=CreatePen(PS_SOLID,1,RGB(236,184,37));HGDIOBJ old=SelectObject(dc,grid);
    int tile=(int)(32*g_scale);if(tile<1)tile=1;for(int x=0;x<w;x+=tile){MoveToEx(dc,x,0,NULL);LineTo(dc,x,h);}for(int y=0;y<h;y+=tile){MoveToEx(dc,0,y,NULL);LineTo(dc,w,y);}SelectObject(dc,old);DeleteObject(grid);
    RECT header=box(36,25,1208,94);round_rect(dc,header,INK,INK,18);
    text(dc,box(58,35,790,38),L"SUPER MARIO MAKER 2",30,FW_BLACK,YELLOW,DT_SINGLELINE|DT_VCENTER);
    text(dc,box(60,77,790,25),L"PC SETTINGS  /  WORKSHOP",14,FW_BOLD,WHITE,DT_SINGLELINE|DT_VCENTER);
    RECT close=box(1160,43,58,58);round_rect(dc,close,WHITE,WHITE,16);text(dc,close,L"×",30,FW_BOLD,INK,DT_CENTER|DT_VCENTER|DT_SINGLELINE);hit(close,3,0);
    for(int i=0;i<5;i++){
        RECT r=box(36+i*244,135,232,55);round_rect(dc,r,i==g_tab?ORANGE:WHITE,i==g_tab?ORANGE:WHITE,15);
        text(dc,r,tab_names[i],21,FW_BOLD,i==g_tab?WHITE:INK,DT_CENTER|DT_VCENTER|DT_SINGLELINE);hit(r,0,i);
    }
    RECT body=box(36,206,1208,434);round_rect(dc,body,CREAM,WHITE,20);
    const Row *list;int count=rows(&list),total=count+(g_tab==0?25:g_tab==3?g_mod_count*2:0);
    if(g_selected>=total)g_selected=total-1;if(g_selected<0)g_selected=0;
    if(g_selected<g_scroll)g_scroll=g_selected;if(g_selected>=g_scroll+7)g_scroll=g_selected-6;
    if(g_scroll>total-7)g_scroll=total>7?total-7:0;
    const wchar_t *hint=L"";
    for(int i=g_scroll;i<total && i<g_scroll+7;i++){
        int id=i<count?list[i].id:g_tab==3?1000+i-count:(i-count<24?BIND+i-count:-1);
        wchar_t mod_label[180];if(g_tab==3 && i>=count){wchar_t name[128];MultiByteToWideChar(CP_ACP,0,g_mod_list[(i-count)/2].name,-1,name,128);swprintf(mod_label,180,(i-count)%2?L"Priority · %ls":L"%ls",name);}
        const wchar_t *label=i<count?list[i].name:g_tab==3?mod_label:i-count<24?key_labels[i-count]:g_bind_pad?L"Reset controller bindings":L"Reset keyboard bindings";
        RECT r=box(55,220+(i-g_scroll)*52,1170,46);
        round_rect(dc,r,i==g_selected?YELLOW:WHITE,i==g_selected?ORANGE:WHITE,10);
        wchar_t value[128];if(g_tab==3 && i>=count){if((i-count)%2)swprintf(value,128,L"%d   ←   →",(i-count)/2+1);else wcscpy(value,g_mod_list[(i-count)/2].enabled?L"On":L"Off");}else if(id>=0)value_text(id,value,128);else wcscpy(value,L"Reset  ›");
        if(g_capture>=0 && id==BIND+g_capture)swprintf(value,128,L"Press a button... %llu",(g_capture_until>GetTickCount64()?(g_capture_until-GetTickCount64()+999)/1000:0));
        text(dc,box(73,223+(i-g_scroll)*52,675,39),label,19,FW_SEMIBOLD,INK,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        text(dc,box(800,223+(i-g_scroll)*52,370,39),value,18,FW_BOLD,INK,DT_SINGLELINE|DT_RIGHT|DT_VCENTER|DT_END_ELLIPSIS);
        hit(r,1,i);
        if((id>=ASPECT && id<=PLAYER) || (id>=OUTPUT_FPS && id<=DYNAMIC_MIN) || id==PROMPTS || id==DEVICE || (id>=1000 && id%2)){
            RECT left=box(756,224+(i-g_scroll)*52,32,36),right=box(1185,224+(i-g_scroll)*52,32,36);
            round_rect(dc,left,WHITE,CREAM,8);round_rect(dc,right,WHITE,CREAM,8);
            text(dc,left,L"‹",25,FW_BOLD,INK,DT_CENTER|DT_VCENTER|DT_SINGLELINE);text(dc,right,L"›",25,FW_BOLD,INK,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            hit(left,6,i);hit(right,7,i);
        }
        if(i==g_selected)hint=i<count?list[i].hint:g_tab==3?L"Mods at the top take priority. Restart to apply changes. Original files and saves stay unchanged.":L"Select an action to rebind it. Esc cancels. F1, Esc and F11 are reserved. Use the wheel or arrows to scroll.";
    }
    if(g_tab==3 && !g_mod_count)text(dc,box(73,380,1115,110),L"Add mods/MyMod/romfs/, then refresh the list. Restart to load files in priority order.",20,FW_NORMAL,MUTED,DT_WORDBREAK);
    text(dc,box(66,595,1015,34),g_capture>=0?(g_bind_pad?L"Release the controller buttons, then press the new button. Esc cancels.":L"Press a key or mouse button. Esc cancels."):g_message[0]?g_message:hint,14,FW_NORMAL,MUTED,DT_WORDBREAK);
    wchar_t position[40];swprintf(position,40,L"%d–%d / %d",g_scroll+1,g_scroll+7<total?g_scroll+7:total,total);text(dc,box(1090,604,125,25),position,13,FW_BOLD,MUTED,DT_RIGHT|DT_SINGLELINE);
    RECT footer=box(36,654,1208,46);round_rect(dc,footer,INK,INK,12);
    text(dc,box(55,660,770,32),L"Esc / Minus  Resume     Q / R  Tabs     Arrows  Adjust     Enter  Select",13,FW_SEMIBOLD,WHITE,DT_SINGLELINE|DT_VCENTER);
    RECT restart=box(856,659,370,34);text(dc,restart,g_dirty?L"Restart to apply  ›":L"Changes saved automatically",14,FW_BOLD,g_dirty?YELLOW:WHITE,DT_SINGLELINE|DT_RIGHT|DT_VCENTER);if(g_dirty)hit(restart,4,0);
    if(g_ask){
        RECT modal=box(290,235,700,250);round_rect(dc,modal,INK,WHITE,25);
        text(dc,box(320,257,640,40),g_ask==RESTART?L"Return to title screen?":L"Quit the game?",28,FW_BOLD,YELLOW,DT_SINGLELINE|DT_CENTER);
        text(dc,box(330,315,620,60),L"Unsaved progress will be lost.",20,FW_NORMAL,WHITE,DT_CENTER|DT_WORDBREAK);
        RECT no=box(330,408,290,50),yes=box(660,408,290,50);round_rect(dc,no,g_yes?WHITE:YELLOW,WHITE,14);round_rect(dc,yes,g_yes?ORANGE:WHITE,WHITE,14);
        text(dc,no,L"Keep playing",19,FW_BOLD,INK,DT_CENTER|DT_VCENTER|DT_SINGLELINE);text(dc,yes,L"Confirm",19,FW_BOLD,g_yes?WHITE:INK,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        g_nhit=0;hit(no,5,0);hit(yes,5,1);
    }
}
static void reposition(void){
    RECT r;GetClientRect(g_owner,&r);POINT p={0,0};ClientToScreen(g_owner,&p);int w=r.right<960?960:r.right,h=r.bottom<540?540:r.bottom;
    p.x-=(w-r.right)/2;p.y-=(h-r.bottom)/2;MONITORINFO mi={sizeof mi};
    if(GetMonitorInfo(MonitorFromWindow(g_owner,MONITOR_DEFAULTTONEAREST),&mi)){
        if(w>mi.rcWork.right-mi.rcWork.left)w=mi.rcWork.right-mi.rcWork.left;if(h>mi.rcWork.bottom-mi.rcWork.top)h=mi.rcWork.bottom-mi.rcWork.top;
        if(p.x<mi.rcWork.left)p.x=mi.rcWork.left;if(p.y<mi.rcWork.top)p.y=mi.rcWork.top;if(p.x+w>mi.rcWork.right)p.x=mi.rcWork.right-w;if(p.y+h>mi.rcWork.bottom)p.y=mi.rcWork.bottom-h;
    }
    SetWindowPos(g_window,HWND_TOP,p.x,p.y,w,h,SWP_NOACTIVATE);
}
static void close_menu(void){g_capture=-1;g_ask=0;InterlockedExchange(&g_open,0);ShowWindow(g_window,SW_HIDE);if(!g_headless){SetForegroundWindow(g_owner);SetFocus(g_owner);}}
static void confirm(void){
    if(!g_yes){g_ask=0;return;}
    int restart=g_ask==RESTART;g_ask=0;close_menu();
    /* Reboot uses the launcher's fresh process, preserving the profile and environment. */
    void report_and_exit(int);
    report_and_exit(restart?10:0);
}
static void reset_keyboard(void){
    if(g_bind_pad){wcscpy(g_message,nnpad_menu_reset()?L"Controller bindings reset.":L"No player 1 controller connected, or couldn't save.");return;}
    static const char *defaults[]={"ENTER, E, Z","SPACE, BACKSPACE, MOUSE_LEFT","X","SHIFT","C","V","Q","R","CTRL","F, MOUSE_RIGHT","F2","TAB","LEFT","UP","RIGHT","DOWN","A","W","D","S","J","I","L","K"};
    for(int i=0;i<24;i++)if(!save(g_controllers,"keyboard",key_names[i],defaults[i]))return;
    wcscpy(g_message,L"Keyboard bindings reset. Changes reload in about a second.");
}
static void activate(int direction){
    const Row *list;int count=rows(&list);g_message[0]=0;
    if(g_selected<count)change(list[g_selected].id,direction);
    else if(g_tab==3)change(1000+g_selected-count,direction);
    else if(g_selected-count<24)change(BIND+g_selected-count,direction);
    else reset_keyboard();
}
static void capture(int vk){
    if(vk==VK_ESCAPE){g_capture=-1;return;}
    if(vk==VK_F1||vk==VK_F11){wcscpy(g_message,L"F1 and F11 are reserved. Choose another key.");return;}
    int action=g_capture,old=nnpad_kb_vk(action,0),other=-1;
    for(int i=0;i<24;i++)if(i!=action && nnpad_kb_vk(i,0)==vk)other=i;
    char value[100];snprintf(value,sizeof value,"0x%02X",vk);
    int n=(int)strlen(value);for(int k=1;k<4;k++){int alias=nnpad_kb_vk(action,k);if(alias && alias!=vk)n+=snprintf(value+n,sizeof value-n,", 0x%02X",alias);}
    if(save(g_controllers,"keyboard",key_names[action],value)){
        if(other>=0){snprintf(value,sizeof value,"0x%02X",old);save(g_controllers,"keyboard",key_names[other],value);}
        wcscpy(g_message,L"Binding saved. Changes reload in about a second.");
    }
    g_capture=-1;
}
static void navigate(int key){
    if(g_capture>=0){if(!g_bind_pad || key==VK_ESCAPE)capture(key);return;}
    if(g_ask){if(key==VK_ESCAPE)g_ask=0;else if(key==VK_LEFT||key==VK_RIGHT||key==VK_TAB)g_yes=!g_yes;else if(key==VK_RETURN)confirm();return;}
    const Row *list;int count=rows(&list)+(g_tab==0?25:g_tab==3?g_mod_count*2:0);
    if(key==VK_ESCAPE||key==VK_F1){close_menu();return;}
    if(key=='Q'||key=='R'){g_tab=(g_tab+(key=='R'?1:4))%5;g_selected=g_scroll=0;g_message[0]=0;return;}
    if(key==VK_UP||key=='W')g_selected=(g_selected+count-1)%count;
    else if(key==VK_DOWN||key=='S')g_selected=(g_selected+1)%count;
    else if(key==VK_LEFT)activate(-1);
    else if(key==VK_RIGHT||key==VK_RETURN||key=='E'||key==VK_SPACE)activate(1);
    else if(key==VK_PRIOR)g_selected=g_selected>6?g_selected-7:0;
    else if(key==VK_NEXT)g_selected=g_selected+7<count?g_selected+7:count-1;
}
static LRESULT CALLBACK procedure(HWND h,UINT msg,WPARAM w,LPARAM l){
    switch(msg){
    case WM_ERASEBKGND:return 1;
    case WM_CLOSE:close_menu();return 0;
    case WM_KEYDOWN:case WM_SYSKEYDOWN:if(w==VK_F11 || (msg==WM_SYSKEYDOWN && w==VK_RETURN && (l&(1LL<<29)))){if(!(l&(1LL<<30)))fullscreen();}else if(!(l&(1LL<<30)) || w==VK_UP || w==VK_DOWN || w==VK_LEFT || w==VK_RIGHT)navigate((int)w);InvalidateRect(h,NULL,FALSE);return 0;
    case WM_MOUSEWHEEL:if(!g_ask && g_capture<0){navigate(GET_WHEEL_DELTA_WPARAM(w)>0?VK_UP:VK_DOWN);InvalidateRect(h,NULL,FALSE);}return 0;
    case WM_MOUSEMOVE:if(!g_ask && g_capture<0){POINT p={GET_X_LPARAM(l),GET_Y_LPARAM(l)};for(int i=g_nhit-1;i>=0;i--)if((g_hits[i].kind==1||g_hits[i].kind==6||g_hits[i].kind==7) && PtInRect(&g_hits[i].rect,p)){if(g_selected!=g_hits[i].value){g_selected=g_hits[i].value;g_message[0]=0;InvalidateRect(h,NULL,FALSE);}break;}}return 0;
    case WM_RBUTTONDOWN:if(g_capture>=0){if(!g_bind_pad)capture(VK_RBUTTON);}else if(g_ask)g_ask=0;else close_menu();InvalidateRect(h,NULL,FALSE);return 0;
    case WM_MBUTTONDOWN:if(g_capture>=0 && !g_bind_pad)capture(VK_MBUTTON);InvalidateRect(h,NULL,FALSE);return 0;
    case WM_LBUTTONDOWN:if(g_capture>=0){if(!g_bind_pad)capture(VK_LBUTTON);}else {POINT p={GET_X_LPARAM(l),GET_Y_LPARAM(l)};for(int i=g_nhit-1;i>=0;i--)if(PtInRect(&g_hits[i].rect,p)){
        int kind=g_hits[i].kind,val=g_hits[i].value;
        if(kind==0){g_tab=val;g_selected=g_scroll=0;g_message[0]=0;}
        else if(kind==1){g_selected=val;activate(1);}
        else if(kind==6||kind==7){g_selected=val;activate(kind==6?-1:1);}
        else if(kind==3)close_menu();else if(kind==4){g_ask=RESTART;g_yes=0;}else if(kind==5){g_yes=val;confirm();}
        break;
    }}InvalidateRect(h,NULL,FALSE);return 0;
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);HDC mem=CreateCompatibleDC(dc);HBITMAP bmp=CreateCompatibleBitmap(dc,r.right,r.bottom);HGDIOBJ old=SelectObject(mem,bmp);paint(mem,r.right,r.bottom);BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);EndPaint(h,&ps);return 0;}
    }
    return DefWindowProcW(h,msg,w,l);
}
static int g_prev_pad;static ULONGLONG g_repeat;
static HWND g_fps_window;
static wchar_t g_fps_text[160]=L"0";
static void fps_text(wchar_t *out,size_t cap,double fps,double output,int generated){
    swprintf(out,cap,L"%.0f",generated?output:fps);
}
static LRESULT CALLBACK fps_procedure(HWND h,UINT msg,WPARAM w,LPARAM l){
    if(msg==WM_NCHITTEST)return HTTRANSPARENT;
    if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_PAINT){
        PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);
        HBRUSH background=CreateSolidBrush(RGB(1,2,3));FillRect(dc,&r,background);DeleteObject(background);
        HFONT font=CreateFontW(-r.bottom*2/3,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Segoe UI");
        HGDIOBJ old=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,0,0));
        RECT shadow=r;shadow.left+=2;shadow.top+=2;DrawTextW(dc,g_fps_text,-1,&shadow,DT_LEFT|DT_TOP|DT_NOPREFIX|DT_SINGLELINE);
        r.left++;r.top++;SetTextColor(dc,RGB(80,255,100));DrawTextW(dc,g_fps_text,-1,&r,DT_LEFT|DT_TOP|DT_NOPREFIX|DT_SINGLELINE);
        SelectObject(dc,old);DeleteObject(font);EndPaint(h,&ps);return 0;
    }
    return DefWindowProcW(h,msg,w,l);
}
static void fps_tick(void){
    if(!g_fps_window)return;
    long long display_frame_count(void);static ULONGLONG start;static long long previous;
    ULONGLONG now=GetTickCount64();long long frames=display_frame_count();
    if(!start){start=now;previous=frames;}
    int refresh=now-start>=500;
    if(refresh){
        double fps=(frames-previous)*1000.0/(now-start);
        fps_text(g_fps_text,sizeof g_fps_text/sizeof *g_fps_text,fps,pc_menu_output_fps(),pc_menu_framegen()!=0);
        start=now;previous=frames;InvalidateRect(g_fps_window,NULL,FALSE);
    }
    if(!g_fps || g_open || g_headless || !IsWindowVisible(g_owner) || IsIconic(g_owner)){
        if(IsWindowVisible(g_fps_window))ShowWindow(g_fps_window,SW_HIDE);return;
    }
    if(refresh || !IsWindowVisible(g_fps_window)){
        RECT client;GetClientRect(g_owner,&client);POINT origin={0,0};ClientToScreen(g_owner,&origin);
        float scale=fmaxf(1.0f,fminf(2.5f,client.right/1280.0f));
        SetWindowPos(g_fps_window,HWND_TOPMOST,origin.x+12,origin.y+12,(int)(76*scale),(int)(30*scale),SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
}
static void timer(void){
    fps_tick();
    if(getenv("SSBU_PC_PROMPT_TEST")){
        static int step;ULONGLONG elapsed=GetTickCount64()-g_test_start;
        if(step<4 && elapsed>65000+(ULONGLONG)step*12000){static const int styles[]={2,3,4,1};InterlockedExchange(&g_prompt_style,styles[step++]);printf("PC prompt test: style %ld selected live\n",g_prompt_style);}
    }
    if(getenv("SSBU_PC_VIDEO_TEST")){
        static int video_step;ULONGLONG elapsed=GetTickCount64()-g_test_start;
        if(video_step==0 && elapsed>18000){InterlockedExchange(&g_framegen,2);video_step++;printf("PC video test: blend mode selected\n");}
        if(video_step==1 && elapsed>24000){InterlockedExchange(&g_framegen,0);video_step++;printf("PC video test: original 60 selected\n");}
        if(video_step==2 && elapsed>28000){InterlockedExchange(&g_framegen,1);video_step++;printf("PC video test: motion mode selected\n");}
    }
    if(getenv("SSBU_PC_MENU_TEST")){
        static int step;static long long paused;long long display_frame_count(void);
        ULONGLONG elapsed=GetTickCount64()-g_test_start;
        if(!step && elapsed>8000){pc_menu_toggle();step=1;printf("PC menu test: opened\n");}
        if(step==1 && getenv("SSBU_PC_MENU_TEST_EXIT") && elapsed>9500){printf("PC menu test: closing owner while paused\n");PostMessage(g_owner,WM_CLOSE,0,0);step=7;}
        if(step==1 && elapsed>10000){paused=display_frame_count();step=2;}
        if(step==2 && elapsed>12000){long long frames=display_frame_count()-paused;printf("PC menu test: paused interval advanced %lld frames\n",frames);close_menu();step=3;}
        if(step==3 && elapsed>14000){InterlockedExchange(&g_upscaler,2);printf("PC menu test: NIS selected\n");step=4;}
        if(step==4 && elapsed>19000){InterlockedExchange(&g_upscaler,0);InterlockedExchange(&g_aa,0);printf("PC menu test: bilinear, SMAA off\n");step=5;}
        if(step==5 && elapsed>23000){InterlockedExchange(&g_upscaler,1);InterlockedExchange(&g_aa,1);printf("PC menu test: FSR1, SMAA on\n");step=6;}
    }
    if(g_capture>=0 && GetTickCount64()>=g_capture_until){g_capture=-1;wcscpy(g_message,L"Binding cancelled. No key was pressed.");InvalidateRect(g_window,NULL,FALSE);}
    if(!g_open && g_blocked){int down=0;for(int i=1;i<256;i++)if(GetAsyncKeyState(i)&0x8000){down=1;break;}nnpad_poll(0);NnPadState held;if(nnpad_get(0,&held) && held.buttons)down=1;if(!down)InterlockedExchange(&g_blocked,0);}
    nnpad_poll(0);NnPadState shortcut_pad={0};
    if(nnpad_get(0,&shortcut_pad))pc_menu_pad_buttons(shortcut_pad.buttons);
    else {
        typedef struct {DWORD packet;WORD buttons;BYTE lt,rt;SHORT lx,ly,rx,ry;} ShortcutState;
        typedef DWORD (WINAPI *ShortcutGet)(DWORD,ShortcutState*);
        static int tried;static ShortcutGet get;
        if(!tried){tried=1;HMODULE module=LoadLibraryW(L"xinput1_4.dll");if(!module)module=LoadLibraryW(L"xinput9_1_0.dll");if(module)get=(ShortcutGet)GetProcAddress(module,"XInputGetState");}
        ShortcutState state={0};pc_menu_pad_buttons(get && get(0,&state)==0 && (state.buttons&0x0020)?1ull<<11:0);
    }
    if(!g_open)return;
    reposition();
    nnpad_poll(0);NnPadState pad={0};int buttons=nnpad_get(0,&pad)?(int)pad.buttons:0;
    int pressed=buttons&~g_prev_pad;g_prev_pad=buttons;
    if(g_wait_pad){if(!buttons)g_wait_pad=0;return;}
    if(g_capture>=0 && g_bind_pad){int result=nnpad_menu_capture_poll(g_capture);if(result){g_capture=-1;g_wait_pad=1;wcscpy(g_message,result>0?L"Binding saved. Duplicate assignments were swapped.":L"Controller disconnected, or couldn't save.");InvalidateRect(g_window,NULL,FALSE);}return;}
    if(g_capture<0){
        if(pressed&(1<<1)){if(g_ask)g_ask=0;else close_menu();}
        else if(pressed&1)navigate(VK_RETURN);
        else if(pressed&(1<<6))navigate('Q');else if(pressed&(1<<7))navigate('R');
        int key=(buttons&(1<<13))||pad.ly>16000?VK_UP:(buttons&(1<<15))||pad.ly< -16000?VK_DOWN:(buttons&(1<<12))||pad.lx< -16000?VK_LEFT:(buttons&(1<<14))||pad.lx>16000?VK_RIGHT:0;
        if(key && (pressed || GetTickCount64()>g_repeat)){navigate(key);g_repeat=GetTickCount64()+(pressed?350:100);}
    }
    InvalidateRect(g_window,NULL,FALSE);
}
static VOID CALLBACK tick(HWND h,UINT m,UINT_PTR id,DWORD time){(void)h;(void)m;(void)id;(void)time;timer();}
void pc_menu_attach(HWND owner){
    init();g_owner=owner;g_test_start=GetTickCount64();g_headless=(getenv("SSBU_SHOW") && !atoi(getenv("SSBU_SHOW")));WNDCLASSW wc={0};wc.lpfnWndProc=procedure;wc.hInstance=GetModuleHandle(NULL);wc.hCursor=LoadCursor(NULL,IDC_ARROW);wc.lpszClassName=L"MM2_PC_OPTIONS";
    RegisterClassW(&wc);g_window=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"Super Mario Maker 2 · Settings",WS_POPUP,0,0,1280,720,owner,NULL,wc.hInstance,NULL);
    GetWindowPlacement(owner,&g_placement);
    WNDCLASSW fps_class={0};fps_class.lpfnWndProc=fps_procedure;fps_class.hInstance=wc.hInstance;fps_class.lpszClassName=L"MM2_PC_FPS";
    RegisterClassW(&fps_class);
    g_fps_window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,fps_class.lpszClassName,L"FPS",WS_POPUP,0,0,340,64,owner,NULL,wc.hInstance,NULL);
    if(g_fps_window)SetLayeredWindowAttributes(g_fps_window,RGB(1,2,3),255,LWA_COLORKEY);
    SetTimer(owner,0x4d32,30,tick);rescan();
}
void pc_menu_toggle(void){
    if(!g_window)return;if(g_open){close_menu();return;}
    InterlockedExchange(&g_blocked,1);InterlockedExchange(&g_open,1);reposition();if(!g_headless){ShowWindow(g_window,SW_SHOW);SetForegroundWindow(g_window);SetFocus(g_window);}InvalidateRect(g_window,NULL,FALSE);
}
int pc_menu_key(UINT msg,WPARAM key,LPARAM flags){
    if(msg==WM_APP+0x32){pc_menu_toggle();return 1;}
    if((msg==WM_SYSKEYDOWN && key==VK_RETURN && (flags&(1LL<<29))) || (msg==WM_KEYDOWN && key==VK_F11)){if(!(flags&(1LL<<30)))fullscreen();return 1;}
    if(msg==WM_KEYDOWN && (key==VK_ESCAPE||key==VK_F1)){if(!(flags&(1LL<<30)))pc_menu_toggle();return 1;}return 0;
}
void pc_menu_pad_buttons(unsigned long long buttons){
    static volatile LONG held;LONG down=(buttons&(1ull<<11))!=0;
    LONG previous=InterlockedExchange(&held,down);
    HWND focus=GetForegroundWindow();
    if(down && !previous && g_owner && (focus==g_owner || focus==g_window) && g_capture<0)
        PostMessage(g_owner,WM_APP+0x32,0,0);
}

static DWORD WINAPI test_wait(LPVOID p){pc_menu_wait();InterlockedIncrement((LONG *)p);return 0;}
int pc_menu_selftest(void){
    init();int bad=0;RECT r;LONG aspect=g_aspect;g_aspect=1;pc_menu_viewport(1920,1200,&r);if(r.left||r.top!=60||r.right!=1920||r.bottom!=1140)bad++;
    wchar_t fps_check[160];fps_text(fps_check,160,60,120,1);
    if(wcscmp(fps_check,L"120"))bad++;
    fps_text(fps_check,160,59.6,120,0);if(wcscmp(fps_check,L"60"))bad++;
    fps_text(fps_check,160,0,0,0);if(wcscmp(fps_check,L"0"))bad++;
    if(fps_procedure(NULL,WM_NCHITTEST,0,0)!=HTTRANSPARENT || fps_procedure(NULL,WM_MOUSEACTIVATE,0,0)!=MA_NOACTIVATE)bad++;
    pc_menu_viewport(2560,1080,&r);if(r.left!=320||r.right!=2240||r.top||r.bottom!=1080)bad++;
    g_aspect=0;pc_menu_viewport(1000,900,&r);if(r.right!=1000||r.bottom!=900)bad++;g_aspect=aspect;
    if(getenv("SSBU_PC_MENU_FIXTURES")){
        if(g_framegen!=1 || gfx_options()->window_mode!=1)bad++;
        char value[200];
        if(!graphics_save("resolution_scale","0.75"))bad++;graphics_get("resolution_scale","missing",value,sizeof value);if(strcmp(value,"0.75"))bad++;
        if(!graphics_save("profile","custom"))bad++;
        if(!graphics_save("resolution_scale","2.00"))bad++;change(RESOLUTION,1);
        graphics_get("resolution_scale","missing",value,sizeof value);if(strcmp(value,"3.00"))bad++;
        change(RESOLUTION,1);graphics_get("resolution_scale","missing",value,sizeof value);if(strcmp(value,"0.25"))bad++;
        if(!graphics_save("resolution_scale","0.75"))bad++;
        LONG icons=g_prompt_style;change(PROMPTS,1);
        if(g_prompt_style!=(icons+1)%5 || GetPrivateProfileIntA("controls","prompt_style",-1,g_settings)!=g_prompt_style)bad++;
        if(save_int("controls","prompt_style",icons))g_prompt_style=icons;else bad++;
        FILE *f=fopen(gfx_options()->path,"rb");int comment=0;if(f){char line[2048];while(fgets(line,sizeof line,f))if(strstr(line,"fixture comment"))comment=1;fclose(f);}if(!comment)bad++;
        LONG done=0;InterlockedExchange(&g_open,1);HANDLE worker=CreateThread(NULL,0,test_wait,&done,0,NULL);Sleep(60);if(!worker||done)bad++;
        InterlockedExchange(&g_open,0);if(worker){if(WaitForSingleObject(worker,1000)!=WAIT_OBJECT_0||done!=1)bad++;CloseHandle(worker);}
        g_capture=0;capture(VK_F1);if(g_capture!=0)bad++;capture(VK_ESCAPE);if(g_capture!=-1)bad++;
        g_capture=0;capture('T');GetPrivateProfileStringA("keyboard","A","",value,sizeof value,g_controllers);if(strncmp(value,"0x54",4))bad++;
        g_ask=RESTART;g_yes=0;navigate(VK_ESCAPE);if(g_ask)bad++;
        if(!pc_menu_mod_path("fixture.txt",value,sizeof value)||!strstr(value,"/B/romfs/"))bad++;
        char dirs[3][260];if(pc_menu_mod_roots("",dirs,3)!=2)bad++;
        const char *base=getenv("SSBU_PC_MENU_BASE");if(base){int nnfs_pc_mod_selftest(const char *);bad+=nnfs_pc_mod_selftest(base);}
        if(!save_int("mods","B",0))bad++;if(!pc_menu_mod_path("fixture.txt",value,sizeof value)||!strstr(value,"/B/romfs/"))bad++;
        rescan();if(g_mod_count!=2 || strcmp(g_mod_list[0].name,"B") || g_mod_list[0].enabled)bad++;
        HWND previous=g_window;g_window=CreateWindowW(L"STATIC",L"PC menu test",WS_POPUP,-32000,-32000,1280,720,NULL,NULL,GetModuleHandle(NULL),NULL);
        if(!g_window)bad++;else {
            HDC dc=CreateCompatibleDC(NULL);HBITMAP bitmap=CreateBitmap(1280,720,1,32,NULL);HGDIOBJ old=SelectObject(dc,bitmap);
            LONG volume=g_volume;g_tab=2;g_selected=g_scroll=0;paint(dc,1280,720);
            procedure(g_window,WM_LBUTTONDOWN,0,MAKELPARAM(770,242));if(g_volume!=volume-5)bad++;
            procedure(g_window,WM_LBUTTONDOWN,0,MAKELPARAM(1200,242));if(g_volume!=volume)bad++;
            SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);DestroyWindow(g_window);g_tab=g_selected=g_scroll=0;
        }g_window=previous;
        LONG previous_fg=g_framegen;change(FRAMEGEN,1);
        if(g_framegen!=(previous_fg+1)%3 || GetPrivateProfileIntA("video","frame_generation",-1,g_settings)!=g_framegen)bad++;
        change(OUTPUT_FPS,1);if(g_framegen!=((previous_fg+1)%3?0:1))bad++;
        if(save_int("video","frame_generation",previous_fg))g_framegen=previous_fg;
        int dynamic_before=g_dynamic_run;change(DYNAMIC_RES,1);if(g_dynamic_run!=dynamic_before)bad++;
        printf("PC menu: atomic flat config, remap capture/cancel, pause/resume, quit cancel, mod priority/snapshot %s\n",bad?"FAILED":"OK");
    }
    const char *out=getenv("SSBU_PC_MENU_PREVIEW");
    if(out){
        HDC dc=CreateCompatibleDC(NULL);BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=1280;info.bmiHeader.biHeight=-720;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void *pixels;HBITMAP bmp=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,NULL,0);HGDIOBJ old=SelectObject(dc,bmp);
        g_tab=getenv("SSBU_PC_MENU_TAB")?atoi(getenv("SSBU_PC_MENU_TAB")):0;if(g_tab<0||g_tab>4)g_tab=0;
        g_selected=g_scroll=getenv("SSBU_PC_MENU_SCROLL")?atoi(getenv("SSBU_PC_MENU_SCROLL")):0;
        paint(dc,1280,720);GdiFlush();BITMAPFILEHEADER file={0};file.bfType=0x4d42;file.bfOffBits=sizeof file+sizeof info.bmiHeader;file.bfSize=file.bfOffBits+1280*720*4;
        FILE *f=fopen(out,"wb");if(f){fwrite(&file,sizeof file,1,f);fwrite(&info.bmiHeader,sizeof info.bmiHeader,1,f);fwrite(pixels,4,1280*720,f);fclose(f);}else bad++;
        SelectObject(dc,old);DeleteObject(bmp);DeleteDC(dc);
    }
    printf("PC menu: viewport geometry and native preview %s\n",bad?"FAILED":"OK");return bad;
}
