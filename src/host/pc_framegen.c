/* Included by pc_post.c. Interpolate P -> C; present P then the midpoint, one game frame behind. */
static LONG64 g_fg_generated,g_fg_outputs,g_fg_deadline,g_fg_last_input;
static int g_fg_history,g_fg_mode,g_fg_slot;
static VTex *g_fg_pair[2];
static void pc_fg_reset(void){g_fg_history=0;g_fg_deadline=0;g_fg_last_input=0;}
static void pc_fg_surface_mode(void){
    static int last=-1;static VkPresentModeKHR original;
    int enabled=pc_menu_framegen()!=0;
    if(last==enabled)return;
    if(last<0)original=V.mode;
    last=enabled;VkPresentModeKHR selected=original;
    if(enabled){
        VkPresentModeKHR modes[16];uint32_t n=16;
        VkResult result=vkGetPhysicalDeviceSurfacePresentModesKHR(V.gpu,V.surface,&n,modes);
        if(result==VK_SUCCESS || result==VK_INCOMPLETE){
            for(uint32_t i=0;i<n;i++)if(modes[i]==VK_PRESENT_MODE_IMMEDIATE_KHR)selected=modes[i];
            for(uint32_t i=0;i<n;i++)if(modes[i]==VK_PRESENT_MODE_MAILBOX_KHR)selected=modes[i];
        }
        if(selected!=VK_PRESENT_MODE_IMMEDIATE_KHR && selected!=VK_PRESENT_MODE_MAILBOX_KHR)
            printf("PC frame generation: unavailable on this surface; retaining 60 Hz simulation\n");
    }
    if(selected!=V.mode){cmd_wait_all();swapchain_destroy();V.mode=selected;InterlockedIncrement64(&g_vk_recreated);}
    pc_fg_reset();
}
static int pc_fg_prepare(VTex *source,int rect[4],int mode){
    LONG64 now=tk();LARGE_INTEGER fq;QueryPerformanceFrequency(&fq);
    if(!mode || pc_menu_is_open() || (V.mode!=VK_PRESENT_MODE_MAILBOX_KHR && V.mode!=VK_PRESENT_MODE_IMMEDIATE_KHR)){
        pc_fg_reset();return 0;
    }
    int w=rect[2],h=rect[3];
    VTex *previous=g_post_images[9+g_fg_slot];
    if(g_fg_mode!=mode || !previous || previous->w!=w || previous->h!=h || (g_fg_last_input && now-g_fg_last_input>fq.QuadPart/20))pc_fg_reset();
    g_fg_mode=mode;g_fg_last_input=now;
    g_fg_slot^=1;
    VTex *current=post_image(9+g_fg_slot,w,h,VK_FORMAT_R8G8B8A8_UNORM);
    vt_layout(source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);vt_layout(current,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit copy={0};copy.srcSubresource=copy.dstSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.srcOffsets[0]=(VkOffset3D){rect[0],rect[1],0};copy.srcOffsets[1]=(VkOffset3D){rect[0]+w,rect[1]+h,1};copy.dstOffsets[1]=(VkOffset3D){w,h,1};
    vkCmdBlitImage(R.cmd,source->img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,current->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy,VK_FILTER_NEAREST);
    vt_layout(current,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if(!g_fg_history){g_fg_history=1;return 0;}
    vt_layout(previous,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VTex *flow=post_image(11,(w+7)/8,(h+7)/8,VK_FORMAT_R8G8B8A8_UNORM);
    float constants[8]={1.0f/w,1.0f/h,(float)mode,0,flow->w,flow->h};
    VkImageView inputs[]={post_view(previous),post_view(current),post_view(flow)};
    /* The blend mode needs no flow search, but still initializes the bound image. */
    if(mode==1)post_pass(7,inputs,2,flow,constants);
    else {vt_layout(flow,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkClearColorValue zero={{0,0,0,0}};VkImageSubresourceRange sr={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(R.cmd,flow->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&sr);vt_layout(flow,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}
    VTex *middle=post_image(12,w,h,VK_FORMAT_R8G8B8A8_UNORM);
    post_pass(8,inputs,3,middle,constants);
    vt_layout(previous,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);vt_layout(middle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    g_fg_pair[0]=previous;g_fg_pair[1]=middle;
    return 1;
}
static void pc_fg_wait(void){
    LARGE_INTEGER fq;QueryPerformanceFrequency(&fq);LONG64 now=tk(),step=fq.QuadPart/120;
    if(!g_fg_deadline || now-g_fg_deadline>step*2)g_fg_deadline=now;
    LONG64 left=g_fg_deadline-now;
    if(left>fq.QuadPart/1000){
        static HANDLE timer;
        if(!timer)timer=CreateWaitableTimerExW(NULL,NULL,2,TIMER_ALL_ACCESS);
        LARGE_INTEGER due;due.QuadPart=-(left-fq.QuadPart/2000)*10000000/fq.QuadPart;
        if(timer && SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE))WaitForSingleObject(timer,50);
        else Sleep((DWORD)(left*1000/fq.QuadPart));
    }
    while(tk()<g_fg_deadline)YieldProcessor();
    g_fg_deadline+=step;
}

static void pc_fg_capture(void){
    static int saved;const char *at=getenv("SSBU_PC_FG_SHOT");
    if(saved || !at || !g_fg_pair[1] || g_fg_generated<atoi(at))return;
    saved=1;const char *dir=getenv("SSBU_SHOT_DIR");if(!dir)dir=".";
    char path[MAX_PATH+32];snprintf(path,sizeof path,"%s/framegen-previous.bmp",dir);save_bmp_vk(path,g_fg_pair[0]);
    snprintf(path,sizeof path,"%s/framegen-middle.bmp",dir);save_bmp_vk(path,g_fg_pair[1]);
    snprintf(path,sizeof path,"%s/framegen-current.bmp",dir);save_bmp_vk(path,g_post_images[9+g_fg_slot]);
}
