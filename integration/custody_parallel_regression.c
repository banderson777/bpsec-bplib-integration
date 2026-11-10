/* Real custody SQL updates, deterministic inter-instance overlap and rollback. */
#include "bplib.h"
#include "bplib_stor_sql.h"
#include "bplib_stor_sql_cust.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BPLib_Instance_t instances[2];
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool paused,entered,resume;
/** Supply a deterministic configuration value in place of mission tables. */
uint32_t BPLib_NC_GetNodeConfigValue(BPLib_NC_Config_t config){(void)config;return 65536;}
/** Return deterministic monotonic time for SQL scheduling and expiration. */
int64_t BPLib_TIME_GetMonotonicTime(void){return 1000;}
/** Execute setup SQL on the selected instance and require success. */
static void execute(unsigned n,const char *sql)
{ assert(sqlite3_exec(instances[n].BundleStorage.db,sql,NULL,NULL,NULL)==SQLITE_OK); }
/** Read one SQLite scalar from the selected instance and finalize the statement. */
static int scalar(unsigned n,const char *sql)
{
    sqlite3_stmt *query=NULL;assert(sqlite3_prepare_v2(instances[n].BundleStorage.db,sql,-1,&query,NULL)==SQLITE_OK);
    assert(sqlite3_step(query)==SQLITE_ROW);int count=sqlite3_column_int(query,0);
    assert(sqlite3_finalize(query)==SQLITE_OK);return count;
}
/** Pause the first instance inside SQL so the second instance can exercise overlapping work. */
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
/** Restore two independent databases and reset the overlap gate before each scenario. */
static void reset(void)
{
    for(unsigned n=0;n<2;++n)
    {
        execute(n,"DROP TRIGGER IF EXISTS fail_second;DELETE FROM bundle_data;"
            "INSERT INTO bundle_data(bundle_id,dest_node,dest_service,is_custodial,egress_attempted,retransmit_trigger,retransmit_timestamp,bundle_bytes) VALUES"
            "(1,10,1,1,0,7,8,1),(2,11,1,1,0,7,8,1),(3,99,99,1,0,7,8,1);");
        if(n==1)execute(n,"UPDATE bundle_data SET dest_node=20,dest_service=bundle_id WHERE bundle_id<=2;");
        instances[n].BundleStorage.BundleCountNotEgressed=3;
    }
}
typedef struct{unsigned node,kind;BPLib_Status_t status;size_t count;} Task;
/** Apply a custody batch under the selected instance’s storage lock and record its status. */
static void *run(void *arg)
{
    Task *task=arg;BPLib_Instance_t *inst=&instances[task->node];
    assert(pthread_mutex_lock(&inst->BundleStorage.lock)==0);
    if(task->kind<3)
    {
        BPLib_STOR_CtUpdateBatch_t batch={0};batch.Size=2;
        for(unsigned i=0;i<2;++i){batch.BundleIDs[i]=i+1;batch.Ops[i]=task->kind==0 ? BPLIB_CT_MARK_DELETE : task->kind==1 ? BPLIB_CT_START_RETRANSMIT : BPLIB_CT_STOP_RETRANSMIT;}
        task->status=BPLib_SQL_UpdateCustodialBundles(inst,&batch);
    }
    else
    {
        BPLib_EID_Pattern_t pattern={0};
        pattern.MinNode=task->node ? 20 : 10;pattern.MaxNode=task->node ? 20 : 11;
        pattern.MinService=1;pattern.MaxService=task->node ? 2 : 1;
        task->count=99;
        task->status=BPLib_SQL_SetNewRetransmitTrigger(inst,0,&pattern,1,100+task->node,&task->count);
    }
    assert(pthread_mutex_unlock(&inst->BundleStorage.lock)==0);return NULL;
}
/** Reject updates to bundle_data to inject statement preparation failure. */
static int deny_update(void *unused,int action,const char *table,const char *column,const char *database,const char *trigger)
{
    (void)unused;(void)column;(void)database;(void)trigger;
    return action==SQLITE_UPDATE && table && strcmp(table,"bundle_data")==0 ? SQLITE_DENY : SQLITE_OK;
}
/** Hold one custody update open while the second instance runs, with optional write or commit
 * failure. */
