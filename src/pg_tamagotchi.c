#include "postgres.h"

#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC_EXT(.name = "pg_tamagotchi", .version = "0.1.0");

PG_FUNCTION_INFO_V1(tama_hatch);
PG_FUNCTION_INFO_V1(tama_status);

Datum
tama_hatch(PG_FUNCTION_ARGS)
{
	text	   *name;
	char	   *name_cstr;
	StringInfoData buf;
	Oid			argtypes[1] = {TEXTOID};
	Datum		values[1];
	int			ret;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("your egg needs a name to hatch"),
				 errhint("Try SELECT tama.hatch('Blobby');")));

	name = PG_GETARG_TEXT_PP(0);
	name_cstr = text_to_cstring(name);

	if (name_cstr[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a pet cannot be named the empty string"),
				 errhint("Try SELECT tama.hatch('Blobby');")));

	/* The result buffer must outlive SPI's memory, so allocate it first. */
	initStringInfo(&buf);

	SPI_connect();

	ret = SPI_execute("SELECT name FROM tama.pet", true, 1);
	if (ret != SPI_OK_SELECT)
		elog(ERROR, "could not check for an existing pet");
	if (SPI_processed > 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("you already have a pet named %s",
						SPI_getvalue(SPI_tuptable->vals[0],
									 SPI_tuptable->tupdesc, 1)),
				 errhint("One pet per database. Care for the one you have.")));

	values[0] = PointerGetDatum(name);
	ret = SPI_execute_with_args("INSERT INTO tama.pet (name) VALUES ($1)",
								1, argtypes, values, NULL, false, 0);
	if (ret != SPI_OK_INSERT)
		elog(ERROR, "the egg refused to hatch");

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

	ret = SPI_execute("SELECT name, hunger, happiness, poop, stress"
					  " FROM tama.pet", true, 1);
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
