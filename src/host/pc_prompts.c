/* Replace identified button textures and scalable-font cells, preserving the game's layout and input mapping. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "pc_prompts.h"
#include "pc_menu.h"
#include "nnpad.h"
#include "pc_prompt_refs.h"
/* shortcut: fingerprints target the original 1.0.0 UI; regenerate refs for changed game/mod artwork. */

typedef struct { int role,w,h,flags; unsigned code; unsigned char *bits; } Glyph;
static Glyph glyphs[512];static LONG nglyphs;static SRWLOCK glyph_lock=SRWLOCK_INIT;
#define GLYPH_FLIPPED 0x40000000
typedef struct { int x,y,glyph; } Cell;
typedef struct { Tex *texture; int w,h,format,role,ncell; unsigned long long generation; unsigned char *original,*upload; Cell cells[512]; } PromptTexture;
static PromptTexture textures[64];static int ntextures;
static unsigned long long hash_bytes(const void *data,size_t size){const unsigned char *p=data;unsigned long long h=14695981039346656037ULL;while(size--)h=(h^*p++)*1099511628211ULL;return h;}
static int code_role(unsigned c){
    if(c>=0xe0a0 && c<=0xe0a9){static const int roles[]={0,1,2,3,6,7,8,9,6,7};return roles[c-0xe0a0];}
    if(c>=0xe0e0 && c<=0xe0e9){static const int roles[]={0,1,2,3,6,7,8,9,6,7};return roles[c-0xe0e0];}
    if(c>=0xe000 && c<=0xe005){static const int roles[]={0,1,2,3,6,7};return roles[c-0xe000];}
    switch(c){case 0xe045:case 0xe0b3:case 0xe0b5:case 0xe0ef:case 0xe0f1:return 10;
    case 0xe046:case 0xe0b4:case 0xe0b6:case 0xe0f0:case 0xe0f2:return 11;
    case 0xe052:case 0xe083:return 6;case 0xe053:case 0xe084:return 7;case 0xe054:case 0xe085:return 8;case 0xe055:case 0xe086:return 9;
    case 0xe042:case 0xe04c:case 0xe0ae:case 0xe0ce:return 0;
    case 0xe043:case 0xe04d:case 0xe0ab:case 0xe0cb:return 1;
    case 0xe04e:case 0xe0ac:case 0xe0cc:return 2;case 0xe04f:case 0xe0ad:case 0xe0cd:return 3;
    case 0xe0af:case 0xe0eb:return 13;case 0xe0b0:case 0xe0ec:return 15;
    case 0xe0b1:case 0xe0ed:return 12;case 0xe0b2:case 0xe0ee:return 14;
    case 0xe08a:case 0xe0c4:return 4;case 0xe08b:case 0xe0c5:return 5;
    case 0xe050:case 0xe081:case 0xe0c1:return 16;case 0xe051:case 0xe082:case 0xe0c2:return 17;default:return -1;}
}
static void record_glyph(unsigned code,int w,int h,const unsigned char *bits,int flags){
    int role=code_role(code);if(role<0 || !bits || w<3 || h<3 || w>192 || h>192 || nglyphs==512)return;
    for(int i=0;i<nglyphs;i++)if(glyphs[i].role==role && glyphs[i].w==w && glyphs[i].h==h && !memcmp(glyphs[i].bits,bits,(size_t)w*h))return;
    unsigned char *copy=malloc((size_t)w*h);if(!copy)return;memcpy(copy,bits,(size_t)w*h);
    glyphs[nglyphs]=(Glyph){role,w,h,flags,code,copy};InterlockedIncrement(&nglyphs);if(getenv("SSBU_PROMPT_TRACE"))printf("PC prompts: learned U+%04X %dx%d flags %x\n",code,w,h,flags);
}
static void record_variants(unsigned code,int w,int h,const unsigned char *bits,int flags){
    record_glyph(code,w,h,bits,flags);
    if(w<3 || h<3 || w>192 || h>192)return;
    unsigned char flipped[192*192];for(int y=0;y<h;y++)memcpy(flipped+y*w,bits+(h-y-1)*w,w);
    record_glyph(code,w,h,flipped,flags|GLYPH_FLIPPED);
}
void pc_prompts_glyph(unsigned code,int w,int h,const unsigned char *bits,int flags){
    if(code_role(code)<0 || !bits)return;
    AcquireSRWLockExclusive(&glyph_lock);record_variants(code,w,h,bits,flags);
    /* The game combines the filled and unfilled outline maps into a 128-level border. */
    if(flags&0xc0)for(int i=0;i<nglyphs;i++){
        Glyph *g=&glyphs[i];if(g->code!=code || g->w!=w || g->h!=h || g->flags==flags || (g->flags&GLYPH_FLIPPED) || !(g->flags&0xc0))continue;
        unsigned char combined[192*192];const unsigned char *filled=(flags&0x40)?bits:g->bits,*ring=(flags&0x80)?bits:g->bits;
        for(int n=0;n<w*h;n++)combined[n]=filled[n]>ring[n]/2?filled[n]-ring[n]/2:0;
        record_variants(code,w,h,combined,0);break;
    }
    ReleaseSRWLockExclusive(&glyph_lock);
}
static int effective_style(void){int style=pc_menu_prompt_style();return style?style:nnpad_prompt_type();}
static void label_for(int role,int style,wchar_t out[32]){
    if(role>=16){
        if(style==1){
            static const int order[]={1,0,3,2};out[0]=0;
            for(int i=0;i<4;i++){int vk=nnpad_kb_vk(16+(role-16)*4+order[i],0);wchar_t key[8];
                if(vk>='A' && vk<='Z')swprintf(key,8,L"%c",vk);else if(!GetKeyNameTextW(MapVirtualKeyW(vk,MAPVK_VK_TO_VSC)<<16,key,8))wcscpy(key,L"?");
                if(i && wcslen(out)<30)wcscat(out,L"/");wcsncat(out,key,31-wcslen(out));
            }
        }else wcscpy(out,role==16?L"LS":L"RS");
        return;
    }
    static const wchar_t *const original[]={L"A",L"B",L"X",L"Y",L"LS",L"RS",L"L",L"R",L"ZL",L"ZR",L"+",L"−",L"←",L"↑",L"→",L"↓"};
    if(style==1){
        int vk=nnpad_kb_vk(role,0);const wchar_t *fixed=vk==VK_RETURN?L"Enter":vk==VK_SPACE?L"Space":vk==VK_SHIFT?L"Shift":vk==VK_CONTROL?L"Ctrl":vk==VK_TAB?L"Tab":vk==VK_LBUTTON?L"M1":vk==VK_RBUTTON?L"M2":vk==VK_MBUTTON?L"M3":NULL;
        if(fixed){wcscpy(out,fixed);return;}if(!vk){wcscpy(out,L"—");return;}
        if(vk>=VK_F1 && vk<=VK_F24){swprintf(out,32,L"F%d",vk-VK_F1+1);return;}
        LONG scan=MapVirtualKeyW(vk,MAPVK_VK_TO_VSC)<<16;if(vk>=VK_LEFT && vk<=VK_DOWN)scan|=1<<24;
        if(!GetKeyNameTextW(scan,out,32))swprintf(out,32,L"%c",vk);return;
    }
    static const char *const defaults[]={"east","south","north","west","leftstick","rightstick","leftshoulder","rightshoulder","lefttrigger","righttrigger","start","back","dpleft","dpup","dpright","dpdown"};
    char source[80];if(!nnpad_menu_binding_name(role,source,sizeof source) || !strcmp(source,"none"))snprintf(source,sizeof source,"%s",defaults[role]);
    static const char *const positions[]={"south","east","west","north","leftstick","rightstick","leftshoulder","rightshoulder","lefttrigger","righttrigger","start","back","dpleft","dpup","dpright","dpdown"};
    static const wchar_t *const xbox[]={L"A",L"B",L"X",L"Y",L"LS",L"RS",L"LB",L"RB",L"LT",L"RT",L"Menu",L"View",L"←",L"↑",L"→",L"↓"};
    static const wchar_t *const ps[]={L"×",L"○",L"□",L"△",L"L3",L"R3",L"L1",L"R1",L"L2",L"R2",L"Options",L"Share",L"←",L"↑",L"→",L"↓"};
    static const wchar_t *const nin[]={L"B",L"A",L"Y",L"X",L"LS",L"RS",L"L",L"R",L"ZL",L"ZR",L"+",L"−",L"←",L"↑",L"→",L"↓"};
    for(int i=0;i<16;i++)if(!strcmp(source,positions[i])){wcscpy(out,style==2?xbox[i]:style==3?ps[i]:nin[i]);return;}
    wcscpy(out,original[role]);
}
static unsigned long long generation(int *style){
    *style=effective_style();unsigned long long h=(unsigned)*style+1;
    for(int i=0;i<18;i++){wchar_t text[32];label_for(i,*style,text);h^=hash_bytes(text,wcslen(text)*sizeof *text);h*=1099511628211ULL;}
    return h;
}
static void draw_label(unsigned char *mask,int w,int h,int role,int style,int border){
    HDC dc=CreateCompatibleDC(NULL);BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void *pixels=NULL;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,NULL,0);if(!bitmap){DeleteDC(dc);memset(mask,0,(size_t)w*h);return;}
    HGDIOBJ old=SelectObject(dc,bitmap);memset(pixels,0,(size_t)w*h*4);wchar_t label[32];label_for(role,style,label);
    if(border){HPEN pen=CreatePen(PS_SOLID,w/32+1,RGB(255,255,255));HGDIOBJ op=SelectObject(dc,pen),ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
        if(style!=1 && role<4)Ellipse(dc,1,1,w-1,h-1);else RoundRect(dc,1,1,w-1,h-1,h/4,h/4);
        SelectObject(dc,op);SelectObject(dc,ob);DeleteObject(pen);
    }
    int size=h*3/5;HFONT font=NULL;HGDIOBJ of=NULL;SIZE extent;
    do{if(font){SelectObject(dc,of);DeleteObject(font);}font=CreateFontW(-size,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");of=SelectObject(dc,font);GetTextExtentPoint32W(dc,label,(int)wcslen(label),&extent);size--;}while(size>5 && extent.cx>w*4/5);
    SetTextColor(dc,RGB(255,255,255));SetBkMode(dc,TRANSPARENT);RECT rect={2,1,w-2,h-1};DrawTextW(dc,label,-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);GdiFlush();
    for(int i=0;i<w*h;i++)mask[i]=((unsigned char *)pixels)[i*4];
    SelectObject(dc,of);DeleteObject(font);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}
static void encode_bc4(unsigned char block[8],const unsigned char values[16]){
    unsigned char lo=255,hi=0;for(int i=0;i<16;i++){if(values[i]<lo)lo=values[i];if(values[i]>hi)hi=values[i];}block[0]=hi;block[1]=lo;
    int palette[8]={hi,lo};for(int i=2;i<8;i++)palette[i]=((8-i)*hi+(i-1)*lo)/7;
    unsigned long long bits=0;for(int i=0;i<16;i++){int best=0,error=1000;for(int k=0;k<8;k++){int e=abs(values[i]-palette[k]);if(e<error){error=e;best=k;}}bits|=(unsigned long long)best<<(i*3);}
    for(int i=0;i<6;i++)block[2+i]=(unsigned char)(bits>>(i*8));
}
static void scan_cells(PromptTexture *p,int x0,int y0,int w,int h){
    int count=InterlockedCompareExchange(&nglyphs,0,0);
    for(int g=0;g<count;g++){
        const Glyph *v=&glyphs[g];if(v->w>w || v->h>h)continue;
        int anchor=0,score=-1;
        for(int i=0;i<v->w*v->h-8;i++)if(i%v->w+8<=v->w){int s=0;for(int k=1;k<8;k++)s+=abs(v->bits[i+k]-v->bits[i+k-1]);if(s>score){score=s;anchor=i;}}
        if(score<32)continue;int ax=anchor%v->w,ay=anchor/v->w;
        for(int y=y0;y<=y0+h-v->h;y++)for(int x=x0;x<=x0+w-v->w;x++){
            if(memcmp(p->original+(y+ay)*p->w+x+ax,v->bits+anchor,8))continue;
            int match=1;for(int r=0;r<v->h;r++)if(memcmp(p->original+(y+r)*p->w+x,v->bits+r*v->w,v->w)){match=0;break;}
            if(!match)continue;int found=0;for(int i=0;i<p->ncell;i++)if(p->cells[i].x==x && p->cells[i].y==y)found=1;
            if(!found && p->ncell<512){p->cells[p->ncell++]=(Cell){x,y,g};printf("PC prompts: font U+%04X role %d, atlas %dx%d at %d,%d\n",v->code,v->role,p->w,p->h,x,y);}
        }
    }
}
static void render_texture(PromptTexture *p,int style){
    if(p->format!=1){
        int block=p->format==0x4b?16:8;
        size_t bytes=(size_t)((p->w+3)/4)*((p->h+3)/4)*block;memcpy(p->upload,p->original,bytes);if(style==4)return;
        unsigned char mask[192*192];if(p->w>192||p->h>192)return;draw_label(mask,p->w,p->h,p->role,style,block==8);
        for(int y=0;y<(p->h+3)/4;y++)for(int x=0;x<(p->w+3)/4;x++){
            unsigned char values[16];for(int i=0;i<16;i++){int px=x*4+i%4,py=y*4+i/4;values[i]=px<p->w&&py<p->h?mask[py*p->w+px]:0;}
            encode_bc4(p->upload+(y*((p->w+3)/4)+x)*block,values);
        }
    }else{
        memcpy(p->upload,p->original,(size_t)p->w*p->h);if(style==4)return;
        for(int i=0;i<p->ncell;i++){Cell c=p->cells[i];Glyph *g=&glyphs[c.glyph];unsigned char mask[192*192];draw_label(mask,g->w,g->h,g->role,style,1);
            for(int y=0;y<g->h;y++)memcpy(p->upload+(c.y+y)*p->w+c.x,mask+((g->flags&GLYPH_FLIPPED)?g->h-y-1:y)*g->w,g->w);
        }
    }
}
const void *pc_prompts_upload(Tex *t,int x,int y,int w,int h,int format,const void *data,size_t size){
    if(!t || !data || w<=0 || h<=0 || (format!=1 && format!=0x49 && format!=0x4b))return data;
    if(format==1 && !InterlockedCompareExchange(&nglyphs,0,0))return data;
    if(format!=1 && size!=(size_t)((w+3)/4)*((h+3)/4)*(format==0x4b?16:8))return data;
    PromptTexture *p=NULL;for(int i=0;i<ntextures;i++)if(textures[i].texture==t)p=&textures[i];
    if(p && (p->w!=t->b.width || p->h!=t->b.height || p->format!=format))return data;
    if(!p && format!=1 && (x || y || w!=t->b.width || h!=t->b.height))return data;
    /* A full original atlas is needed before live replacement can preserve unrelated characters. */
    if(!p && format==1 && (x || y || w!=t->b.width || h!=t->b.height))return data;
    int role=-1;
    if(!p && format!=1 && !x && !y){unsigned long long hash=hash_bytes(data,size);for(unsigned i=0;i<sizeof pc_prompt_refs/sizeof *pc_prompt_refs;i++)if(pc_prompt_refs[i].hash==hash && pc_prompt_refs[i].w==w && pc_prompt_refs[i].h==h)role=pc_prompt_refs[i].role;if(role<0)return data;}
    if(!p && format==1 && (t->b.width<128 || t->b.height<128 || t->b.width>4096 || t->b.height>4096))return data;
    if(!p){
        for(int i=0;i<ntextures;i++)if(!textures[i].texture || textures[i].texture->dead){p=&textures[i];free(p->original);free(p->upload);memset(p,0,sizeof *p);break;}
        if(!p){if(ntextures==64)return data;p=&textures[ntextures++];}
        *p=(PromptTexture){.texture=t,.w=t->b.width,.h=t->b.height,.format=format,.role=role};size_t bytes=format==1?(size_t)p->w*p->h:size;
        p->original=calloc(1,bytes);p->upload=malloc(bytes);if(!p->original || !p->upload){free(p->original);free(p->upload);p->original=p->upload=NULL;return data;}
        if(role>=0)printf("PC prompts: identified button role %d, %dx%d\n",role,w,h);
    }
    if(!p->original || x<0 || y<0 || x+w>p->w || y+h>p->h)return data;
    if(format!=1){if(x||y||w!=p->w||h!=p->h)return data;memcpy(p->original,data,size);}
    else {
        if(size!=(size_t)w*h)return data;
        int n=0;for(int i=0;i<p->ncell;i++){Cell c=p->cells[i];Glyph *g=&glyphs[c.glyph];if(c.x<x+w && c.x+g->w>x && c.y<y+h && c.y+g->h>y)continue;p->cells[n++]=c;}p->ncell=n;
        for(int row=0;row<h;row++)memcpy(p->original+(y+row)*p->w+x,(const unsigned char *)data+row*w,w);
        scan_cells(p,x,y,w,h);
        if(!p->ncell){free(p->original);free(p->upload);memset(p,0,sizeof *p);return data;}
    }
    int style;p->generation=generation(&style);render_texture(p,style);
    if(format==1 && (x || y || w!=p->w || h!=p->h)){
        static unsigned char *region;static size_t capacity;if(size>capacity){void *next=realloc(region,size);if(!next)return data;region=next;capacity=size;}
        for(int row=0;row<h;row++)memcpy(region+row*w,p->upload+(y+row)*p->w+x,w);return region;
    }
    return p->upload;
}
void pc_prompts_refresh(void (*upload)(Tex *,const void *,size_t)){
    int style;unsigned long long version=generation(&style);
    for(int i=0;i<ntextures;i++){PromptTexture *p=&textures[i];if(!p->original || p->texture->dead || p->generation==version || (p->role<0 && !p->ncell))continue;
        p->generation=version;render_texture(p,style);size_t size=p->format==1?(size_t)p->w*p->h:(size_t)((p->w+3)/4)*((p->h+3)/4)*(p->format==0x4b?16:8);upload(p->texture,p->upload,size);
    }
}
