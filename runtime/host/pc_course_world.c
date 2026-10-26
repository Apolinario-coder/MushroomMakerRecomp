/* MM2 3.0.3 Course World: local scene entry and MariOver catalogue worker. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>
#include "pc_course_world.h"
#include "pc_menu.h"
#include "pc_mii_silhouette.h"

#pragma pack(push,1)
typedef struct {char magic[4];uint32_t version,count,error;char message[512];} CatalogueHeader;
typedef struct {uint64_t id,pid;uint16_t code[10],title[33],author[11],description[101];uint32_t style,difficulty,theme,likes,plays,clears,attempts,tag1,tag2,world_record,weekly_likes,weekly_plays;uint64_t uploaded;char maker[10];uint8_t region,mii[88];uint16_t pose,hat,shirt,pants;} CatalogueRecord;
#pragma pack(pop)
_Static_assert(sizeof(CatalogueHeader)==528,"catalogue header ABI");
_Static_assert(sizeof(CatalogueRecord)==489,"catalogue record ABI");
static _Alignas(8) unsigned char g_courses[100][0x838];
static HANDLE g_request, g_media_request;
static unsigned g_result_offset, g_page_size=20;
static int g_has_more;
static unsigned g_query[5],g_last_query[5],g_last_offset;
static int g_last_more,g_last_valid;
static DWORD g_media_tick;
static void cancel_media(void){
    if(!g_media_request)return;
    /* This child has already published its list and writes only atomic picture-cache files. */
    if(WaitForSingleObject(g_media_request,0)==WAIT_TIMEOUT)TerminateProcess(g_media_request,0);
    CloseHandle(g_media_request);g_media_request=NULL;
}
static wchar_t g_output[MAX_PATH];
static unsigned g_count;
static uint64_t g_validated_course;
static int g_local_play;
static volatile LONG g_native_pause;
int pc_course_world_pause_open(void){return InterlockedCompareExchange(&g_native_pause,0,0)!=0;}
static unsigned g_session_kind;
static volatile LONG g_wheel, g_wheel_tick, g_scroll_capture_tick, g_scroll_cancelled;
static uint64_t g_drag_bar;
static int g_scroll_down;
static float g_drag_offset;
int display_mouse_position(int *x,int *y,int *down);

void pc_course_world_mouse_wheel(int delta){
    InterlockedExchange(&g_wheel_tick,(LONG)GetTickCount());
    InterlockedExchangeAdd(&g_wheel,delta);
}
void pc_course_world_mouse_cancel(void){
    InterlockedExchange(&g_wheel,0);
    InterlockedExchange(&g_scroll_capture_tick,0);
    InterlockedExchange(&g_scroll_cancelled,1);
}
int pc_course_world_mouse_captured(void){
    DWORD tick=(DWORD)InterlockedCompareExchange(&g_scroll_capture_tick,0,0);
    return tick && GetTickCount()-tick<100 && !pc_menu_blocks_input();
}
static float scroll_float(uint64_t address){float value;memcpy(&value,(void*)(uintptr_t)address,4);return value;}

