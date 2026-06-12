#include "postgres.h"

#include <string.h>

#include "commands/extension.h"
#include "common/pg_prng.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC_EXT(.name = "pg_tamagotchi", .version = "0.1.0");

PG_FUNCTION_INFO_V1(tama_hatch);
PG_FUNCTION_INFO_V1(tama_feed);
PG_FUNCTION_INFO_V1(tama_talk);
PG_FUNCTION_INFO_V1(tama_status);

#define TAMA_TICK_SECONDS 10

/*
 * A table qualified with whatever schema the extension actually lives in.
 * Hardcoding "tama.pet" would break if the schema is renamed, and worse,
 * would follow an impostor schema recreated under that name.
 */
static char *tama_relname(const char *relname) {
  Oid extoid = get_extension_oid("pg_tamagotchi", false);
  char *nspname = get_namespace_name(get_extension_schema(extoid));

  return psprintf("%s.%s", quote_identifier(nspname), quote_identifier(relname));
}

static char *tama_pet_relname(void) {
  return tama_relname("pet");
}

static char *tama_message_relname(void) {
  return tama_relname("message");
}

/*
 * One beat of the pet's life. Time is caught up lazily when someone checks
 * on the pet, so CREATE EXTENSION is enough to make it age.
 *
 * Stats become feelings here. Dead tuples are poop, idle-in-transaction
 * sessions in this database are stress, and the change in buffer-cache hits
 * since the last tick is mood. Using the delta rather than the lifetime
 * ratio keeps the cache reading sensitive to recent activity, and the
 * greatest(0, ...) guards keep it sane across a pg_stat_reset.
 */
static void tama_tick(void) {
  char *relname = tama_pet_relname();
  int ret;

  ret = SPI_execute(psprintf(
    "WITH stat AS ("
    "  SELECT blks_hit AS cur_hit, blks_read AS cur_read"
    "  FROM pg_stat_database WHERE datname = current_database()"
    "),"
    "tick AS ("
    "  SELECT t.only_one, t.cache_hit_ratio AS old_ratio,"
    "         greatest(0, stat.cur_hit - t.prev_blks_hit) AS d_hit,"
    "         greatest(0, stat.cur_read - t.prev_blks_read) AS d_read,"
    "         stat.cur_hit, stat.cur_read,"
    "         greatest(0, floor(extract(epoch FROM"
    "           (clock_timestamp() - t.last_tick_at)) / %d))::int AS elapsed"
    "  FROM %s t, stat"
    "  FOR UPDATE OF t"
    ") "
    "UPDATE %s p SET"
    "  hunger = least(p.hunger + tick.elapsed, 100),"
    "  poop = (SELECT coalesce(sum(n_dead_tup), 0)"
    "          FROM pg_stat_user_tables),"
    "  stress = (SELECT count(*) FROM pg_stat_activity"
    "            WHERE state = 'idle in transaction'"
    "              AND datname = current_database()),"
    "  cache_hit_ratio = CASE WHEN tick.d_hit + tick.d_read > 0"
    "    THEN tick.d_hit::float8 / (tick.d_hit + tick.d_read)"
    "    ELSE tick.old_ratio END,"
    "  prev_blks_hit = tick.cur_hit,"
    "  prev_blks_read = tick.cur_read,"
    "  last_tick_at = CASE WHEN tick.elapsed > 0"
    "    THEN p.last_tick_at + tick.elapsed * (%d * interval '1 second')"
    "    ELSE p.last_tick_at"
    "  END"
    " FROM tick"
    " WHERE p.only_one = tick.only_one",
    TAMA_TICK_SECONDS, relname, relname, TAMA_TICK_SECONDS),
    false, 0);

  if (ret != SPI_OK_UPDATE) {
    elog(ERROR, "pg_tamagotchi: tick failed");
  }
}

