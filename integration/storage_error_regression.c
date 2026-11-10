/* Real SQLite failure injection for cleanup and post-egress/custody updates. */
#include "bplib.h"
#include "bplib_stor_sql.h"
#include "bplib_stor_sql_load.h"
#include "bplib_stor_sql_cust.h"
#include "bplib_ct_ccs.h"
#include "bplib_ct_db.h"
#include "bplib_cbor.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/** Supply a deterministic configuration value in place of mission tables. */
uint32_t BPLib_NC_GetNodeConfigValue(BPLib_NC_Config_t config) { (void)config; return 65536; }
static unsigned retry_allocations, retry_queue_calls;
/** Count portable-load allocation attempts and force allocation failure. */
BPLib_Bundle_t *BPLib_MEM_BundleAlloc(BPLib_MEM_Pool_t *pool, const void *data, size_t len)
{ (void)pool; (void)data; (void)len; ++retry_allocations; return NULL; }
/** Reject synthetic portable bundle bytes in this storage fault harness. */
BPLib_Status_t BPLib_CBOR_DecodeBundle(BPLib_Instance_t *inst, const void *wire, size_t len, BPLib_Bundle_t *bundle)
{ (void)inst; (void)wire; (void)len; (void)bundle; return BPLIB_ERROR; }
static BPLib_Instance_t instance;
BPLib_EID_t BPLIB_EID_INSTANCE;
/** Disable unsolicited custody GC so cases control when storage maintenance runs. */
bool BPLib_CT_TriggerCustodialGarbageCollection(BPLib_Instance_t *inst) { (void)inst; return true; }
/** Ignore telemetry increments outside the test’s assertions. */
void BPLib_AS_Increment(BPLib_EID_t eid, BPLib_AS_Counter_t counter, uint32_t amount)
{ (void)eid; (void)counter; (void)amount; }
/** Accept events without the flight event service. */
BPLib_Status_t BPLib_EM_SendEvent(uint16_t id, BPLib_EM_EventType_t type, const char *format, ...)
{ (void)id; (void)type; (void)format; return BPLIB_SUCCESS; }
/* Queue/memory boundaries are instrumented; SQL, batches and storage orchestration are real. */
bool BPLib_QM_IsIngressIdle(BPLib_Instance_t *inst){(void)inst;return true;}
/** Count queue probes and report room so retry paths reach push. */
bool BPLib_QM_WaitQueueIsFull(BPLib_QM_WaitQueue_t *queue){(void)queue;++retry_queue_calls;return false;}
/** Count and reject queue insertion attempts without retaining a bundle. */
bool BPLib_QM_WaitQueueTryPush(BPLib_QM_WaitQueue_t *queue,const void *item,int timeout)
{(void)queue;(void)item;(void)timeout;++retry_queue_calls;return false;}
/** Return zero memory use for the telemetry fixture. */
size_t BPLib_MEM_GetBytesInUse(BPLib_MEM_Pool_t *pool){(void)pool;return 0;}
static bool fail_allocation;
void *__real_malloc(size_t size);
/** Fail one armed allocation; otherwise delegate to the normal allocator. */
void *__wrap_malloc(size_t size)
{
    if (fail_allocation) { fail_allocation = false; return NULL; }
    return __real_malloc(size);
}
static unsigned custody_deleted, premature_delete;
/** Return deterministic monotonic time for SQL scheduling and expiration. */
int64_t BPLib_TIME_GetMonotonicTime(void) { return 1000; }
/** Read one scalar from the active fixture database and finalize the statement. */
static int scalar(const char *sql)
{
    sqlite3_stmt *query = NULL;
    assert(sqlite3_prepare_v2(instance.BundleStorage.db, sql, -1, &query, NULL) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW);
    int value = sqlite3_column_int(query, 0);
    sqlite3_finalize(query);
    return value;
}
/** Apply fixture SQL and require it to succeed. */
static void sql(const char *text)
{ assert(sqlite3_exec(instance.BundleStorage.db, text, NULL, NULL, NULL) == SQLITE_OK); }
/** Count removal of a custody entry to observe committed discard effects. */
void BPLib_CT_DeleteBundleFromCtdb(BPLib_Instance_t *inst, uint32_t id)
{
    (void)inst; (void)id;
    ++custody_deleted;
    if (!sqlite3_get_autocommit(instance.BundleStorage.db) || scalar("SELECT count(*) FROM bundle_data")) ++premature_delete;
}
/** Replace the database contents with a controlled two-row storage fixture. */
static void fixture(bool egressed, bool custodial)
{
    sql("DELETE FROM bundle_data; INSERT INTO bundle_data (id,bundle_id,action_timestamp,retransmit_timestamp,retransmit_trigger,egress_attempted,is_custodial,bundle_bytes) VALUES (1,1,0,100,3000,0,1,10),(2,2,0,100,3000,0,1,20);");
    if (egressed) sql("UPDATE bundle_data SET egress_attempted=1;");
    if (!custodial) sql("UPDATE bundle_data SET is_custodial=0;");
    instance.BundleStorage.BytesStorageInUse = 30;
    instance.BundleStorage.BundleCountStored = 2;
    instance.BundleStorage.BundleCountNotEgressed = 2;
    custody_deleted = premature_delete = 0;
}
/** Assert the test operation left the connection outside a transaction with no live statements. */
static void idle(void)
{
    assert(sqlite3_get_autocommit(instance.BundleStorage.db));
    assert(sqlite3_next_stmt(instance.BundleStorage.db, NULL) == NULL);
}
/** Deny deletion from the bundle table to simulate statement preparation failure. */
static int deny_delete(void *unused, int action, const char *table, const char *column, const char *database, const char *trigger)
{
    (void)unused; (void)column; (void)database; (void)trigger;
    return action == SQLITE_DELETE && table && strcmp(table,"bundle_data") == 0 ? SQLITE_DENY : SQLITE_OK;
}
/** Reject an update to the retransmission trigger during custody handling. */
static int deny_stop(void *unused, int action, const char *table, const char *column, const char *database, const char *trigger)
{
    (void)unused; (void)database; (void)trigger;
    return action == SQLITE_UPDATE && table && column && strcmp(table,"bundle_data") == 0 && strcmp(column,"retransmit_trigger") == 0 ? SQLITE_DENY : SQLITE_OK;
}
/** Install a deferred constraint failure triggered by the chosen write operation. */
static void commit_failure(const char *event)
{
    char text[512];
    snprintf(text,sizeof(text),"CREATE TRIGGER fail_commit AFTER %s ON bundle_data BEGIN INSERT INTO bad VALUES(123); END;",event);
    sql(text);
}
/** Verify expired or egressed discard reports failures and preserves custody until commit. */
static void discard(bool egressed)
{
    size_t count = 99;
    BPLib_Status_t (*operation)(BPLib_Instance_t *, size_t *) = egressed ? BPLib_SQL_DiscardEgressed : BPLib_SQL_DiscardExpired;
    fixture(egressed, true);
    sql("CREATE TRIGGER reject_delete BEFORE DELETE ON bundle_data WHEN OLD.bundle_id=2 BEGIN SELECT RAISE(FAIL,'injected delete failure'); END;");
    BPLib_Status_t status = operation(&instance, &count);
    fprintf(stderr,"delete failure: status=%d count=%lu rows=%d CT-deletes=%u open-txn=%d\n",status,(unsigned long)count,scalar("SELECT count(*) FROM bundle_data"),custody_deleted,!sqlite3_get_autocommit(instance.BundleStorage.db));
    assert(status != BPLIB_SUCCESS && count == 0 && custody_deleted == 0);
    assert(instance.BundleStorage.BytesStorageInUse == 30 && scalar("SELECT count(*) FROM bundle_data") == 2);
    idle();
    assert(BPLib_STOR_GarbageCollect(&instance) != BPLIB_SUCCESS);
    assert(instance.BundleStorage.BytesStorageInUse == 30 && custody_deleted == 0);
    assert(instance.BundleStorage.BundleCountStored == 2); idle();
    sql("DROP TRIGGER reject_delete;");
    commit_failure("DELETE");
    count = 99;
    assert(operation(&instance,&count) != BPLIB_SUCCESS && count == 0 && custody_deleted == 0);
    assert(instance.BundleStorage.BytesStorageInUse == 30 && scalar("SELECT count(*) FROM bundle_data") == 2);
    idle();
    sql("DROP TRIGGER fail_commit;");
    assert(sqlite3_set_authorizer(instance.BundleStorage.db,deny_delete,NULL) == SQLITE_OK);
    count = 99;
    assert(operation(&instance,&count) != BPLIB_SUCCESS && count == 0);
    assert(sqlite3_set_authorizer(instance.BundleStorage.db,NULL,NULL) == SQLITE_OK);
    idle();
    fail_allocation = true;
    assert(operation(&instance,&count) != BPLIB_SUCCESS && !fail_allocation && count == 0);
    assert(custody_deleted == 0 && scalar("SELECT count(*) FROM bundle_data") == 2);
    assert(instance.BundleStorage.BytesStorageInUse == 30); idle();
    instance.BundleStorage.BytesStorageInUse = 5;
    assert(operation(&instance,&count) != BPLIB_SUCCESS && count == 0 && custody_deleted == 0);
    assert(instance.BundleStorage.BytesStorageInUse == 5 && scalar("SELECT count(*) FROM bundle_data") == 2); idle();
    instance.BundleStorage.BytesStorageInUse = 30;
    sql("CREATE TRIGGER ignore_delete BEFORE DELETE ON bundle_data WHEN OLD.id=2 BEGIN SELECT RAISE(IGNORE); END;");
    assert(operation(&instance,&count) != BPLIB_SUCCESS && count == 0 && custody_deleted == 0);
    assert(scalar("SELECT count(*) FROM bundle_data") == 2); idle();
    sql("DROP TRIGGER ignore_delete; BEGIN;");
    assert(operation(&instance,&count) != BPLIB_SUCCESS && count == 0);
    assert(!sqlite3_get_autocommit(instance.BundleStorage.db));
    sql("ROLLBACK;"); idle();
    assert(operation(&instance,&count) == BPLIB_SUCCESS && count == 2);
    assert(instance.BundleStorage.BytesStorageInUse == 0 && custody_deleted == 2 && premature_delete == 0);
    idle();
    assert(operation(&instance,&count) == BPLIB_SUCCESS && count == 0);
    idle();
}
/** Check marking a loaded batch as attempted, including rollback on SQL errors. */
static void egress(void)
{
    BPLib_STOR_LoadBatch_t batch = {.Size=2,.BundleRowIDs={1,2}};
    fixture(false, false);
    sql("CREATE TRIGGER reject_mark BEFORE UPDATE OF egress_attempted ON bundle_data WHEN NEW.id=2 BEGIN SELECT RAISE(FAIL,'injected mark failure'); END;");
    BPLib_Status_t status = BPLib_SQL_MarkBatchEgressed(&instance,&batch,false);
    fprintf(stderr,"mark failure: status=%d flags=%d not-egressed=%lu\n",status,scalar("SELECT sum(egress_attempted) FROM bundle_data"),(unsigned long)instance.BundleStorage.BundleCountNotEgressed);
    assert(status != BPLIB_SUCCESS && scalar("SELECT sum(egress_attempted) FROM bundle_data") == 0);
    assert(instance.BundleStorage.BundleCountNotEgressed == 2); idle();
    sql("DROP TRIGGER reject_mark;");
    fixture(false, true);
    sql("CREATE TRIGGER reject_reset BEFORE UPDATE OF retransmit_timestamp ON bundle_data WHEN NEW.id=2 BEGIN SELECT RAISE(FAIL,'injected timer failure'); END;");
    assert(BPLib_SQL_MarkBatchEgressed(&instance,&batch,false) != BPLIB_SUCCESS);
    assert(scalar("SELECT sum(retransmit_timestamp) FROM bundle_data") == 200); idle();
    sql("DROP TRIGGER reject_reset;");
    fixture(false, false); commit_failure("UPDATE");
    assert(BPLib_SQL_MarkBatchEgressed(&instance,&batch,false) != BPLIB_SUCCESS);
    assert(scalar("SELECT sum(egress_attempted) FROM bundle_data") == 0 && instance.BundleStorage.BundleCountNotEgressed == 2); idle();
    sql("DROP TRIGGER fail_commit;");
    assert(BPLib_SQL_MarkBatchEgressed(&instance,&batch,true) == BPLIB_SUCCESS);
    assert(scalar("SELECT sum(egress_attempted) FROM bundle_data") == 2 && instance.BundleStorage.BundleCountNotEgressed == 0); idle();
    batch.Size=0;
    assert(BPLib_SQL_MarkBatchEgressed(&instance,&batch,false) == BPLIB_SUCCESS); idle();
}
/** Reject updates to the egress-attempted column for the retry scenario. */
static int deny_mark(void *unused,int action,const char *table,const char *column,const char *database,const char *trigger)
{
    (void)unused;(void)database;(void)trigger;
    return action==SQLITE_UPDATE && table && column && strcmp(table,"bundle_data")==0 && strcmp(column,"egress_attempted")==0 ? SQLITE_DENY : SQLITE_OK;
}
/** Check that failed egress accounting preserves queued state for another attempt. */
static void egress_retry(unsigned only)
{
    for(unsigned local=0;local<2;++local)for(unsigned fault=0;fault<4;++fault)
    {
        if((only<4 && fault!=only) || (local && fault==3))continue;
        fixture(false,fault==3);
        BPLib_STOR_LoadBatch_t *batch=local ? &instance.BundleStorage.ChannelLoadBatches[0] : &instance.BundleStorage.ContactLoadBatches[0];
        batch->Size=2;batch->ReadIndex=2;batch->BundleRowIDs[0]=1;batch->BundleRowIDs[1]=2;
        instance.BundleStorage.LastActiveTime=4321;retry_allocations=retry_queue_calls=0;
        if(fault==0)assert(sqlite3_set_authorizer(instance.BundleStorage.db,deny_mark,NULL)==SQLITE_OK);
        else if(fault==1)sql("CREATE TRIGGER reject_retry BEFORE UPDATE OF egress_attempted ON bundle_data WHEN NEW.id=2 BEGIN SELECT RAISE(FAIL,'injected second mark');END;");
        else if(fault==2)commit_failure("UPDATE");
        else sql("CREATE TRIGGER reject_retry BEFORE UPDATE OF retransmit_timestamp ON bundle_data WHEN NEW.id=2 BEGIN SELECT RAISE(FAIL,'injected second timer');END;");
        size_t count=99;BPLib_Status_t status=BPLib_STOR_EgressForID(&instance,0,local,&count);
        if(fault==0)assert(sqlite3_set_authorizer(instance.BundleStorage.db,NULL,NULL)==SQLITE_OK);
        fprintf(stderr,"retry local=%u fault=%u status=%d count=%zu batch=%zu reader=%zu flags=%d\n",local,fault,status,count,batch->Size,batch->ReadIndex,scalar("SELECT sum(egress_attempted) FROM bundle_data"));
        assert(status==BPLIB_STOR_SQL_MARK_EGRESSED_ERR && count==0);
        assert(batch->Size==2 && batch->ReadIndex==2 && batch->BundleRowIDs[0]==1 && batch->BundleRowIDs[1]==2);
        assert(scalar("SELECT sum(egress_attempted) FROM bundle_data")==0);
        assert(scalar("SELECT sum(retransmit_timestamp) FROM bundle_data")==200);
        assert(instance.BundleStorage.BundleCountNotEgressed==2 && instance.BundleStorage.BundleCountStored==2 && instance.BundleStorage.BytesStorageInUse==30);
        assert(!retry_allocations && !retry_queue_calls && instance.BundleStorage.LastActiveTime==4321);
        idle();assert(pthread_mutex_trylock(&instance.BundleStorage.lock)==0);assert(pthread_mutex_unlock(&instance.BundleStorage.lock)==0);
        if(fault==2){assert(scalar("SELECT count(*) FROM bad")==0);sql("DROP TRIGGER fail_commit;");}
        else if(fault)sql("DROP TRIGGER reject_retry;");
        count=99;assert(BPLib_STOR_EgressForID(&instance,0,local,&count)==BPLIB_SUCCESS && count==0);
        assert(batch->Size==0 && batch->ReadIndex==0);
        assert(scalar("SELECT sum(egress_attempted) FROM bundle_data")==(fault==3 ? 0 : 2));
        assert(scalar("SELECT sum(retransmit_timestamp) FROM bundle_data")==(fault==3 ? 8000 : 200));
        assert(instance.BundleStorage.BundleCountNotEgressed==(fault==3 ? 2 : 0));
        assert(!retry_allocations && !retry_queue_calls && instance.BundleStorage.LastActiveTime==4321);
        idle();assert(pthread_mutex_trylock(&instance.BundleStorage.lock)==0);assert(pthread_mutex_unlock(&instance.BundleStorage.lock)==0);
    }
}
/** Exercise rollback and retry for mixed custody update operations. */
static void custody(void)
{
    BPLib_STOR_CtUpdateBatch_t batch = {.Size=2,.BundleIDs={1,2},.Ops={BPLIB_CT_MARK_DELETE,BPLIB_CT_START_RETRANSMIT}};
    fixture(false,true);
    sql("CREATE TRIGGER reject_mark BEFORE UPDATE OF egress_attempted ON bundle_data BEGIN SELECT RAISE(FAIL,'injected mark failure'); END;");
    BPLib_Status_t status=BPLib_SQL_UpdateCustodialBundles(&instance,&batch);
    fprintf(stderr,"custody mark failure: status=%d timestamps=%d\n",status,scalar("SELECT sum(retransmit_timestamp) FROM bundle_data"));
    assert(status != BPLIB_SUCCESS && scalar("SELECT sum(retransmit_timestamp) FROM bundle_data") == 200);
    assert(instance.BundleStorage.BundleCountNotEgressed == 2); idle();
    sql("DROP TRIGGER reject_mark;");
    sql("CREATE TRIGGER reject_reset BEFORE UPDATE OF retransmit_timestamp ON bundle_data BEGIN SELECT RAISE(FAIL,'injected timer failure'); END;");
    assert(BPLib_SQL_UpdateCustodialBundles(&instance,&batch) != BPLIB_SUCCESS);
    assert(scalar("SELECT sum(egress_attempted) FROM bundle_data") == 0 && instance.BundleStorage.BundleCountNotEgressed == 2); idle();
    sql("DROP TRIGGER reject_reset;");
    commit_failure("UPDATE");
    assert(BPLib_SQL_UpdateCustodialBundles(&instance,&batch) != BPLIB_SUCCESS);
    assert(scalar("SELECT sum(egress_attempted) FROM bundle_data") == 0 && instance.BundleStorage.BundleCountNotEgressed == 2); idle();
    sql("DROP TRIGGER fail_commit;");
    assert(sqlite3_set_authorizer(instance.BundleStorage.db,deny_stop,NULL) == SQLITE_OK);
    assert(BPLib_SQL_UpdateCustodialBundles(&instance,&batch) != BPLIB_SUCCESS);
    assert(sqlite3_set_authorizer(instance.BundleStorage.db,NULL,NULL) == SQLITE_OK); idle();
    assert(BPLib_SQL_UpdateCustodialBundles(&instance,&batch) == BPLIB_SUCCESS);
    assert(scalar("SELECT sum(egress_attempted) FROM bundle_data") == 1 && instance.BundleStorage.BundleCountNotEgressed == 1);
    assert(scalar("SELECT retransmit_timestamp FROM bundle_data WHERE id=2") == 1000); idle();
    assert(BPLib_SQL_UpdateCustodialBundles(&instance,&batch) == BPLIB_SUCCESS && instance.BundleStorage.BundleCountNotEgressed == 1); idle();
    batch.Size = 1; batch.BundleIDs[0] = 2; batch.Ops[0] = BPLIB_CT_STOP_RETRANSMIT;
    assert(BPLib_SQL_UpdateCustodialBundles(&instance,&batch) == BPLIB_SUCCESS);
    sqlite3_stmt *query = NULL;
    assert(sqlite3_prepare_v2(instance.BundleStorage.db,"SELECT retransmit_trigger FROM bundle_data WHERE id=2",-1,&query,NULL) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int64(query,0) == (sqlite3_int64)BPLIB_NO_RETRANSMIT_TRIGGER);
    sqlite3_finalize(query); idle();
    sql("UPDATE bundle_data SET retransmit_timestamp=9 WHERE id=2;");
    batch.Size=2; batch.Ops[0]=BPLIB_CT_START_RETRANSMIT; batch.Ops[1]=(BPLib_CT_StorOp_t)-1;
    assert(BPLib_SQL_UpdateCustodialBundles(&instance,&batch) != BPLIB_SUCCESS);
    assert(scalar("SELECT retransmit_timestamp FROM bundle_data WHERE id=2") == 9); idle();
}
/* Poison recycled pool storage and account for every partial-load allocation. */
static unsigned load_blocks, allocation_call, fail_on_call;
/** Allocate a tracked legacy load block or fail at the configured allocation. */
BPLib_MEM_Block_t *BPLib_MEM_BlockAlloc(BPLib_MEM_Pool_t *pool, size_t size)
{
    (void)pool;
    if (++allocation_call == fail_on_call) return NULL;
    BPLib_MEM_Block_t *block = malloc(size);
    assert(block);
    memset(block, 0xa5, size);
    block->next = NULL;
    block->used_len = 0;
    ++load_blocks;
    return block;
}
/** Release a legacy fixture block chain and decrement its live allocation count. */
void BPLib_MEM_BlockListFree(BPLib_MEM_Pool_t *pool, BPLib_MEM_Block_t *head)
{
    (void)pool;
    while (head) { BPLib_MEM_Block_t *next = head->next; free(head); --load_blocks; head = next; }
}
/** Recover and free the fixture block chain that owns this bundle. */
void BPLib_MEM_BundleFree(BPLib_MEM_Pool_t *pool, BPLib_Bundle_t *bundle)
{
    BPLib_MEM_Block_t *head=(void *)((char *)bundle - offsetof(BPLib_MEM_Block_t,user_data.Bundle));
    BPLib_MEM_BlockListFree(pool,head);
}
/** Populate blob rows for a controlled legacy stored-bundle load. */
static void load_fixture(int chunk_size)
{
    sql("DELETE FROM bundle_blobs;");
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(instance.BundleStorage.db,
        "INSERT INTO bundle_blobs(bundle_row,blob_data) VALUES(2147483648,zeroblob(?))",-1,&stmt,NULL)==SQLITE_OK);
    assert(sqlite3_bind_int(stmt,1,sizeof(BPLib_BBlocks_t))==SQLITE_OK);
    assert(sqlite3_step(stmt)==SQLITE_DONE);
    sqlite3_reset(stmt);
    assert(sqlite3_bind_int(stmt,1,chunk_size)==SQLITE_OK);
    assert(sqlite3_step(stmt)==SQLITE_DONE);
    sqlite3_finalize(stmt);
    allocation_call = fail_on_call = 0;
}
/** Exercise load argument checks, corrupt rows, allocation failure, and cleanup. */
static void load(void)
{
    BPLib_Bundle_t *bundle = (void *)1;
    assert(BPLib_SQL_LoadBundle(NULL,1,&bundle)==BPLIB_NULL_PTR_ERROR && bundle==NULL);
    assert(BPLib_SQL_LoadBundle(&instance,1,NULL)==BPLIB_NULL_PTR_ERROR);
    bundle=(void *)1;
    assert(BPLib_SQL_LoadBundle(&instance,-1,&bundle)==BPLIB_STOR_PARAM_ERR && bundle==NULL);
    sql("INSERT INTO bundle_data(id,bundle_id,action_timestamp,retransmit_timestamp,retransmit_trigger,egress_attempted,is_custodial,bundle_bytes) VALUES(2147483648,1,0,0,0,0,0,4);");
    char update_bytes[128];
    snprintf(update_bytes,sizeof(update_bytes),"UPDATE bundle_data SET bundle_bytes=%zu,is_custodial=1,retransmit_trigger=123 WHERE id=2147483648;",sizeof(BPLib_BBlocks_t)+4);
    sql(update_bytes);
    load_fixture(4);
    assert(BPLib_SQL_LoadBundle(&instance,2147483648LL,&bundle)==BPLIB_SUCCESS);
    assert(bundle && bundle->blob && bundle->blob->used_len==4 && load_blocks==2);
    assert(bundle->Meta.TotalBytes==sizeof(BPLib_BBlocks_t)+4);
    assert(bundle->Meta.IsCustodial && bundle->Meta.RetransmitTime==123);
    assert(bundle->Meta.IngressID==UINT32_MAX && bundle->Meta.EgressID==UINT32_MAX && !bundle->Meta.LocalBundle);
    /* Pool block ownership is separate from the returned embedded bundle. */
    BPLib_MEM_Block_t *head=(void *)((char *)bundle - offsetof(BPLib_MEM_Block_t,user_data.Bundle));
    BPLib_MEM_BlockListFree(&instance.pool,head); idle();
    for (unsigned failure=1; failure<=2; ++failure)
    {
        allocation_call=0; fail_on_call=failure; bundle=(void *)1;
        assert(BPLib_SQL_LoadBundle(&instance,2147483648LL,&bundle)==BPLIB_STOR_NO_MEM_ERR);
        assert(bundle==NULL && load_blocks==0); idle();
    }
    sql("UPDATE bundle_data SET bundle_bytes=1;");
    allocation_call=fail_on_call=0;
    assert(BPLib_SQL_LoadBundle(&instance,2147483648LL,&bundle)==BPLIB_STOR_SQL_LOAD_ERR);
    assert(bundle==NULL && load_blocks==0); idle();
    sql(update_bytes);
    sql("UPDATE bundle_data SET is_custodial=2;");
    assert(BPLib_SQL_LoadBundle(&instance,2147483648LL,&bundle)==BPLIB_STOR_SQL_LOAD_ERR);
    assert(bundle==NULL && load_blocks==0); idle();
    sql(update_bytes);
    load_fixture(0); bundle=(void *)1;
    assert(BPLib_SQL_LoadBundle(&instance,2147483648LL,&bundle)==BPLIB_STOR_SQL_LOAD_ERR);
    assert(bundle==NULL && load_blocks==0); idle();
    load_fixture(4);
    sql("DELETE FROM bundle_blobs WHERE id=(SELECT max(id) FROM bundle_blobs);");
    assert(BPLib_SQL_LoadBundle(&instance,2147483648LL,&bundle)==BPLIB_STOR_SQL_LOAD_ERR);
    assert(bundle==NULL && load_blocks==0); idle();
    load_fixture(4);
    BPLib_BBlocks_t metadata={0};
    metadata.AdminRecordPayload=(void *)1;
    sqlite3_stmt *stmt=NULL;
    assert(sqlite3_prepare_v2(instance.BundleStorage.db,"UPDATE bundle_blobs SET blob_data=? WHERE id=(SELECT min(id) FROM bundle_blobs)",-1,&stmt,NULL)==SQLITE_OK);
    assert(sqlite3_bind_blob(stmt,1,&metadata,sizeof(metadata),SQLITE_STATIC)==SQLITE_OK);
    assert(sqlite3_step(stmt)==SQLITE_DONE); sqlite3_finalize(stmt);
    assert(BPLib_SQL_LoadBundle(&instance,2147483648LL,&bundle)==BPLIB_STOR_SQL_LOAD_ERR);
    assert(bundle==NULL && load_blocks==0); idle();
    load_fixture(BPLIB_MEM_BIG_BLK_DATA_SIZE+1); bundle=(void *)1;
    assert(BPLib_SQL_LoadBundle(&instance,2147483648LL,&bundle)==BPLIB_STOR_SQL_LOAD_ERR);
    assert(bundle==NULL && load_blocks==0); idle();
    sql("UPDATE bundle_blobs SET blob_data=x'00';"); bundle=(void *)1;
    assert(BPLib_SQL_LoadBundle(&instance,2147483648LL,&bundle)==BPLIB_STOR_SQL_LOAD_ERR);
    assert(bundle==NULL && load_blocks==0); idle();
    bundle=(void *)1;
    assert(BPLib_SQL_LoadBundle(&instance,42,&bundle)==BPLIB_STOR_NO_BUNDLE_FOUND_ERR && bundle==NULL); idle();
    sql("DROP TABLE bundle_blobs;"); bundle=(void *)1;
    assert(BPLib_SQL_LoadBundle(&instance,2147483648LL,&bundle)==BPLIB_STOR_SQL_LOAD_ERR && bundle==NULL); idle();
}
static BPLib_CT_DbEntry_t queue_entry;
static unsigned queue_lookup_calls;
static bool queue_lookup_missing;
/** Instrument sequence lookups so rejected CCS ranges can be checked for side effects. */
BPLib_Status_t BPLib_CT_GetEntryFromCtdbWithSeq(BPLib_CT_Context_t *context,
    uint64_t id, uint64_t number, BPLib_CT_DbEntry_t **entry)
{
    (void)context; (void)id; (void)number;
    ++queue_lookup_calls;
    *entry=queue_lookup_missing ? NULL : &queue_entry;
    return BPLIB_SUCCESS;
}
/** Enqueue one producer’s deterministic custody updates in the contention case. */
static void *queue_writer(void *unused)
{
    unsigned producer=(unsigned)(uintptr_t)unused;
    for (unsigned n=0; n<137; ++n)
        assert(BPLib_STOR_AddToCustodialUpdateBatch(&instance,10+producer*137+n,BPLIB_CT_MARK_DELETE)==BPLIB_SUCCESS);
    return NULL;
}
/** Exercise concurrent custody producers and verify flush or retry preserves their work. */
static void custody_queue(void)
{
    fixture(false,true);
    sql("UPDATE bundle_data SET action_timestamp=999999;");
    assert(BPLib_STOR_AddToCustodialUpdateBatch(NULL,1,BPLIB_CT_MARK_DELETE)==BPLIB_NULL_PTR_ERROR);
    assert(BPLib_STOR_AddToCustodialUpdateBatch(&instance,1,(BPLib_CT_StorOp_t)-1)==BPLIB_STOR_PARAM_ERR);
    sql("CREATE TRIGGER reject_queue BEFORE UPDATE ON bundle_data BEGIN SELECT RAISE(FAIL,'queue failure'); END;");
    for (unsigned n=0; n<BPLIB_STOR_CT_BATCH_SIZE; ++n)
        assert(BPLib_STOR_AddToCustodialUpdateBatch(&instance,1,BPLIB_CT_START_RETRANSMIT)!=BPLIB_SUCCESS);
    assert(instance.BundleStorage.CustodyUpdateBatch.Size==0);
    assert(scalar("SELECT retransmit_timestamp FROM bundle_data WHERE id=1")==100);
    idle();
    assert(pthread_mutex_init(&instance.Ct.Lock,NULL)==0);
    queue_entry.BundleId=2; queue_entry.State=BPLib_CT_InCustody;
    instance.Ct.BundleCountInCustody=1;
    BPLib_CT_BundleSeqCollection_t collection={.FirstSeqNum=1,.SeqRangeLen=1,.SeqRange={1},.DispositionCode=BPLib_CT_CustodyAccepted};
    assert(BPLib_CT_ProcessBundleSeqCollection(&instance,&collection)!=BPLIB_SUCCESS);
    assert(queue_entry.State==BPLib_CT_InCustody && instance.Ct.BundleCountInCustody==1);
    sql("DROP TRIGGER reject_queue;");
    assert(BPLib_STOR_GarbageCollect(&instance)==BPLIB_SUCCESS);
    assert(instance.BundleStorage.CustodyUpdateBatch.Size==0);
    assert(scalar("SELECT retransmit_timestamp FROM bundle_data WHERE id=1")==100); idle();
    assert(BPLib_CT_ProcessBundleSeqCollection(&instance,&collection)==BPLIB_SUCCESS);
    assert(queue_entry.State==BPLib_CT_Transferred && instance.Ct.BundleCountInCustody==0);
    pthread_mutex_destroy(&instance.Ct.Lock);
    sql("DELETE FROM bundle_data; WITH RECURSIVE ids(n) AS (SELECT 10 UNION ALL SELECT n+1 FROM ids WHERE n<557) INSERT INTO bundle_data(id,bundle_id,action_timestamp,retransmit_timestamp,retransmit_trigger,is_custodial,bundle_bytes) SELECT n,n,999999,100,3000,1,1 FROM ids;");
    instance.BundleStorage.BundleCountNotEgressed=548;
    pthread_t writers[4];
    for (unsigned n=0;n<4;++n) assert(pthread_create(&writers[n],NULL,queue_writer,(void *)(uintptr_t)n)==0);
    for (unsigned n=0;n<4;++n) assert(pthread_join(writers[n],NULL)==0);
    BPLib_STOR_UpdateCustodialBundles(&instance);
    assert(instance.BundleStorage.CustodyUpdateBatch.Size==0);
    assert(scalar("SELECT sum(egress_attempted) FROM bundle_data")==548);
    assert(instance.BundleStorage.BundleCountNotEgressed==0); idle();
}
/** Validate EID selection predicates and reject unsupported or malformed filters. */
static void lookup(void)
{
    BPLib_EID_Pattern_t patterns[3]={{.MinNode=0,.MaxNode=0,.MinService=1,.MaxService=1},
        {.MinNode=2,.MaxNode=2,.MinService=1,.MaxService=1},{0}};
    BPLib_STOR_LoadBatch_t batch={0}; size_t updated=99;
    assert(BPLib_SQL_FindForEIDs(NULL,&batch,patterns,1,true)==BPLIB_NULL_PTR_ERROR);
    assert(BPLib_SQL_FindForEIDs(&instance,&batch,patterns,0,true)==BPLIB_STOR_PARAM_ERR);
    assert(BPLib_SQL_SetNewRetransmitTrigger(NULL,0,patterns,1,321,&updated)==BPLIB_NULL_PTR_ERROR && updated==0);
    assert(BPLib_SQL_SetNewRetransmitTrigger(&instance,0,NULL,1,321,&updated)==BPLIB_NULL_PTR_ERROR);
    assert(BPLib_SQL_SetNewRetransmitTrigger(&instance,0,patterns,0,321,&updated)==BPLIB_STOR_PARAM_ERR);
    assert(BPLib_SQL_SetNewRetransmitTrigger(&instance,0,patterns,1,321,NULL)==BPLIB_NULL_PTR_ERROR);
    fixture(false,true);
    sql("UPDATE bundle_data SET action_timestamp=999999,dest_node=CASE WHEN id=1 THEN 0 ELSE 2 END,dest_service=1;");
    assert(BPLib_SQL_FindForEIDs(&instance,&batch,patterns,3,true)==BPLIB_SUCCESS && batch.Size==2); idle();
    assert(BPLib_SQL_SetNewRetransmitTrigger(&instance,0,patterns,3,321,&updated)==BPLIB_SUCCESS && updated==2);
    assert(scalar("SELECT sum(retransmit_trigger) FROM bundle_data")==642); idle();
    patterns[0].MinNode=1; patterns[0].MaxNode=0;
    assert(BPLib_SQL_FindForEIDs(&instance,&batch,patterns,1,true)==BPLIB_STOR_PARAM_ERR);
    patterns[0].MinNode=0; patterns[0].MaxNode=UINT64_MAX;
    assert(BPLib_SQL_SetNewRetransmitTrigger(&instance,0,patterns,1,321,&updated)==BPLIB_STOR_PARAM_ERR && updated==0);
    idle();
}
/** Check that malformed or unbounded sequence ranges fail before custody lookup. */
static void ccs_ranges(void)
{
    BPLib_CT_BundleSeqCollection_t collection={.FirstSeqNum=1,.SeqRangeLen=1,.SeqRange={1},.DispositionCode=BPLib_CT_CustodyAccepted};
    assert(BPLib_CT_ProcessBundleSeqCollection(NULL,&collection)==BPLIB_NULL_PTR_ERROR);
    assert(BPLib_CT_ProcessBundleSeqCollection(&instance,NULL)==BPLIB_NULL_PTR_ERROR);
    collection.SeqRangeLen=BPLIB_CT_MAX_SEQ_RANGE_LEN+1;
    assert(BPLib_CT_ProcessBundleSeqCollection(&instance,&collection)==BPLIB_STOR_PARAM_ERR);
    collection.SeqRangeLen=1; collection.FirstSeqNum=UINT64_MAX; collection.SeqRange[0]=2;
    assert(BPLib_CT_ProcessBundleSeqCollection(&instance,&collection)==BPLIB_STOR_PARAM_ERR);
    collection.FirstSeqNum=0; collection.SeqRange[0]=BPLIB_CT_MAX_CCS_SEQUENCE_SPAN+1ULL;
    assert(BPLib_CT_ProcessBundleSeqCollection(&instance,&collection)==BPLIB_STOR_PARAM_ERR);
    collection.SeqRangeLen=2; collection.SeqRange[0]=1; collection.SeqRange[1]=UINT64_MAX;
    assert(BPLib_CT_ProcessBundleSeqCollection(&instance,&collection)==BPLIB_STOR_PARAM_ERR);
    collection.FirstSeqNum=0; collection.SeqRange[0]=BPLIB_CT_MAX_CCS_SEQUENCE_SPAN; collection.SeqRange[1]=1;
    assert(BPLib_CT_ProcessBundleSeqCollection(&instance,&collection)==BPLIB_STOR_PARAM_ERR);
    assert(queue_lookup_calls==0); /* Validation precedes locks, reads and writes. */
    assert(pthread_mutex_init(&instance.Ct.Lock,NULL)==0);
    fixture(false,true); queue_entry.BundleId=1; queue_entry.State=BPLib_CT_InCustody;
    instance.Ct.BundleCountInCustody=1;
    collection.FirstSeqNum=1; collection.SeqRangeLen=1; collection.SeqRange[0]=1;
    queue_lookup_missing=true;
    assert(BPLib_CT_ProcessBundleSeqCollection(&instance,&collection)==BPLIB_NOT_FOUND_ERR);
    assert(scalar("SELECT egress_attempted FROM bundle_data WHERE id=1")==0);
    queue_lookup_missing=false;
    collection.FirstSeqNum=UINT64_MAX; collection.SeqRangeLen=1; collection.SeqRange[0]=1;
    assert(BPLib_CT_ProcessBundleSeqCollection(&instance,&collection)==BPLIB_SUCCESS);
    assert(queue_entry.State==BPLib_CT_Transferred && instance.Ct.BundleCountInCustody==0);
    assert(scalar("SELECT egress_attempted FROM bundle_data WHERE id=1")==1);
    collection.FirstSeqNum=0; collection.SeqRange[0]=BPLIB_CT_MAX_CCS_SEQUENCE_SPAN;
    queue_lookup_calls=0;
    assert(BPLib_CT_ProcessBundleSeqCollection(&instance,&collection)==BPLIB_SUCCESS);
    assert(queue_lookup_calls==BPLIB_CT_MAX_CCS_SEQUENCE_SPAN);
    pthread_mutex_destroy(&instance.Ct.Lock); idle();
}
/** Initialize the store and run the failure mode selected by CTest. */
int main(int argc, char **argv)
{
    assert(argc==2 && BPLib_SQL_Init(&instance,":memory:")==BPLIB_SUCCESS);
    assert(pthread_mutex_init(&instance.BundleStorage.lock,NULL) == 0);
    size_t count = 99;
    assert(BPLib_SQL_DiscardExpired(NULL,&count) == BPLIB_NULL_PTR_ERROR && count == 0);
    assert(BPLib_SQL_DiscardEgressed(&instance,NULL) == BPLIB_NULL_PTR_ERROR);
    assert(BPLib_SQL_MarkBatchEgressed(NULL,NULL,false) == BPLIB_NULL_PTR_ERROR);
    assert(BPLib_SQL_UpdateCustodialBundles(NULL,NULL) == BPLIB_NULL_PTR_ERROR);
    sql("CREATE TABLE parent(id INTEGER PRIMARY KEY); CREATE TABLE bad(parent_id INTEGER REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED);");
    if (!strcmp(argv[1],"discard_expired")) discard(false);
    else if (!strcmp(argv[1],"discard_egressed")) discard(true);
    else if (!strcmp(argv[1],"mark_egress")) { egress(); egress_retry(4); }
    else if (!strcmp(argv[1],"egress_retry")) egress_retry(4);
    else if (!strcmp(argv[1],"retry_prepare")) egress_retry(0);
    else if (!strcmp(argv[1],"retry_step")) egress_retry(1);
    else if (!strcmp(argv[1],"retry_commit")) egress_retry(2);
    else if (!strcmp(argv[1],"retry_timer")) egress_retry(3);
    else if (!strcmp(argv[1],"load")) load();
    else if (!strcmp(argv[1],"custody_queue")) custody_queue();
    else if (!strcmp(argv[1],"lookup")) lookup();
    else if (!strcmp(argv[1],"ccs_ranges")) ccs_ranges();
    else { assert(!strcmp(argv[1],"custody_update")); custody(); }
    assert(sqlite3_close(instance.BundleStorage.db)==SQLITE_OK);
    pthread_mutex_destroy(&instance.BundleStorage.lock);
    puts("PASS atomic error propagation, rollback, cleanup, accounting, and successful retry");
    return 0;
}
