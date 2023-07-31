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

#include "access/sdir.h"
#include "postgres.h"

#include "access/xact.h"
#include "commands/tablecmds.h"
#include "nodes/makefuncs.h"
#include "storage/lockdefs.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "access/skey.h"
#include "access/table.h"
#include "access/genam.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "utils/snapmgr.h"

#include "commands/label_commands.h"
#include "utils/ag_cache.h"
#include "utils/agtype.h"
#include "utils/junction_table.h"
#include "catalog/ag_catalog.h"

#define gen_label_relation_name(label_name);

void create_junction_table(char *graph_name);
static void create_table_for_junc_table(char *graph_name, char *label_name,
					char *schema_name, char *rel_name);

static List *create_junc_table_table_elements(char *graph_name, char *label_name,
					      char *schema_name, char *rel_name);
void drop_properties_column (char *label_name,
			     char *schema_name, Oid nsp_id);
int32 junction_table_label_id(const char *graph_name, Oid graph_oid, graphid element_graphid);
Oid ag_junction_id(const char *graph_name, const char* table_name ,Oid graph_oid, char *table_kind);

void create_junction_table(char *graph_name)
{
    graph_cache_data *cache_data;
    Oid nsp_id;
    char *schema_name;
    char *rel_name;
    char *label_name = AG_JUNCTION_TABLE;

    cache_data = search_graph_name_cache(graph_name);
    if (!cache_data)
    {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" does not exist", graph_name)));
    }
    nsp_id = cache_data->namespace;
    schema_name = get_namespace_name(nsp_id);
    rel_name = label_name;

    // create a table for the junction table
    create_table_for_junc_table(graph_name, label_name, schema_name, rel_name);


    CommandCounterIncrement();
};

static void create_table_for_junc_table(char *graph_name, char *label_name,
				    char *schema_name, char *rel_name)
{
    CreateStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(CreateStmt);

    // relpersistence is set to RELPERSISTENCE_PERMANENT by makeRangeVar()
    create_stmt->relation = makeRangeVar(schema_name, rel_name, -1);

    create_stmt->tableElts =
        create_junc_table_table_elements(graph_name, label_name,
					 schema_name, rel_name);


    create_stmt->inhRelations = NIL;
    create_stmt->partbound = NULL;
    create_stmt->ofTypename = NULL;
    create_stmt->constraints = NIL;
    create_stmt->options = NIL;
    create_stmt->oncommit = ONCOMMIT_NOOP;
    create_stmt->tablespacename = NULL;
    create_stmt->if_not_exists = false;

    wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated CREATE TABLE command)",
                   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver,
                   NULL);
    // CommandCounterIncrement() is called in ProcessUtility()
}

static List *create_junc_table_table_elements(char *graph_name, char *label_name,
					      char *schema_name, char *rel_name)
{
    ColumnDef *id;
    ColumnDef *props;
    ColumnDef *label_id;
    Constraint *not_null;
    List *func_name;
    FuncCall *func;
    Constraint *props_default, *pk;

    // "ag_catalog"."agtype_build_map"()
    func_name = list_make2(makeString("ag_catalog"),
                           makeString("agtype_build_map"));
    func = makeFuncCall(func_name, NIL, -1);

    props_default = makeNode(Constraint);
    props_default->contype = CONSTR_DEFAULT;
    props_default->location = -1;
    props_default->raw_expr = (Node *)func;
    props_default->cooked_expr = NULL;

    pk = makeNode(Constraint);
    pk->contype = CONSTR_PRIMARY;
    pk->location = -1;
    pk->keys = NULL;
    pk->options = NIL;
    pk->indexname = NULL;
    pk->indexspace = NULL;

    not_null = makeNode(Constraint);
    not_null->contype = CONSTR_NOTNULL;
    not_null->location = -1;

    // "id" eid PRIMARY KEY DEFAULT "ag_catalog"."_graphid"(...)
    id = makeColumnDef(AG_VERTEX_COLNAME_ID, EIDOID, -1, InvalidOid);
    id->constraints = list_make1(pk);
    /*
     * The "properties" column will be dropped after the creation of "_ag_label_vertex" 
     * We need it here so the "_ag_label_vertex" inherits the columns correctly and in the 
     * right order.
     */

    props = makeColumnDef(AG_VERTEX_COLNAME_PROPERTIES, AGTYPEOID, -1,
                          InvalidOid);
    props->constraints = list_make2(not_null,
                                    props_default);

    // "label_id" integer NOT NULL DEFAULT "ag_catalog"."_label_id(...)"
    label_id = makeColumnDef(AG_VERTEX_COLNAME_LABEL_ID, INT4OID, -1,
                             InvalidOid);

    return list_make3(id, props, label_id);
}

