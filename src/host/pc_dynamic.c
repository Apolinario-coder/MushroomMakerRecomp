/* Included by nvn_vk_draw.c; only the render thread changes image sizes. */
#include "pc_video_policy.h"
static void pc_dynamic_update(LONG64 frame){
    static PcResolutionControl control;
    static LONG64 consumed=-1;
    static int disabled;
    static float maximum;
    if(!pc_menu_dynamic_resolution() || !g_ts_on || disabled || pc_menu_is_open())return;
    if(!maximum)maximum=rs_scale();
    PcGpuSample *sample=NULL;
    for(int i=0;i<128;i++){
        PcGpuSample *q=&g_drs_gpu[i];
        if(q->count && !q->pending && q->frame>consumed && q->frame<frame-1 && (!sample || q->frame>sample->frame))sample=q;
    }
    if(!sample)return;
    consumed=sample->frame;
    float low=fminf(maximum,pc_menu_dynamic_min()/100.0f),next=pc_resolution_step(&control,sample->ms,rs_scale(),low,maximum);
    const char *test=getenv("SSBU_PC_DRS_TEST");
    if(test && frame>300 && frame<700)next=low;
    if(test && frame>=700 && frame<900)next=maximum;
    if(fabsf(next-rs_scale())<.001f)return;
    VTex *old[256];int n=0;
    for(VTex *v=g_vtex_all;v;v=v->next)if(v->rs && v->t && !v->t->dead){
        /* Preserve every mip, including temporal colour/depth data, before replacing a target. */
        if(n==256 || v->samples!=1 || !fmt_supported(v->fmt,VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT)){
            printf("PC dynamic resolution: fixed scale retained (target format %d, samples %d cannot be preserved)\n",v->fmt,v->samples);disabled=1;return;
        }
        old[n++]=v;
    }
    if(!n)return;
    cmd_wait_all();
    g_rs=next;
    for(int i=0;i<n;i++){
        VTex *a=old[i];Tex *t=a->t;t->vk=NULL;VTex *b=vt_get2(t,0);
        if(a->layout!=VK_IMAGE_LAYOUT_UNDEFINED){
            vt_layout(a,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);vt_layout(b,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkFilter filter=a->aspect==VK_IMAGE_ASPECT_COLOR_BIT && fmt_supported(a->fmt,VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
            for(int level=0;level<a->levels && level<b->levels;level++)for(int bit=1;bit<=VK_IMAGE_ASPECT_STENCIL_BIT;bit<<=1)if(a->aspect&bit){
                VkImageBlit copy={0};copy.srcSubresource=copy.dstSubresource=(VkImageSubresourceLayers){bit,level,0,1};
                copy.srcOffsets[1]=(VkOffset3D){lvl_(a->w,level),lvl_(a->h,level),1};
                copy.dstOffsets[1]=(VkOffset3D){lvl_(b->w,level),lvl_(b->h,level),1};
                vkCmdBlitImage(R.cmd,a->img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,b->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy,filter);
            }
        }
    }
    cmd_wait_all();
    for(int i=0;i<n;i++)vt_destroy(old[i]);
    R.pipe_bound=NULL;R.dyn_dirty=R.vi_dirty=1;
    printf("PC dynamic resolution: %.2f ms GPU (smoothed %.2f), scale %.4f, %d targets preserved\n",sample->ms,control.average,next,n);
}