/* The original Nwk_CourseList PartsCtrl owns this scroll area at +744. */
static int scroll_property(Ctx *c,uint64_t handle,unsigned type,unsigned getter,float *out){
    if(!handle || !rd64(handle+8) || !rd64(handle+24))return 0;
    Ctx saved=*c;uint64_t sp=saved.x[31]-64;
    wr64(sp,0x7102945e28ULL);wr32(sp+8,type);
    c->x[31]=sp;c->x[0]=handle;c->x[1]=sp;
    call_indirect(c,rd64(rd64(handle)+32));
    if(getter==160){
        c->x[0]=rd64(handle+8);call_indirect(c,rd64(rd64(handle+24)+getter));
        memcpy(out,(void*)(uintptr_t)c->x[0],12);
    }else{
        c->x[0]=sp+16;c->x[1]=rd64(handle+8);call_indirect(c,rd64(rd64(handle+24)+getter));
        memcpy(out,(void*)(uintptr_t)(sp+16),type==0x2b?8:12);
    }
    *c=saved;return 1;
}
static void mouse_scroll_bar(Ctx *c){
    uint64_t page=c->x[0],area=page+744,bar=rd64(area+112);
    if(g_count<2 || g_local_play || !bar || !rd64(area+288))return;
    uint64_t pad=rd64(area+88),input=pad?rd64(pad+536):0;
    if(!input || !(rd32(input+5296)&1)){
        pc_course_world_mouse_cancel();g_drag_bar=0;g_scroll_down=0;return;
    }
    int x,y,down;
    if(pc_menu_blocks_input() || !display_mouse_position(&x,&y,&down)){
        pc_course_world_mouse_cancel();g_drag_bar=0;g_scroll_down=0;return;
    }
    if(InterlockedExchange(&g_scroll_cancelled,0)){g_drag_bar=0;g_scroll_down=down;}
    if(!down){
        g_drag_bar=0;g_scroll_down=0;
        if(!InterlockedCompareExchange(&g_wheel,0,0))return;
    }
    float range=fabsf(scroll_float(area+368)-scroll_float(area+356));
    if(!isfinite(range) || range<1)return;
    float position[3],center[3],size[2];
    if(!scroll_property(c,rd64(area+288),8,160,position) ||
       !scroll_property(c,rd64(bar+704),0x29,688,center) ||
       !scroll_property(c,rd64(bar+704),0x2b,720,size))return;
    float rate=position[1]/range,px=x-640.0f,py=360.0f-y;
    float height=size[1],thumb=height*scroll_float(bar+760);
    if(!isfinite(rate) || !isfinite(center[0]) || !isfinite(center[1]) ||
       !isfinite(height) || !isfinite(size[0]) || height<1 || !isfinite(thumb))return;
    thumb=fminf(height,fmaxf(16,thumb));
    float travel=height-thumb,top=center[1]+height*0.5f-thumb*0.5f;
    if(travel<1)return;
    float next=rate;
    int hit=fabsf(px-center[0])<=fmaxf(15,size[0]*0.5f) && fabsf(py-center[1])<=height*0.5f;
    if(down && !g_scroll_down && hit){
        g_drag_bar=bar;
        float pointer=(top-py)/travel;
        g_drag_offset=fabsf(py-(top-rate*travel))<=thumb*0.5f?rate-pointer:0;
    }
    if(!down)g_drag_bar=0;
    if(g_drag_bar==bar){
        next=(top-py)/travel+g_drag_offset;
        InterlockedExchange(&g_scroll_capture_tick,(LONG)GetTickCount());
    }
    g_scroll_down=down;
    if(GetTickCount()-(DWORD)InterlockedCompareExchange(&g_wheel_tick,0,0)<200){
        LONG delta=InterlockedExchange(&g_wheel,0);
        next-=(float)delta/(120.0f*(g_count-1));
    }else InterlockedExchange(&g_wheel,0);
    next=fminf(1,fmaxf(0,next));
    if(next==rate)return;
    Ctx saved=*c;
    c->x[0]=area;c->v[0].f[0]=next;
    call_indirect(c,0x7101c724f0ULL);
    c->x[0]=area;call_indirect(c,0x7101c6c1a0ULL);
    *c=saved;
}
static _Alignas(8) unsigned char g_download[0x78050];

static int project_root(wchar_t *root){
    const char *configured=getenv("SSBU_PROJECT_ROOT");
    if(configured){
        if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,configured,-1,root,MAX_PATH))return 0;
    }else{
        if(!GetModuleFileNameW(NULL,root,MAX_PATH))return 0;
        wchar_t *slash=wcsrchr(root,L'\\');if(!slash)return 0;*slash=0;
    }
    for(unsigned depth=0;depth<7;depth++){
        wchar_t script[MAX_PATH];
        if(swprintf(script,MAX_PATH,L"%ls\\tools\\native_catalogue.py",root)>0 &&
           GetFileAttributesW(script)!=INVALID_FILE_ATTRIBUTES)return 1;
        if(configured)return 0;
        wchar_t *slash=wcsrchr(root,L'\\');if(!slash || slash<=root+2)return 0;*slash=0;
    }
    return 0;
}

static void finish_worker(Ctx *c,uint64_t worker,unsigned status){
    wr32(worker+388,status);wr32(worker+184,1);
    c->x[0]=worker+8;c->x[1]=1;call_indirect(c,0x71008b9320ULL);
}

