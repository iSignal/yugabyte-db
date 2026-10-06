/*--------------------------------------------------------------------------------------------------
 *
 * yb_preload_exclusion.c
 *	  Leaves the relations of the schemas configured by
 *	  ysql_catalog_preload_exclude_schemas out of the catalog caches and relcache
 *	  built by relcache preloading.
 *
 * YbDoPreloadRelCache brackets the cache fill with YbBeginPreloadExclusion and
 * YbEndPreloadExclusion. In between, YBLoadRelations and YbPreloadCatalogCache
 * skip what YbIsPreloadExcludedRelation and YbIsPreloadExcludedTuple report.
 *
 * Copyright (c) YugabyteDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 *
 * src/backend/utils/cache/yb_preload_exclusion.c
 *
 *--------------------------------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "pg_yb_utils.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/varlena.h"
#include "utils/yb_preload_exclusion.h"
#include "yb/yql/pggate/ybc_gflags.h"

/*
 * Names of the schemas excluded by ysql_catalog_preload_exclude_schemas for
 * this backend's (login role, database), parsed once per backend. They are
 * resolved to OIDs at each preload, so a schema dropped and recreated under
 * the same name stays excluded.
 */
static List *yb_preload_excluded_nsp_names = NIL;
static bool yb_preload_excluded_nsp_names_initialized = false;

/*
 * Relations (and their row types) left out of the catalog caches and relcache
 * by the relcache preload in progress, or NULL outside of such a preload.
 *
 * The prefetched catalog data stays complete, so any lookup of an excluded
 * relation made while the prefetcher is active is still answered correctly;
 * after that, excluded relations are loaded on demand.
 */
typedef struct YbPreloadExclusion
{
	MemoryContext context;
	HTAB	   *relids;
	HTAB	   *reltypes;
} YbPreloadExclusion;

static YbPreloadExclusion *yb_preload_exclusion = NULL;

/*
 * Return the first separator in str that is outside double quotes, or NULL.
 */
static char *
YbFindUnquotedSeparator(char *str, char separator)
{
	bool		in_quotes = false;

	for (; *str != '\0'; ++str)
	{
		if (*str == '"')
			in_quotes = !in_quotes;
		else if (*str == separator && !in_quotes)
			return str;
	}
	return NULL;
}

/*
 * Parse a single identifier in place, with the same rules as a search_path
 * element: unquoted names are downcased, double-quoted names are kept as is.
 */
static bool
YbParsePreloadExclusionName(char *raw, char **name)
{
	List	   *names;
	bool		ok = (SplitIdentifierString(raw, ',', &names) &&
					  list_length(names) == 1);

	if (ok)
		*name = linitial(names);
	list_free(names);
	return ok;
}

/*
 * Parse one "<role>@<database>:<schema>[,<schema>...]" entry in place and, if
 * it applies to this backend, append its schema names to
 * yb_preload_excluded_nsp_names. A malformed entry is ignored as a whole.
 */
static void
YbParsePreloadExclusionEntry(char *entry)
{
	char	   *original = pstrdup(entry);
	char	   *at = YbFindUnquotedSeparator(entry, '@');
	char	   *colon = at ? YbFindUnquotedSeparator(at + 1, ':') : NULL;
	char	   *role;
	char	   *database;
	List	   *nsp_names = NIL;
	ListCell   *lc;
	MemoryContext oldcxt;

	if (colon == NULL)
		goto malformed;
	*at = '\0';
	*colon = '\0';
	if (!YbParsePreloadExclusionName(entry, &role) ||
		!YbParsePreloadExclusionName(at + 1, &database) ||
		!SplitIdentifierString(colon + 1, ',', &nsp_names) ||
		nsp_names == NIL)
		goto malformed;

	if (strcmp(role, MyProcPort->user_name) == 0 &&
		strcmp(database, MyProcPort->database_name) == 0)
	{
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		foreach(lc, nsp_names)
			yb_preload_excluded_nsp_names =
				lappend(yb_preload_excluded_nsp_names, pstrdup(lfirst(lc)));
		MemoryContextSwitchTo(oldcxt);
	}
	list_free(nsp_names);
	pfree(original);
	return;

malformed:
	list_free(nsp_names);
	ereport(LOG,
			(errmsg("ysql_catalog_preload_exclude_schemas: ignoring "
					"malformed entry \"%s\"", original)));
	pfree(original);
}

