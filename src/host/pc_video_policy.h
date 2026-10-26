#ifndef PC_VIDEO_POLICY_H
#define PC_VIDEO_POLICY_H
#include <math.h>
/* MM2 renders its handheld scene at 720p before composing into a 1080p window texture. */
static int pc_resolution_target(int w,int h){
    for(int k=0;k<7;k++)if((w==(1920>>k) && h==(1080>>k)) || (w==(1280>>k) && h==(720>>k)))return 1;
    return 0;
}
/* A 60 Hz simulation has 16.67 ms; reserve time for presentation and scheduling. */
typedef struct { double average; int samples, over, under, cooldown; } PcResolutionControl;
static float pc_resolution_step(PcResolutionControl *s,double ms,float scale,float low,float high){
    if(!isfinite(ms)||ms<=0||ms>250)return scale;
    s->average=s->samples?s->average*.9+ms*.1:ms;s->samples++;
    if(s->cooldown){s->cooldown--;return scale;}
    if(s->samples<30)return scale;
    s->over=s->average>15.0?s->over+1:0;
    s->under=s->average<11.5?s->under+1:0;
    float next=scale;
    if(s->over>=12)next=scale-.0625f;
    else if(s->under>=120)next=scale+.0625f;
    if(next<low)next=low;if(next>high)next=high;
    if(fabsf(next-scale)>.001f){s->over=s->under=0;s->cooldown=60;}
    return next;
}
#endif
