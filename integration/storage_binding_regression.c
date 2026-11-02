/* Real SQLite binding lifetimes and failures in the actual chunk writer. */
#include "bplib_stor_sql_store.h"
#undef NDEBUG
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int rows(sqlite3 *db)
{
    sqlite3_stmt *query=NULL;
    assert(sqlite3_prepare_v2(db,"SELECT count(*) FROM bundle_blobs",-1,&query,NULL)==SQLITE_OK);
    assert(sqlite3_step(query)==SQLITE_ROW);int value=sqlite3_column_int(query,0);
    assert(sqlite3_finalize(query)==SQLITE_OK);return value;
}
static void released(sqlite3_stmt *statement)
{
    char *expanded=sqlite3_expanded_sql(statement);assert(expanded);
    assert(strstr(expanded,"VALUES (NULL, NULL)")!=NULL);
    sqlite3_free(expanded);
}
int main(void)
{
    sqlite3 *db=NULL;sqlite3_stmt *statement=NULL;
    assert(sqlite3_open(":memory:",&db)==SQLITE_OK);
    assert(sqlite3_exec(db,"CREATE TABLE bundle_blobs(bundle_row INTEGER UNIQUE,blob_data BLOB);",NULL,NULL,NULL)==SQLITE_OK);
    assert(sqlite3_prepare_v2(db,InsertBlobSQL,-1,&statement,NULL)==SQLITE_OK);
    unsigned char bytes[256];memset(bytes,'A',sizeof(bytes));
    assert(BPLib_SQL_StoreChunk(77,bytes,4,statement)==SQLITE_DONE);
    /* The caller may free/reuse its buffer immediately after return. */
    released(statement);memset(bytes,'B',sizeof(bytes));
    assert(rows(db)==1);
    int old_limit=sqlite3_limit(db,SQLITE_LIMIT_LENGTH,128);
    assert(BPLib_SQL_StoreChunk(78,bytes,sizeof(bytes),statement)==SQLITE_TOOBIG);
    assert(rows(db)==1);released(statement);
    sqlite3_limit(db,SQLITE_LIMIT_LENGTH,old_limit);
    assert(BPLib_SQL_StoreChunk(78,bytes,4,statement)==SQLITE_DONE);released(statement);
    assert(BPLib_SQL_StoreChunk(77,bytes,4,statement)==SQLITE_CONSTRAINT);
    assert(rows(db)==2);released(statement);
    assert(BPLib_SQL_StoreChunk(79,bytes,4,statement)==SQLITE_DONE);released(statement);
    assert(BPLib_SQL_StoreChunk(80,NULL,4,statement)==SQLITE_MISUSE);released(statement);
    assert(BPLib_SQL_StoreChunk(80,bytes,(size_t)INT_MAX+1,statement)==SQLITE_TOOBIG);released(statement);
    assert(BPLib_SQL_StoreChunk(80,bytes,0,statement)==SQLITE_DONE);released(statement);
    assert(BPLib_SQL_StoreChunk(81,bytes,4,NULL)==SQLITE_MISUSE);
    sqlite3_stmt *wrong=NULL;
    assert(sqlite3_prepare_v2(db,"INSERT INTO bundle_blobs(blob_data) VALUES(?)",-1,&wrong,NULL)==SQLITE_OK);
    assert(BPLib_SQL_StoreChunk(81,bytes,4,wrong)==SQLITE_RANGE);
    assert(rows(db)==4);assert(sqlite3_finalize(wrong)==SQLITE_OK);
    for(unsigned i=0;i<64;++i)
    {
        bytes[0]=(unsigned char)i;
        assert(BPLib_SQL_StoreChunk(100+i,bytes,4,statement)==SQLITE_DONE);
        released(statement);
    }
    assert(rows(db)==68);
    sqlite3_stmt *query=NULL;
    assert(sqlite3_prepare_v2(db,"SELECT hex(blob_data) FROM bundle_blobs WHERE bundle_row=77",-1,&query,NULL)==SQLITE_OK);
    assert(sqlite3_step(query)==SQLITE_ROW && strcmp((const char *)sqlite3_column_text(query,0),"41414141")==0);
    assert(sqlite3_finalize(query)==SQLITE_OK);
    assert(sqlite3_finalize(statement)==SQLITE_OK);
    assert(sqlite3_next_stmt(db,NULL)==NULL && sqlite3_close(db)==SQLITE_OK);
    puts("PASS: released blob bindings, bind/step error propagation, safe buffer reuse and statement retry");
    return 0;
}
