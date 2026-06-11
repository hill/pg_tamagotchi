#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/wait_event.h"

#include "pg_tamagotchi.h"

PGDLLEXPORT void tama_worker_main(Datum main_arg);

static void
tama_tick(void)
{
	elog(LOG, "pg_tamagotchi: tick");
}

void
tama_worker_main(Datum main_arg)
{
	uint32		wait_event_info;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(tama_database, NULL, 0);

	wait_event_info = WaitEventExtensionNew("PgTamagotchiMain");

	elog(LOG, "pg_tamagotchi: worker started, the pet lives in database \"%s\"",
		 tama_database);

	for (;;)
	{
		tama_tick();

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 tama_tick_interval * 1000L,
						 wait_event_info);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
	}
}
