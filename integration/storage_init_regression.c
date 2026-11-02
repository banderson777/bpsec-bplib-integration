/* Exercise real storage initialization while a read-only monitor holds a lock. */
#include "bplib.h"
#include "bplib_stor_sql.h"
#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static BPLib_Instance_t instance;
static int interrupt_query(void *unused) { (void)unused; return 1; }
static void *release_reader(void *arg)
{
    struct timespec pause = {0, 150000000};
    nanosleep(&pause, NULL);
    assert(sqlite3_exec(arg, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    return NULL;
}
static sqlite3 *locked_database(char *path)
{
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE monitor (id); INSERT INTO monitor VALUES (1);", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "BEGIN; SELECT * FROM monitor;", NULL, NULL, NULL) == SQLITE_OK);
    return db;
}
static void sentinels(void)
{
    instance.BundleStorage.BundleCountStored=91;
    instance.BundleStorage.BundleCountNotEgressed=92;
    instance.BundleStorage.BytesStorageInUse=93;
}
static void preserved(void)
{
    assert(instance.BundleStorage.BundleCountStored==91);
    assert(instance.BundleStorage.BundleCountNotEgressed==92);
    assert(instance.BundleStorage.BytesStorageInUse==93);
}
static void metadata(const char *rows,bool valid,uint32_t stored,size_t pending,size_t bytes)
{
    char path[]="/tmp/bplib-accounting-XXXXXX";int fd=mkstemp(path);assert(fd>=0 && close(fd)==0);
    assert(BPLib_SQL_Init(&instance,path)==BPLIB_SUCCESS);
    assert(sqlite3_exec(instance.BundleStorage.db,rows,NULL,NULL,NULL)==SQLITE_OK);
    assert(sqlite3_close(instance.BundleStorage.db)==SQLITE_OK);instance.BundleStorage.db=NULL;
    sentinels();BPLib_Status_t status=BPLib_SQL_Init(&instance,path);
    fprintf(stderr,"recovery status=%d stored=%u pending=%zu bytes=%zu rows=%s\n",status,instance.BundleStorage.BundleCountStored,instance.BundleStorage.BundleCountNotEgressed,instance.BundleStorage.BytesStorageInUse,rows);
    if(valid)
    {
        assert(status==BPLIB_SUCCESS);
        assert(instance.BundleStorage.BundleCountStored==stored && instance.BundleStorage.BundleCountNotEgressed==pending && instance.BundleStorage.BytesStorageInUse==bytes);
        assert(sqlite3_next_stmt(instance.BundleStorage.db,NULL)==NULL);
        assert(sqlite3_close(instance.BundleStorage.db)==SQLITE_OK);instance.BundleStorage.db=NULL;
    }
    else {assert(status==BPLIB_STOR_SQL_INIT_ERR && instance.BundleStorage.db==NULL);preserved();}
    assert(unlink(path)==0);
}
static int deny_bytes(void *unused,int action,const char *table,const char *column,const char *db,const char *trigger)
{
    (void)unused;(void)db;(void)trigger;
    return action==SQLITE_READ && table && column && strcmp(table,"bundle_data")==0 && strcmp(column,"bundle_bytes")==0 ? SQLITE_DENY : SQLITE_OK;
}
static int fail_after_row(unsigned event,void *arg,void *stmt,void *unused)
{
    (void)event;(void)stmt;(void)unused;sqlite3_progress_handler(arg,1,interrupt_query,NULL);return 0;
}
static void recovery_fault(bool step)
{
    assert(BPLib_SQL_Init(&instance,":memory:")==BPLIB_SUCCESS);sqlite3 *db=instance.BundleStorage.db;
    assert(sqlite3_exec(db,"INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(10,0),(20,1);",NULL,NULL,NULL)==SQLITE_OK);
    sentinels();
    if(step)assert(sqlite3_trace_v2(db,SQLITE_TRACE_ROW,fail_after_row,db)==SQLITE_OK);
    else assert(sqlite3_set_authorizer(db,deny_bytes,NULL)==SQLITE_OK);
    SQL_Status_t status=BPLib_SQL_InitTable(&instance);
    sqlite3_progress_handler(db,0,NULL,NULL);assert(sqlite3_trace_v2(db,0,NULL,NULL)==SQLITE_OK);assert(sqlite3_set_authorizer(db,NULL,NULL)==SQLITE_OK);
    fprintf(stderr,"recovery fault step=%d status=%u stored=%u pending=%zu bytes=%zu\n",step,status,instance.BundleStorage.BundleCountStored,instance.BundleStorage.BundleCountNotEgressed,instance.BundleStorage.BytesStorageInUse);
    assert(status!=SQLITE_OK);preserved();
    assert(sqlite3_get_autocommit(db) && sqlite3_next_stmt(db,NULL)==NULL);
    assert(BPLib_SQL_InitTable(&instance)==SQLITE_OK);
    assert(instance.BundleStorage.BundleCountStored==2 && instance.BundleStorage.BundleCountNotEgressed==1 && instance.BundleStorage.BytesStorageInUse==30);
    assert(sqlite3_close(db)==SQLITE_OK);instance.BundleStorage.db=NULL;
}
static void accounting_checks(const char *mode)
{
    if(!mode || !strcmp(mode,"negative"))metadata("INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(-1,0);",false,0,0,0);
    if(!mode || !strcmp(mode,"flag"))metadata("INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(1,2);",false,0,0,0);
    if(!mode || !strcmp(mode,"prepare"))recovery_fault(false);
    if(!mode || !strcmp(mode,"step"))recovery_fault(true);
    if(mode)return;
    const char *invalid[]={
        "INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(NULL,0);",
        "INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(0.5,0);",
        "INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES('bad',0);",
        "INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(1,-1);",
        "INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(1,NULL);",
        "INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(1,0.5);",
        "INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(1,'bad');",
        "INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(9223372036854775807,0),(1,0);"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i)metadata(invalid[i],false,0,0,0);
    metadata("SELECT 1;",true,0,0,0);
    metadata("INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(0,0),(10,0),(20,1);",true,3,2,30);
    if(SIZE_MAX>=UINT64_C(3000000000))metadata("INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(3000000000,1);",true,1,0,(size_t)UINT64_C(3000000000));
}
static SQL_Status_t scalar_call(unsigned kind,sqlite3 *db,uint64_t *value)
{
    if(kind==0){uint32_t v=(uint32_t)*value;SQL_Status_t status=BPLib_SQL_GetNumStoredBundles(db,&v);*value=v;return status;}
    if(kind==1){size_t v=(size_t)*value;SQL_Status_t status=BPLib_SQL_GetNumEgressed(db,&v);*value=v;return status;}
    return BPLib_SQL_GetTotalBundleBytes(db,value);
}
static void scalar_rows(sqlite3 *db,const char *rows)
{
    assert(sqlite3_exec(db,"DELETE FROM bundle_data;",NULL,NULL,NULL)==SQLITE_OK);
    assert(sqlite3_exec(db,rows,NULL,NULL,NULL)==SQLITE_OK);
}
static void scalar_invalid(sqlite3 *db,unsigned kind,const char *rows)
{
    scalar_rows(db,rows);uint64_t value=77;SQL_Status_t status=scalar_call(kind,db,&value);
    fprintf(stderr,"scalar kind=%u status=%u value=%llu rows=%s\n",kind,status,(unsigned long long)value,rows);
    assert(status!=SQLITE_OK && value==77 && sqlite3_next_stmt(db,NULL)==NULL);
}
static int deny_select(void *unused,int action,const char *table,const char *column,const char *db,const char *trigger)
{(void)unused;(void)table;(void)column;(void)db;(void)trigger;return action==SQLITE_SELECT ? SQLITE_DENY : SQLITE_OK;}
static void forced_count_step(sqlite3_context *ctx,int argc,sqlite3_value **argv){(void)ctx;(void)argc;(void)argv;}
static void forced_count_final(sqlite3_context *ctx){sqlite3_result_int64(ctx,*(sqlite3_int64 *)sqlite3_user_data(ctx));}
static void scalar_checks(const char *mode)
{
    assert(BPLib_SQL_Init(&instance,":memory:")==BPLIB_SUCCESS);sqlite3 *db=instance.BundleStorage.db;
    if(!mode || !strcmp(mode,"scalar-bytes"))scalar_invalid(db,2,"INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(-1,0);");
    if(!mode || !strcmp(mode,"scalar-flag"))scalar_invalid(db,1,"INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(1,2);");
    if(!mode || !strcmp(mode,"scalar-completion"))
    {
        scalar_rows(db,"INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(10,1);");
        for(unsigned kind=0;kind<3;++kind)
        {
            assert(sqlite3_trace_v2(db,SQLITE_TRACE_ROW,fail_after_row,db)==SQLITE_OK);
            uint64_t value=77;SQL_Status_t status=scalar_call(kind,db,&value);
            sqlite3_progress_handler(db,0,NULL,NULL);assert(sqlite3_trace_v2(db,0,NULL,NULL)==SQLITE_OK);
            fprintf(stderr,"scalar completion kind=%u status=%u value=%llu\n",kind,status,(unsigned long long)value);
            assert(status==SQLITE_INTERRUPT && value==77 && sqlite3_next_stmt(db,NULL)==NULL);
            assert(scalar_call(kind,db,&value)==SQLITE_OK && value==(kind==2 ? 10u : 1u));
        }
    }
    if(!mode)
    {
        const char *bad_bytes[]={"NULL","0.5","'bad'"};char sql[256];
        for(size_t i=0;i<3;++i){snprintf(sql,sizeof(sql),"INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(%s,0);",bad_bytes[i]);scalar_invalid(db,2,sql);}
        const char *bad_flags[]={"-1","NULL","0.5","'bad'"};
        for(size_t i=0;i<4;++i){snprintf(sql,sizeof(sql),"INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(1,%s);",bad_flags[i]);scalar_invalid(db,1,sql);}
        scalar_invalid(db,2,"INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(9223372036854775807,0),(1,0);");
        scalar_rows(db,"SELECT 1;");
        for(unsigned kind=0;kind<3;++kind){uint64_t value=77;assert(scalar_call(kind,db,&value)==SQLITE_OK && value==0);}
        scalar_rows(db,"INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(3000000000,0),(0,1);");
        for(unsigned kind=0;kind<3;++kind)
        {
            uint64_t value=77;assert(scalar_call(kind,db,&value)==SQLITE_OK && value==(kind==0 ? 2u : kind==1 ? 1u : UINT64_C(3000000000)));
            assert(sqlite3_set_authorizer(db,deny_select,NULL)==SQLITE_OK);value=77;
            assert(scalar_call(kind,db,&value)==SQLITE_AUTH && value==77 && sqlite3_next_stmt(db,NULL)==NULL);
            assert(sqlite3_set_authorizer(db,NULL,NULL)==SQLITE_OK);
            assert(scalar_call(kind,db,&value)==SQLITE_OK);
            value=77;assert(scalar_call(kind,NULL,&value)==SQLITE_MISUSE && value==77);
        }
        assert(BPLib_SQL_GetNumStoredBundles(db,NULL)==SQLITE_MISUSE);
        assert(BPLib_SQL_GetNumEgressed(db,NULL)==SQLITE_MISUSE);
        assert(BPLib_SQL_GetTotalBundleBytes(db,NULL)==SQLITE_MISUSE);
        scalar_rows(db,"INSERT INTO bundle_data(bundle_bytes,egress_attempted) VALUES(9223372036854775807,1);");
        uint64_t max_bytes=0;assert(BPLib_SQL_GetTotalBundleBytes(db,&max_bytes)==SQLITE_OK && max_bytes==INT64_MAX);
    }
    if(!mode || !strcmp(mode,"scalar-wide"))
    {
        sqlite3_int64 forced=UINT32_MAX;uint64_t value=77;
        assert(sqlite3_create_function(db,"count",0,SQLITE_UTF8,&forced,NULL,forced_count_step,forced_count_final)==SQLITE_OK);
        assert(scalar_call(0,db,&value)==SQLITE_OK && value==UINT32_MAX);
        forced=(sqlite3_int64)UINT32_MAX+1;value=77;SQL_Status_t status=scalar_call(0,db,&value);
        fprintf(stderr,"scalar oversized count status=%u value=%llu\n",status,(unsigned long long)value);
        assert(status==SQLITE_TOOBIG && value==77);
        forced=-1;assert(scalar_call(0,db,&value)==SQLITE_CORRUPT && value==77);
    }
    assert(sqlite3_next_stmt(db,NULL)==NULL && sqlite3_get_autocommit(db));
    assert(sqlite3_close(db)==SQLITE_OK);instance.BundleStorage.db=NULL;
}
int main(int argc,char **argv)
{
    if(argc>1){if(strncmp(argv[1],"scalar-",7)==0)scalar_checks(argv[1]);else accounting_checks(argv[1]);return 0;}
    scalar_checks(NULL);
    accounting_checks(NULL);
    char path[] = "/tmp/bplib-storage-init-XXXXXX";
    sqlite3 *reader = locked_database(path);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, release_reader, reader) == 0);
    BPLib_Status_t status = BPLib_SQL_Init(&instance, path);
    assert(pthread_join(thread, NULL) == 0);
    assert(sqlite3_close(reader) == SQLITE_OK);
    assert(status == BPLIB_SUCCESS);
    sqlite3_stmt *query = NULL;
    assert(sqlite3_prepare_v2(instance.BundleStorage.db, "SELECT count(*) FROM bundle_data", -1, &query, NULL) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 0);
    sqlite3_finalize(query);
    uint32_t stored; size_t egressed; uint64_t bytes;
    sqlite3_progress_handler(instance.BundleStorage.db, 1, interrupt_query, NULL);
    assert(BPLib_SQL_GetNumStoredBundles(instance.BundleStorage.db, &stored) == SQLITE_INTERRUPT);
    assert(sqlite3_next_stmt(instance.BundleStorage.db, NULL) == NULL);
    assert(BPLib_SQL_GetNumEgressed(instance.BundleStorage.db, &egressed) == SQLITE_INTERRUPT);
    assert(sqlite3_next_stmt(instance.BundleStorage.db, NULL) == NULL);
    assert(BPLib_SQL_GetTotalBundleBytes(instance.BundleStorage.db, &bytes) == SQLITE_INTERRUPT);
    assert(sqlite3_next_stmt(instance.BundleStorage.db, NULL) == NULL);
    sqlite3_progress_handler(instance.BundleStorage.db, 0, NULL, NULL);
    assert(sqlite3_close(instance.BundleStorage.db) == SQLITE_OK);
    instance.BundleStorage.db = NULL;
    assert(unlink(path) == 0);
    puts("PASS storage initialization waits for a transient read-only monitor lock");
    char blocked_path[] = "/tmp/bplib-storage-blocked-XXXXXX";
    reader = locked_database(blocked_path);
    struct timespec start, end;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    assert(BPLib_SQL_Init(&instance, blocked_path) == BPLIB_STOR_SQL_INIT_ERR);
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    double elapsed = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1e9;
    assert(elapsed < 5.0 && instance.BundleStorage.db == NULL);
    assert(sqlite3_exec(reader, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(reader) == SQLITE_OK);
    assert(unlink(blocked_path) == 0);
    puts("PASS persistent SQLite contention fails within a finite budget and closes the failed handle");
    return 0;
}
