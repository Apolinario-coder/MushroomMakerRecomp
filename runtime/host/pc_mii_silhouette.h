/* 3.0.3 UI fallback: retain the game's silhouette instead of generating Mii models. */
#ifndef PC_MII_SILHOUETTE_H
#define PC_MII_SILHOUETTE_H
#include "nvn_front.h"
static uint64_t pc_silhouette_texture(Ctx *c){
    static SRWLOCK lock=SRWLOCK_INIT;static uint64_t index;
    AcquireSRWLockExclusive(&lock);
    if(!index){
        uint64_t root=rd64(0x7102a03430ULL),object=nvn_front_silhouette();
        if(root && object){
            Ctx saved=*c;c->x[0]=rd64(root);c->x[1]=object;c->x[2]=c->x[3]=c->x[4]=0;
            call_indirect(c,0x71002ecd90ULL);uint32_t result=(uint32_t)c->x[0];*c=saved;
            if(result!=UINT32_MAX)index=result;
        }
    }
    uint64_t result=index;ReleaseSRWLockExclusive(&lock);return result;
}
static int pc_mii_silhouette_dispatch(Ctx *c,uint64_t address){
    uint64_t object=c->x[0];
    if(address==0x7101411360ULL){
        /* The optional 3D preview has no model in silhouette mode. */
        c->x[0]=object+48;c->x[1]=0;call_indirect(c,0x71008b9320ULL);return 1;
    }
    if(address!=0x7101419310ULL && address!=0x7101419730ULL)return 0;
    /* Some loading screens wait for a ready icon even when the model is optional. */
    uint64_t texture=pc_silhouette_texture(c);if(!texture)return 0;
    wr64(object+200,texture);wr16(object+208,128);wr16(object+210,128);
    wr8(object+224,0);wr32(object+272,1);
    c->x[0]=object+16;c->x[1]=5;call_indirect(c,0x71008b9320ULL);
    static int said;if(!said){said=1;puts("PC Mii: native silhouette icons; firmware resources not required");fflush(stdout);}
    return 1;
}
#endif
