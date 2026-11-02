/* Real SQLite transactions and storage finalization; custody/queue boundaries observed. */
#include "bplib.h"
#include "bplib_stor_sql.h"
#include "bplib_stor_sql_store.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* This test isolates transaction/custody behavior, not wire encoding. */
static size_t wire_size=4;
BPLib_Status_t BPLib_CBOR_EncodeBundle(BPLib_Bundle_t *bundle, void *out, size_t capacity, size_t *length)
{
    assert(capacity>=wire_size); memset(out,0xa5,wire_size); memcpy(out,bundle->blob->user_data.BigData,4); *length=wire_size; return BPLIB_SUCCESS;
}
bool BPLib_EID_IsValid(BPLib_EID_t *eid) { (void)eid; return true; }
static BPLib_Instance_t instance;
BPLib_EID_t BPLIB_EID_INSTANCE;
static unsigned accepted, refused, pushed, freed;
static int expected_rows=2;
static BPLib_Bundle_t *outgoing[2];
static int rows(void)
{
    sqlite3_stmt *query = NULL;
    assert(sqlite3_prepare_v2(instance.BundleStorage.db, "SELECT count(*) FROM bundle_data", -1, &query, NULL) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW);
    int count = sqlite3_column_int(query, 0);
    sqlite3_finalize(query);
    return count;
}
int64_t BPLib_TIME_GetMonotonicTime(void) { return 1000; }
uint64_t BPLib_TIME_GetCurrentDtnTime(void) { return 100; }
uint32_t BPLib_NC_GetNodeConfigValue(BPLib_NC_Config_t config) { (void)config; return 3600000; }
void BPLib_AS_Increment(BPLib_EID_t eid, BPLib_AS_Counter_t counter, uint32_t amount)
{ (void)eid; (void)counter; (void)amount; }
BPLib_Status_t BPLib_EM_SendEvent(uint16_t id, BPLib_EM_EventType_t type, const char *format, ...)
{ (void)id; (void)type; (void)format; return BPLIB_SUCCESS; }
BPLib_Status_t BPLib_CT_SignalCustody(BPLib_Instance_t *inst, BPLib_Bundle_t *bundle, BPLib_CT_DispositionCode_t disposition, bool stored)
{
    (void)inst; (void)bundle;
    if (disposition == BPLib_CT_CustodyAccepted) { assert(stored && rows() == expected_rows); ++accepted; }
    else
    {
        assert(!stored);
        assert(pthread_mutex_trylock(&inst->BundleStorage.lock)==0);
        assert(pthread_mutex_unlock(&inst->BundleStorage.lock)==0);
        ++refused;
    }
    return BPLIB_SUCCESS;
}
BPLib_Status_t BPLib_CLA_GetContactRunState(uint32_t id, BPLib_CLA_ContactRunState_t *state)
{ (void)id; *state = BPLIB_CLA_STARTED; return BPLIB_SUCCESS; }
bool BPLib_QM_WaitQueueTryPush(BPLib_QM_WaitQueue_t *queue, const void *item, int timeout)
{ (void)queue; (void)timeout; assert(pushed < 2); outgoing[pushed++] = *(BPLib_Bundle_t *const *)item; return true; }
void BPLib_MEM_BundleFree(BPLib_MEM_Pool_t *pool, BPLib_Bundle_t *bundle)
{ (void)pool; free(bundle->blob); free(bundle); ++freed; }
static BPLib_Bundle_t *make_bundle(unsigned id)
{
    BPLib_Bundle_t *bundle = calloc(1, sizeof(*bundle)); assert(bundle);
    bundle->blob = calloc(1, sizeof(*bundle->blob)); assert(bundle->blob);
    bundle->blob->used_len = 4;
    bundle->blocks.PrimaryBlock.BundleId = id;
    bundle->blocks.PrimaryBlock.Timestamp.CreateTime = 1;
    bundle->blocks.PrimaryBlock.Lifetime = 3600000;
    bundle->Meta.TotalBytes = sizeof(BPLib_BBlocks_t) + 4;
    bundle->Meta.IsCustodial = true;
    bundle->Meta.EgressID = 0;
    return bundle;
}
static void batch(bool success)
{
    accepted = refused = pushed = freed = 0;
    assert(BPLib_STOR_StoreBundle(&instance, make_bundle(1)) == BPLIB_SUCCESS);
    assert(accepted == 0 && refused == 0 && rows() == 0);
    BPLib_Status_t status = BPLib_STOR_StoreBundle(&instance, make_bundle(2));
    if (!success && status==BPLIB_SUCCESS)
        fprintf(stderr,"UNEXPECTED COMMIT: rows=%d accepted=%u refused=%u pushed=%u\n",rows(),accepted,refused,pushed);
    assert((status == BPLIB_SUCCESS) == success);
    assert(instance.BundleStorage.InsertBatchSize == 0);
    if (success)
    {
        assert(rows() == 2 && accepted == 2 && refused == 0 && pushed == 2);
        for (unsigned i = 0; i < pushed; ++i) BPLib_MEM_BundleFree(&instance.pool, outgoing[i]);
        assert(freed == 2);
    }
    else
    {
        assert(rows() == 0 && accepted == 0 && refused == 2 && pushed == 0 && freed == 2);
        assert(instance.BundleStorage.BundleCountStored == 0 && instance.BundleStorage.BytesStorageInUse == 0);
    }
}
static void capacity_rejection(void)
{
    size_t used[]={SIZE_MAX-31,1,BPLIB_MAX_STORED_BUNDLE_BYTES,BPLIB_MAX_STORED_BUNDLE_BYTES+1,BPLIB_MAX_STORED_BUNDLE_BYTES-64};
    size_t incoming[]={64,SIZE_MAX,0,0,64};
    for(size_t i=0;i<sizeof(used)/sizeof(used[0]);++i)
    {
        accepted=refused=pushed=freed=0;
        instance.BundleStorage.BytesStorageInUse=used[i];
        BPLib_Bundle_t *bundle=make_bundle(99);bundle->Meta.TotalBytes=incoming[i];
        assert(BPLib_STOR_StoreBundle(&instance,bundle)==BPLIB_NO_STOR_ERR);
        assert(refused==1 && freed==1 && !accepted && !pushed && !rows());
        assert(!instance.BundleStorage.InsertBatchSize && instance.BundleStorage.BytesStorageInUse==used[i]);
    }
    accepted=refused=pushed=freed=0;
    BPLib_Bundle_t *first=make_bundle(1), *second=make_bundle(2);
    size_t cost=first->Meta.TotalBytes;
    instance.BundleStorage.BytesStorageInUse=BPLIB_MAX_STORED_BUNDLE_BYTES-2*cost;
    assert(BPLib_STOR_StoreBundle(&instance,first)==BPLIB_SUCCESS);
    assert(BPLib_STOR_StoreBundle(&instance,second)==BPLIB_NO_STOR_ERR);
    assert(instance.BundleStorage.InsertBatchSize==1 && instance.BundleStorage.InsertBatch[0]==first);
    assert(freed==1 && refused==1 && !accepted && !pushed && !rows());
    expected_rows=1;
    assert(BPLib_STOR_FlushPending(&instance)==BPLIB_SUCCESS);
    assert(rows()==1 && accepted==1 && pushed==1 && !instance.BundleStorage.InsertBatchSize);
    BPLib_MEM_BundleFree(&instance.pool,outgoing[0]);assert(freed==2);
    assert(sqlite3_exec(instance.BundleStorage.db,"DELETE FROM bundle_data",NULL,NULL,NULL)==SQLITE_OK);
    expected_rows=2;
    accepted=refused=pushed=freed=0;
    first=make_bundle(99);first->Meta.IsCustodial=false;
    instance.BundleStorage.BytesStorageInUse=BPLIB_MAX_STORED_BUNDLE_BYTES;
    assert(BPLib_STOR_StoreBundle(&instance,first)==BPLIB_NO_STOR_ERR);
    assert(freed==1 && !refused && !accepted);
    instance.BundleStorage.BytesStorageInUse=0;
    instance.BundleStorage.BundleCountStored=instance.BundleStorage.BundleCountNotEgressed=0;
}
static void ignored_write(const char *mode)
{
    const char *trigger;
    if(strcmp(mode,"unique")==0)
    {
        assert(sqlite3_exec(instance.BundleStorage.db,"CREATE TABLE unique_guard(value INTEGER UNIQUE);INSERT INTO unique_guard VALUES(1);",NULL,NULL,NULL)==SQLITE_OK);
        trigger="CREATE TRIGGER reject_unique AFTER INSERT ON bundle_data WHEN NEW.bundle_id=2 BEGIN INSERT INTO unique_guard VALUES(1);END;";
        expected_rows=1;
    }
    else if(strcmp(mode,"blob")==0)
        trigger="CREATE TRIGGER ignore_write BEFORE INSERT ON bundle_blobs WHEN (SELECT bundle_id FROM bundle_data WHERE id=NEW.bundle_row)=2 BEGIN SELECT RAISE(IGNORE);END;";
    else if(strcmp(mode,"metadata")==0)
        trigger="CREATE TRIGGER ignore_write BEFORE INSERT ON bundle_data WHEN NEW.bundle_id=2 BEGIN SELECT RAISE(IGNORE);END;";
    else
    {
        assert(strcmp(mode,"recovery")==0);
        trigger="CREATE TRIGGER ignore_write BEFORE INSERT ON bundle_recovery WHEN (SELECT bundle_id FROM bundle_data WHERE id=NEW.bundle_row)=2 BEGIN SELECT RAISE(IGNORE);END;";
    }
    assert(sqlite3_exec(instance.BundleStorage.db,trigger,NULL,NULL,NULL)==SQLITE_OK);
    batch(false);
    assert(sqlite3_exec(instance.BundleStorage.db,"DROP TRIGGER IF EXISTS reject_unique;DROP TRIGGER IF EXISTS ignore_write;DROP TABLE IF EXISTS unique_guard;",NULL,NULL,NULL)==SQLITE_OK);
    expected_rows=2;
}
static void true_duplicates(void)
{
    size_t bytes=99,duplicates=99,custody=99;
    instance.BundleStorage.BytesStorageInUse=BPLIB_MAX_STORED_BUNDLE_BYTES;
    instance.BundleStorage.InsertBatchSize=2;
    instance.BundleStorage.InsertBatch[0]=make_bundle(1);
    instance.BundleStorage.InsertBatch[1]=make_bundle(2);
    assert(BPLib_SQL_Store(&instance,&bytes,&duplicates,&custody)==BPLIB_SUCCESS);
    assert(!bytes && duplicates==2 && !custody && rows()==2);
    for(size_t i=0;i<2;++i)BPLib_MEM_BundleFree(&instance.pool,instance.BundleStorage.InsertBatch[i]);
    instance.BundleStorage.InsertBatchSize=0;
    instance.BundleStorage.BytesStorageInUse=0;
    instance.BundleStorage.InsertBatchSize=2;
    instance.BundleStorage.InsertBatch[0]=make_bundle(3);
    instance.BundleStorage.InsertBatch[1]=make_bundle(3);
    assert(BPLib_SQL_Store(&instance,&bytes,&duplicates,&custody)==BPLIB_SUCCESS);
    assert(bytes==sizeof(BPLib_BBlocks_t)+4 && duplicates==1 && custody==1 && rows()==3);
    for(size_t i=0;i<2;++i)BPLib_MEM_BundleFree(&instance.pool,instance.BundleStorage.InsertBatch[i]);
    instance.BundleStorage.InsertBatchSize=0;
}
int main(int argc,char **argv)
{
    assert(BPLib_SQL_Init(&instance, ":memory:") == BPLIB_SUCCESS);
    assert(pthread_mutex_init(&instance.BundleStorage.lock, NULL) == 0);
    if(argc>1)
    {
        assert(argc==2);ignored_write(argv[1]);
        assert(sqlite3_close(instance.BundleStorage.db)==SQLITE_OK);
        assert(pthread_mutex_destroy(&instance.BundleStorage.lock)==0);return 0;
    }
    ignored_write("unique");ignored_write("blob");ignored_write("recovery");ignored_write("metadata");
    int previous_limit=sqlite3_limit(instance.BundleStorage.db,SQLITE_LIMIT_LENGTH,512);
    wire_size=1024;
    batch(false); /* A blob bind failure must roll back, never commit NULL data. */
    wire_size=4;
    sqlite3_limit(instance.BundleStorage.db,SQLITE_LIMIT_LENGTH,previous_limit);
    capacity_rejection();
    assert(sqlite3_exec(instance.BundleStorage.db,
        "CREATE TRIGGER reject_second BEFORE INSERT ON bundle_data WHEN NEW.bundle_id=2 BEGIN SELECT RAISE(FAIL, 'injected insert failure'); END;", NULL, NULL, NULL) == SQLITE_OK);
    batch(false);
    assert(sqlite3_exec(instance.BundleStorage.db, "DROP TRIGGER reject_second; "
        "CREATE TABLE parent (id INTEGER PRIMARY KEY); "
        "CREATE TABLE bad (parent_id INTEGER REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED); "
        "CREATE TRIGGER reject_commit AFTER INSERT ON bundle_data BEGIN INSERT INTO bad VALUES(123); END;", NULL, NULL, NULL) == SQLITE_OK);
    batch(false);
    assert(sqlite3_exec(instance.BundleStorage.db, "DROP TRIGGER reject_commit;", NULL, NULL, NULL) == SQLITE_OK);
    batch(true);
    true_duplicates();
    assert(sqlite3_next_stmt(instance.BundleStorage.db, NULL) == NULL);
    assert(sqlite3_close(instance.BundleStorage.db) == SQLITE_OK);
    pthread_mutex_destroy(&instance.BundleStorage.lock);
    puts("PASS insert/COMMIT failures roll back, refuse custody, free batches, and never enqueue; success acknowledges only after commit");
    return 0;
}
