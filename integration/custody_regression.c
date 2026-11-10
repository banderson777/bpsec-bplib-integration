#define _GNU_SOURCE
#include "bplib_inst.h"
#include "bplib_ct.h"
#include "bplib_ct_ccs.h"
#include "bplib_em.h"
#include "bplib_as.h"
#include "bplib_ct_db.h"
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdio.h>

static uint64_t allocated = 1000;
static BPLib_Status_t allocator_status;
/** Supply the next synthetic custody sequence ID and a configurable allocator status. */
static BPLib_Status_t allocate(uint64_t *id)
{
    *id = ++allocated;
    return allocator_status;
}

/** Accept test events without starting flight telemetry services. */
BPLib_Status_t BPLib_EM_SendEvent(uint16_t id, BPLib_EM_EventType_t type, const char *text, ...)
{ (void)id; (void)type; (void)text; return BPLIB_SUCCESS; }
/** Acknowledge fixture collections without processing their data. */
BPLib_Status_t BPLib_CT_ProcessBundleSeqCollection(BPLib_Instance_t *inst, BPLib_CT_BundleSeqCollection_t *collection)
{ (void)inst; (void)collection; return BPLIB_SUCCESS; }

BPLib_EID_t BPLIB_EID_INSTANCE;
static BPLib_CT_DbEntry_t entry;
static BPLib_Status_t update_status;
static bool lookup_null;
/** Return the controlled lookup result, including success with a missing entry. */
BPLib_Status_t BPLib_CT_GetEntryFromCtdbWithId(BPLib_CT_Context_t *ct,uint32_t id,BPLib_CT_DbEntry_t **out)
{ (void)ct; (void)id; *out=lookup_null ? NULL : &entry; return BPLIB_SUCCESS; }
/** Return the active sequence ID for the fixture contact. */
uint64_t BPLib_CT_GetSequenceId(BPLib_CT_Context_t *ct,uint32_t contact)
{ return ct->SeqCounters[contact].Id; }
/** Advance the fixture counter, asserting the caller has already handled rollover. */
uint64_t BPLib_CT_GetNextSequenceNum(BPLib_CT_Context_t *ct,uint32_t contact)
{ assert(ct->SeqCounters[contact].Counter<UINT64_MAX); return ct->SeqCounters[contact].Counter++; }
/** Inject the configured index update result; publish the new key only on success. */
BPLib_Status_t BPLib_CT_UpdateEntry(BPLib_Instance_t *inst,BPLib_CT_DbEntry_t *item,uint64_t id,uint64_t number)
{ (void)inst; if (update_status != BPLIB_SUCCESS) return update_status; item->SeqId=id; item->SeqNum=number; return BPLIB_SUCCESS; }
/** Copy the fixture endpoint used by the custody block. */
void BPLib_EID_CopyEids(BPLib_EID_t *out,BPLib_EID_t value) { *out=value; }
/** Discard accounting updates outside this test’s assertions. */
void BPLib_AS_Increment(BPLib_EID_t eid,BPLib_AS_Counter_t counter,uint32_t amount)
{ (void)eid; (void)counter; (void)amount; }
/** Require a new sequence stream when the contact counter would wrap. */
static void sequence_rollover(BPLib_Instance_t *inst)
{
    BPLib_Bundle_t bundle={0};
    bundle.blocks.ExtBlocks[0].Header.BlockType=BPLib_BlockType_CTEB;
    inst->Ct.SeqCounters[0].Id=44; inst->Ct.SeqCounters[0].Counter=UINT64_MAX-1;
    entry.State=BPLib_CT_InCustody;
    assert(BPLib_CT_UpdateBundle(inst,&bundle)==BPLIB_SUCCESS);
    assert(bundle.blocks.ExtBlocks[0].BlockData.CustodyBlockData.BundleSeqNum==UINT64_MAX-1);
    assert(inst->Ct.SeqCounters[0].Counter==UINT64_MAX);
    allocated=2000; allocator_status=BPLIB_SUCCESS; entry.State=BPLib_CT_InCustody;
    assert(BPLib_CT_UpdateBundle(inst,&bundle)==BPLIB_SUCCESS);
    assert(entry.SeqId==2001 && entry.SeqNum==0 && inst->Ct.SeqCounters[0].Counter==1);
    allocator_status=BPLIB_ERROR; entry.State=BPLib_CT_InCustody;
    inst->Ct.SeqCounters[0].Counter=UINT64_MAX;
    bundle.blocks.ExtBlocks[0].Header.RequiresEncode=false;
    assert(BPLib_CT_UpdateBundle(inst,&bundle)==BPLIB_ERROR);
    assert(entry.State==BPLib_CT_InCustody && inst->Ct.SeqCounters[0].Id==2001 && inst->Ct.SeqCounters[0].Counter==UINT64_MAX);
    assert(bundle.blocks.ExtBlocks[0].BlockData.CustodyBlockData.BundleSeqId==2001 && bundle.blocks.ExtBlocks[0].BlockData.CustodyBlockData.BundleSeqNum==0);
    assert(!bundle.blocks.ExtBlocks[0].Header.RequiresEncode);
    allocator_status=BPLIB_SUCCESS;
}
/** Verify failed indexing does not publish new custody extension fields. */
static void sequence_insert_failure(BPLib_Instance_t *inst)
{
    BPLib_Bundle_t bundle={0};
    bundle.blocks.ExtBlocks[0].Header.BlockType=BPLib_BlockType_CTEB;
    BPLib_CustodyBlockData_t *cteb=&bundle.blocks.ExtBlocks[0].BlockData.CustodyBlockData;
    cteb->BundleSeqId=123; cteb->BundleSeqNum=456; cteb->BlockSrcAdminEID.Node=99;
    BPLib_CustodyBlockData_t before=*cteb;
    inst->Ct.SeqCounters[0].Id=7; inst->Ct.SeqCounters[0].Counter=42;
    entry.State=BPLib_CT_InCustody;
    update_status=BPLIB_RBT_DUPLICATE;
    assert(BPLib_CT_UpdateBundle(inst,&bundle)==BPLIB_RBT_DUPLICATE);
    assert(entry.State==BPLib_CT_InCustody && memcmp(cteb,&before,sizeof(before))==0);
    assert(!bundle.blocks.ExtBlocks[0].Header.RequiresEncode && inst->Ct.SeqCounters[0].Counter==43);
    update_status=BPLIB_SUCCESS;
    assert(BPLib_CT_UpdateBundle(inst,&bundle)==BPLIB_SUCCESS);
    assert(entry.State==BPLib_CT_Transmitted && entry.SeqId==7 && entry.SeqNum==43);
    assert(cteb->BundleSeqId==7 && cteb->BundleSeqNum==43 && bundle.blocks.ExtBlocks[0].Header.RequiresEncode);
    lookup_null=true;
    assert(BPLib_CT_UpdateBundle(inst,&bundle)==BPLIB_NOT_FOUND_ERR);
    lookup_null=false;
}
/** Exercise sequence allocation and custody error paths using an error-checking lock. */
int main(void)
{
    BPLib_Instance_t instance = {0};
    pthread_mutexattr_t attributes;
    assert(pthread_mutexattr_init(&attributes) == 0);
    assert(pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK) == 0);
    assert(pthread_mutex_init(&instance.Ct.Lock, &attributes) == 0);
    pthread_mutexattr_destroy(&attributes);
    instance.Ct.AllocateSequenceId = allocate;
    assert(BPLib_CT_AssignSeqCounter(&instance, 0) == BPLIB_SUCCESS);
    assert(instance.Ct.SeqCounters[0].Id == 1001);
    assert(BPLib_CT_AssignSeqCounter(&instance, 1) == BPLIB_SUCCESS);
    assert(instance.Ct.SeqCounters[1].Id == 1002);
    allocator_status = BPLIB_ERROR;
    assert(BPLib_CT_AssignSeqCounter(&instance, 0) == BPLIB_ERROR);
    assert(instance.Ct.SeqCounters[0].Id == 1001);
    allocator_status = BPLIB_SUCCESS;
    allocated = BPLIB_CT_SEQ_ID_ROLLOVER_VALUE - 1;
    assert(BPLib_CT_AssignSeqCounter(&instance, 0) == BPLIB_INVALID_CONFIG_ERR);
    assert(instance.Ct.SeqCounters[0].Id == 1001);
    sequence_rollover(&instance);
    sequence_insert_failure(&instance);
    assert(pthread_mutex_trylock(&instance.Ct.Lock) == 0);
    BPLib_CT_DeserializedCcs_t ccs = {0};
    assert(BPLib_CT_ProcessCcs(&instance, &ccs) == BPLIB_SUCCESS);
    /* Processing an empty CCS must not release a mutex it never acquired. */
    assert(pthread_mutex_unlock(&instance.Ct.Lock) == 0);
    instance.Ct.AllocateSequenceId = NULL;
    assert(BPLib_CT_AssignSeqCounter(&instance, 0) == BPLIB_SUCCESS);
    assert(instance.Ct.SeqCounters[0].Id == 1);
    instance.Ct.LastSeqCounterId=BPLIB_CT_SEQ_ID_ROLLOVER_VALUE-1;
    assert(BPLib_CT_AssignSeqCounter(&instance,0)==BPLIB_SUCCESS && instance.Ct.SeqCounters[0].Id==BPLIB_CT_SEQ_ID_ROLLOVER_VALUE+1);
    instance.Ct.LastSeqCounterId=UINT64_MAX;
    assert(BPLib_CT_AssignSeqCounter(&instance,0)==BPLIB_INVALID_CONFIG_ERR);
    assert(instance.Ct.SeqCounters[0].Id==BPLIB_CT_SEQ_ID_ROLLOVER_VALUE+1);
    pthread_mutex_destroy(&instance.Ct.Lock);
    puts("PASS custody allocator failure isolation and CCS lock ownership");
    return 0;
}
