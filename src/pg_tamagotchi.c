#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC_EXT(.name = "pg_tamagotchi", .version = "0.1.0");

PG_FUNCTION_INFO_V1(tama_status);

Datum
tama_status(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text("an egg sits here, waiting. (.)"));
}