static void start_course(uint64_t worker){
    cancel_media();
    g_validated_course=0;g_local_play=0;
    wr64(worker+384,0);wr64(worker+396,0);wr32(worker+388,1);
    uint64_t id=rd64(worker+200);unsigned i;
    for(i=0;i<g_count && rd64((uintptr_t)g_courses[i])!=id;i++);
    if(i==g_count){wr32(worker+388,12);return;}
    if(g_request){CloseHandle(g_request);g_request=NULL;}
    const char *fixture=getenv("SSBU_COURSE_WORLD_DOWNLOAD");
    if(fixture){MultiByteToWideChar(CP_UTF8,0,fixture,-1,g_output,MAX_PATH);return;}
    wchar_t directory[MAX_PATH],python[MAX_PATH]=L"python.exe",command[MAX_PATH*4],temporary[MAX_PATH],code[10],root[MAX_PATH];
    if(!project_root(root)){wr32(worker+388,12);puts("PC Course World: project tools not found");return;}
    DWORD size=sizeof python;RegGetValueW(HKEY_CURRENT_USER,L"Software\\Python\\PythonCore\\3.13\\InstallPath",L"ExecutablePath",RRF_RT_REG_SZ,NULL,python,&size);
    wchar_t selected_python[MAX_PATH];DWORD python_chars=GetEnvironmentVariableW(L"SSBU_PYTHON",selected_python,MAX_PATH);
    if(python_chars && python_chars<MAX_PATH)wcscpy(python,selected_python);
    GetModuleFileNameW(NULL,directory,MAX_PATH);wchar_t *slash=wcsrchr(directory,L'\\');if(slash)*slash=0;
    memcpy(code,g_courses[i]+28,sizeof code);
    GetTempPathW(MAX_PATH,temporary);GetTempFileNameW(temporary,L"m2d",0,g_output);
    swprintf(command,sizeof command/sizeof *command,L"\"%ls\" \"%ls/tools/native_course.py\" --game-version 3.0.3 --output \"%ls\" --code %ls --data-id %llu",python,root,g_output,code,(unsigned long long)id);
    STARTUPINFOW startup={sizeof startup};PROCESS_INFORMATION process={0};
    if(CreateProcessW(NULL,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,directory,&startup,&process)){
        CloseHandle(process.hThread);g_request=process.hProcess;
    }else wr32(worker+388,12);
    printf("PC Course World: course download %llu\n",(unsigned long long)id);fflush(stdout);
}

static unsigned receive_course(Ctx *c,uint64_t worker,int prepare){
    if(g_request && WaitForSingleObject(g_request,0)==WAIT_TIMEOUT)return 1;
    if(rd32(worker+388)!=1)return rd32(worker+388);
    if(g_request){CloseHandle(g_request);g_request=NULL;}
    unsigned char *incoming=malloc(sizeof g_download);
    FILE *file=_wfopen(g_output,L"rb");CatalogueHeader h={0};int ok=0;
    if(file){
        if(incoming && fread(&h,1,sizeof h,file)==sizeof h && !memcmp(h.magic,"M2DL",4) && h.version==1 && !h.error && h.count==sizeof g_download)
            ok=fread(incoming,1,sizeof g_download,file)==sizeof g_download && fgetc(file)==EOF;
        fclose(file);
    }
    if(!getenv("SSBU_COURSE_WORLD_DOWNLOAD"))DeleteFileW(g_output);h.message[511]=0;
    if(!ok){free(incoming);printf("PC Course World: download rejected: %s\n",h.message);fflush(stdout);return 12;}
    uint64_t id=rd64(worker+200),raw=(uintptr_t)incoming,manager=rd64(0x7102c115e0ULL);
    if(!manager || rd64(raw+0x78000)!=id || memcmp(incoming+0x1c00c,"SCDL",4)){free(incoming);return 12;}
    c->x[0]=raw+0x1c010;c->x[1]=0x5bfc0;call_indirect(c,0x710029b2a0ULL);
    if((uint32_t)c->x[0]!=rd32(raw+0x1c008)){free(incoming);return 12;}
    memcpy(g_download,incoming,sizeof g_download);free(incoming);raw=(uintptr_t)g_download;
    wr64(manager+0x15e550,raw);
    if(prepare){
        unsigned i;for(i=0;i<g_count && rd64((uintptr_t)g_courses[i])!=id;i++);
        uint64_t context=rd64(0x7102c4b710ULL),resource=rd64(0x7102b23738ULL);
        if(i==g_count || !context || !resource)return 12;
        c->x[0]=context;c->x[1]=(uintptr_t)g_courses[i];c->x[2]=raw;call_indirect(c,0x71017727b0ULL);
        c->x[0]=resource;c->x[1]=raw+0x1c000;c->x[2]=1;call_indirect(c,0x7100fcab10ULL);
        if(c->x[0]&1)return 12;
    }
    g_validated_course=id;
    printf("PC Course World: validated course %llu, native preparation %d\n",(unsigned long long)id,prepare);fflush(stdout);return 3;
}

