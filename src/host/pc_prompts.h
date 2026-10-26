#ifndef PC_PROMPTS_H
#define PC_PROMPTS_H
#include "nvn_front.h"
void pc_prompts_glyph(unsigned code,int width,int height,const unsigned char *bits,int flags);
/* Returned upload memory is owned by the prompt cache; the caller must copy it before another call. */
const void *pc_prompts_upload(Tex *t,int x,int y,int w,int h,int format,const void *data,size_t size);
void pc_prompts_refresh(void (*upload)(Tex *,const void *,size_t));
#endif
