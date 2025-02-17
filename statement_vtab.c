/*
 * SQLite module to define virtual tables and table-valued functions natively using SQL.
 * In the interest of compatibility with SQLite's own license (or rather lack thereof),
 * the author disclaims copyright to this source code.
 */

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#include <string.h>
#include <stdio.h>
#include <assert.h>

struct statement_vtab {
	sqlite3_vtab base;
	sqlite3* db;
	char* sql;
	size_t sql_len;
	int num_inputs;
	int num_outputs;
};

struct statement_cursor {
	sqlite3_vtab_cursor base;
	sqlite3_stmt* stmt;
	int rowid;
	int param_argc;
	sqlite3_value** param_argv;
};

static char* build_create_statement(sqlite3_stmt* stmt) {
	sqlite3_str* sql = sqlite3_str_new(NULL);
	sqlite3_str_appendall(sql,"CREATE TABLE x( ");
	for(int i = 0, nout = sqlite3_column_count(stmt); i < nout; i++) {
		const char* name = sqlite3_column_name(stmt,i);
		if(!name) {
			sqlite3_free(sqlite3_str_finish(sql));
			return NULL;
		}
		const char* type = sqlite3_column_decltype(stmt,i);
		sqlite3_str_appendf(sql,"%Q %s,",name,(type?type:""));
	}
	for(int i = 0, nargs = sqlite3_bind_parameter_count(stmt); i < nargs; i++) {
		const char* name = sqlite3_bind_parameter_name(stmt,i+1);
		if(name)
			sqlite3_str_appendf(sql,"%Q hidden,",name+1);
		else
			sqlite3_str_appendf(sql,"'%d' hidden,",i+1);
	}
	if(sqlite3_str_length(sql))
		sqlite3_str_value(sql)[sqlite3_str_length(sql)-1] = ')';
	return sqlite3_str_finish(sql);
}

static int statement_vtab_destroy(sqlite3_vtab* pVTab){
	sqlite3_free(((struct statement_vtab*)pVTab)->sql);
	sqlite3_free(pVTab);
	return SQLITE_OK;
}

static int statement_vtab_create(sqlite3* db, void* pAux, int argc, const char* const* argv, sqlite3_vtab** ppVtab, char** pzErr) {
	size_t len;
	if(argc < 4 || (len = strlen(argv[3])) < 3) {
		if(!(*pzErr = sqlite3_mprintf("no statement provided")))
			return SQLITE_NOMEM;
		return SQLITE_MISUSE;
	}
	if(argv[3][0] != '(' || argv[3][len-1] != ')') {
		if(!(*pzErr = sqlite3_mprintf("statement must be parenthesized")))
			return SQLITE_NOMEM;
		return SQLITE_MISUSE;
	}

	int ret;
	sqlite3_mutex* mutex = sqlite3_db_mutex(db); // only needed to ensure correctness of sqlite3_errmsg
	sqlite3_stmt* stmt = NULL;
	char* create = NULL;

	struct statement_vtab* vtab = sqlite3_malloc64(sizeof(*vtab));
	if(!vtab)
		return SQLITE_NOMEM;
	memset(vtab,0,sizeof(*vtab));
	*ppVtab = &vtab->base;

	vtab->db = db;
	vtab->sql_len = len-2;
	if(!(vtab->sql = sqlite3_mprintf("%.*s",vtab->sql_len,argv[3]+1))) {
		ret = SQLITE_NOMEM;
		goto error;
	}

	sqlite3_mutex_enter(mutex);
	if((ret = sqlite3_prepare_v2(db,vtab->sql,vtab->sql_len,&stmt,NULL)) != SQLITE_OK)
		goto sqlite_error;
	sqlite3_mutex_leave(mutex);
	if(!sqlite3_stmt_readonly(stmt)) {
		ret = SQLITE_ERROR;
		if(!(*pzErr = sqlite3_mprintf("Statement must be read only.")))
			ret = SQLITE_NOMEM;
		goto error;
	}

	vtab->num_inputs = sqlite3_bind_parameter_count(stmt);
	vtab->num_outputs = sqlite3_column_count(stmt);

	if(!(create = build_create_statement(stmt))) {
		ret = SQLITE_NOMEM;
		goto error;
	}
	sqlite3_mutex_enter(mutex);
	if((ret = sqlite3_declare_vtab(db,create)) != SQLITE_OK)
		goto sqlite_error;
	sqlite3_mutex_leave(mutex);

	sqlite3_free(create);
	sqlite3_finalize(stmt);
	return SQLITE_OK;

sqlite_error:
	if(!(*pzErr = sqlite3_mprintf("%s",sqlite3_errmsg(db))))
		ret = SQLITE_NOMEM;
	sqlite3_mutex_leave(mutex);
error:
	sqlite3_free(create);
	sqlite3_finalize(stmt);
	statement_vtab_destroy(*ppVtab);
	*ppVtab = NULL;
	return ret;
}

