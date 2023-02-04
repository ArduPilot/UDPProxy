#include "keydb.h"
#include <string.h>
#include <stdlib.h>

TDB_CONTEXT *db_open(void)
{
    return tdb_open(KEY_FILE, 1000, 0, O_RDWR | O_CREAT, 0600);
}

TDB_CONTEXT *db_open_transaction(void)
{
    auto *db = db_open();
    if (db == nullptr) {
        return db;
    }
    tdb_transaction_start(db);
    return db;
}

void db_close(TDB_CONTEXT *db)
{
    tdb_close(db);
}

void db_close_cancel(TDB_CONTEXT *db)
{
    tdb_transaction_cancel(db);
    db_close(db);
}

void db_close_commit(TDB_CONTEXT *db)
{
    tdb_transaction_prepare_commit(db);
    tdb_transaction_commit(db);
    db_close(db);
}

bool db_load_key(TDB_CONTEXT *db, int port2, struct KeyEntry &key)
{
    TDB_DATA k;
    k.dptr = (uint8_t *)&port2;
    k.dsize = sizeof(int);

    auto d = tdb_fetch(db, k);
    if (d.dptr == nullptr || d.dsize != sizeof(key)) {
        return false;
    }
    memcpy(&key, d.dptr, sizeof(key));
    free(d.dptr);
    return key.magic == KEY_MAGIC;
}

bool db_save_key(TDB_CONTEXT *tdb, int port2, const struct KeyEntry &ke)
{
    TDB_DATA k;
    k.dptr = (uint8_t*)&port2;
    k.dsize = sizeof(int);
    TDB_DATA d;
    d.dptr = (uint8_t*)&ke;
    d.dsize = sizeof(ke);

    return tdb_store(tdb, k, d, TDB_REPLACE) == 0;
}