static void tama_log_message(const char *speaker, const char *body) {
  Oid argtypes[2] = {TEXTOID, TEXTOID};
  Datum values[2];
  int ret;

  values[0] = PointerGetDatum(cstring_to_text(speaker));
  values[1] = PointerGetDatum(cstring_to_text(body));

  ret = SPI_execute_with_args(
    psprintf("INSERT INTO %s (speaker, body) VALUES ($1, $2)",
             tama_message_relname()),
    2, argtypes, values, NULL, false, 0);
  if (ret != SPI_OK_INSERT) {
    elog(ERROR, "could not remember the conversation");
  }
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

/* Uniform random index in [0, n). */
static int pick(int n) {
  return (int) pg_prng_uint64_range(&pg_global_prng_state, 0, n - 1);
}

/* Mint a pronounceable name, two or three syllables, capitalized. */
static char *tama_random_name(void) {
  StringInfoData buf;
  int syllables = 2 + pick(2);

  initStringInfo(&buf);
  for (int i = 0; i < syllables; i++) {
    appendStringInfoString(&buf, name_onsets[pick(NELEMS(name_onsets))]);
    appendStringInfoString(&buf, name_vowels[pick(NELEMS(name_vowels))]);
  }
  if (pg_prng_double(&pg_global_prng_state) < 0.5) {
    appendStringInfoString(&buf, name_codas[pick(NELEMS(name_codas))]);
  }

  buf.data[0] = pg_toupper((unsigned char) buf.data[0]);

  return buf.data;
}

/* SELECT tama.hatch(name). Creates the pet, refuses if one already exists. */
Datum tama_hatch(PG_FUNCTION_ARGS) {
  char *name_cstr;
  StringInfoData buf;
  Oid argtypes[1] = {TEXTOID};
  Datum values[1];
  int ret;

  /* A nameless egg gets a pronounceable name minted for it */
  if (PG_ARGISNULL(0)) {
    name_cstr = tama_random_name();
  } else {
    name_cstr = text_to_cstring(PG_GETARG_TEXT_PP(0));
  }

  if (name_cstr[0] == '\0') {
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("a pet cannot be named the empty string"),
             errhint("Try SELECT tama.hatch('Blobby'); or let "
                     "tama.hatch() pick a name.")));
  }

  /* The result buffer must outlive SPI's memory, so allocate it first. */
  initStringInfo(&buf);

  SPI_connect();

  /*
   * The insert itself is the authority on whether a pet exists. A
   * separate check-then-insert would race against concurrent hatches
   * and read a stale snapshot when called twice in one statement.
   *
   * Seed the cache counters from the live stats now, so the first tick
   * measures activity since hatch rather than the database's lifetime.
   */
  values[0] = PointerGetDatum(cstring_to_text(name_cstr));
  ret = SPI_execute_with_args(
    psprintf("INSERT INTO %s (name, prev_blks_hit, prev_blks_read)"
             " SELECT $1, blks_hit, blks_read"
             " FROM pg_stat_database WHERE datname = current_database()"
             " ON CONFLICT DO NOTHING",
             tama_pet_relname()),
    1, argtypes, values, NULL, false, 0);
  if (ret != SPI_OK_INSERT) {
    elog(ERROR, "the egg refused to hatch");
  }

  if (SPI_processed == 0) {
    ret = SPI_execute(psprintf("SELECT name FROM %s", tama_pet_relname()),
                      false, 1);
    if (ret != SPI_OK_SELECT) {
      elog(ERROR, "could not look in on the pet");
    }
    if (SPI_processed > 0) {
      ereport(ERROR,
              (errcode(ERRCODE_UNIQUE_VIOLATION),
               errmsg("you already have a pet named %s",
                      SPI_getvalue(SPI_tuptable->vals[0],
                                   SPI_tuptable->tupdesc, 1)),
               errhint("One pet per database. Care for the one you have.")));
    }
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

/* SELECT tama.feed(food). Lowers hunger and gives the pet a little joy. */
Datum tama_feed(PG_FUNCTION_ARGS) {
  char *food_cstr;
  StringInfoData buf;
  TupleDesc tupdesc;
  HeapTuple tuple;
  bool isnull;
  char *name;
  int32 hunger;
  int32 happiness;
  int ret;

  if (PG_ARGISNULL(0)) {
    food_cstr = "snack";
  } else {
    food_cstr = text_to_cstring(PG_GETARG_TEXT_PP(0));
  }

  if (food_cstr[0] == '\0') {
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("food cannot be the empty string"),
             errhint("Try SELECT tama.feed('apple'); or let "
                     "tama.feed() pick a snack.")));
  }

  initStringInfo(&buf);

  SPI_connect();

  tama_tick();

  ret = SPI_execute(psprintf(
    "UPDATE %s SET"
    "  hunger = greatest(hunger - 25, 0),"
    "  happiness = least(happiness + CASE WHEN hunger > 0 THEN 5 ELSE 1 END, 100)"
    " RETURNING name, hunger, happiness",
    tama_pet_relname()), false, 1);
  if (ret != SPI_OK_UPDATE_RETURNING) {
    elog(ERROR, "could not feed the pet");
  }

  if (SPI_processed == 0) {
    appendStringInfoString(&buf,
                           "There is no pet to feed yet. "
                           "SELECT tama.hatch('a name you like');\n");
    SPI_finish();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
  }

  tupdesc = SPI_tuptable->tupdesc;
  tuple = SPI_tuptable->vals[0];
  name = SPI_getvalue(tuple, tupdesc, 1);
  hunger = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 2, &isnull));
  happiness = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));

  appendStringInfo(&buf,
                   "%s munches the %s. hunger %d/100  happiness %d/100\n",
                   name, food_cstr, hunger, happiness);

  SPI_finish();

  PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}