// if these point to the literal same function sqlite makes statement_vtab eponymous, which we don't want
static int statement_vtab_connect(sqlite3* db, void* pAux, int argc, const char* const* argv, sqlite3_vtab** ppVtab, char** pzErr) {
	return statement_vtab_create(db,pAux,argc,argv,ppVtab,pzErr);
}

static int statement_vtab_open(sqlite3_vtab* pVTab, sqlite3_vtab_cursor** ppCursor) {
	struct statement_vtab* vtab = (struct statement_vtab*)pVTab;
	struct statement_cursor* cur = sqlite3_malloc64(sizeof(*cur));
	if(!cur)
		return SQLITE_NOMEM;

	*ppCursor = &cur->base;
	cur->param_argv = sqlite3_malloc(sizeof(*cur->param_argv)*vtab->num_inputs);
	return sqlite3_prepare_v2(vtab->db,vtab->sql,vtab->sql_len,&cur->stmt,NULL);
}

static int statement_vtab_close(sqlite3_vtab_cursor* cur){
	struct statement_cursor* stmtcur = (struct statement_cursor*)cur;
	sqlite3_finalize(stmtcur->stmt);
	sqlite3_free(stmtcur->param_argv);
	sqlite3_free(cur);
	return SQLITE_OK;
}

static int statement_vtab_next(sqlite3_vtab_cursor* cur){
	struct statement_cursor* stmtcur = (struct statement_cursor*)cur;
	int ret = sqlite3_step(stmtcur->stmt);
	if(ret == SQLITE_ROW) {
		stmtcur->rowid++;
		return SQLITE_OK;
	}
	return ret == SQLITE_DONE ? SQLITE_OK : ret;
}

static int statement_vtab_rowid(sqlite3_vtab_cursor* cur, sqlite_int64* pRowid) {
	*pRowid = ((struct statement_cursor*)cur)->rowid;
	return SQLITE_OK;
}

static int statement_vtab_eof(sqlite3_vtab_cursor* cur) {
	return !sqlite3_stmt_busy(((struct statement_cursor*)cur)->stmt);
}

static int statement_vtab_column(sqlite3_vtab_cursor* cur, sqlite3_context* ctx, int i) {
	struct statement_cursor* stmtcur = (struct statement_cursor*)cur;
	int num_outputs = ((struct statement_vtab*)cur->pVtab)->num_outputs;
	if(i < num_outputs)
		sqlite3_result_value(ctx,sqlite3_column_value(stmtcur->stmt,i));
	else if(i-num_outputs < stmtcur->param_argc)
		sqlite3_result_value(ctx,stmtcur->param_argv[i-num_outputs]);
	return SQLITE_OK;
}

// xBestIndex needs to communicate which columns are constrained by the where clause to xFilter;
// in terms of a statement table this translates to which parameters will be available to bind.
static int statement_vtab_filter(sqlite3_vtab_cursor* cur, int idxNum, const char* idxStr, int argc, sqlite3_value** argv) {
	struct statement_cursor* stmtcur = (struct statement_cursor*)cur;
	stmtcur->rowid = 1;
	sqlite3_stmt* stmt = stmtcur->stmt;
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);

	int ret;
	for(int i = 0; i < argc; i++)
		if((ret = sqlite3_bind_value(stmt,idxStr?((int*)idxStr)[i]:i+1,argv[i])) != SQLITE_OK)
			return ret;
	ret = sqlite3_step(stmt);
	if(!(ret == SQLITE_ROW || ret == SQLITE_DONE))
		return ret;

	assert(((struct statement_vtab*)cur->pVtab)->num_inputs >= argc);
	if((stmtcur->param_argc = argc)) // these seem to persist for the remainder of the statement, so just shallow copy
		memcpy(stmtcur->param_argv,argv,sizeof(*stmtcur->param_argv)*argc);

	return SQLITE_OK;
}