static void
YbInitPreloadExcludedNamespaces(void)
{
	const char *flag = YBCGetGFlags()->ysql_catalog_preload_exclude_schemas;
	char	   *flag_copy;
	char	   *next;

	if (yb_preload_excluded_nsp_names_initialized)
		return;
	yb_preload_excluded_nsp_names_initialized = true;

	/*
	 * Only client backends are matched, by their login role: the filter must
	 * not change if the session user changes later.
	 */
	if (!IS_NON_EMPTY_STR_FLAG(flag) || MyProcPort == NULL ||
		MyProcPort->user_name == NULL || MyProcPort->database_name == NULL ||
		YBCIsInitDbModeEnvVarSet() || IsBinaryUpgrade ||
		YbUseMinimalCatalogCachesPreload())
		return;

	flag_copy = pstrdup(flag);
	for (char *entry = flag_copy; entry != NULL; entry = next)
	{
		next = YbFindUnquotedSeparator(entry, ';');
		if (next != NULL)
			*next++ = '\0';
		if (entry[strspn(entry, " \t\n\r\f\v")] != '\0')
			YbParsePreloadExclusionEntry(entry);
	}
	pfree(flag_copy);
}

/*
 * Look up the excluded schema names in the prefetched pg_namespace rows. Like
 * the pg_class scan in YbBeginPreloadExclusion, this must be a plain scan so
 * that the prefetcher serves it.
 */
static List *
YbResolvePreloadExcludedNamespaces(void)
{
	List	   *nsps = NIL;
	Relation	pg_namespace_desc;
	SysScanDesc scandesc;
	HeapTuple	tuple;

	pg_namespace_desc = table_open(NamespaceRelationId, AccessShareLock);
	scandesc = systable_beginscan(pg_namespace_desc, InvalidOid,
								  false /* indexOk */ , NULL, 0, NULL);
	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
	{
		Form_pg_namespace nspp = (Form_pg_namespace) GETSTRUCT(tuple);
		ListCell   *lc;

		foreach(lc, yb_preload_excluded_nsp_names)
		{
			if (strcmp(NameStr(nspp->nspname), lfirst(lc)) != 0)
				continue;
			if (IsCatalogNamespace(nspp->oid) || IsToastNamespace(nspp->oid))
				ereport(LOG,
						(errmsg("ysql_catalog_preload_exclude_schemas: "
								"ignoring system schema \"%s\"",
								NameStr(nspp->nspname))));
			else
				nsps = lappend_oid(nsps, nspp->oid);
			break;
		}
	}
	systable_endscan(scandesc);
	table_close(pg_namespace_desc, AccessShareLock);
	return nsps;
}

/*
 * Collect the relations to leave out of this preload. Must run after the
 * catalogs are prefetched and before any catalog cache is filled.
 */
