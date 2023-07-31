/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef AG_JUNCTION_TABLE_COMMANDS_H
#define AG_JUNCTION_TABLE_COMMANDS_H

#include "postgres.h"
#include "utils/graphid.h"

#define AG_JUNCTION_TABLE "_ag_junction_table"
#define Natts_ag_junction_table 2
#define Anum_ag_junc_label_id 2

void create_junction_table(char *graph_name);
void drop_properties_column (char *label_name,
				    char *schema_name, Oid nsp_id);
int32 junction_table_label_id(const char *graph_name, Oid graph_oid, graphid element_graphid);

/* #define ag_junction_table_index_id(graph_name, graph_oid)		\ */
/*     ag_junction_id(graph_name, "_ag_junction_table_pkey", graph_oid, "index") */
#define ag_junction_table_id(graph_name, graph_oid)			\
    ag_junction_id(graph_name, AG_JUNCTION_TABLE, graph_oid, "table")

#endif
