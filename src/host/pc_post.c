/* Included by nvn_vk_draw.c: SMAA 1x, FSR 1 EASU/RCAS and NIS adapters from SkyRecomp. */
#include "pc_post_sources.h"
#include "pc_post_vendor/AreaTex.h"
#include "pc_post_vendor/SearchTex.h"
static VTex *g_post_images[13];
static VkPipeline g_post_pipelines[9];
static const char *const g_post_names[]={"smaa_edge.frag","smaa_weight.frag","smaa_blend.frag","easu.frag","rcas.frag","nis_easu.frag","nis_rcas.frag","motion.frag","interpolate.frag"};
static VkDescriptorSetLayout g_post_dl;
static VkPipelineLayout g_post_layout;
static VkSampler g_post_sampler;
static LONG64 g_post_frames;
static VTex *g_post_last;

static VTex *post_image(int slot,int w,int h,VkFormat format){
    VTex *vt=g_post_images[slot];if(vt && vt->w==w && vt->h==h && vt->fmt==format)return vt;
    if(vt){
        if(g_ngarbage==g_garbage_cap){g_garbage_cap=g_garbage_cap?g_garbage_cap*2:256;g_garbage=realloc(g_garbage,sizeof *g_garbage*(size_t)g_garbage_cap);}
        g_garbage[g_ngarbage].vt=vt;g_garbage[g_ngarbage++].serial=R.serial+1;
    }
    vt=calloc(1,sizeof *vt);vt->magic=0x56544558;vt->fmt=format;vt->aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    vt->w=w;vt->h=h;vt->d=vt->levels=vt->layers=vt->samples=1;vt->bpb=4;vt->bw=vt->bh=1;
    VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent=(VkExtent3D){w,h,1};
    ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
    ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VKC(vkCreateImage(V.device,&ci,NULL,&vt->img));VkMemoryRequirements rq;vkGetImageMemoryRequirements(V.device,vt->img,&rq);
    vt->mem=vmem_alloc(rq,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0);VKC(vkBindImageMemory(V.device,vt->img,vt->mem.mem,vt->mem.off));
    vt->next=g_vtex_all;if(g_vtex_all)g_vtex_all->prev=vt;g_vtex_all=vt;R.images_live++;
    return g_post_images[slot]=vt;
}
static VkImageView post_view(VTex *vt){
    if(vt->views)return vt->views->view;
    VView *view=calloc(1,sizeof *view);VkImageViewCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image=vt->img;ci.viewType=VK_IMAGE_VIEW_TYPE_2D;ci.format=vt->fmt;ci.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VKC(vkCreateImageView(V.device,&ci,NULL,&view->view));vt->views=view;return view->view;
}
static VkPipeline post_pipeline(int index){
    if(g_post_pipelines[index])return g_post_pipelines[index];
    if(!g_post_layout){
        VkDescriptorSetLayoutBinding bindings[3];for(int i=0;i<3;i++)bindings[i]=(VkDescriptorSetLayoutBinding){i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
        VkDescriptorSetLayoutCreateInfo dl={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=3;dl.pBindings=bindings;VKC(vkCreateDescriptorSetLayout(V.device,&dl,NULL,&g_post_dl));
        VkPushConstantRange range={VK_SHADER_STAGE_FRAGMENT_BIT,0,32};VkPipelineLayoutCreateInfo pl={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&g_post_dl;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&range;
        VKC(vkCreatePipelineLayout(V.device,&pl,NULL,&g_post_layout));
        VkSamplerCreateInfo si={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=0;VKC(vkCreateSampler(V.device,&si,NULL,&g_post_sampler));
    }
    int stage;uint64_t hash;VkShaderModule vs=module_from_glsl(0,pc_post_source("post.vert",&stage),"PC post vertex",&hash);
    VkShaderModule fs=module_from_glsl(1,pc_post_source(g_post_names[index],&stage),g_post_names[index],&hash);
    if(!vs||!fs)vkd_fail("PC post shader failed to compile");
    VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[0].pName="main";stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;stages[1].pName="main";
    VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds={VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState attachment={0};attachment.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo blend={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;
    VkDynamicState dyn[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic={VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=dyn;
    VkFormat format=VK_FORMAT_R8G8B8A8_UNORM;VkPipelineRenderingCreateInfo rendering={VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&format;
    VkGraphicsPipelineCreateInfo pi={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.pNext=&rendering;pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;pi.pViewportState=&vp;pi.pRasterizationState=&rs;
    pi.pMultisampleState=&ms;pi.pDepthStencilState=&ds;pi.pColorBlendState=&blend;pi.pDynamicState=&dynamic;pi.layout=g_post_layout;
    VKC(vkCreateGraphicsPipelines(V.device,g_pcache,1,&pi,NULL,&g_post_pipelines[index]));vkDestroyShaderModule(V.device,vs,NULL);vkDestroyShaderModule(V.device,fs,NULL);
    printf("PC post: compiled %s\n",g_post_names[index]);return g_post_pipelines[index];
}
static VkImageView post_lut(int index){
    int slot=7+index;VTex *t=g_post_images[slot];if(!t){
        t=post_image(slot,index?64:160,index?16:560,index?VK_FORMAT_R8_UNORM:VK_FORMAT_R8G8_UNORM);
        vt_layout(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);uint64_t size=(uint64_t)t->w*t->h*(index?1:2);
        uint64_t offset=ring_raw(index?searchTexBytes:areaTexBytes,size);
        VkBufferImageCopy copy={0};copy.bufferOffset=offset;copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){t->w,t->h,1};
        vkCmdCopyBufferToImage(R.cmd,R.ring,t->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);vt_layout(t,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }return post_view(t);
}
static void post_pass(int index,VkImageView *inputs,int n,VTex *out,const float constants[8]){
    VkPipeline pipeline=post_pipeline(index);vt_layout(out,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};color.imageView=post_view(out);color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;color.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info={VK_STRUCTURE_TYPE_RENDERING_INFO};info.renderArea.extent=(VkExtent2D){out->w,out->h};info.layerCount=1;info.colorAttachmentCount=1;info.pColorAttachments=&color;
    VkDescriptorSet set=set_alloc(g_post_dl);VkDescriptorImageInfo image[3];VkWriteDescriptorSet writes[3];
    for(int i=0;i<n;i++){image[i]=(VkDescriptorImageInfo){g_post_sampler,inputs[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};writes[i]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&image[i];}
    vkUpdateDescriptorSets(V.device,n,writes,0,NULL);vkCmdBeginRendering(R.cmd,&info);
    vkCmdBindPipeline(R.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(R.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g_post_layout,0,1,&set,0,NULL);
    VkViewport viewport={0,0,out->w,out->h,0,1};VkRect2D scissor={{0,0},{out->w,out->h}};vkCmdSetViewport(R.cmd,0,1,&viewport);vkCmdSetScissor(R.cmd,0,1,&scissor);
    vkCmdPushConstants(R.cmd,g_post_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,32,constants);vkCmdDraw(R.cmd,3,1,0,0);vkCmdEndRendering(R.cmd);
    vt_layout(out,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);R.pipe_bound=NULL;R.dyn_dirty=R.vi_dirty=1;
}
static void pc_post_process(VTex **source,int rect[4],int outw,int outh){
    int aa=pc_menu_smaa(),up=pc_menu_upscaler();int scale=up && (outw>rect[2] || outh>rect[3]);
    if(!aa && !scale)return;
    if(rect[0]<0||rect[1]<0||rect[2]<=0||rect[3]<=0||rect[0]+rect[2]>(*source)->w||rect[1]+rect[3]>(*source)->h)vkd_fail("PC post crop outside source texture");
    int iw=rect[2],ih=rect[3];VTex *input=post_image(6,iw,ih,VK_FORMAT_R8G8B8A8_UNORM);vt_layout(input,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit copy={0};copy.srcSubresource=copy.dstSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.srcOffsets[0]=(VkOffset3D){rect[0],rect[1],0};copy.srcOffsets[1]=(VkOffset3D){rect[0]+iw,rect[1]+ih,1};copy.dstOffsets[1]=(VkOffset3D){iw,ih,1};
    vkCmdBlitImage(R.cmd,(*source)->img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,input->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy,VK_FILTER_NEAREST);vt_layout(input,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VTex *current=input;
    if(aa){
        VTex *edges=post_image(0,iw,ih,VK_FORMAT_R8G8B8A8_UNORM),*weights=post_image(1,iw,ih,VK_FORMAT_R8G8B8A8_UNORM),*blend=post_image(2,iw,ih,VK_FORMAT_R8G8B8A8_UNORM);
        float metrics[8]={1.0f/iw,1.0f/ih,iw,ih};VkImageView a[]={post_view(current)};post_pass(0,a,1,edges,metrics);
        VkImageView b[]={post_view(edges),post_lut(0),post_lut(1)};post_pass(1,b,3,weights,metrics);
        VkImageView c[]={post_view(current),post_view(weights)};post_pass(2,c,2,blend,metrics);current=blend;
    }
    if(scale){
        VTex *scaled=post_image(3,outw,outh,VK_FORMAT_R8G8B8A8_UNORM),*sharp=post_image(4,outw,outh,VK_FORMAT_R8G8B8A8_UNORM);
        float constants[8]={(float)iw/outw,(float)ih/outh,.5f*iw/outw-.5f,.5f*ih/outh-.5f,iw,ih};VkImageView a[]={post_view(current)};post_pass(up==1?3:5,a,1,scaled,constants);
        float sharpen[8]={up==1?exp2f(-.2f):.35f,0,0,0,outw,outh};VkImageView b[]={post_view(scaled)};post_pass(up==1?4:6,b,1,sharp,sharpen);current=sharp;
    }
    vt_layout(current,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);g_post_last=current;*source=current;rect[0]=rect[1]=0;rect[2]=current->w;rect[3]=current->h;g_post_frames++;
    static int logged_up=-1,logged_aa=-1;if(logged_up!=up||logged_aa!=aa){printf("PC post: %dx%d -> %dx%d, SMAA=%d, upscaler=%s, upscale=%d\n",iw,ih,outw,outh,aa,up==1?"FSR1":up==2?"NIS":"bilinear",scale);logged_up=up;logged_aa=aa;}
}
static void pc_post_capture(void){
    static int saved;const char *at=getenv("SSBU_PC_POST_SHOT");
    if(!saved && at && g_post_last && g_post_frames>=atoi(at)){
        saved=1;char path[MAX_PATH+32];const char *dir=getenv("SSBU_SHOT_DIR");snprintf(path,sizeof path,"%s/pc-post.bmp",dir?dir:".");save_bmp_vk(path,g_post_last);
    }
}
#include "pc_framegen.c"
static void pc_post_shutdown(void){
    printf("PC frame generation: %lld generated, %lld output presents; simulation clock unchanged (60 Hz)\n",(long long)g_fg_generated,(long long)g_fg_outputs);
    for(int i=0;i<9;i++)if(g_post_pipelines[i])vkDestroyPipeline(V.device,g_post_pipelines[i],NULL);
    if(g_post_sampler)vkDestroySampler(V.device,g_post_sampler,NULL);if(g_post_layout)vkDestroyPipelineLayout(V.device,g_post_layout,NULL);if(g_post_dl)vkDestroyDescriptorSetLayout(V.device,g_post_dl,NULL);
    printf("PC post: %lld frames processed\n",(long long)g_post_frames);
}
