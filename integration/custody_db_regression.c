/* Real CTDB and red-black trees; controlled pool allocation and accounting. */
#include "bplib_inst.h"
#include "bplib_ct_db.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

static size_t live_blocks, allocations;
static bool fail_allocation;
BPLib_MEM_Block_t *BPLib_MEM_BlockAlloc(BPLib_MEM_Pool_t *pool, size_t size)
{
    (void)pool; (void)size;
    if (fail_allocation) return NULL;
    BPLib_MEM_Block_t *block = calloc(1, sizeof(*block)); assert(block);
    /* Reused pool contents must not become tree links or an arbitrary state. */
    memset(&block->user_data.DbEntry, 0xa5, sizeof(block->user_data.DbEntry));
    live_blocks++; allocations++; return block;
}
void BPLib_MEM_BlockFree(BPLib_MEM_Pool_t *pool, BPLib_MEM_Block_t *block)
{ (void)pool; assert(block && live_blocks); live_blocks--; free(block); }
BPLib_MEM_Block_t *BPLib_MEM_GetBlockFromUserData(void *data)
{ return (BPLib_MEM_Block_t *)((uint8_t *)data - offsetof(BPLib_MEM_Block_t, user_data)); }
static BPLib_CT_DbEntry_t *by_id(BPLib_Instance_t *inst, uint32_t id)
{
    BPLib_CT_DbEntry_t *entry = NULL;
    assert(BPLib_CT_GetEntryFromCtdbWithId(&inst->Ct,id,&entry)==BPLIB_SUCCESS && entry);
    return entry;
}
static void absent_id(BPLib_Instance_t *inst,uint32_t id)
{
    BPLib_CT_DbEntry_t *entry=(void *)1;
    assert(BPLib_CT_GetEntryFromCtdbWithId(&inst->Ct,id,&entry)==BPLIB_NOT_FOUND_ERR && entry==NULL);
}
static void absent_sequence(BPLib_Instance_t *inst,uint64_t id,uint64_t number)
{
    BPLib_CT_DbEntry_t *entry=(void *)1;
    assert(BPLib_CT_GetEntryFromCtdbWithSeq(&inst->Ct,id,number,&entry)==BPLIB_NOT_FOUND_ERR && entry==NULL);
}
int main(void)
{
    BPLib_Instance_t inst={0};
    assert(BPLib_CT_InitEntry(&inst,11)==BPLIB_SUCCESS);
    BPLib_CT_DbEntry_t *original=by_id(&inst,11);
    assert(BPLib_CT_InitEntry(&inst,11)==BPLIB_RBT_DUPLICATE);
    assert(inst.Ct.CurrDbSize==1 && live_blocks==1 && by_id(&inst,11)==original);
    fail_allocation=true;
    assert(BPLib_CT_InitEntry(&inst,12)==BPLIB_NULL_PTR_ERROR);
    assert(BPLib_CT_AddToCtdb(&inst,4,5,12)==BPLIB_NULL_PTR_ERROR);
    fail_allocation=false;
    size_t before=allocations;
    for (size_t excess=0;excess<2;++excess)
    {
        inst.Ct.CurrDbSize=BPLIB_CT_DB_MAX_ENTRIES+excess;
        assert(BPLib_CT_InitEntry(&inst,12)==BPLIB_CT_FULL_DB_ERR);
        assert(BPLib_CT_AddToCtdb(&inst,4,5,12)==BPLIB_CT_FULL_DB_ERR && allocations==before);
    }
    inst.Ct.CurrDbSize=1;
    assert(BPLib_CT_AddToCtdb(&inst,4,5,12)==BPLIB_SUCCESS);
    assert(by_id(&inst,12)->State==BPLib_CT_Transmitted);
    assert(BPLib_CT_AddToCtdb(&inst,4,6,12)==BPLIB_RBT_DUPLICATE);
    absent_sequence(&inst,4,6);
    assert(BPLib_CT_AddToCtdb(&inst,4,5,13)==BPLIB_RBT_DUPLICATE);
    absent_id(&inst,13);
    assert(inst.Ct.CurrDbSize==2 && live_blocks==2);
    assert(BPLib_CT_InitEntry(&inst,13)==BPLIB_SUCCESS);
    BPLib_CT_DbEntry_t *entry=by_id(&inst,13);
    entry->State=BPLib_CT_InCustody;
    uint64_t old_id=entry->SeqId, old_number=entry->SeqNum;
    assert(BPLib_CT_UpdateEntry(&inst,entry,4,5)==BPLIB_RBT_DUPLICATE);
    assert(entry->State==BPLib_CT_InCustody && entry->SeqId==old_id && entry->SeqNum==old_number);
    assert(BPLib_CT_UpdateEntry(&inst,entry,4,7)==BPLIB_SUCCESS && entry->State==BPLib_CT_Transmitted);
    assert(BPLib_CT_UpdateEntry(&inst,entry,99,88)!=BPLIB_SUCCESS);
    assert(entry->SeqId==4 && entry->SeqNum==7 && entry->State==BPLib_CT_Transmitted);
    BPLib_CT_DbEntry_t *found=NULL;
    assert(BPLib_CT_GetEntryFromCtdbWithSeq(&inst.Ct,4,7,&found)==BPLIB_SUCCESS && found==entry);
    absent_sequence(&inst,99,88);
    inst.Ct.BundleCountInCustody=2;
    assert(BPLib_CT_RemoveFromCtdb(&inst,original)==BPLIB_SUCCESS);
    assert(BPLib_CT_RemoveFromCtdb(&inst,by_id(&inst,12))==BPLIB_SUCCESS);
    assert(BPLib_CT_RemoveFromCtdb(&inst,entry)==BPLIB_SUCCESS);
    assert(!live_blocks && !inst.Ct.CurrDbSize && !inst.Ct.BundleCountInCustody);
    for (uint32_t i=1;i<=64;++i)
    {
        assert(BPLib_CT_AddToCtdb(&inst,77,i,100+i)==BPLIB_SUCCESS);
        inst.Ct.BundleCountInCustody++;
        assert(BPLib_CT_InitEntry(&inst,100+i)==BPLIB_RBT_DUPLICATE);
        assert(BPLib_CT_AddToCtdb(&inst,77,1000+i,100+i)==BPLIB_RBT_DUPLICATE);
        absent_sequence(&inst,77,1000+i);
        assert(BPLib_CT_AddToCtdb(&inst,77,i,1000+i)==BPLIB_RBT_DUPLICATE);
        absent_id(&inst,1000+i);
        assert(inst.Ct.CurrDbSize==i && live_blocks==i);
    }
    for (uint32_t parity=0;parity<2;++parity)
        for (uint32_t i=1+parity;i<=64;i+=2)
            assert(BPLib_CT_RemoveFromCtdb(&inst,by_id(&inst,100+i))==BPLIB_SUCCESS);
    assert(!live_blocks && !inst.Ct.CurrDbSize && !inst.Ct.BundleCountInCustody);
    assert(!inst.Ct.IdTreeRoot.root && !inst.Ct.SeqTreeRoot.root);
    assert(BPLib_CT_InitEntry(NULL,1)==BPLIB_NULL_PTR_ERROR);
    assert(BPLib_CT_AddToCtdb(NULL,1,1,1)==BPLIB_NULL_PTR_ERROR);
    assert(BPLib_CT_UpdateEntry(NULL,NULL,1,1)==BPLIB_NULL_PTR_ERROR);
    assert(BPLib_CT_UpdateEntry(&inst,NULL,1,1)==BPLIB_NULL_PTR_ERROR);
    assert(BPLib_CT_RemoveFromCtdb(NULL,NULL)==BPLIB_NULL_PTR_ERROR);
    found=(void *)1;
    assert(BPLib_CT_GetEntryFromCtdbWithId(NULL,1,&found)==BPLIB_NULL_PTR_ERROR && found==NULL);
    assert(BPLib_CT_GetEntryFromCtdbWithId(&inst.Ct,1,NULL)==BPLIB_NULL_PTR_ERROR);
    found=(void *)1;
    assert(BPLib_CT_GetEntryFromCtdbWithSeq(NULL,1,1,&found)==BPLIB_NULL_PTR_ERROR && found==NULL);
    assert(BPLib_CT_GetEntryFromCtdbWithSeq(&inst.Ct,1,1,NULL)==BPLIB_NULL_PTR_ERROR);
    puts("PASS: real custody indexes, allocation/duplicate rollback, sequence retry and balanced-tree cleanup");
    return 0;
}
