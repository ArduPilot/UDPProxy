#include "keydb.h"

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