void
YbBeginPreloadExclusion(int log_level)
{
	MemoryContext context;
	MemoryContext oldcxt;
	YbPreloadExclusion *exclusion;
	HASHCTL		ctl;
	List	   *nsps;
	Relation	pg_class_desc;
	SysScanDesc scandesc;
	HeapTuple	tuple;

	Assert(yb_preload_exclusion == NULL);
	YbInitPreloadExcludedNamespaces();
	if (yb_preload_excluded_nsp_names == NIL)
		return;

	context = AllocSetContextCreate(CurrentMemoryContext,
									"YbPreloadExclusion",
									ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(context);
	nsps = YbResolvePreloadExcludedNamespaces();
	if (nsps == NIL)
	{
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(context);
		elog(log_level, "Preloading relcache excludes no schemas: "
			 "none of the configured schemas exist");
		return;
	}
	exclusion = palloc0(sizeof(YbPreloadExclusion));
	exclusion->context = context;
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(Oid);
	ctl.hcxt = context;
	exclusion->relids = hash_create("YbPreloadExcludedRelids", 1024, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	exclusion->reltypes = hash_create("YbPreloadExcludedRelTypes", 1024, &ctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	MemoryContextSwitchTo(oldcxt);

	pg_class_desc = table_open(RelationRelationId, AccessShareLock);
	scandesc = systable_beginscan(pg_class_desc, InvalidOid,
								  false /* indexOk */ , NULL, 0, NULL);
	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
	{
		Form_pg_class relp = (Form_pg_class) GETSTRUCT(tuple);

		if (IsSystemClass(relp->oid, relp))
			continue;
		if (!list_member_oid(nsps, relp->relnamespace))
			continue;

		hash_search(exclusion->relids, &relp->oid, HASH_ENTER, NULL);
		if (OidIsValid(relp->reltype))
			hash_search(exclusion->reltypes, &relp->reltype, HASH_ENTER, NULL);
	}
	systable_endscan(scandesc);
	table_close(pg_class_desc, AccessShareLock);

	yb_preload_exclusion = exclusion;
	elog(log_level,
		 "Preloading relcache excludes %ld relation(s) in %d schema(s)",
		 hash_get_num_entries(exclusion->relids),
		 list_length(nsps));
}

void
YbEndPreloadExclusion(void)
{
	if (yb_preload_exclusion == NULL)
		return;
	MemoryContextDelete(yb_preload_exclusion->context);
	yb_preload_exclusion = NULL;
}

bool
YbIsPreloadExcludedRelation(Oid relid)
{
	return yb_preload_exclusion != NULL &&
		hash_search(yb_preload_exclusion->relids, &relid, HASH_FIND,
					NULL) != NULL;
}

static bool
YbIsPreloadExcludedRowType(Oid typid)
{
	return yb_preload_exclusion != NULL &&
		hash_search(yb_preload_exclusion->reltypes, &typid, HASH_FIND,
					NULL) != NULL;
}

/*
 * Whether a tuple of a relation-scoped catalog belongs to a relation that the
 * relcache preload in progress excludes (ysql_catalog_preload_exclude_schemas).
 * All tuples of an excluded relation are skipped together, which keeps the
 * per-relation catcache lists built by YbPreloadCatalogCache complete for the
 * relations kept.
 */
bool
YbIsPreloadExcludedTuple(Oid catalog_relid, HeapTuple tuple)
{
	Oid			relid;

	switch (catalog_relid)
	{
		case RelationRelationId:
			relid = ((Form_pg_class) GETSTRUCT(tuple))->oid;
			break;
		case AttributeRelationId:
			relid = ((Form_pg_attribute) GETSTRUCT(tuple))->attrelid;
			break;
		case StatisticRelationId:
			relid = ((Form_pg_statistic) GETSTRUCT(tuple))->starelid;
			break;
		case IndexRelationId:
			relid = ((Form_pg_index) GETSTRUCT(tuple))->indexrelid;
			break;
		case ConstraintRelationId:
			relid = ((Form_pg_constraint) GETSTRUCT(tuple))->conrelid;
			break;
		case RewriteRelationId:
			relid = ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;
			break;
		case PartitionedRelationId:
			relid = ((Form_pg_partitioned_table) GETSTRUCT(tuple))->partrelid;
			break;
		case TypeRelationId:
			{
				Form_pg_type typ = (Form_pg_type) GETSTRUCT(tuple);

				/* Array types of excluded row types go with them. */
				if (OidIsValid(typ->typelem) &&
					YbIsPreloadExcludedRowType(typ->typelem))
					return true;
				relid = typ->typrelid;
				break;
			}
		default:
			return false;
	}
	return OidIsValid(relid) && YbIsPreloadExcludedRelation(relid);
}

bool
YbPreloadExclusionConfigured(void)
{
	return yb_preload_excluded_nsp_names != NIL;
}
