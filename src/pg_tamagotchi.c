#include "postgres.h"

#include "commands/extension.h"
#include "common/pg_prng.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "pg_tamagotchi.h"

PG_MODULE_MAGIC_EXT(.name = "pg_tamagotchi", .version = "0.1.0");

PG_FUNCTION_INFO_V1(tama_hatch);
PG_FUNCTION_INFO_V1(tama_status);

int			tama_tick_interval = 10;
char	   *tama_database = NULL;

void
_PG_init(void)
{
	BackgroundWorker worker;

	/* GUCs are defined whenever the library loads, by any backend */
	DefineCustomIntVariable("pg_tamagotchi.tick_interval",
							"Seconds between pet ticks.",
							NULL,
							&tama_tick_interval,
							10, 1, 3600,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	DefineCustomStringVariable("pg_tamagotchi.database",
							   "Database the pet lives in.",
							   NULL,
							   &tama_database,
							   "postgres",
							   PGC_POSTMASTER,
							   0,
							   NULL, NULL, NULL);

	MarkGUCPrefixReserved("pg_tamagotchi");

	/* Workers can only be registered while the postmaster is preloading */
	if (!process_shared_preload_libraries_in_progress)
		return;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;

	/*
	 * Never auto-restart. If the configured database doesn't exist the
	 * worker dies FATAL at connect; a restart interval would turn that
	 * one log line into an infinite crash loop the operator can only
	 * stop with a full cluster restart.
	 */
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(worker.bgw_library_name, sizeof(worker.bgw_library_name),
			 "pg_tamagotchi");
	snprintf(worker.bgw_function_name, sizeof(worker.bgw_function_name),
			 "tama_worker_main");
	snprintf(worker.bgw_name, sizeof(worker.bgw_name), "pg_tamagotchi worker");
	snprintf(worker.bgw_type, sizeof(worker.bgw_type), "pg_tamagotchi");
	RegisterBackgroundWorker(&worker);
}

/*
 * The pet table, qualified with whatever schema the extension actually
 * lives in. Hardcoding "tama.pet" would break if the schema is renamed,
 * and worse, would follow an impostor schema recreated under that name.
 */
static char *
tama_pet_relname(void)
{
	Oid			extoid = get_extension_oid("pg_tamagotchi", false);
	char	   *nspname = get_namespace_name(get_extension_schema(extoid));

	return psprintf("%s.pet", quote_identifier(nspname));
}

/* Syllable parts for minting names. Every combination is pronounceable. */
static const char *const name_onsets[] = {
	"b", "c", "d", "f", "g", "h", "j", "k", "l", "m", "n",
	"p", "r", "s", "t", "v", "w", "z", "br", "cr", "dr",
	"fr", "gr", "pr", "tr", "st", "sl", "sp", "th", "ch"
};
static const char *const name_vowels[] = {
	"a", "e", "i", "o", "u", "ai", "ee", "oo", "ou", "ia"
};
static const char *const name_codas[] = {"n", "r", "s", "t", "l", "k", "m"};

#define NELEMS(a) ((int) (sizeof(a) / sizeof((a)[0])))

static int
pick(int n)
{
	return (int) pg_prng_uint64_range(&pg_global_prng_state, 0, n - 1);
}

static char *
tama_random_name(void)
{
	StringInfoData buf;
	int			syllables = 2 + pick(2);

	initStringInfo(&buf);
	for (int i = 0; i < syllables; i++)
	{
		appendStringInfoString(&buf, name_onsets[pick(NELEMS(name_onsets))]);
		appendStringInfoString(&buf, name_vowels[pick(NELEMS(name_vowels))]);
	}
	if (pg_prng_double(&pg_global_prng_state) < 0.5)
		appendStringInfoString(&buf, name_codas[pick(NELEMS(name_codas))]);

	buf.data[0] = pg_toupper((unsigned char) buf.data[0]);

	return buf.data;
}

Datum
tama_hatch(PG_FUNCTION_ARGS)
{
	char	   *name_cstr;
	StringInfoData buf;
	Oid			argtypes[1] = {TEXTOID};
	Datum		values[1];
	int			ret;

	/* A nameless egg gets a pronounceable name minted for it */
	if (PG_ARGISNULL(0))
		name_cstr = tama_random_name();
	else
		name_cstr = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (name_cstr[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a pet cannot be named the empty string"),
				 errhint("Try SELECT tama.hatch('Blobby'); or let "
						 "tama.hatch() pick a name.")));

	/* The result buffer must outlive SPI's memory, so allocate it first. */
	initStringInfo(&buf);

	SPI_connect();

	/*
	 * The insert itself is the authority on whether a pet exists. A
	 * separate check-then-insert would race against concurrent hatches
	 * and read a stale snapshot when called twice in one statement.
	 */
	values[0] = PointerGetDatum(cstring_to_text(name_cstr));
	ret = SPI_execute_with_args(
		psprintf("INSERT INTO %s (name) VALUES ($1) ON CONFLICT DO NOTHING",
				 tama_pet_relname()),
		1, argtypes, values, NULL, false, 0);
	if (ret != SPI_OK_INSERT)
		elog(ERROR, "the egg refused to hatch");

	if (SPI_processed == 0)
	{
		ret = SPI_execute(psprintf("SELECT name FROM %s", tama_pet_relname()),
						  false, 1);
		if (ret != SPI_OK_SELECT)
			elog(ERROR, "could not look in on the pet");
		if (SPI_processed > 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNIQUE_VIOLATION),
					 errmsg("you already have a pet named %s",
							SPI_getvalue(SPI_tuptable->vals[0],
										 SPI_tuptable->tupdesc, 1)),
					 errhint("One pet per database. Care for the one you have.")));
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("you already have a pet"),
				 errhint("One pet per database. Care for the one you have.")));
	}

	appendStringInfo(&buf,
					 "*crack*\n"
					 " (\\_/)\n"
					 " (o.o)  %s hatched! Take good care of them.\n",
					 name_cstr);

	SPI_finish();

	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}

Datum
tama_status(PG_FUNCTION_ARGS)
{
	StringInfoData buf;
	int			ret;

	initStringInfo(&buf);

	SPI_connect();

	ret = SPI_execute(psprintf("SELECT name, hunger, happiness, poop, stress"
							   " FROM %s", tama_pet_relname()),
					  false, 1);
	if (ret != SPI_OK_SELECT)
		elog(ERROR, "could not look in on the pet");

	if (SPI_processed == 0)
	{
		appendStringInfoString(&buf,
							   "  _____\n"
							   " /     \\   a speckled egg sits here, waiting.\n"
							   " \\_____/   SELECT tama.hatch('a name you like');\n");
	}
	else
	{
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		HeapTuple	tuple = SPI_tuptable->vals[0];
		bool		isnull;
		char	   *name = SPI_getvalue(tuple, tupdesc, 1);
		int32		hunger = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 2, &isnull));
		int32		happiness = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
		int64		poop = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 4, &isnull));
		int32		stress = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 5, &isnull));

		appendStringInfo(&buf,
						 " (\\_/)\n"
						 " (o.o)  %s\n"
						 " (\" \")  hunger %d/100  happiness %d/100"
						 "  stress %d  poop " INT64_FORMAT "\n",
						 name, hunger, happiness, stress, poop);
	}

	SPI_finish();

	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}