static void request_scene(Ctx *c,unsigned scene,unsigned slot){
    uint64_t sp=c->x[31];c->x[31]=sp-32;wr64(sp-32,UINT64_MAX);wr32(sp-24,UINT32_MAX);
    c->x[0]=scene;c->x[1]=sp-32;call_indirect(c,0x7101792070ULL);
    c->x[0]=scene;call_indirect(c,0x7101790480ULL);
    c->x[0]=scene;c->x[1]=slot;c->x[2]=sp-32;call_indirect(c,0x7101792890ULL);
    c->x[31]=sp;c->x[0]=0;call_indirect(c,0x7101791020ULL);
}

static int start_local_play(Ctx *c){
    uint64_t file=rd64(0x7102a67b70ULL),base=file?rd64(file+72):0;
    printf("PC Course World: play entry file %llx buffer %llx validated %llu\n",(unsigned long long)file,(unsigned long long)base,(unsigned long long)g_validated_course);fflush(stdout);
    if(!base || !g_validated_course)return 0;
    uint64_t context=rd64(0x7102c0f0d8ULL);
    if(!context){puts("PC Course World: play session context absent");fflush(stdout);return 0;}
    /* Native Coursebot play must not enter the Nintendo session monitor. */
    g_local_play=1;g_session_kind=rd32(context+884);wr32(context+884,0);
    memcpy((void*)(uintptr_t)base,g_download+0x1c000,0x5c000);
    memcpy((void*)(uintptr_t)(base+0x5c000),g_download,0x1c000);
    memset((void*)(uintptr_t)(base+0x78000),0,0x68000);
    c->x[0]=base+0x78004;c->x[1]=0x67fcc;call_indirect(c,0x710029b2a0ULL);wr32(base+0x78000,(uint32_t)c->x[0]);
    request_scene(c,4,180);
    puts("PC Course World: native memory course requested");fflush(stdout);return 1;
}

static void publish_thumbnails(uint64_t manager){
    unsigned count=rd32(manager+0x132850);uint64_t slots=rd64(manager+0x132858);
    if(!slots || count>100)return;
    wchar_t path[MAX_PATH],root[MAX_PATH];if(!project_root(root))return;
    unsigned loaded=0;
    for(unsigned i=0;i<g_count && i<count;i++){
        uint64_t entry=(uintptr_t)g_courses[i],id=rd64(entry),slot=slots+i*0x1c1b8;
        swprintf(path,MAX_PATH,L"%ls/build/cache/mariover/%llu.jpg",root,(unsigned long long)id);
        if(rd64(slot+0x1c000)==id && rd8(slot+0x1c1b4))continue;
        if(rd64(slot+0x1c000)!=id){
            wr64(slot+0x1c000,id);wr8(slot+0x1c1b0,0);wr8(slot+0x1c1b4,0);
        }
        FILE *file=_wfopen(path,L"rb");if(!file)continue;
        unsigned char bytes[0x1c000];
        int ok=fread(bytes,1,sizeof bytes,file)==sizeof bytes && fgetc(file)==EOF;fclose(file);
        if(!ok || bytes[0]!=0xff || bytes[1]!=0xd8)continue;
        unsigned length=0;
        for(unsigned end=2;end<=0x1bf9c;end++)if(bytes[end-2]==0xff && bytes[end-1]==0xd9)length=end;
        if(!length)continue;
        memcpy((void*)(uintptr_t)slot,bytes,sizeof bytes);wr64(slot+0x1c000,id);
        /* The worker already decrypted the JPEG; native readers also need its footer length and ready flag. */
        wr32(slot+0x1bf9c,length);
        uint64_t source=entry+496,target=slot+0x1c008;
        memcpy((void*)(uintptr_t)target,(void*)(uintptr_t)source,424);
        for(unsigned offset=0;offset<424;offset+=8){
            uint64_t value=rd64(target+offset);
            if(value>=source && value<source+424)wr64(target+offset,target+value-source);
        }
        wr8(slot+0x1c1b0,1);wr8(slot+0x1c1b1,0);wr8(slot+0x1c1b2,0);wr8(slot+0x1c1b3,0);wr8(slot+0x1c1b4,1);loaded++;
    }
    if(loaded){printf("PC Course World: %u thumbnail cache slots populated\n",loaded);fflush(stdout);}
}

