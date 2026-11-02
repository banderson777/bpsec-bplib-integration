/* Real CTDB and red-black trees; controlled pool allocation and accounting. */
#include "bplib_inst.h"
#include "bplib_ct_db.h"
#include "bplib_ct_ccs.h"
#include "bplib_crc.h"
#include "bplib_bi.h"
#include "bplib_em.h"
#include "bplib_as.h"
#include <stdarg.h>
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
BPLib_EID_t BPLIB_EID_INSTANCE;
static unsigned signals, accepted, refused, policy_calls;
static uint32_t identifier=11;
BPLib_CRC_Val_t BPLib_CRC_Calculate(const void *data,size_t length,BPLib_CRC_Type_t type)
{ (void)data;(void)length;(void)type;return identifier; }
void BPLib_BI_GetBundleInfo(BPLib_Bundle_t *bundle,char *out,size_t length)
{ (void)bundle;snprintf(out,length,"admission fixture"); }
BPLib_Status_t BPLib_PDB_AcceptCustody(BPLib_Bundle_t *bundle)
{ (void)bundle;policy_calls++;return BPLIB_SUCCESS; }
BPLib_Status_t BPLib_EM_SendEvent(uint16_t id,BPLib_EM_EventType_t type,const char *format,...)
{
    (void)id;(void)type;char buffer[512];va_list args;va_start(args,format);
    vsnprintf(buffer,sizeof(buffer),format,args);va_end(args);return BPLIB_SUCCESS;
}
void BPLib_AS_Increment(BPLib_EID_t eid,BPLib_AS_Counter_t counter,uint32_t value)
{ (void)eid;(void)counter;(void)value; }
size_t BPLib_CT_GetOpenCcsIdx(BPLib_Instance_t *inst,BPLib_EID_t *peer,uint64_t sequence)
{ (void)inst;(void)peer;(void)sequence;return 0; }
BPLib_Status_t BPLib_CT_AddToOpenCcs(BPLib_Instance_t *inst,size_t slot,uint32_t contact,
                                  BPLib_CustodyBlockData_t *cteb,BPLib_CT_DispositionCode_t code)
{
    (void)inst;(void)slot;(void)contact;(void)cteb;
    signals++; if(code==BPLib_CT_CustodyAccepted)accepted++;else refused++;
    return BPLIB_SUCCESS;
}
static void reset_signals(void) { signals=accepted=refused=policy_calls=0; }
static BPLib_CT_DbEntry_t *lookup(BPLib_Instance_t *inst)
{
    BPLib_CT_DbEntry_t *entry=NULL;
    assert(BPLib_CT_GetEntryFromCtdbWithId(&inst->Ct,identifier,&entry)==BPLIB_SUCCESS);
    return entry;
}
int main(void)
{
    BPLib_Instance_t inst={0};BPLib_Bundle_t bundle={0};
    assert(pthread_mutex_init(&inst.Ct.Lock,NULL)==0);
    assert(pthread_mutex_init(&inst.BundleStorage.lock,NULL)==0);
    bundle.blocks.ExtBlocks[0].Header.BlockType=BPLib_BlockType_CTEB;
    bundle.Meta.TotalBytes=64;
    assert(BPLib_CT_InitEntry(&inst,11)==BPLIB_SUCCESS);
    BPLib_CT_DbEntry_t *original=lookup(&inst);
    original->State=BPLib_CT_InCustody;inst.Ct.BundleCountInCustody=1;
    inst.BundleStorage.BytesStorageInUse=BPLIB_MAX_STORED_BUNDLE_BYTES;
    inst.Ct.CurrDbSize=BPLIB_CT_DB_MAX_ENTRIES;
    assert(BPLib_CT_ProcessNewBundle(&inst,&bundle)==BPLIB_CT_DUPLICATE_ERR);
    assert(lookup(&inst)==original && live_blocks==1 && accepted==1 && refused==0);
    for(unsigned pending=0;pending<2;++pending)
        for(unsigned pressure=0;pressure<4;++pressure)
        {
            reset_signals();original->State=pending ? BPLib_CT_Initialized : BPLib_CT_InCustody;
            inst.Ct.BundleCountInCustody=pending ? 0 : 1;
            inst.Ct.CurrDbSize=(pressure&1) ? BPLIB_CT_DB_MAX_ENTRIES : 1;
            inst.BundleStorage.BytesStorageInUse=(pressure&2) ? SIZE_MAX : 0;
            assert(BPLib_CT_ProcessNewBundle(&inst,&bundle)==BPLIB_CT_DUPLICATE_ERR);
            assert(lookup(&inst)==original && live_blocks==1 && policy_calls==0);
            assert(signals==(pending ? 0 : 1) && refused==0);
            assert(inst.Ct.BundleCountInCustody==(pending ? 0 : 1));
        }
    original->State=BPLib_CT_Initialized;inst.Ct.CurrDbSize=1;
    assert(BPLib_CT_RemoveFromCtdb(&inst,original)==BPLIB_SUCCESS && !live_blocks);
    identifier=22;
    size_t used[]={SIZE_MAX-31,1,BPLIB_MAX_STORED_BUNDLE_BYTES,BPLIB_MAX_STORED_BUNDLE_BYTES+1,BPLIB_MAX_STORED_BUNDLE_BYTES-64};
    size_t incoming[]={64,SIZE_MAX,0,0,64};
    for(size_t i=0;i<sizeof(used)/sizeof(used[0]);++i)
    {
        reset_signals();inst.BundleStorage.BytesStorageInUse=used[i];bundle.Meta.TotalBytes=incoming[i];
        assert(BPLib_CT_ProcessNewBundle(&inst,&bundle)==BPLIB_CT_CUSTODY_REFUSED_ERR);
        assert(!live_blocks && !inst.Ct.CurrDbSize && refused==1 && !accepted && !policy_calls);
    }
    reset_signals();inst.BundleStorage.BytesStorageInUse=BPLIB_MAX_STORED_BUNDLE_BYTES-65;bundle.Meta.TotalBytes=64;
    assert(BPLib_CT_ProcessNewBundle(&inst,&bundle)==BPLIB_SUCCESS);
    assert(lookup(&inst)->State==BPLib_CT_Initialized && !signals && policy_calls==1);
    assert(BPLib_CT_RemoveFromCtdb(&inst,lookup(&inst))==BPLIB_SUCCESS && !live_blocks);
    inst.BundleStorage.BytesStorageInUse=BPLIB_MAX_STORED_BUNDLE_BYTES;
    assert(pthread_mutex_lock(&inst.BundleStorage.lock)==0);
    assert(BPLib_CT_ProcessNewBundle(&inst,&bundle)==BPLIB_SUCCESS);
    assert(lookup(&inst)->State==BPLib_CT_Initialized); /* Storage must check later. */
    assert(BPLib_CT_RemoveFromCtdb(&inst,lookup(&inst))==BPLIB_SUCCESS);
    assert(pthread_mutex_unlock(&inst.BundleStorage.lock)==0);
    assert(pthread_mutex_destroy(&inst.BundleStorage.lock)==0);
    assert(pthread_mutex_destroy(&inst.Ct.Lock)==0);
    puts("PASS: full-resource duplicates retain original custody; pending commits remain unacknowledged; admission arithmetic cannot wrap");
    return 0;
}