static int statement_vtab_best_index(sqlite3_vtab* pVTab, sqlite3_index_info* index_info){
	int num_outputs = ((struct statement_vtab*)pVTab)->num_outputs;
	int out_constraints = 0;
	index_info->orderByConsumed = 0;
	index_info->estimatedCost = 1;
	index_info->estimatedRows = 1;
	int col_max = 0;
	sqlite3_uint64 used_cols = 0;
	for(int i = 0; i < index_info->nConstraint; i++) {
		// skip if this is a constraint on one of our output columns
		if(index_info->aConstraint[i].iColumn < num_outputs)
			continue;
		// a given query plan is only usable if all provided "input" columns are usable and have equal constraints only
		// is this redundant / an EQ constraint ever unusable?
		if(!index_info->aConstraint[i].usable || index_info->aConstraint[i].op != SQLITE_INDEX_CONSTRAINT_EQ)
			return SQLITE_CONSTRAINT;

		int col_index = index_info->aConstraint[i].iColumn - num_outputs;
		index_info->aConstraintUsage[i].argvIndex = col_index+1;
		index_info->aConstraintUsage[i].omit = 1;

		if(col_index+1 > col_max)
			col_max = col_index+1;
		if(col_index < 64)
			used_cols |= 1ull << col_index;

		out_constraints++;
	}

	// if the constrained columns are contiguous then we can just tell sqlite to order the arg vector provided to xFilter
	// in the same order as our column bindings, so there's no need to map between these
	// (this will always be the case when calling the vtab as a table-valued function)
	// only support this optimization for up to 64 constrained columns since checking for continuity more generally would cost as much
	// as just allocating the mapping
	sqlite_uint64 required_cols = (col_max < 64 ? 1ull << col_max : 0ull)-1;
	if(!out_constraints || (col_max <= 64 && used_cols == required_cols))
		return SQLITE_OK;

	// otherwise map the constraint index as provided to xFilter to column index for bindings
	// if this is sparse e.g. where arg1 = x and arg3 = y then we store this separately in idxStr
	int* colmap = sqlite3_malloc64(sizeof(*colmap)*out_constraints);
	if(!colmap)
		return SQLITE_NOMEM;

	int argc = 0;
	int old_index;
	for(int i = 0; i < index_info->nConstraint; i++)
		if((old_index = index_info->aConstraintUsage[i].argvIndex)) {
			colmap[argc] = old_index;
			index_info->aConstraintUsage[i].argvIndex = ++argc;
		}

	index_info->idxStr = (char*)colmap;
	index_info->needToFreeIdxStr = 1;

	return SQLITE_OK;
}

static sqlite3_module statement_vtab_module = {
	.xCreate     = statement_vtab_create,
	.xConnect    = statement_vtab_connect,
	.xBestIndex  = statement_vtab_best_index,
	.xDisconnect = statement_vtab_destroy,
	.xDestroy    = statement_vtab_destroy,
	.xOpen       = statement_vtab_open,
	.xClose      = statement_vtab_close,
	.xFilter     = statement_vtab_filter,
	.xNext       = statement_vtab_next,
	.xEof        = statement_vtab_eof,
	.xColumn     = statement_vtab_column,
	.xRowid      = statement_vtab_rowid,
};

int sqlite3_statementvtab_init(sqlite3* db, char** pzErrMsg, const sqlite3_api_routines* pApi) {
	SQLITE_EXTENSION_INIT2(pApi);
	return sqlite3_create_module(db, "statement", &statement_vtab_module, NULL);
}
