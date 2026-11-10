/* Real SQLite transactions across instances, with deterministic encoder overlap. */
#include "bplib.h"
#include "bplib_stor_sql.h"
#include "bplib_stor_sql_store.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

static BPLib_Instance_t instances[2];
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool pause_first, entered, resume_first;
BPLib_EID_t BPLIB_EID_INSTANCE;
/** Emit fixture wire bytes and pause one encoder so two independent stores overlap
 * deterministically. */
BPLib_Status_t BPLib_CBOR_EncodeBundle(BPLib_Bundle_t *bundle,void *out,size_t capacity,size_t *length)
{
    if(bundle->blocks.PrimaryBlock.DestEID.Node==1)
    {
        assert(pthread_mutex_lock(&gate)==0);
        if(pause_first)
        {
            entered=true;assert(pthread_cond_signal(&changed)==0);
            while(!resume_first)assert(pthread_cond_wait(&changed,&gate)==0);
        }
        assert(pthread_mutex_unlock(&gate)==0);
    }
    assert(capacity>=4);memset(out,bundle->blocks.PrimaryBlock.DestEID.Node==1 ? 'A' : 'B',4);
    *length=4;return BPLIB_SUCCESS;
}
/** Accept fixture endpoints so the test can focus on isolated storage. */
bool BPLib_EID_IsValid(BPLib_EID_t *eid){(void)eid;return true;}
/** Supply a deterministic configuration value in place of mission tables. */
uint32_t BPLib_NC_GetNodeConfigValue(BPLib_NC_Config_t config){(void)config;return 65536;}
/** Return deterministic monotonic time for SQL scheduling and expiration. */
int64_t BPLib_TIME_GetMonotonicTime(void){return 1000;}
/** Return the fixture DTN time for metadata expiration calculations. */
uint64_t BPLib_TIME_GetCurrentDtnTime(void){return 100;}
/** Read a single result from the selected instance and finalize its statement. */
static int scalar(unsigned node,const char *sql)
{
    sqlite3_stmt *statement=NULL;int result;
    assert(sqlite3_prepare_v2(instances[node].BundleStorage.db,sql,-1,&statement,NULL)==SQLITE_OK);
    assert(sqlite3_step(statement)==SQLITE_ROW);result=sqlite3_column_int(statement,0);
    assert(sqlite3_finalize(statement)==SQLITE_OK);return result;
}
typedef struct{unsigned node,first,count;BPLib_Status_t status;} Task;
/** Store a batch of numbered fixture bundles on the worker’s assigned instance. */
static void *store(void *arg)
{
    Task *task=arg;task->status=BPLIB_SUCCESS;
    for(unsigned i=0;i<task->count;++i)
    {
        BPLib_Bundle_t bundle={0};bundle.blocks.PrimaryBlock.BundleId=task->first+i;
        bundle.blocks.PrimaryBlock.Timestamp.CreateTime=1;bundle.blocks.PrimaryBlock.Lifetime=3600000;
        bundle.blocks.PrimaryBlock.DestEID.Node=task->node+1;
        bundle.Meta.IsCustodial=true;bundle.Meta.TotalBytes=sizeof(BPLib_BBlocks_t)+4;
        BPLib_Instance_t *inst=&instances[task->node];size_t bytes=99,duplicates=99,custody=99;
        assert(pthread_mutex_lock(&inst->BundleStorage.lock)==0);
        inst->BundleStorage.InsertBatchSize=1;inst->BundleStorage.InsertBatch[0]=&bundle;
        task->status=BPLib_SQL_Store(inst,&bytes,&duplicates,&custody);
        inst->BundleStorage.InsertBatchSize=0;
        if(task->status==BPLIB_SUCCESS)
        {
            assert(bytes==sizeof(BPLib_BBlocks_t)+4 && !duplicates && custody==1);
            inst->BundleStorage.BytesStorageInUse+=bytes;
        }
        else assert(!bytes && !duplicates && !custody);
        assert(sqlite3_get_autocommit(inst->BundleStorage.db));
        assert(pthread_mutex_unlock(&inst->BundleStorage.lock)==0);
        if(task->status!=BPLIB_SUCCESS)break;
    }
    return NULL;
}
/** Reject blob insertion to exercise one instance’s prepare failure. */
static int deny_blob(void *unused,int action,const char *table,const char *column,const char *database,const char *trigger)
{
    (void)unused;(void)column;(void)database;(void)trigger;
    return action==SQLITE_INSERT && table && strcmp(table,"bundle_blobs")==0 ? SQLITE_DENY : SQLITE_OK;
}
/** Reject metadata lookup to exercise a failure before blob insertion. */
static int deny_lookup(void *unused,int action,const char *table,const char *column,const char *database,const char *trigger)
{
    (void)unused;(void)column;(void)database;(void)trigger;
    return action==SQLITE_READ && table && strcmp(table,"bundle_data")==0 ? SQLITE_DENY : SQLITE_OK;
}
/** Run two stores at the same time and verify payload isolation and rollback. */
static void overlap(unsigned failure)
{
    unsigned start=failure==2 ? 505 : failure==1 ? 303 : 101;
    Task first={0,start,1,0},second={1,start+1,1,0};pthread_t thread;
    assert(pthread_mutex_lock(&gate)==0);pause_first=true;entered=resume_first=false;
    assert(pthread_mutex_unlock(&gate)==0);
    assert(pthread_create(&thread,NULL,store,&first)==0);
    assert(pthread_mutex_lock(&gate)==0);
    while(!entered)assert(pthread_cond_wait(&changed,&gate)==0);
    assert(pthread_mutex_unlock(&gate)==0);
    if(failure)assert(sqlite3_set_authorizer(instances[1].BundleStorage.db,failure==1 ? deny_blob : deny_lookup,NULL)==SQLITE_OK);
    store(&second);
    if(failure)assert(sqlite3_set_authorizer(instances[1].BundleStorage.db,NULL,NULL)==SQLITE_OK);
    assert(pthread_mutex_lock(&gate)==0);resume_first=true;assert(pthread_cond_signal(&changed)==0);
    assert(pthread_mutex_unlock(&gate)==0);assert(pthread_join(thread,NULL)==0);
    assert(first.status==BPLIB_SUCCESS);
    assert((second.status==BPLIB_SUCCESS)==!failure);
    for(unsigned n=0;n<2;++n)assert(sqlite3_next_stmt(instances[n].BundleStorage.db,NULL)==NULL);
    assert(pthread_mutex_lock(&gate)==0);pause_first=false;assert(pthread_mutex_unlock(&gate)==0);
}
/** Create independent stores and run nominal and faulted overlap scenarios. */
int main(void)
{
    for(unsigned n=0;n<2;++n)
    {
        assert(BPLib_SQL_Init(&instances[n],":memory:")==BPLIB_SUCCESS);
        assert(pthread_mutex_init(&instances[n].BundleStorage.lock,NULL)==0);
    }
    overlap(false);
    assert(scalar(0,"SELECT count(*) FROM bundle_data WHERE bundle_id=101")==1);
    assert(scalar(1,"SELECT count(*) FROM bundle_data WHERE bundle_id=102")==1);
    Task tasks[2]={{0,1000,40,0},{1,1000,40,0}};pthread_t threads[2];
    for(unsigned n=0;n<2;++n)assert(pthread_create(&threads[n],NULL,store,&tasks[n])==0);
    for(unsigned n=0;n<2;++n){assert(pthread_join(threads[n],NULL)==0);assert(tasks[n].status==BPLIB_SUCCESS);}
    overlap(true);overlap(2);
    assert(scalar(0,"SELECT count(*) FROM bundle_data")==43);
    assert(scalar(1,"SELECT count(*) FROM bundle_data")==41);
    assert(scalar(1,"SELECT count(*) FROM bundle_data WHERE bundle_id=304")==0);
    assert(scalar(1,"SELECT count(*) FROM bundle_data WHERE bundle_id=506")==0);
    assert(scalar(0,"SELECT count(*) FROM bundle_blobs")==43);
    assert(scalar(1,"SELECT count(*) FROM bundle_blobs")==41);
    assert(scalar(0,"SELECT count(*) FROM bundle_recovery")==43);
    assert(scalar(1,"SELECT count(*) FROM bundle_recovery")==41);
    assert(scalar(0,"SELECT count(*) FROM bundle_blobs WHERE blob_data=x'41414141'")==43);
    assert(scalar(1,"SELECT count(*) FROM bundle_blobs WHERE blob_data=x'42424242'")==41);
    for(unsigned n=0;n<2;++n)
    {
        assert(sqlite3_next_stmt(instances[n].BundleStorage.db,NULL)==NULL);
        assert(sqlite3_close(instances[n].BundleStorage.db)==SQLITE_OK);
        assert(pthread_mutex_destroy(&instances[n].BundleStorage.lock)==0);
    }
    assert(pthread_cond_destroy(&changed)==0 && pthread_mutex_destroy(&gate)==0);
    puts("PASS: overlapping instance transactions, parallel writes, isolated preparation failure, distinct payloads, and statement cleanup");
    return 0;
}