static void overlap(unsigned kind,unsigned failure)
{
    reset();
    if(failure==1)execute(1,"CREATE TRIGGER fail_second BEFORE UPDATE ON bundle_data WHEN NEW.bundle_id=2 BEGIN SELECT RAISE(FAIL,'injected second-row failure');END;");
    Task a={0,kind,0,0},b={1,kind,0,0};pthread_t thread;
    assert(pthread_mutex_lock(&gate)==0);paused=true;entered=resume=false;assert(pthread_mutex_unlock(&gate)==0);
    assert(pthread_create(&thread,NULL,run,&a)==0);
    assert(pthread_mutex_lock(&gate)==0);while(!entered)assert(pthread_cond_wait(&changed,&gate)==0);assert(pthread_mutex_unlock(&gate)==0);
    if(failure==2)assert(sqlite3_set_authorizer(instances[1].BundleStorage.db,deny_update,NULL)==SQLITE_OK);
    run(&b);
    if(failure==2)assert(sqlite3_set_authorizer(instances[1].BundleStorage.db,NULL,NULL)==SQLITE_OK);
    assert(pthread_mutex_lock(&gate)==0);resume=true;assert(pthread_cond_signal(&changed)==0);assert(pthread_mutex_unlock(&gate)==0);
    assert(pthread_join(thread,NULL)==0);
    assert(pthread_mutex_lock(&gate)==0);paused=false;assert(pthread_mutex_unlock(&gate)==0);
    assert(a.status==BPLIB_SUCCESS && ((b.status==BPLIB_SUCCESS)==!failure));
    for(unsigned n=0;n<2;++n)
    {
        sqlite3 *db=instances[n].BundleStorage.db;
        assert(scalar(n,"SELECT count(*) FROM bundle_data WHERE bundle_id=3 AND egress_attempted=0 AND retransmit_trigger=7 AND retransmit_timestamp=8")==1);
        assert(sqlite3_get_autocommit(db));assert(sqlite3_next_stmt(db,NULL)==NULL);
        if(n==1 && failure)
        {
            assert(scalar(n,"SELECT count(*) FROM bundle_data WHERE egress_attempted!=0 OR retransmit_trigger!=7 OR retransmit_timestamp!=8")==0);
            assert(instances[n].BundleStorage.BundleCountNotEgressed==3);
            if(kind==3)assert(b.count==0);
        }
        else if(kind==0)assert(instances[n].BundleStorage.BundleCountNotEgressed==1 && scalar(n,"SELECT count(*) FROM bundle_data WHERE egress_attempted=1")==2);
        else if(kind==1)assert(scalar(n,"SELECT count(*) FROM bundle_data WHERE retransmit_timestamp=1000")==2);
        else if(kind==2)assert(scalar(n,"SELECT count(*) FROM bundle_data WHERE bundle_id<=2 AND retransmit_trigger=0")==2);
        else {assert((n ? b.count : a.count)==2);assert(scalar(n,n ? "SELECT count(*) FROM bundle_data WHERE retransmit_trigger=101" : "SELECT count(*) FROM bundle_data WHERE retransmit_trigger=100")==2);}
    }
}
/** Exercise parallel custody transactions and verify failed work leaves no statements or lost
 * outer transaction. */
int main(int argc,char **argv)
{
    for(unsigned n=0;n<2;++n)
    {
        assert(BPLib_SQL_Init(&instances[n],":memory:")==BPLIB_SUCCESS);
        assert(pthread_mutex_init(&instances[n].BundleStorage.lock,NULL)==0);
        assert(sqlite3_create_function(instances[n].BundleStorage.db,"test_gate",0,SQLITE_UTF8,(void *)(uintptr_t)n,pause_update,NULL,NULL)==SQLITE_OK);
        execute(n,"CREATE TRIGGER pause_change AFTER UPDATE ON bundle_data WHEN NEW.bundle_id=1 BEGIN SELECT test_gate();END;");
    }
    if(argc>1 && strcmp(argv[1],"filter-failure")==0)
    {
        reset();execute(1,"CREATE TRIGGER fail_second BEFORE UPDATE ON bundle_data WHEN NEW.bundle_id=2 BEGIN SELECT RAISE(FAIL,'injected failure');END;");
        Task task={1,3,0,0};run(&task);
        int partial=scalar(1,"SELECT count(*) FROM bundle_data WHERE retransmit_trigger!=7");
        fprintf(stderr,"failed reschedule: status=%d changed=%d reported=%zu\n",task.status,partial,task.count);
        assert(task.status!=BPLIB_SUCCESS && partial==0 && task.count==0);
    }
    else if(argc>1)overlap((unsigned)atoi(argv[1]),false);
    else
    {
        for(unsigned kind=0;kind<4;++kind){overlap(kind,0);overlap(kind,1);overlap(kind,2);}
        reset();
        execute(1,"CREATE TABLE commit_parent(id INTEGER PRIMARY KEY);"
            "CREATE TABLE commit_bad(parent_id INTEGER REFERENCES commit_parent(id) DEFERRABLE INITIALLY DEFERRED);"
            "CREATE TRIGGER fail_commit AFTER UPDATE ON bundle_data BEGIN INSERT INTO commit_bad VALUES(123);END;");
        Task committed={1,3,0,99};run(&committed);
        assert(committed.status!=BPLIB_SUCCESS && committed.count==0);
        assert(sqlite3_get_autocommit(instances[1].BundleStorage.db));
        assert(scalar(1,"SELECT count(*) FROM bundle_data WHERE retransmit_trigger!=7 OR retransmit_timestamp!=8")==0);
        assert(scalar(1,"SELECT count(*) FROM commit_bad")==0);
        execute(1,"DROP TRIGGER fail_commit;DROP TABLE commit_bad;DROP TABLE commit_parent;");
        run(&committed);assert(committed.status==BPLIB_SUCCESS && committed.count==2);
        reset();
        execute(1,"BEGIN;INSERT INTO bundle_data(bundle_id) VALUES(4);");
        Task nested={1,3,0,99};run(&nested);
        assert(nested.status!=BPLIB_SUCCESS && nested.count==0);
        assert(!sqlite3_get_autocommit(instances[1].BundleStorage.db));
        assert(scalar(1,"SELECT count(*) FROM bundle_data")==4);
        execute(1,"ROLLBACK;");
        assert(scalar(1,"SELECT count(*) FROM bundle_data")==3);
    }
    for(unsigned n=0;n<2;++n){assert(sqlite3_close(instances[n].BundleStorage.db)==SQLITE_OK);assert(pthread_mutex_destroy(&instances[n].BundleStorage.lock)==0);}
    assert(pthread_cond_destroy(&changed)==0 && pthread_mutex_destroy(&gate)==0);
    puts("PASS: isolated custody SQL statements and query buffers, atomic rescheduling, rollback counters and cleanup");
    return 0;
}