static int publish_result(void){
    uint64_t manager=rd64(0x7102c115e0ULL);
    if(!manager)return 0;
    uint64_t ring=manager+0xff238,data=rd64(ring);unsigned capacity=rd32(ring+8);
    printf("PC Course World: publish search ring capacity %u count %u\n",capacity,g_count);fflush(stdout);
    if(!data || capacity<g_count || capacity>1000)return 0;
    for(unsigned i=0;i<g_count;i++){
        uint64_t source=(uint64_t)(uintptr_t)g_courses[i],target=data+i*0x838;
        memcpy((void*)(uintptr_t)target,(void*)(uintptr_t)source,0x838);
        for(unsigned offset=0;offset<0x838;offset+=8){
            uint64_t value=rd64(target+offset);
            if(value>=source && value<source+0x838)wr64(target+offset,target+value-source);
        }
    }
    publish_thumbnails(manager);
    wr32(ring+12,0);wr32(ring+16,g_count);
    /* The native list snapshots these result fields in f_71017390ac. */
    /* The guest stores a zero-based page index and a separate Next-page flag. */
    wr32(manager+0x132830,g_result_offset/g_page_size);wr32(manager+0x132838,g_count);wr32(manager+0x13283c,g_result_offset+g_count+(g_has_more?1:0));
    wr8(manager+0x132834,g_has_more);wr8(manager+0x132848,g_count!=0);return 1;
}

