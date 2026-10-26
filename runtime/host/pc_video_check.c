/* Synthetic GPU test: moving gradient, interpolation, reset after gaps, and real target preservation. */
static void pc_test_pixels(VTex *image,unsigned char *out){
    uint64_t bytes=(uint64_t)image->w*image->h*4;VMem memory;
    VkBuffer buffer=vbuf_new(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&memory);
    vt_layout(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){image->aspect&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){image->w,image->h,1};
    vkCmdCopyImageToBuffer(R.cmd,image->img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);cmd_wait_all();
    memcpy(out,memory.map,(size_t)bytes);vkDestroyBuffer(V.device,buffer,NULL);vmem_free(memory);
}
static void vkd_video_selftest(void *arg){
    int bad=0,w=128,h=64;unsigned char pixels[128*64*4],result[128*64*4];
    VTex *source=post_image(6,w,h,VK_FORMAT_R8G8B8A8_UNORM);int rect[4]={0,0,w,h};
    for(int mode=1;mode<=2;mode++){
        pc_fg_reset();
        for(int frame=0;frame<2;frame++){
            for(int y=0;y<h;y++)for(int x=0;x<w;x++){
                int at=(y*w+x)*4,px=x-frame*8;if(px<0)px=0;
                pixels[at]=px*2;pixels[at+1]=y*4;pixels[at+2]=(px+y)*255/192;pixels[at+3]=255;
            }
            vt_layout(source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);uint64_t offset=ring_raw(pixels,sizeof pixels);
            VkBufferImageCopy copy={0};copy.bufferOffset=offset;copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){w,h,1};
            vkCmdCopyBufferToImage(R.cmd,R.ring,source->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            int ready=pc_fg_prepare(source,rect,mode);bad+=ready!=(frame==1);
        }
        pc_test_pixels(g_fg_pair[1],result);
        for(int y=16;y<48;y+=8)for(int x=32;x<100;x+=8){
            int at=(y*w+x)*4;bad+=abs(result[at]-(x-4)*2)>4;bad+=abs(result[at+1]-y*4)>4;
        }
        g_fg_last_input=tk()-1000000000;bad+=pc_fg_prepare(source,rect,mode)!=0;
        printf("PC video test: mode %d midpoint pixels and history gap reset %s\n",mode,bad?"FAILED":"OK");
    }
    bad+=pc_fg_prepare(source,rect,0)!=0 || g_fg_history;
    if(pc_menu_dynamic_resolution()){
        Tex tex={0};tex.b.target=1;tex.b.width=1920;tex.b.height=1080;tex.b.depth=tex.b.samples=1;tex.b.levels=3;tex.b.format=0x25;
        VTex *before=vt_get2(&tex,0);vt_layout(before,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue color={{.25f,.5f,.75f,1}};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,VK_REMAINING_MIP_LEVELS,0,1};
        vkCmdClearColorImage(R.cmd,before->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
        Tex depth={0};depth.b=tex.b;depth.b.format=0x34;VTex *depth_before=vt_get2(&depth,0);vt_layout(depth_before,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue depth_value={.375f,0};range.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdClearDepthStencilImage(R.cmd,depth_before->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&depth_value,1,&range);cmd_wait_all();
        g_drs_gpu[2]=(PcGpuSample){.frame=2,.count=1,.ms=10};pc_dynamic_update(400);
        VTex *after=tex.vk;bad+=after==before || after->w!=960 || after->h!=540;
        unsigned char *readback=malloc((size_t)after->w*after->h*4);pc_test_pixels(after,readback);
        int at=(after->w*(after->h/2)+after->w/2)*4;
        bad+=abs(readback[at]-64)>1 || abs(readback[at+1]-128)>1 || abs(readback[at+2]-191)>1;
        VTex *depth_after=depth.vk;bad+=depth_after==depth_before || depth_after->w!=after->w || depth_after->h!=after->h;
        pc_test_pixels(depth_after,readback);float value;memcpy(&value,readback+at,4);bad+=fabsf(value-.375f)>.0001f;
        free(readback);cmd_wait_all();vt_destroy(after);vt_destroy(depth_after);tex.vk=depth.vk=NULL;
        printf("PC video test: real dynamic target resize and preserved colour/depth mips %s\n",bad?"FAILED":"OK");
    }
    *(int *)arg=bad;
}
