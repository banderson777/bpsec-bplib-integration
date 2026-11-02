/* Real wire decoding, SQLite persistence, and abrupt process-exit recovery. */
#include "bplib.h"
#include "bplib_cbor.h"
#include "bplib_cbor_internal.h"
#include "bplib_stor_sql.h"
#include "bplib_stor_sql_store.h"
#include "bplib_stor_sql_load.h"
#include "bplib_ct_ccs.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static BPLib_Instance_t instance;
static int allocations, fail_after = -1;
BPLib_Status_t BPLib_MEM_PoolImplInit(BPLib_MEM_PoolImpl_t *p, void *m, size_t s, size_t b)
{ (void)p; (void)m; (void)s; (void)b; return BPLIB_SUCCESS; }
void BPLib_MEM_PoolImplDestroy(BPLib_MEM_PoolImpl_t *p) { (void)p; }
void *BPLib_MEM_PoolImplAlloc(BPLib_MEM_PoolImpl_t *p, size_t size)
{
    (void)p;
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    void *ptr=malloc(size);
    if (ptr) { memset(ptr,0xa5,size); ++allocations; }
    return ptr;
}
void BPLib_MEM_PoolImplFree(BPLib_MEM_PoolImpl_t *p, void *ptr)
{ (void)p; free(ptr); --allocations; }
size_t BPLib_MEM_GetBytesInUseImpl(BPLib_MEM_PoolImpl_t *p) { (void)p; return allocations*2048; }
size_t BPLib_MEM_GetBytesFreeImpl(BPLib_MEM_PoolImpl_t *p) { (void)p; return 0; }
int64_t BPLib_TIME_GetMonotonicTime(void) { return 1000; }
uint64_t BPLib_TIME_GetCurrentDtnTime(void) { return 100; }
uint32_t BPLib_NC_GetNodeConfigValue(BPLib_NC_Config_t config) { (void)config; return 3600000; }
BPLib_Status_t BPLib_EM_SendEvent(uint16_t id, BPLib_EM_EventType_t type, const char *format, ...)
{ (void)id; (void)type; (void)format; return BPLIB_SUCCESS; }
void BPLib_AS_Increment(BPLib_EID_t eid, BPLib_AS_Counter_t counter, uint32_t amount)
{ (void)eid; (void)counter; (void)amount; }
static void sql(const char *text)
{ assert(sqlite3_exec(instance.BundleStorage.db,text,NULL,NULL,NULL)==SQLITE_OK); }
static int scalar(const char *text)
{
    sqlite3_stmt *stmt=NULL;
    assert(sqlite3_prepare_v2(instance.BundleStorage.db,text,-1,&stmt,NULL)==SQLITE_OK);
    assert(sqlite3_step(stmt)==SQLITE_ROW);
    int value=sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt); return value;
}
static void idle(void)
{
    assert(allocations==0);
    assert(sqlite3_next_stmt(instance.BundleStorage.db,NULL)==NULL);
    assert(sqlite3_get_autocommit(instance.BundleStorage.db));
}
static unsigned char wire[]={
    0x9f, 0x89,7,2,2, 0x82,2,0x82,1,1, 0x82,2,0x82,2,1,
    0x82,1,0, 0x82,1,0, 0x1a,0,0x0f,0x42,0x40, 0x44,0,0,0,0,
    0x85,1,1,0,0,0x43,0x82,13,0xa0, 0xff
};
static void initialize(void)
{
    memset(&instance,0,sizeof(instance));
    assert(pthread_mutex_init(&instance.pool.lock,NULL)==0);
    assert(pthread_mutex_init(&instance.BundleStorage.lock,NULL)==0);
    BPLIB_EID_INSTANCE.Scheme=BPLIB_EID_SCHEME_IPN;
    BPLIB_EID_INSTANCE.IpnSspFormat=BPLIB_EID_IPN_SSP_FORMAT_TWO_DIGIT;
    BPLIB_EID_INSTANCE.Allocator=0; BPLIB_EID_INSTANCE.Node=1; BPLIB_EID_INSTANCE.Service=1;
    BPLib_CRC_Init();
    BPLib_CBOR_GenerateBlockCrc(wire,BPLib_CRC_Type_CRC32C,1,30);
}
static void store(void)
{
    BPLib_Bundle_t *bundle=BPLib_MEM_BundleAlloc(&instance.pool,wire,sizeof(wire));
    assert(bundle);
    assert(BPLib_CBOR_DecodeBundle(&instance,wire,sizeof(wire),bundle)==BPLIB_SUCCESS);
    assert(bundle->blocks.AdminRecordPayload && bundle->blocks.AdminRecordPayload->AdminRecordType==BPLib_CT_CcsRecordTypeCode);
    bundle->blocks.PrimaryBlock.BundleId=123;
    bundle->blocks.PrimaryBlock.MonoTime.Time=222;
    bundle->blocks.PrimaryBlock.MonoTime.BootEra=7;
    bundle->Meta.IngressID=7; bundle->Meta.LocalBundle=true;
    bundle->Meta.IsCustodial=true; bundle->Meta.RetransmitTime=456;
    instance.BundleStorage.InsertBatch[0]=bundle; instance.BundleStorage.InsertBatchSize=1;
    size_t bytes=0,duplicates=0,custody=0;
    assert(BPLib_SQL_Store(&instance,&bytes,&duplicates,&custody)==BPLIB_SUCCESS);
    assert(bytes==sizeof(wire)+sizeof(BPLib_BBlocks_t) && custody==1 && duplicates==0);
    instance.BundleStorage.InsertBatchSize=0;
    BPLib_MEM_BundleFree(&instance.pool,bundle);
    assert(scalar("SELECT count(*) FROM bundle_recovery WHERE format=2 AND ingress_id=7 AND local_bundle=1")==1);
    assert(scalar("SELECT sum(length(blob_data)) FROM bundle_blobs")==sizeof(wire));
    idle();
}
static void verify(void)
{
    BPLib_Bundle_t *bundle=(void *)1;
    assert(BPLib_SQL_LoadBundle(&instance,1,&bundle)==BPLIB_SUCCESS && bundle);
    assert(bundle->blocks.PrimaryBlock.BundleId==123);
    assert(bundle->blocks.PrimaryBlock.MonoTime.Time==222 && bundle->blocks.PrimaryBlock.MonoTime.BootEra==7);
    assert(bundle->Meta.IngressID==7 && bundle->Meta.LocalBundle && bundle->Meta.IsCustodial);
    assert(bundle->Meta.RetransmitTime==456 && bundle->Meta.TotalBytes==sizeof(wire)+sizeof(BPLib_BBlocks_t));
    assert(bundle->blocks.AdminRecordPayload && bundle->blocks.AdminRecordPayload->AdminRecordType==BPLib_CT_CcsRecordTypeCode);
    assert(bundle->blocks.AdminRecordPayload->AdminRecordBody.CCS.NumBundleSeqCollections==0);
    unsigned char copy[sizeof(wire)]; size_t length=0;
    assert(BPLib_MEM_BlobCopyOut(bundle,copy,sizeof(copy),&length)==BPLIB_SUCCESS && length==sizeof(wire));
    assert(memcmp(copy,wire,sizeof(wire))==0);
    BPLib_MEM_BundleFree(&instance.pool,bundle); idle();
}
static void recovery(const char *path)
{
    store();
    assert(sqlite3_close(instance.BundleStorage.db)==SQLITE_OK);
    assert(BPLib_SQL_Init(&instance,path)==BPLIB_SUCCESS);
    verify();
    for (fail_after=0;fail_after<3;)
    {
        int next=fail_after+1;
        BPLib_Bundle_t *bundle=(void *)1;
        assert(BPLib_SQL_LoadBundle(&instance,1,&bundle)!=BPLIB_SUCCESS && bundle==NULL);
        idle(); fail_after=next;
    }
    fail_after=-1; verify();
    /* Decode failure after allocating an administrative object must free it. */
    unsigned char invalid[sizeof(wire)]; memcpy(invalid,wire,sizeof(wire)); invalid[sizeof(wire)-3]=0x63;
    sqlite3_stmt *replace=NULL;
    assert(sqlite3_prepare_v2(instance.BundleStorage.db,"UPDATE bundle_blobs SET blob_data=?",-1,&replace,NULL)==SQLITE_OK);
    assert(sqlite3_bind_blob(replace,1,invalid,sizeof(invalid),SQLITE_STATIC)==SQLITE_OK);
    assert(sqlite3_step(replace)==SQLITE_DONE); sqlite3_finalize(replace); replace=NULL;
    BPLib_Bundle_t *bad=(void *)1;
    assert(BPLib_SQL_LoadBundle(&instance,1,&bad)==BPLIB_STOR_SQL_LOAD_ERR && bad==NULL); idle();
    assert(sqlite3_prepare_v2(instance.BundleStorage.db,"UPDATE bundle_blobs SET blob_data=?",-1,&replace,NULL)==SQLITE_OK);
    assert(sqlite3_bind_blob(replace,1,wire,sizeof(wire),SQLITE_STATIC)==SQLITE_OK);
    assert(sqlite3_step(replace)==SQLITE_DONE); sqlite3_finalize(replace); verify();
    sql("UPDATE bundle_recovery SET wire_bytes=wire_bytes+1;");
    BPLib_Bundle_t *bundle=(void *)1;
    assert(BPLib_SQL_LoadBundle(&instance,1,&bundle)==BPLIB_STOR_SQL_LOAD_ERR && bundle==NULL); idle();
    sql("UPDATE bundle_recovery SET wire_bytes=wire_bytes-1;");
    sql("UPDATE bundle_blobs SET blob_data=zeroblob(length(blob_data));");
    assert(BPLib_SQL_LoadBundle(&instance,1,&bundle)==BPLIB_STOR_SQL_LOAD_ERR && bundle==NULL); idle();
    /* Legacy migration never follows the saved process pointer. */
    sql("DELETE FROM bundle_recovery; DELETE FROM bundle_blobs;");
    BPLib_BBlocks_t legacy={0}; legacy.AdminRecordPayload=(void *)1;
    legacy.PrimaryBlock.BundleId=123;
    legacy.PrimaryBlock.MonoTime.Time=222; legacy.PrimaryBlock.MonoTime.BootEra=7;
    sqlite3_stmt *insert=NULL;
    assert(sqlite3_prepare_v2(instance.BundleStorage.db,"INSERT INTO bundle_blobs(bundle_row,blob_data) VALUES(1,?)",-1,&insert,NULL)==SQLITE_OK);
    assert(sqlite3_bind_blob(insert,1,&legacy,sizeof(legacy),SQLITE_STATIC)==SQLITE_OK);
    assert(sqlite3_step(insert)==SQLITE_DONE); sqlite3_reset(insert);
    assert(sqlite3_bind_blob(insert,1,wire,sizeof(wire),SQLITE_STATIC)==SQLITE_OK);
    assert(sqlite3_step(insert)==SQLITE_DONE); sqlite3_finalize(insert);
    assert(BPLib_SQL_LoadBundle(&instance,1,&bundle)==BPLIB_SUCCESS && bundle);
    assert(bundle->blocks.AdminRecordPayload && bundle->blocks.AdminRecordPayload!=(void *)1);
    assert(bundle->blocks.AdminRecordPayload->AdminRecordType==BPLib_CT_CcsRecordTypeCode);
    assert(bundle->blocks.PrimaryBlock.BundleId==123);
    assert(bundle->blocks.PrimaryBlock.MonoTime.Time==222 && bundle->blocks.PrimaryBlock.MonoTime.BootEra==7);
    assert(bundle->Meta.IngressID==UINT32_MAX && !bundle->Meta.LocalBundle);
    BPLib_MEM_BundleFree(&instance.pool,bundle); idle();
}
static void constructed(void)
{
    /* The local API owns just ADU bytes until the dirty headers are encoded. */
    unsigned char payload[4099];
    for (size_t i=0;i<sizeof(payload);++i) payload[i]=(unsigned char)(i*17);
    BPLib_Bundle_t *bundle=BPLib_MEM_BundleAlloc(&instance.pool,payload,sizeof(payload));
    assert(bundle);
    BPLib_Bundle_t template={0};
    assert(BPLib_CBOR_DecodeBundle(&instance,wire,sizeof(wire),&template)==BPLIB_SUCCESS);
    bundle->blocks.PrimaryBlock=template.blocks.PrimaryBlock;
    bundle->blocks.PrimaryBlock.BundleProcFlags=0;
    bundle->blocks.PrimaryBlock.RequiresEncode=true;
    bundle->blocks.PrimaryBlock.BundleId=123;
    bundle->blocks.PrimaryBlock.MonoTime.Time=222;
    bundle->blocks.PrimaryBlock.MonoTime.BootEra=7;
    bundle->blocks.PayloadHeader=template.blocks.PayloadHeader;
    bundle->blocks.PayloadHeader.RequiresEncode=true;
    bundle->blocks.PayloadHeader.DataOffsetStart=0;
    bundle->blocks.PayloadHeader.DataSize=sizeof(payload);
    bundle->Meta.IngressID=3; bundle->Meta.LocalBundle=true;
    BPLib_MEM_BlockFree(&instance.pool,BPLib_MEM_GetBlockFromUserData(template.blocks.AdminRecordPayload));
    unsigned char encoded[8192]; size_t encoded_length=0;
    assert(BPLib_CBOR_EncodeBundle(bundle,encoded,sizeof(encoded),&encoded_length)==BPLIB_SUCCESS);
    for (size_t capacity=encoded_length-2;capacity<=encoded_length+1;++capacity)
    {
        unsigned char *guard=malloc(capacity+1); assert(guard); memset(guard,0xa5,capacity+1);
        size_t count=99;
        BPLib_Status_t status=BPLib_CBOR_EncodeBundle(bundle,guard,capacity,&count);
        assert(guard[capacity]==0xa5);
        assert((status==BPLIB_SUCCESS)==(capacity>=encoded_length)); free(guard);
    }
    /* Local extension blocks have not received egress EIDs yet. */
    bundle->blocks.ExtBlocks[0].Header.BlockType=BPLib_BlockType_PrevNode;
    bundle->blocks.ExtBlocks[0].Header.BlockNum=2;
    bundle->blocks.ExtBlocks[0].Header.CrcType=BPLib_CRC_Type_CRC16;
    bundle->blocks.ExtBlocks[1].Header.BlockType=BPLib_BlockType_CTEB;
    bundle->blocks.ExtBlocks[1].Header.BlockNum=3;
    bundle->blocks.ExtBlocks[1].Header.CrcType=BPLib_CRC_Type_CRC16;
    instance.BundleStorage.InsertBatch[0]=bundle; instance.BundleStorage.InsertBatchSize=1;
    size_t bytes=0,duplicates=0,custody=0;
    assert(BPLib_SQL_Store(&instance,&bytes,&duplicates,&custody)==BPLIB_SUCCESS);
    assert(bytes>encoded_length+sizeof(BPLib_BBlocks_t));
    instance.BundleStorage.InsertBatchSize=0; BPLib_MEM_BundleFree(&instance.pool,bundle);
    assert(BPLib_SQL_LoadBundle(&instance,1,&bundle)==BPLIB_SUCCESS);
    assert(bundle->Meta.IngressID==3 && bundle->Meta.LocalBundle && !bundle->Meta.IsCustodial);
    assert(BPLib_EID_IsMatch(&bundle->blocks.ExtBlocks[0].BlockData.PrevNodeBlockData.PrevNodeId,&BPLIB_EID_INSTANCE));
    assert(BPLib_EID_IsMatch(&bundle->blocks.ExtBlocks[1].BlockData.CustodyBlockData.BlockSrcAdminEID,&BPLIB_EID_INSTANCE));
    unsigned char restored[sizeof(payload)];
    assert(BPLib_MEM_CopyOutFromOffset(bundle,bundle->blocks.PayloadHeader.DataOffsetStart,
        sizeof(restored),restored,sizeof(restored))==BPLIB_SUCCESS);
    assert(memcmp(restored,payload,sizeof(payload))==0);
    BPLib_MEM_BundleFree(&instance.pool,bundle); idle();
}
static BPLib_CT_OpenCcs_t sent_ccs[128];
static size_t sent_count;
void BPLib_ARP_ProcessInProgressCcs(BPLib_Instance_t *inst, BPLib_CT_OpenCcs_t *open)
{
    (void)inst;
    assert(sent_count<128 && open->InProgress && open->Size>=BPLIB_MINIMUM_ENCODED_CCS_LEN);
    BPLib_CT_DeserializedCcs_t body={0};
    for (size_t i=0;i<BPLIB_CT_MAX_SEQ_COLLECTIONS;++i)
        if (open->BundleSeqCollections[i].SeqRangeLen)
            body.BundleSeqCollections[body.NumBundleSeqCollections++]=open->BundleSeqCollections[i];
    assert(body.NumBundleSeqCollections>0);
    unsigned char data[2048]; QCBOREncodeContext enc; UsefulBufC encoded;
    QCBOREncode_Init(&enc,(UsefulBuf){data,sizeof(data)});
    assert(BPLib_CBOR_EncodeCcs(&enc,&body)==BPLIB_SUCCESS);
    assert(QCBOREncode_Finish(&enc,&encoded)==QCBOR_SUCCESS);
    QCBORDecodeContext dec; QCBORDecode_Init(&dec,encoded,QCBOR_DECODE_MODE_NORMAL);
    BPLib_CT_BundleSeqCollection_t parsed[BPLIB_CT_MAX_RECVD_SEQ_COLLECTIONS]; size_t count=0;
    assert(BPLib_QCBOR_BundleSeqCollectionParserImpl(&dec,parsed,&count)==BPLIB_SUCCESS);
    assert(count==body.NumBundleSeqCollections);
    for (size_t i=0;i<count;++i)
    {
        BPLib_CT_BundleSeqCollection_t *original=&body.BundleSeqCollections[i];
        assert(parsed[i].SeqId==original->SeqId && parsed[i].FirstSeqNum==original->FirstSeqNum);
        assert(parsed[i].DispositionCode==original->DispositionCode && parsed[i].SeqRangeLen==original->SeqRangeLen);
        assert(memcmp(parsed[i].SeqRange,original->SeqRange,sizeof(parsed[i].SeqRange))==0);
        uint64_t span=0;
        for (size_t j=0;j<parsed[i].SeqRangeLen;++j) { assert(parsed[i].SeqRange[j]>0); span+=parsed[i].SeqRange[j]; }
        assert(span<=BPLIB_CT_MAX_CCS_SEQUENCE_SPAN && span-1<=UINT64_MAX-parsed[i].FirstSeqNum);
    }
    sent_ccs[sent_count++]=*open;
}
static BPLib_EID_t sender_peer(void)
{ BPLib_EID_t peer=BPLIB_EID_INSTANCE; peer.Node=2; return peer; }
static size_t sender_add(uint64_t number,uint64_t id,BPLib_CT_DispositionCode_t code)
{
    BPLib_CustodyBlockData_t cteb={0}; cteb.BundleSeqNum=number; cteb.BundleSeqId=id;
    cteb.BlockSrcAdminEID=sender_peer();
    size_t slot=BPLib_CT_GetOpenCcsIdx(&instance,&cteb.BlockSrcAdminEID,id);
    assert(slot<BPLIB_CT_MAX_OPEN_CCS);
    assert(BPLib_CT_AddToOpenCcs(&instance,slot,0,&cteb,code)==BPLIB_SUCCESS);
    return slot;
}
static void sender_reset(void)
{
    memset(instance.Ct.OpenCcss,0,sizeof(instance.Ct.OpenCcss)); sent_count=0;
    instance.ContCtxt[0].Config.CSSizeTrigger=UINT32_MAX;
    instance.ContCtxt[0].Config.CSTimeTrigger=1000000;
}
static void sender_flush(void)
{
    for (size_t i=0;i<BPLIB_CT_MAX_OPEN_CCS;++i)
        if (instance.Ct.OpenCcss[i].InProgress) BPLib_CT_BuildAndSendOpenCcs_Impl(&instance,&instance.Ct.OpenCcss[i]);
}
static bool sender_contains(uint64_t number,uint64_t id,BPLib_CT_DispositionCode_t code)
{
    for (size_t i=0;i<sent_count;++i) for (size_t j=0;j<BPLIB_CT_MAX_SEQ_COLLECTIONS;++j)
    {
        BPLib_CT_BundleSeqCollection_t *c=&sent_ccs[i].BundleSeqCollections[j];
        uint64_t position=c->FirstSeqNum;
        if (c->SeqId!=id || c->DispositionCode!=code) continue;
        for (size_t k=0;k<c->SeqRangeLen;++k)
        {
            if (k%2==0 && number>=position && number-position<c->SeqRange[k]) return true;
            position+=c->SeqRange[k];
        }
    }
    return false;
}
static void ccs_sender(void)
{
    sender_reset();
    const uint64_t reordered[]={10,14,12,11,13,9,9}; size_t slot=0;
    for (size_t i=0;i<sizeof(reordered)/sizeof(reordered[0]);++i) slot=sender_add(reordered[i],7,BPLib_CT_CustodyAccepted);
    BPLib_CT_BundleSeqCollection_t *c=&instance.Ct.OpenCcss[slot].BundleSeqCollections[0];
    assert(sent_count==0 && c->FirstSeqNum==9 && c->SeqRangeLen==1 && c->SeqRange[0]==6 && c->LastSeqNumAdded==14);
    sender_add(10,7,BPLib_CT_CustodyRefused); sender_flush();
    for (uint64_t n=9;n<=14;++n) assert(sender_contains(n,7,BPLib_CT_CustodyAccepted));
    assert(!sender_contains(8,7,BPLib_CT_CustodyAccepted) && !sender_contains(15,7,BPLib_CT_CustodyAccepted));
    assert(sender_contains(10,7,BPLib_CT_CustodyRefused));
    sender_reset(); sender_add(0,7,BPLib_CT_CustodyAccepted);
    sender_add(BPLIB_CT_MAX_CCS_SEQUENCE_SPAN-1,7,BPLib_CT_CustodyAccepted);
    sender_add(BPLIB_CT_MAX_CCS_SEQUENCE_SPAN,7,BPLib_CT_CustodyAccepted);
    assert(sent_count==1); sender_flush(); assert(sent_count==2);
    assert(sender_contains(0,7,1) && sender_contains(BPLIB_CT_MAX_CCS_SEQUENCE_SPAN-1,7,1) && sender_contains(BPLIB_CT_MAX_CCS_SEQUENCE_SPAN,7,1));
    sender_reset(); sender_add(BPLIB_CT_MAX_CCS_SEQUENCE_SPAN+100ULL,7,1); sender_add(0,7,1);
    assert(sent_count==1); sender_flush(); assert(sender_contains(0,7,1));
    sender_reset(); sender_add(UINT64_MAX-1,7,1); slot=sender_add(UINT64_MAX,7,1);
    assert(instance.Ct.OpenCcss[slot].BundleSeqCollections[0].SeqRange[0]==2);
    sender_add(0,7,1); assert(sent_count==1); sender_add(0,8,1); sender_flush();
    assert(sender_contains(UINT64_MAX,7,1) && sender_contains(UINT64_MAX-1,7,1) && sender_contains(0,7,1) && sender_contains(0,8,1));
    sender_reset();
    if (BPLIB_CT_MAX_CCS_SEQUENCE_SPAN>=17)
    {
        for (uint64_t n=0;n<=16;n+=4) sender_add(n,7,1);
        sender_add(2,7,1); assert(sent_count==1);
        for (uint64_t n=0;n<=16;n+=4) assert(sender_contains(n,7,1));
        assert(sender_contains(2,7,1));
    }
    sender_reset(); instance.ContCtxt[0].Config.CSSizeTrigger=BPLIB_MINIMUM_ENCODED_CCS_LEN+1;
    sender_add(1,7,1); sender_add(2,7,1); assert(sent_count==2);
    assert(sender_contains(1,7,1) && sender_contains(2,7,1));
    sender_reset();
    for (uint64_t id=1;id<=BPLIB_CT_MAX_OPEN_CCS;++id) sender_add(1,id,-1);
    BPLib_EID_t peer=sender_peer(); slot=BPLib_CT_GetOpenCcsIdx(&instance,&peer,999);
    assert(sent_count==1 && instance.Ct.OpenCcss[slot].BundleSeqCollections[0].SeqId==999);
    sender_add(2,999,-1);
    assert(BPLib_CT_GetOpenCcsIdx(&instance,&peer,999)==slot && sent_count==1);
    sender_flush(); assert(sender_contains(2,999,-1));
    BPLib_CT_BundleSeqCollection_t single={.FirstSeqNum=BPLIB_CT_MAX_CCS_SEQUENCE_SPAN+100ULL,.SeqRangeLen=1,.SeqRange={1}};
    size_t size=BPLIB_MINIMUM_ENCODED_CCS_LEN+1;
    assert(BPLib_CT_InsertOldSeqNumToOpenCcs(&single,0,&size)==BPLIB_BUF_LEN_ERROR);
    assert(single.FirstSeqNum==BPLIB_CT_MAX_CCS_SEQUENCE_SPAN+100ULL && single.SeqRangeLen==1 && size==BPLIB_MINIMUM_ENCODED_CCS_LEN+1);
    assert(BPLib_CT_InsertOldSeqNumToOpenCcs(NULL,0,&size)==BPLIB_NULL_PTR_ERROR);
    BPLib_CustodyBlockData_t data={0};
    assert(BPLib_CT_AddToOpenCcs(NULL,0,0,&data,1)==BPLIB_NULL_PTR_ERROR);
    assert(BPLib_CT_AddToOpenCcs(&instance,BPLIB_CT_MAX_OPEN_CCS,0,&data,1)!=BPLIB_SUCCESS);
    assert(BPLib_CT_AddToOpenCcs(&instance,0,BPLIB_MAX_NUM_CONTACTS,&data,1)==BPLIB_INVALID_CONT_ID_ERR);
    assert(BPLib_CT_AddToOpenCcs(&instance,0,0,&data,2)==BPLIB_INVALID_CONFIG_ERR);
    assert(BPLib_CT_GetOpenCcsIdx(NULL,&peer,7)==BPLIB_CT_MAX_OPEN_CCS);
    idle();
}
static void crash(sqlite3_context *context, int argc, sqlite3_value **argv)
{ (void)context; (void)argc; (void)argv; _exit(71); }
static void child(const char *path, bool during_commit)
{
    assert(BPLib_SQL_Init(&instance,path)==BPLIB_SUCCESS);
    if (during_commit)
    {
        assert(sqlite3_create_function(instance.BundleStorage.db,"crash",0,SQLITE_UTF8,NULL,crash,NULL,NULL)==SQLITE_OK);
        sql("CREATE TRIGGER crash_update BEFORE UPDATE ON bundle_data BEGIN SELECT crash(); END;");
    }
    assert(BPLib_STOR_AddToCustodialUpdateBatch(&instance,123,BPLIB_CT_MARK_DELETE)==BPLIB_SUCCESS);
    _exit(72); /* Simulate loss before the in-memory custody state is updated. */
}
static void durability(const char *path)
{
    store();
    assert(sqlite3_close(instance.BundleStorage.db)==SQLITE_OK);
    for (int during=1;during>=0;--during)
    {
        pid_t pid=fork(); assert(pid>=0);
        if (pid==0) child(path,during!=0);
        int status=0; assert(waitpid(pid,&status,0)==pid && WIFEXITED(status));
        assert(WEXITSTATUS(status)==(during?71:72));
        assert(BPLib_SQL_Init(&instance,path)==BPLIB_SUCCESS);
        assert(scalar("SELECT egress_attempted FROM bundle_data WHERE bundle_id=123")==!during);
        assert(instance.BundleStorage.BundleCountNotEgressed==(size_t)during);
        if (during) sql("DROP TRIGGER crash_update;");
        idle(); assert(sqlite3_close(instance.BundleStorage.db)==SQLITE_OK);
    }
    assert(BPLib_SQL_Init(&instance,path)==BPLIB_SUCCESS);
}
static void ccs_parse(void)
{
    BPLib_CT_BundleSeqCollection_t parsed[BPLIB_CT_MAX_RECVD_SEQ_COLLECTIONS];
    const int64_t codes[]={1,-1,2,0,INT64_C(4294967297),-INT64_C(4294967295),INT64_MAX,INT64_MIN};
    for (size_t i=0;i<sizeof(codes)/sizeof(codes[0])+1;++i)
    {
        unsigned char wire_data[128]; QCBOREncodeContext enc; UsefulBufC bytes;
        QCBOREncode_Init(&enc,(UsefulBuf){wire_data,sizeof(wire_data)});
        QCBOREncode_OpenMap(&enc);
        if (i==sizeof(codes)/sizeof(codes[0])) QCBOREncode_AddSZString(&enc,"accepted");
        else QCBOREncode_AddInt64(&enc,codes[i]);
        QCBOREncode_OpenArray(&enc); QCBOREncode_AddUInt64(&enc,1); QCBOREncode_AddUInt64(&enc,1);
        QCBOREncode_OpenArray(&enc); QCBOREncode_AddUInt64(&enc,1); QCBOREncode_CloseArray(&enc);
        QCBOREncode_CloseArray(&enc); QCBOREncode_CloseMap(&enc);
        assert(QCBOREncode_Finish(&enc,&bytes)==QCBOR_SUCCESS);
        QCBORDecodeContext dec; QCBORDecode_Init(&dec,bytes,QCBOR_DECODE_MODE_NORMAL);
        size_t count=99;
        BPLib_Status_t status=BPLib_QCBOR_BundleSeqCollectionParserImpl(&dec,parsed,&count);
        if (i<3) { assert(status==BPLIB_SUCCESS && count==1 && (int64_t)parsed[0].DispositionCode==codes[i]); }
        else assert(status==BPLIB_CBOR_DEC_INV_DISP_CODE_ERR && count==0);
    }
    size_t count=99;
    assert(BPLib_QCBOR_BundleSeqCollectionParserImpl(NULL,parsed,&count)==BPLIB_NULL_PTR_ERROR && count==0);
    assert(BPLib_QCBOR_BundleSeqCollectionParserImpl(NULL,parsed,NULL)==BPLIB_NULL_PTR_ERROR);
}
int main(int argc,char **argv)
{
    assert(argc==2); initialize();
    char path[]="/tmp/bplib-recovery-XXXXXX";
    int fd=mkstemp(path); assert(fd>=0); close(fd);
    assert(BPLib_SQL_Init(&instance,path)==BPLIB_SUCCESS);
    if (!strcmp(argv[1],"recovery")) recovery(path);
    else if (!strcmp(argv[1],"constructed")) constructed();
    else if (!strcmp(argv[1],"ccs_parse")) ccs_parse();
    else if (!strcmp(argv[1],"ccs_sender")) ccs_sender();
    else { assert(!strcmp(argv[1],"durability")); durability(path); }
    idle(); assert(sqlite3_close(instance.BundleStorage.db)==SQLITE_OK);
    pthread_mutex_destroy(&instance.pool.lock); pthread_mutex_destroy(&instance.BundleStorage.lock);
    unlink(path); char extra[128]; snprintf(extra,sizeof(extra),"%s-wal",path); unlink(extra);
    snprintf(extra,sizeof(extra),"%s-shm",path); unlink(extra);
    puts("PASS portable administrative-record recovery and synchronous custody durability");
    return 0;
}
