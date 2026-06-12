#include "postgres.h"

#include "access/xact.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#include "pg_tamagotchi.h"

PGDLLEXPORT void tama_worker_main(Datum main_arg);

/*
 * One beat of the pet's life. Reads the database's own statistics and
 * writes them into the pet, all inside one transaction.
 */
static void tama_tick(void) {
  int ret;

  /*
   * Workers have no client sending statements, so nothing advances the
   * statement timestamp for us. Without this, now() in every tick
   * returns the worker's start time, forever.
   */
  SetCurrentStatementStartTimestamp();
  StartTransactionCommand();

  /* Until CREATE EXTENSION runs in this database there is nothing to do */
  if (!OidIsValid(get_extension_oid("pg_tamagotchi", true))) {
    CommitTransactionCommand();
    return;
  }

  SPI_connect();
  PushActiveSnapshot(GetTransactionSnapshot());
  pgstat_report_activity(STATE_RUNNING, "pg_tamagotchi tick");

  ret = SPI_execute(psprintf(
    "UPDATE %s SET"
    "  hunger = least(hunger + 1, 100),"
    "  poop = (SELECT coalesce(sum(n_dead_tup), 0)"
    "          FROM pg_stat_user_tables),"
    "  cache_hit_ratio = (SELECT blks_hit::float8"
    "                            / nullif(blks_hit + blks_read, 0)"
    "                     FROM pg_stat_database"
    "                     WHERE datname = current_database()),"
    "  stress = (SELECT count(*) FROM pg_stat_activity"
    "            WHERE state = 'idle in transaction'),"
    "  last_tick_at = now()",
    tama_pet_relname()), false, 0);
  if (ret != SPI_OK_UPDATE) {
    elog(ERROR, "pg_tamagotchi: tick failed");
  }

  SPI_finish();
  PopActiveSnapshot();
  CommitTransactionCommand();
  pgstat_report_activity(STATE_IDLE, NULL);
}

/* Worker entry point. Connects to one database, then ticks forever. */
void tama_worker_main(Datum main_arg) {
  uint32 wait_event_info;
  MemoryContext worker_ctx;

  pqsignal(SIGHUP, SignalHandlerForConfigReload);
  pqsignal(SIGTERM, die);
  BackgroundWorkerUnblockSignals();

  BackgroundWorkerInitializeConnection(tama_database, NULL, 0);

  wait_event_info = WaitEventExtensionNew("PgTamagotchiMain");

  elog(LOG, "pg_tamagotchi: worker started, the pet lives in database \"%s\"",
       tama_database);

  worker_ctx = CurrentMemoryContext;

  while (true) {
    /*
     * A failed tick should cost one log entry, not the pet's life.
     * FATAL (like a shutdown request) is not caught and still
     * terminates the worker.
     */
    PG_TRY();
    {
      tama_tick();
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(worker_ctx);
      EmitErrorReport();
      FlushErrorState();
      AbortCurrentTransaction();
      pgstat_report_activity(STATE_IDLE, NULL);
    }
    PG_END_TRY();

    (void) WaitLatch(MyLatch,
                     WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                     tama_tick_interval * 1000L,
                     wait_event_info);
    ResetLatch(MyLatch);
    CHECK_FOR_INTERRUPTS();

    if (ConfigReloadPending) {
      ConfigReloadPending = false;
      ProcessConfigFile(PGC_SIGHUP);
    }
  }
}