static int load_catalogue(void){
    FILE *file=_wfopen(g_output,L"rb");if(!file)return 0;
    CatalogueHeader h;CatalogueRecord items[100];int ok=0;
    if(fread(&h,1,sizeof h,file)!=sizeof h || memcmp(h.magic,"M2CW",4) || h.version!=4 || h.count>100)goto done;
    h.message[511]=0;
    if(h.error){printf("PC Course World: API error: %s\n",h.message);goto done;}
    if(fread(items,sizeof *items,h.count,file)!=h.count || fgetc(file)!=EOF)goto done;
    unsigned offset=0,more=h.count>=g_page_size;
    if(!strncmp(h.message,"M2PAGE ",7) &&
       (sscanf(h.message,"M2PAGE %u %u",&offset,&more)!=2 || offset!=g_result_offset || more>1))goto done;
    uint64_t cache=0x7102dbf3b0ULL,base=0x7102dbf6f0ULL;
    if(rd32(cache+4)<h.count || !rd64(cache+8) || rd64(base+64)!=base+76 || rd64(base+1360)!=base+1372){
        printf("PC Course World: catalogue ABI mismatch\n");goto done;
    }
    for(unsigned i=0;i<h.count;i++){
        CatalogueRecord *item=items+i;
        if(!item->id || item->code[9] || item->title[32] || item->author[10] || item->description[100] || item->tag1>15 || item->tag2>15 || item->maker[9] || item->region>3 || item->style>4 || item->difficulty>3 || item->theme>9)goto done;
    }
    for(unsigned i=0;i<h.count;i++){
        uint64_t entry=(uint64_t)(uintptr_t)g_courses[i];
        memcpy(g_courses[i],(void*)(uintptr_t)base,0x838);
        for(unsigned offset=0;offset<0x838;offset+=8){
            uint64_t value=rd64(entry+offset);
            if(value>=base && value<base+0x838)wr64(entry+offset,entry+(value-base));
        }
        wr64(entry,items[i].id);wr64(entry+48,items[i].pid);
        memcpy((void*)(uintptr_t)(entry+28),items[i].code,20);
        memcpy((void*)(uintptr_t)(entry+76),items[i].title,66);
        memcpy((void*)(uintptr_t)(entry+1372),items[i].author,22);
        wr8(entry+1344,1);
        memcpy((void*)(uintptr_t)(entry+164),items[i].description,202);
        wr8(entry+368,items[i].style);wr8(entry+369,items[i].theme);wr32(entry+384,items[i].difficulty);
        wr64(entry+376,items[i].uploaded);
        wr8(entry+400,items[i].tag1<=13?items[i].tag1:0);wr8(entry+401,items[i].tag2<=13?items[i].tag2:0);
        wr32(entry+444,items[i].weekly_likes);wr32(entry+448,items[i].weekly_plays);
        wr32(entry+452,items[i].likes);wr32(entry+424,items[i].plays);
        wr32(entry+436,items[i].clears);wr32(entry+428,items[i].attempts);wr32(entry+480,items[i].world_record);
        wr64(entry+496,items[i].id);wr32(entry+788,0x1c000);
        wr64(entry+1400,items[i].pid);
        memcpy((void*)(uintptr_t)(entry+1428),items[i].maker,10);
        wr8(entry+1440,items[i].region);
        memcpy((void*)(uintptr_t)(entry+1444),items[i].mii,88);
        /* Native UserInfo orders clothing as shirt, pants, hat, then pose. */
        wr16(entry+1532,items[i].shirt);wr16(entry+1534,items[i].pants);
        wr16(entry+1536,items[i].hat);wr16(entry+1538,items[i].pose);
    }
    g_has_more=more!=0;
    g_count=h.count;ok=1;
    printf("PC Course World: %u API course records decoded\n",h.count);fflush(stdout);
done:
    fclose(file);return ok;
}