/* SELECT tama.talk(message). Stores the exchange and lets the pet answer. */
Datum tama_talk(PG_FUNCTION_ARGS) {
  char *message_cstr = NULL;
  char *reply;
  StringInfoData buf;
  TupleDesc tupdesc;
  HeapTuple tuple;
  bool isnull;
  bool cache_isnull;
  char *name;
  int32 hunger;
  int32 happiness;
  int64 poop;
  int32 stress;
  Datum cache_hit_datum;
  float8 cache_hit_ratio = 1.0;
  int ret;

  if (!PG_ARGISNULL(0)) {
    message_cstr = text_to_cstring(PG_GETARG_TEXT_PP(0));
    if (message_cstr[0] == '\0') {
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("message cannot be the empty string"),
               errhint("Try SELECT tama.talk('hello'); or call "
                       "tama.talk() with no message.")));
    }
  }

  initStringInfo(&buf);

  SPI_connect();

  tama_tick();

  ret = SPI_execute(psprintf("SELECT name, hunger, happiness, poop, stress,"
                             " cache_hit_ratio FROM %s",
                             tama_pet_relname()),
                    false, 1);
  if (ret != SPI_OK_SELECT) {
    elog(ERROR, "could not talk to the pet");
  }

  if (SPI_processed == 0) {
    appendStringInfoString(&buf,
                           "The egg is quiet. "
                           "SELECT tama.hatch('a name you like');\n");
    SPI_finish();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
  }

  tupdesc = SPI_tuptable->tupdesc;
  tuple = SPI_tuptable->vals[0];
  name = SPI_getvalue(tuple, tupdesc, 1);
  hunger = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 2, &isnull));
  happiness = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
  poop = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 4, &isnull));
  stress = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 5, &isnull));
  cache_hit_datum = SPI_getbinval(tuple, tupdesc, 6, &cache_isnull);
  if (!cache_isnull) {
    cache_hit_ratio = DatumGetFloat8(cache_hit_datum);
  }

  if (hunger >= 80) {
    reply = psprintf("%s says: I am too hungry to think about query plans.",
                     name);
  } else if (stress > 0) {
    reply = psprintf("%s says: I can hear %d idle transaction%s. Hold me.",
                     name, stress, stress == 1 ? "" : "s");
  } else if (poop >= 50) {
    reply = psprintf("%s says: There are dead tuples everywhere. Vacuum day?",
                     name);
  } else if (!cache_isnull && cache_hit_ratio < 0.90) {
    reply = psprintf("%s says: The buffer cache feels drafty today.", name);
  } else if (happiness >= 90) {
    reply = psprintf("%s says: I feel indexed and adored.", name);
  } else if (message_cstr != NULL && strchr(message_cstr, '?') != NULL) {
    reply = psprintf("%s says: That is a very good question for a tiny database pet.",
                     name);
  } else if (message_cstr == NULL) {
    reply = psprintf("%s chirps from inside the database.", name);
  } else {
    reply = psprintf("%s says: I hear you. Also, I live in your database.",
                     name);
  }

  if (message_cstr != NULL) {
    tama_log_message("you", message_cstr);
  }
  tama_log_message("pet", reply);

  if (message_cstr != NULL) {
    appendStringInfo(&buf, "you: %s\n", message_cstr);
  }
  appendStringInfo(&buf, "%s\n", reply);

  SPI_finish();

  PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}

