/* Real SQL ownership, deterministic instance overlap and size telemetry checks. */
#include "bplib.h"
#include "bplib_stor_sql.h"
#include "bplib_stor_sql_load.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static BPLib_Instance_t instances[2];
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool paused,entered,resume;
uint32_t BPLib_NC_GetNodeConfigValue(BPLib_NC_Config_t config){(void)config;return 65536;}
int64_t BPLib_TIME_GetMonotonicTime(void){return 1000;}
static void execute(unsigned n,const char *sql)
{ assert(sqlite3_exec(instances[n].BundleStorage.db,sql,NULL,NULL,NULL)==SQLITE_OK); }
static int scalar(unsigned n,const char *sql)
{
    sqlite3_stmt *query=NULL;assert(sqlite3_prepare_v2(instances[n].BundleStorage.db,sql,-1,&query,NULL)==SQLITE_OK);
    assert(sqlite3_step(query)==SQLITE_ROW);int count=sqlite3_column_int(query,0);
    assert(sqlite3_finalize(query)==SQLITE_OK);return count;
}
static void pause_update(sqlite3_context *context,int argc,sqlite3_value **argv)
{
    (void)argc;(void)argv;
    if((uintptr_t)sqlite3_user_data(context)==0)
    {
        assert(pthread_mutex_lock(&gate)==0);
        if(paused)
        {
            entered=true;assert(pthread_cond_signal(&changed)==0);
            while(!resume)assert(pthread_cond_wait(&changed,&gate)==0);
        }
        assert(pthread_mutex_unlock(&gate)==0);
    }
    sqlite3_result_int(context,0);
}