static void start_catalogue(uint64_t worker,int detailed){
    cancel_media();
    wr64(worker+384,0);wr64(worker+396,0);wr32(worker+388,1);
    unsigned tab=rd32(worker+284),difficulty=rd32(worker+288);
    /* +296 is the sort mode, not the page size; +300 is the page index. */
    unsigned page=rd32(worker+300);g_page_size=20;
    /* Keep native signed result counters in range without a catalogue-sized page cap. */
    if(page>(0x7fffffffU-101)/g_page_size){wr32(worker+388,2);return;}
    g_result_offset=page*g_page_size;
    g_has_more=0;
    unsigned tag1=255,tag2=255;
    if(detailed){
        uint64_t history=0x7102dcc0f8ULL;unsigned index=rd32(history);
        if(index>11){wr32(worker+388,2);return;}
        uint64_t query=history+4+index*40;
        tab=3;difficulty=rd32(query);tag1=rd8(query+8);tag2=rd8(query+9);
        if((tag1!=255 && tag1>15) || (tag2!=255 && tag2>15)){wr32(worker+388,2);return;}
    }
    printf("PC Course World: catalogue tab %u difficulty %u fields296=%u fields300=%u\n",tab,difficulty,rd32(worker+296),rd32(worker+300));fflush(stdout);
    if((tab!=0 && tab!=1 && tab!=2 && tab!=3 && tab!=5) || difficulty>4){wr32(worker+388,2);return;}
    g_query[0]=tab;g_query[1]=difficulty;g_query[2]=tag1;g_query[3]=tag2;g_query[4]=rd32(worker+296);
    wchar_t directory[MAX_PATH],command[MAX_PATH*4],python[MAX_PATH]=L"python.exe",root[MAX_PATH];
    DWORD python_size=sizeof python;RegGetValueW(HKEY_CURRENT_USER,L"Software\\Python\\PythonCore\\3.13\\InstallPath",L"ExecutablePath",RRF_RT_REG_SZ,NULL,python,&python_size);
    wchar_t selected_python[MAX_PATH];DWORD python_chars=GetEnvironmentVariableW(L"SSBU_PYTHON",selected_python,MAX_PATH);
    if(python_chars && python_chars<MAX_PATH)wcscpy(python,selected_python);
    if(g_request){CloseHandle(g_request);g_request=NULL;}
    GetModuleFileNameW(NULL,directory,MAX_PATH);wchar_t *slash=wcsrchr(directory,L'\\');if(slash)*slash=0;
    const char *fixture=getenv("SSBU_COURSE_WORLD_DATA");
    if(fixture){
        MultiByteToWideChar(CP_UTF8,0,fixture,-1,g_output,MAX_PATH);
        if(GetFileAttributesW(g_output)!=INVALID_FILE_ATTRIBUTES && (GetFileAttributesW(g_output)&FILE_ATTRIBUTE_DIRECTORY)){
            wchar_t folder[MAX_PATH];wcscpy(folder,g_output);swprintf(g_output,MAX_PATH,L"%ls/page-%u.bin",folder,g_result_offset);
        }
        return;
    }
    if(!project_root(root)){wr32(worker+388,2);puts("PC Course World: project tools not found");return;}
    wchar_t temporary[MAX_PATH];GetTempPathW(MAX_PATH,temporary);GetTempFileNameW(temporary,L"m2c",0,g_output);
    static const wchar_t *difficulties[]={L"e",L"n",L"ex",L"sex",L"all"};
    /* shortcut: Hot uses Popular because MariOver has no Hot Courses endpoint. */
    swprintf(command,sizeof command/sizeof *command,L"\"%ls\" \"%ls/tools/native_catalogue.py\" --version 4 --output \"%ls\" --mode %ls --difficulty %ls --tag1 %u --tag2 %u --offset %u --count %u --tab %u",python,root,g_output,tab==2?L"new":L"popular",difficulties[difficulty],tag1,tag2,g_result_offset,g_page_size,tab);
    STARTUPINFOW startup={sizeof startup};PROCESS_INFORMATION process={0};
    if(CreateProcessW(NULL,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,directory,&startup,&process)){
        CloseHandle(process.hThread);g_request=process.hProcess;
    }else wr32(worker+388,2);
}

