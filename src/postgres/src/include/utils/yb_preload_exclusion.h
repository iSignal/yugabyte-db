/*--------------------------------------------------------------------------------------------------
 *
 * yb_preload_exclusion.h
 *	  Declarations for leaving the schemas configured by
 *	  ysql_catalog_preload_exclude_schemas out of relcache preloading.
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
 * src/include/utils/yb_preload_exclusion.h
 *
 *--------------------------------------------------------------------------------------------------
 */
#ifndef YB_PRELOAD_EXCLUSION_H
#define YB_PRELOAD_EXCLUSION_H

#include "access/htup.h"

extern void YbBeginPreloadExclusion(int log_level);
extern void YbEndPreloadExclusion(void);
extern bool YbIsPreloadExcludedRelation(Oid relid);
extern bool YbIsPreloadExcludedTuple(Oid catalog_relid, HeapTuple tuple);
extern bool YbPreloadExclusionConfigured(void);

#endif							/* YB_PRELOAD_EXCLUSION_H */