void drop_properties_column(char *label_name,
				    char *schema_name, Oid nsp_id)
{
    ParseState *pstate;
    AlterTableStmt *tbl_stmt;
    AlterTableCmd *tbl_cmd;
    RangeVar *rv;
    AlterTableUtilityContext atuc;
    Oid relid;
    
    rv = makeRangeVar(schema_name, label_name, -1);

    pstate = make_parsestate(NULL);
    pstate->p_sourcetext = "(generated ALTER TABLE command)";

    tbl_stmt = makeNode(AlterTableStmt);
    tbl_stmt->relation = rv;
    tbl_stmt->missing_ok = false;

    tbl_cmd = makeNode(AlterTableCmd);
    tbl_cmd->subtype = AT_DropColumn;
    tbl_cmd->name = AG_VERTEX_COLNAME_PROPERTIES;

    tbl_stmt->cmds = list_make1(tbl_cmd);
    relid = get_relname_relid(label_name, nsp_id);
    atuc.relid = relid;
    atuc.queryEnv = pstate->p_queryEnv;
    atuc.queryString = pstate->p_sourcetext;

    AlterTable(tbl_stmt, AccessExclusiveLock, &atuc);

    CommandCounterIncrement();
}

int32 junction_table_label_id(const char *graph_name, Oid graph_oid, graphid element_graphid)
{
    ScanKeyData scan_key[1];
    TableScanDesc scan_desc;
    HeapTuple tuple;
    TupleDesc tupdesc;
    Relation junc_table;
    bool column_is_null = false;
    int64 result;
    Snapshot snapshot = GetActiveSnapshot();

    ScanKeyInit(&scan_key[0], 1, BTEqualStrategyNumber, F_INT4EQ,
		Int64GetDatum(element_graphid)); 
    junc_table = table_open(ag_junction_table_id(graph_name, graph_oid), ShareLock);
    /* scan_desc = systable_beginscan(junc_table, ag_junction_table_index_id(graph_name, graph_oid), true, NULL, 1, &scan_key); */
    scan_desc = table_beginscan(junc_table, snapshot, 1, scan_key);
    tuple = heap_getnext(scan_desc, ForwardScanDirection);
    
    if (!HeapTupleIsValid(tuple))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("Entry %lu does not exist", element_graphid)));
    }
    tupdesc = RelationGetDescr(junc_table);
    
    if (tupdesc->natts != Natts_ag_junction_table)
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("Invalid number of attributes for %s.%s", graph_name, AG_JUNCTION_TABLE)));
    }
    /* result = DatumGetInt64(heap_getattr(tuple, Anum_ag_junc_label_id, */
    /*                                             tupdesc, &column_is_null)); */
    result = column_get_datum(tupdesc, tuple, 0, "id", INT8OID, true);
    table_endscan(scan_desc);
    table_close(junc_table, ShareLock);

    return result;
}

Oid ag_junction_id(const char *graph_name, const char* table_name ,Oid graph_oid, char *table_kind)
{
    Oid id;

    id = get_relname_relid(table_name, graph_oid);
    if (!OidIsValid(id))
    {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                        errmsg("Junction %s doesn't exist", table_kind)));
    }

    return id;
}
