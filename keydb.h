/*
  key database structure
 */
#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <tdb.h>

#define KEY_FILE "keys.tdb"

#define KEY_MAGIC 0x6b73e867a72cdd1fULL

struct KeyEntry {
    uint64_t magic;
    uint64_t timestamp;
    uint8_t secret_key[32];
    int port1;
    uint32_t connections;
    uint32_t count1;
    uint32_t count2;
    char name[32];
};

/*
  open DB with or without a transaction
 */
TDB_CONTEXT *db_open(void);
void db_close(TDB_CONTEXT *db);
TDB_CONTEXT *db_open_transaction(void);
void db_close_cancel(TDB_CONTEXT *db);
void db_close_commit(TDB_CONTEXT *db);
bool db_load_key(TDB_CONTEXT *tdb, int port2, struct KeyEntry &key);
bool db_save_key(TDB_CONTEXT *tdb, int port2, const struct KeyEntry &key);