/*
 * A three-line creature whose eyes reflect the pet's mood, plus the matching
 * mood word. The thresholds and order match the tama.vitals view exactly.
 */
static const char *tama_face(int32 hunger, int32 happiness, int64 poop,
                             int32 stress, const char **mood_out) {
  if (hunger >= 80) {
    *mood_out = "hungry";
    return " (\\_/)\n (>_<)\n (umu)";
  }
  if (stress > 0) {
    *mood_out = "stressed";
    return " (\\_/)\n (O_O)\n (;;;)";
  }
  if (poop >= 50) {
    *mood_out = "grubby";
    return " (\\_/)\n (-_-)\n (~~~)";
  }
  if (happiness >= 90) {
    *mood_out = "delighted";
    return " (\\_/)\n (^o^)\n (vwv)";
  }
  if (happiness <= 30) {
    *mood_out = "sad";
    return " (\\_/)\n (T_T)\n (._.)";
  }
  *mood_out = "content";
  return " (\\_/)\n (o.o)\n (\" \")";
}

/* SELECT tama.status(). Renders the unhatched egg or the pet's vitals. */
Datum tama_status(PG_FUNCTION_ARGS) {
  StringInfoData buf;
  int ret;

  initStringInfo(&buf);

  SPI_connect();

  tama_tick();

  ret = SPI_execute(psprintf("SELECT name, hunger, happiness, poop, stress"
                             " FROM %s", tama_pet_relname()),
                    false, 1);
  if (ret != SPI_OK_SELECT) {
    elog(ERROR, "could not look in on the pet");
  }

  if (SPI_processed == 0) {
    appendStringInfoString(&buf,
                           "  _____\n"
                           " /     \\   a speckled egg sits here, waiting.\n"
                           " \\_____/   SELECT tama.hatch('a name you like');\n");
  } else {
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    HeapTuple tuple = SPI_tuptable->vals[0];
    bool isnull;
    char *name = SPI_getvalue(tuple, tupdesc, 1);
    int32 hunger = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 2, &isnull));
    int32 happiness = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
    int64 poop = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 4, &isnull));
    int32 stress = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 5, &isnull));
    const char *mood;
    const char *face = tama_face(hunger, happiness, poop, stress, &mood);

    appendStringInfo(&buf,
                     "%s\n"
                     "%s is feeling %s.\n"
                     "hunger %d/100  happiness %d/100"
                     "  stress %d  poop " INT64_FORMAT "\n",
                     face, name, mood, hunger, happiness, stress, poop);
  }

  SPI_finish();

  PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}