int pc_course_world_dispatch(Ctx *c,uint64_t addr){
    if(pc_mii_silhouette_dispatch(c,addr))return 1;
    if(addr==0x7101ab3c50ULL){
        if(g_media_request && !g_local_play && GetTickCount()-g_media_tick>=250){
            g_media_tick=GetTickCount();uint64_t manager=rd64(0x7102c115e0ULL);
            if(manager)publish_thumbnails(manager);
            if(WaitForSingleObject(g_media_request,0)!=WAIT_TIMEOUT){CloseHandle(g_media_request);g_media_request=NULL;}
        }
        mouse_scroll_bar(c);return 0;
    }
    if(addr==0x7101bc2470ULL)InterlockedExchange(&g_native_pause,rd32(c->x[0]+640)<4);
    if(addr==0x7101bc2470ULL && g_local_play){
        uint64_t page=c->x[0],child=rd64(page+688);
        /* Original PauseMenu action 4 is Exit Course, after confirmation. */
        if(rd32(page+640)==3 && child && rd32(child+684)==4 && rd8(child+688)){
            uint64_t context=rd64(0x7102c0f0d8ULL);
            if(context)wr32(context+884,g_session_kind);
            wr32(page+640,4);InterlockedExchange(&g_native_pause,0);g_local_play=0;g_validated_course=0;request_scene(c,33,33);
            puts("PC Course World: return to original catalogue requested");fflush(stdout);return 1;
        }
    }
    if(addr==0x7101736410ULL || addr==0x710173aa10ULL){
        return start_local_play(c);
    }
    if(addr==0x710176cf70ULL){
        uint64_t worker=c->x[0],manager=rd64(0x7102c115e0ULL);
        int ready=g_validated_course && g_validated_course==rd64(worker+200) && manager && rd64(manager+0x15e550)==(uintptr_t)g_download;
        wr64(worker+384,0);wr64(worker+396,0);wr32(worker+388,ready?1:12);return 1;
    }
    if(addr==0x710176d070ULL){g_local_play=rd32(c->x[0]+388)==1;finish_worker(c,c->x[0],g_local_play?3:12);return 1;}
    if(addr==0x710176c670ULL || addr==0x710176c8e0ULL || addr==0x710176cb10ULL){start_course(c->x[0]);return 1;}
    if(addr==0x710176c810ULL || addr==0x710176c940ULL || addr==0x710176cc00ULL){
        uint64_t worker=c->x[0];unsigned status=receive_course(c,worker,addr!=0x710176c810ULL);
        if(status!=1)finish_worker(c,worker,status);return 1;
    }
    if(addr==0x7101c48ae0ULL && rd32(c->x[0]+212)==5){
        c->x[0]+=8;c->x[1]=22;call_indirect(c,0x71008b9320ULL);return 1;
    }
    if(addr==0x7101c4a040ULL){
        /* The native backend enters the original Network scene without a Nintendo login. */
        uint64_t sp=c->x[31],ui=rd64(c->x[0]+192);
        c->x[0]=ui;call_indirect(c,rd64(rd64(ui)+176));
        c->x[31]=sp-32;wr64(sp-32,UINT64_MAX);wr32(sp-24,UINT32_MAX);
        c->x[0]=33;c->x[1]=sp-32;call_indirect(c,0x7101792070ULL);
        c->x[0]=33;call_indirect(c,0x7101790480ULL);
        c->x[0]=33;c->x[1]=33;c->x[2]=sp-32;call_indirect(c,0x7101792890ULL);
        c->x[31]=sp;c->x[0]=0;call_indirect(c,0x7101791020ULL);
        puts("PC Course World: original Network scene requested");return 1;
    }
    if(addr==0x710177d600ULL){
        /* Keep local Mii initialization, then skip the Nintendo user-info download. */
        puts("PC Course World: local Mii initialization");fflush(stdout);
        c->x[0]+=592;c->x[1]=2;call_indirect(c,0x71008b9320ULL);return 1;
    }
    if(addr==0x710177d6b0ULL){
        puts("PC Course World: local user initialization");fflush(stdout);
        c->x[0]+=592;c->x[1]=4;call_indirect(c,0x71008b9320ULL);return 1;
    }
    if(addr==0x710176e410ULL || addr==0x710176e550ULL){start_catalogue(c->x[0],addr==0x710176e550ULL);return 1;}
    if(addr==0x710176e480ULL || addr==0x710176e710ULL){
        if(g_request && WaitForSingleObject(g_request,0)==WAIT_TIMEOUT){
            CatalogueHeader header={0};FILE *ready=_wfopen(g_output,L"rb");
            int published=ready && fread(&header,1,sizeof header,ready)==sizeof header && !memcmp(header.magic,"M2CW",4);
            if(ready)fclose(ready);
            if(!published)return 1;
            cancel_media();
            g_media_request=g_request;g_request=NULL;g_media_tick=0;
        }
        if(rd32(c->x[0]+388)==1){
            if(g_request){CloseHandle(g_request);g_request=NULL;}
            int loaded=load_catalogue(),ok=0;
            if(loaded){
                g_last_valid=0;
                ok=publish_result();
                if(ok){memcpy(g_last_query,g_query,sizeof g_query);g_last_offset=g_result_offset;g_last_more=g_has_more;g_last_valid=g_count!=0;}
            }else if(g_last_valid && g_count && !memcmp(g_query,g_last_query,sizeof g_query)){
                /* A failed sample is retryable; restore the actual page number and its records. */
                g_result_offset=g_last_offset;g_has_more=g_last_more;
                wr32(c->x[0]+300,g_result_offset/g_page_size);ok=publish_result();
                if(ok){printf("PC Course World: keeping page %u after failed request; Next can retry\n",g_result_offset/g_page_size+1);fflush(stdout);}
            }
            if(!getenv("SSBU_COURSE_WORLD_DATA"))DeleteFileW(g_output);
            wr32(c->x[0]+388,ok?3:2);
        }
        wr32(c->x[0]+184,1);c->x[0]+=8;c->x[1]=1;call_indirect(c,0x71008b9320ULL);return 1;
    }
    return 0;
}