void BPLib_CT_DeleteBundleFromCtdb(BPLib_Instance_t *inst,uint32_t id){(void)inst;(void)id;}
static int trace_row(unsigned event,void *unused,void *stmt,void *value)
{
    (void)event;(void)unused;(void)stmt;(void)value;
    assert(pthread_mutex_lock(&gate)==0);
    if(paused){entered=true;assert(pthread_cond_signal(&changed)==0);while(!resume)assert(pthread_cond_wait(&changed,&gate)==0);}
    assert(pthread_mutex_unlock(&gate)==0);return 0;
}
static void reset(unsigned kind)
{
    for(unsigned n=0;n<2;++n)
    {
        execute(n,"DROP TRIGGER IF EXISTS fail_second;DELETE FROM bundle_data;DELETE FROM sqlite_sequence WHERE name='bundle_data';"
          "INSERT INTO bundle_data(bundle_id,action_timestamp,dest_node,dest_service,is_custodial,egress_attempted,retransmit_trigger,retransmit_timestamp,bundle_bytes) VALUES"
          "(1,2000,10,1,0,0,7,8,3),(2,2000,11,1,1,0,7,8,5),(3,2000,99,99,0,0,7,8,11);");
        if(n)execute(n,"UPDATE bundle_data SET dest_node=20,dest_service=bundle_id,bundle_bytes=bundle_bytes+10 WHERE bundle_id<=2;");
        if(kind==2)execute(n,"UPDATE bundle_data SET action_timestamp=0 WHERE bundle_id<=2;");
        if(kind==3)execute(n,"UPDATE bundle_data SET egress_attempted=1 WHERE bundle_id<=2;");
        instances[n].BundleStorage.BundleCountNotEgressed=kind==3 ? 1 : 3;
        instances[n].BundleStorage.BytesStorageInUse=19+20*n;
    }
}
typedef struct{unsigned node,kind;BPLib_Status_t status;size_t count;uint32_t total;uint64_t bytes;BPLib_STOR_LoadBatch_t batch;} Task;
static void *run(void *arg)
{
    Task *t=arg;BPLib_Instance_t *inst=&instances[t->node];sqlite3 *db=inst->BundleStorage.db;
    assert(pthread_mutex_lock(&inst->BundleStorage.lock)==0);
    t->batch.Size=0;
    if(t->kind<2){t->batch.Size=2;t->batch.BundleRowIDs[0]=1;t->batch.BundleRowIDs[1]=2;t->status=BPLib_SQL_MarkBatchEgressed(inst,&t->batch,t->kind==0);}
    else if(t->kind==2)t->status=BPLib_SQL_DiscardExpired(inst,&t->count);
    else if(t->kind==3)t->status=BPLib_SQL_DiscardEgressed(inst,&t->count);
    else if(t->kind<6){BPLib_EID_Pattern_t p={0};p.MinNode=t->node ? 20 : 10;p.MaxNode=t->node ? 20 : 11;p.MinService=1;p.MaxService=t->node ? 2 : 1;t->status=BPLib_SQL_FindForEIDs(inst,&t->batch,&p,1,t->kind==4);}
    else if(t->kind==6)t->status=BPLib_SQL_GetNumStoredBundles(db,&t->total);
    else if(t->kind==7)t->status=BPLib_SQL_GetNumEgressed(db,&t->count);
    else t->status=BPLib_SQL_GetTotalBundleBytes(db,&t->bytes);
    assert(pthread_mutex_unlock(&inst->BundleStorage.lock)==0);return NULL;
}
static int deny(void *arg,int action,const char *table,const char *column,const char *database,const char *trigger)
{
    (void)database;(void)trigger;
    if(arg)return (action==SQLITE_DELETE || (action==SQLITE_UPDATE && column && strcmp(column,"retransmit_timestamp")==0)) ? SQLITE_DENY : SQLITE_OK;
    return table && strcmp(table,"bundle_data")==0 && (action==SQLITE_UPDATE || action==SQLITE_DELETE || action==SQLITE_READ) ? SQLITE_DENY : SQLITE_OK;
}
static void overlap(unsigned kind,unsigned failure)
{
    reset(kind);
    if(failure==1)execute(1,kind<2 ? "CREATE TRIGGER fail_second BEFORE UPDATE ON bundle_data WHEN NEW.bundle_id=2 BEGIN SELECT RAISE(FAIL,'injected update');END;" : "CREATE TRIGGER fail_second BEFORE DELETE ON bundle_data WHEN OLD.bundle_id=2 BEGIN SELECT RAISE(FAIL,'injected delete');END;");
    Task a={.node=0,.kind=kind},b={.node=1,.kind=kind};pthread_t thread;
    if(kind>=4)assert(sqlite3_trace_v2(instances[0].BundleStorage.db,SQLITE_TRACE_ROW,trace_row,NULL)==SQLITE_OK);
    assert(pthread_mutex_lock(&gate)==0);paused=true;entered=resume=false;assert(pthread_mutex_unlock(&gate)==0);
    assert(pthread_create(&thread,NULL,run,&a)==0);
    assert(pthread_mutex_lock(&gate)==0);while(!entered)assert(pthread_cond_wait(&changed,&gate)==0);assert(pthread_mutex_unlock(&gate)==0);
    if(failure>=2)assert(sqlite3_set_authorizer(instances[1].BundleStorage.db,deny,failure==3 ? (void *)1 : NULL)==SQLITE_OK);
    run(&b);
    if(failure>=2)assert(sqlite3_set_authorizer(instances[1].BundleStorage.db,NULL,NULL)==SQLITE_OK);
    assert(pthread_mutex_lock(&gate)==0);resume=true;assert(pthread_cond_signal(&changed)==0);assert(pthread_mutex_unlock(&gate)==0);
    assert(pthread_join(thread,NULL)==0);
    assert(pthread_mutex_lock(&gate)==0);paused=false;assert(pthread_mutex_unlock(&gate)==0);
    assert(sqlite3_trace_v2(instances[0].BundleStorage.db,0,NULL,NULL)==SQLITE_OK);
    assert(a.status==BPLIB_SUCCESS && ((b.status==BPLIB_SUCCESS)==!failure));
    for(unsigned n=0;n<2;++n)
    {
        sqlite3 *db=instances[n].BundleStorage.db;Task *t=n ? &b : &a;
        assert(sqlite3_get_autocommit(db) && sqlite3_next_stmt(db,NULL)==NULL);
        assert(scalar(n,"SELECT count(*) FROM bundle_data WHERE bundle_id=3 AND egress_attempted=0 AND retransmit_trigger=7 AND retransmit_timestamp=8")==1);
        if(n && failure){assert(scalar(n,"SELECT count(*) FROM bundle_data WHERE retransmit_timestamp!=8")==0);assert(scalar(n,"SELECT count(*) FROM bundle_data WHERE egress_attempted=1")==(kind==3 ? 2 : 0));assert(scalar(n,"SELECT count(*) FROM bundle_data")==3);assert(instances[n].BundleStorage.BytesStorageInUse==39);assert(instances[n].BundleStorage.BundleCountNotEgressed==(kind==3 ? 1 : 3));if(kind==2 || kind==3)assert(t->count==0);continue;}
        if(kind<2){assert(instances[n].BundleStorage.BundleCountNotEgressed==(kind==0 ? 1 : 2));assert(scalar(n,kind==0 ? "SELECT count(*) FROM bundle_data WHERE egress_attempted=1" : "SELECT count(*) FROM bundle_data WHERE retransmit_timestamp=1007")== (kind==0 ? 2 : 1));}
        else if(kind<4){assert(t->count==2 && scalar(n,"SELECT count(*) FROM bundle_data")==1);assert(instances[n].BundleStorage.BytesStorageInUse==11);}
        else if(kind<6){assert(t->batch.Size==2 && t->batch.BundleRowIDs[0]==1 && t->batch.BundleRowIDs[1]==2);}
        else if(kind==6)assert(t->total==3);
        else if(kind==7)assert(t->count==0);
        else assert(t->bytes==19+20*n);
    }
}
/* Actual SQLite geometry and failure paths for storage telemetry. */
static int interrupt_query(void *unused){(void)unused;return 1;}
typedef struct{sqlite3 *db;const char *query;} SizeInterrupt;
static int interrupt_size_step(unsigned event,void *arg,void *statement,void *unused)
{
    (void)event;(void)unused;SizeInterrupt *fault=arg;
    if(strcmp(sqlite3_sql(statement),fault->query)==0)
        sqlite3_progress_handler(fault->db,1,interrupt_query,NULL);
    return 0;
}
static int deny_pragma(void *arg,int action,const char *name,const char *value,const char *database,const char *trigger)
{
    (void)value;(void)database;(void)trigger;
    return action==SQLITE_PRAGMA && name && strcmp(name,(const char *)arg)==0 ? SQLITE_DENY : SQLITE_OK;
}
static void size_checks(bool failure_only)
{
    BPLib_Instance_t inst={0};size_t bytes=99;
    char path[]="/tmp/bplib-size-XXXXXX";int fd=mkstemp(path);assert(fd>=0 && close(fd)==0);
    assert(sqlite3_open(path,&inst.BundleStorage.db)==SQLITE_OK);
    sqlite3 *db=inst.BundleStorage.db;
    if(!failure_only)
    {
        for(unsigned page=512;page<=65536;page*=2)
        {
            char sql[128];snprintf(sql,sizeof(sql),"PRAGMA page_size=%u;VACUUM;CREATE TABLE IF NOT EXISTS size_data(data);",page);
            assert(sqlite3_exec(db,sql,NULL,NULL,NULL)==SQLITE_OK);
            sqlite3_stmt *q=NULL;assert(sqlite3_prepare_v2(db,"PRAGMA page_count;",-1,&q,NULL)==SQLITE_OK);
            assert(sqlite3_step(q)==SQLITE_ROW);size_t count=(size_t)sqlite3_column_int64(q,0);assert(sqlite3_finalize(q)==SQLITE_OK);
            assert(BPLib_SQL_GetDbSize(&inst,&bytes)==BPLIB_SUCCESS);
            fprintf(stderr,"page_size=%u pages=%zu reported=%zu expected=%zu\n",page,count,bytes,count*page);
            struct stat info;assert(stat(path,&info)==0);
            assert(bytes==count*page && bytes==(size_t)info.st_size && bytes>0 && sqlite3_next_stmt(db,NULL)==NULL);
        }
    }
    const char *queries[]={"PRAGMA page_count;","PRAGMA page_size;"};
    for(unsigned i=0;i<2;++i)
    {
        SizeInterrupt fault={db,queries[i]};
        assert(sqlite3_trace_v2(db,SQLITE_TRACE_STMT,interrupt_size_step,&fault)==SQLITE_OK);
        bytes=99;BPLib_Status_t status=BPLib_SQL_GetDbSize(&inst,&bytes);
        sqlite3_progress_handler(db,0,NULL,NULL);
        assert(sqlite3_trace_v2(db,0,NULL,NULL)==SQLITE_OK);
        fprintf(stderr,"interrupted %s status=%d reported=%zu\n",queries[i],status,bytes);
        assert(status!=BPLIB_SUCCESS && bytes==0 && sqlite3_next_stmt(db,NULL)==NULL);
    }
    const char *pragmas[]={"page_count","page_size"};
    for(unsigned i=0;i<2;++i)
    {
        assert(sqlite3_set_authorizer(db,deny_pragma,(void *)pragmas[i])==SQLITE_OK);
        bytes=99;assert(BPLib_SQL_GetDbSize(&inst,&bytes)!=BPLIB_SUCCESS && bytes==0);
        assert(sqlite3_next_stmt(db,NULL)==NULL);
        assert(sqlite3_set_authorizer(db,NULL,NULL)==SQLITE_OK);
    }
    assert(BPLib_SQL_GetDbSize(&inst,&bytes)==BPLIB_SUCCESS);
    bytes=99;assert(BPLib_SQL_GetDbSize(NULL,&bytes)==BPLIB_NULL_PTR_ERROR && bytes==0);
    assert(BPLib_SQL_GetDbSize(&inst,NULL)==BPLIB_NULL_PTR_ERROR);
    assert(sqlite3_close(db)==SQLITE_OK && unlink(path)==0);
    inst.BundleStorage.db=NULL;bytes=99;
    assert(BPLib_SQL_GetDbSize(&inst,&bytes)!=BPLIB_SUCCESS && bytes==0);
}
/* A failed lookup must not publish an incomplete batch for the next poll. */
static int interrupt_selection(unsigned event,void *arg,void *statement,void *unused)
{
    (void)event;(void)statement;(void)unused;
    sqlite3_progress_handler(arg,1,interrupt_query,NULL);return 0;
}
static void selection_failure_checks(bool capacity_only)
{
    for(unsigned local=0;local<2;++local)
    {
        reset(4);sqlite3 *db=instances[1].BundleStorage.db;
        BPLib_EID_Pattern_t pattern={.MinNode=20,.MaxNode=20,.MinService=1,.MaxService=2};
        for(unsigned seeded=0;seeded<(capacity_only ? 1u : 2u);++seeded)
        {
            BPLib_STOR_LoadBatch_t batch={0};
            size_t original=capacity_only ? BPLIB_STOR_LOADBATCHSIZE-1 : seeded;
            for(size_t i=0;i<original;++i)batch.BundleRowIDs[i]=10000+(int64_t)i;
            batch.Size=original;batch.ReadIndex=original;
            if(!capacity_only)assert(sqlite3_trace_v2(db,SQLITE_TRACE_ROW,interrupt_selection,db)==SQLITE_OK);
            BPLib_Status_t status=BPLib_SQL_FindForEIDs(&instances[1],&batch,&pattern,1,local);
            sqlite3_progress_handler(db,0,NULL,NULL);assert(sqlite3_trace_v2(db,0,NULL,NULL)==SQLITE_OK);
            fprintf(stderr,"selection local=%u capacity=%d status=%d size=%zu original=%zu reader=%zu\n",local,capacity_only,status,batch.Size,original,batch.ReadIndex);
            assert(status!=BPLIB_SUCCESS && batch.Size==original && batch.ReadIndex==original);
            for(size_t i=0;i<original;++i)assert(batch.BundleRowIDs[i]==10000+(int64_t)i);
            assert(sqlite3_get_autocommit(db) && sqlite3_next_stmt(db,NULL)==NULL);
            if(!capacity_only)
            {
                assert(BPLib_SQL_FindForEIDs(&instances[1],&batch,&pattern,1,local)==BPLIB_SUCCESS);
                assert(batch.Size==original+2 && batch.ReadIndex==original);
                assert(batch.BundleRowIDs[original]==1 && batch.BundleRowIDs[original+1]==2);
                assert(sqlite3_next_stmt(db,NULL)==NULL);
            }
        }
    }
}
int main(int argc,char **argv)
{
    for(unsigned n=0;n<2;++n){assert(BPLib_SQL_Init(&instances[n],":memory:")==BPLIB_SUCCESS);assert(pthread_mutex_init(&instances[n].BundleStorage.lock,NULL)==0);assert(sqlite3_create_function(instances[n].BundleStorage.db,"test_gate",0,SQLITE_UTF8,(void *)(uintptr_t)n,pause_update,NULL,NULL)==SQLITE_OK);execute(n,"CREATE TRIGGER pause_change AFTER UPDATE ON bundle_data WHEN NEW.bundle_id=1 BEGIN SELECT test_gate();END;CREATE TRIGGER pause_delete AFTER DELETE ON bundle_data WHEN OLD.bundle_id=1 BEGIN SELECT test_gate();END;");}
    if(argc>1 && strcmp(argv[1],"selection-failure")==0)selection_failure_checks(false);
    else if(argc>1 && strcmp(argv[1],"selection-capacity")==0)selection_failure_checks(true);
    else if(argc>1 && strncmp(argv[1],"db-size",7)==0)size_checks(strcmp(argv[1],"db-size-failure")==0);
    else if(argc>1)overlap((unsigned)atoi(argv[1]),0);
    else for(unsigned kind=0;kind<9;++kind){overlap(kind,0);overlap(kind,2);if(kind<4)overlap(kind,1);if(kind>=1 && kind<=3)overlap(kind,3);}
    if(argc==1){size_checks(false);selection_failure_checks(false);selection_failure_checks(true);}
    for(unsigned n=0;n<2;++n){assert(sqlite3_close(instances[n].BundleStorage.db)==SQLITE_OK);assert(pthread_mutex_destroy(&instances[n].BundleStorage.lock)==0);}
    assert(pthread_cond_destroy(&changed)==0 && pthread_mutex_destroy(&gate)==0);
    puts("PASS: isolated SQL loading, egress, counts and discard statements, rollback accounting and cleanup");return 0;
}
