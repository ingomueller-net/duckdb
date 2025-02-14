#include "duckdb/function/table/table_scan.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "duckdb/main/client_data.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Table Scan
//===--------------------------------------------------------------------===//
bool TableScanParallelStateNext(ClientContext &context, const FunctionData *bind_data_p,
                                LocalTableFunctionState *local_state, GlobalTableFunctionState *gstate);

struct TableScanLocalState : public LocalTableFunctionState {
	//! The current position in the scan
	TableScanState scan_state;
	//! The DataChunk containing all read columns (even filter columns that are immediately removed)
	DataChunk all_columns;
};

static StorageIndex TransformStorageIndex(const ColumnIndex &column_id) {
	vector<StorageIndex> result;
	for (auto &child_id : column_id.GetChildIndexes()) {
		result.push_back(TransformStorageIndex(child_id));
	}
	return StorageIndex(column_id.GetPrimaryIndex(), std::move(result));
}

static StorageIndex GetStorageIndex(TableCatalogEntry &table, const ColumnIndex &column_id) {
	if (column_id.IsRowIdColumn()) {
		return StorageIndex();
	}
	// the index of the base ColumnIndex is equal to the physical column index in the table
	// for any child indices - the indices are already the physical indices
	// (since only the top-level can have generated columns)
	auto &col = table.GetColumn(column_id.ToLogical());
	auto result = TransformStorageIndex(column_id);
	result.SetIndex(col.StorageOid());
	return result;
}

struct TableScanGlobalState : public GlobalTableFunctionState {
	TableScanGlobalState(ClientContext &context, const FunctionData *bind_data_p) {
		D_ASSERT(bind_data_p);
		auto &bind_data = bind_data_p->Cast<TableScanBindData>();
		max_threads = bind_data.table.GetStorage().MaxThreads(context);
	}

	ParallelTableScanState state;
	idx_t max_threads;

	vector<idx_t> projection_ids;
	vector<LogicalType> scanned_types;

	idx_t MaxThreads() const override {
		return max_threads;
	}

	bool CanRemoveFilterColumns() const {
		return !projection_ids.empty();
	}
};

static unique_ptr<LocalTableFunctionState> TableScanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                              GlobalTableFunctionState *gstate) {
	auto result = make_uniq<TableScanLocalState>();
	auto &bind_data = input.bind_data->Cast<TableScanBindData>();
	vector<StorageIndex> storage_ids;
	for (auto &col : input.column_indexes) {
		storage_ids.push_back(GetStorageIndex(bind_data.table, col));
	}
	result->scan_state.Initialize(std::move(storage_ids), input.filters.get(), input.sample_options.get());
	TableScanParallelStateNext(context.client, input.bind_data.get(), result.get(), gstate);
	if (input.CanRemoveFilterColumns()) {
		auto &tsgs = gstate->Cast<TableScanGlobalState>();
		result->all_columns.Initialize(context.client, tsgs.scanned_types);
	}

	result->scan_state.options.force_fetch_row = ClientConfig::GetConfig(context.client).force_fetch_row;

	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> TableScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	D_ASSERT(input.bind_data);
	auto &bind_data = input.bind_data->Cast<TableScanBindData>();
	auto result = make_uniq<TableScanGlobalState>(context, input.bind_data.get());
	bind_data.table.GetStorage().InitializeParallelScan(context, result->state);
	if (input.CanRemoveFilterColumns()) {
		result->projection_ids = input.projection_ids;
		const auto &columns = bind_data.table.GetColumns();
		for (const auto &col_idx : input.column_indexes) {
			if (col_idx.IsRowIdColumn()) {
				result->scanned_types.emplace_back(LogicalType::ROW_TYPE);
			} else {
				result->scanned_types.push_back(columns.GetColumn(col_idx.ToLogical()).Type());
			}
		}
	}
	return std::move(result);
}

static unique_ptr<BaseStatistics> TableScanStatistics(ClientContext &context, const FunctionData *bind_data_p,
                                                      column_t column_id) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	auto &local_storage = LocalStorage::Get(context, bind_data.table.catalog);
	if (local_storage.Find(bind_data.table.GetStorage())) {
		// we don't emit any statistics for tables that have outstanding transaction-local data
		return nullptr;
	}
	return bind_data.table.GetStatistics(context, column_id);
}

static void TableScanFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<TableScanBindData>();
	auto &gstate = data_p.global_state->Cast<TableScanGlobalState>();
	auto &state = data_p.local_state->Cast<TableScanLocalState>();
	auto &transaction = DuckTransaction::Get(context, bind_data.table.catalog);
	auto &storage = bind_data.table.GetStorage();

	state.scan_state.options.force_fetch_row = ClientConfig::GetConfig(context).force_fetch_row;
	do {
		if (bind_data.is_create_index) {
			storage.CreateIndexScan(state.scan_state, output,
			                        TableScanType::TABLE_SCAN_COMMITTED_ROWS_OMIT_PERMANENTLY_DELETED);
		} else if (gstate.CanRemoveFilterColumns()) {
			state.all_columns.Reset();
			storage.Scan(transaction, state.all_columns, state.scan_state);
			output.ReferenceColumns(state.all_columns, gstate.projection_ids);
		} else {
			storage.Scan(transaction, output, state.scan_state);
		}
		if (output.size() > 0) {
			return;
		}
		if (!TableScanParallelStateNext(context, data_p.bind_data.get(), data_p.local_state.get(),
		                                data_p.global_state.get())) {
			return;
		}
	} while (true);
}

bool TableScanParallelStateNext(ClientContext &context, const FunctionData *bind_data_p,
                                LocalTableFunctionState *local_state, GlobalTableFunctionState *global_state) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	auto &parallel_state = global_state->Cast<TableScanGlobalState>();
	auto &state = local_state->Cast<TableScanLocalState>();
	auto &storage = bind_data.table.GetStorage();

	return storage.NextParallelScan(context, parallel_state.state, state.scan_state);
}

double TableScanProgress(ClientContext &context, const FunctionData *bind_data_p,
                         const GlobalTableFunctionState *gstate_p) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	auto &gstate = gstate_p->Cast<TableScanGlobalState>();
	auto &storage = bind_data.table.GetStorage();
	idx_t total_rows = storage.GetTotalRows();
	if (total_rows == 0) {
		//! Table is either empty or smaller than a vector size, so it is finished
		return 100;
	}
	idx_t scanned_rows = gstate.state.scan_state.processed_rows;
	scanned_rows += gstate.state.local_state.processed_rows;
	auto percentage = 100 * (static_cast<double>(scanned_rows) / static_cast<double>(total_rows));
	if (percentage > 100) {
		//! In case the last chunk has less elements than STANDARD_VECTOR_SIZE, if our percentage is over 100
		//! It means we finished this table.
		return 100;
	}
	return percentage;
}

OperatorPartitionData TableScanGetPartitionData(ClientContext &context, TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("TableScan::GetPartitionData: partition columns not supported");
	}
	auto &state = input.local_state->Cast<TableScanLocalState>();
	if (state.scan_state.table_state.row_group) {
		return OperatorPartitionData(state.scan_state.table_state.batch_index);
	}
	if (state.scan_state.local_state.row_group) {
		return OperatorPartitionData(state.scan_state.table_state.batch_index +
		                             state.scan_state.local_state.batch_index);
	}
	return OperatorPartitionData(0);
}

BindInfo TableScanGetBindInfo(const optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	return BindInfo(bind_data.table);
}

void TableScanDependency(LogicalDependencyList &entries, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	entries.AddDependency(bind_data.table);
}

unique_ptr<NodeStatistics> TableScanCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	auto &local_storage = LocalStorage::Get(context, bind_data.table.catalog);
	auto &storage = bind_data.table.GetStorage();
	idx_t table_rows = storage.GetTotalRows();
	idx_t estimated_cardinality = table_rows + local_storage.AddedRows(bind_data.table.GetStorage());
	return make_uniq<NodeStatistics>(table_rows, estimated_cardinality);
}

//===--------------------------------------------------------------------===//
// Index Scan
//===--------------------------------------------------------------------===//
struct IndexScanGlobalState : public GlobalTableFunctionState {
	IndexScanGlobalState(const data_ptr_t row_id_data, const idx_t count)
	    : row_ids(LogicalType::ROW_TYPE, row_id_data), row_ids_count(count), row_ids_offset(0) {
	}

	const Vector row_ids;
	const idx_t row_ids_count;
	idx_t row_ids_offset;
	ColumnFetchState fetch_state;
	TableScanState local_storage_state;
	vector<StorageIndex> column_ids;
	bool finished;
};

static unique_ptr<GlobalTableFunctionState> IndexScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TableScanBindData>();

	data_ptr_t row_id_data = nullptr;
	if (!bind_data.row_ids.empty()) {
		row_id_data = (data_ptr_t)&bind_data.row_ids[0]; // NOLINT - this is not pretty
	}

	auto result = make_uniq<IndexScanGlobalState>(row_id_data, bind_data.row_ids.size());
	auto &local_storage = LocalStorage::Get(context, bind_data.table.catalog);

	result->local_storage_state.options.force_fetch_row = ClientConfig::GetConfig(context).force_fetch_row;
	result->column_ids.reserve(input.column_ids.size());
	for (auto &col_id : input.column_indexes) {
		result->column_ids.push_back(GetStorageIndex(bind_data.table, col_id));
	}

	result->local_storage_state.Initialize(result->column_ids, input.filters.get());
	local_storage.InitializeScan(bind_data.table.GetStorage(), result->local_storage_state.local_state, input.filters);

	result->finished = false;
	return std::move(result);
}

static void IndexScanFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<TableScanBindData>();
	auto &state = data_p.global_state->Cast<IndexScanGlobalState>();
	auto &transaction = DuckTransaction::Get(context, bind_data.table.catalog);
	auto &local_storage = LocalStorage::Get(transaction);

	if (!state.finished) {
		auto remaining = state.row_ids_count - state.row_ids_offset;
		auto scan_count = remaining < STANDARD_VECTOR_SIZE ? remaining : STANDARD_VECTOR_SIZE;

		Vector row_ids(state.row_ids, state.row_ids_offset, state.row_ids_offset + scan_count);
		bind_data.table.GetStorage().Fetch(transaction, output, state.column_ids, row_ids, scan_count,
		                                   state.fetch_state);

		state.row_ids_offset += scan_count;
		if (state.row_ids_offset == state.row_ids_count) {
			state.finished = true;
		}
	}
	if (output.size() == 0) {
		local_storage.Scan(state.local_storage_state.local_state, state.column_ids, output);
	}
}

static void RewriteIndexExpression(Index &index, LogicalGet &get, Expression &expr, bool &rewrite_possible) {
	if (expr.type == ExpressionType::BOUND_COLUMN_REF) {
		auto &bound_colref = expr.Cast<BoundColumnRefExpression>();
		// bound column ref: rewrite to fit in the current set of bound column ids
		bound_colref.binding.table_index = get.table_index;
		auto &column_ids = index.GetColumnIds();
		auto &get_column_ids = get.GetColumnIds();
		column_t referenced_column = column_ids[bound_colref.binding.column_index];
		// search for the referenced column in the set of column_ids
		for (idx_t i = 0; i < get_column_ids.size(); i++) {
			auto column_id = get_column_ids[i].GetPrimaryIndex();
			if (column_id == referenced_column) {
				bound_colref.binding.column_index = i;
				return;
			}
		}
		// column id not found in bound columns in the LogicalGet: rewrite not possible
		rewrite_possible = false;
	}
	ExpressionIterator::EnumerateChildren(
	    expr, [&](Expression &child) { RewriteIndexExpression(index, get, child, rewrite_possible); });
}

void TableScanPushdownComplexFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
                                    vector<unique_ptr<Expression>> &filters) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	auto &table = bind_data.table;
	auto &storage = table.GetStorage();

	auto &config = ClientConfig::GetConfig(context);
	if (!config.enable_optimizer) {
		// we only push index scans if the optimizer is enabled
		return;
	}
	if (bind_data.is_index_scan) {
		return;
	}
	if (!get.table_filters.filters.empty()) {
		// if there were filters before we can't convert this to an index scan
		return;
	}
	if (!get.projection_ids.empty()) {
		// if columns were pruned by RemoveUnusedColumns we can't convert this to an index scan,
		// because index scan does not support filter_prune (yet)
		return;
	}
	if (filters.empty()) {
		// no indexes or no filters: skip the pushdown
		return;
	}

	auto checkpoint_lock = storage.GetSharedCheckpointLock();
	auto &info = storage.GetDataTableInfo();

	// bind and scan any ART indexes
	info->GetIndexes().BindAndScan<ART>(context, *info, [&](ART &art_index) {
		// first rewrite the index expression so the ColumnBindings align with the column bindings of the current table
		if (art_index.unbound_expressions.size() > 1) {
			// NOTE: index scans are not (yet) supported for compound index keys
			return false;
		}

		auto index_expression = art_index.unbound_expressions[0]->Copy();
		bool rewrite_possible = true;
		RewriteIndexExpression(art_index, get, *index_expression, rewrite_possible);
		if (!rewrite_possible) {
			// could not rewrite!
			return false;
		}

		// Try to find a matching index for any of the filter expressions.
		for (auto &filter : filters) {
			auto index_state = art_index.TryInitializeScan(*index_expression, *filter);
			if (index_state != nullptr) {

				auto &db_config = DBConfig::GetConfig(context);
				auto index_scan_percentage = db_config.GetSetting<IndexScanPercentageSetting>(context);
				auto index_scan_max_count = db_config.GetSetting<IndexScanMaxCountSetting>(context);

				auto total_rows = storage.GetTotalRows();
				auto total_rows_from_percentage = LossyNumericCast<idx_t>(double(total_rows) * index_scan_percentage);
				auto max_count = MaxValue(index_scan_max_count, total_rows_from_percentage);

				// Check if we can use an index scan, and already retrieve the matching row ids.
				if (art_index.Scan(*index_state, max_count, bind_data.row_ids)) {
					bind_data.is_index_scan = true;
					get.function = TableScanFunction::GetIndexScanFunction();
					return true;
				}

				// Clear the row ids in case we exceeded the maximum count and stopped scanning.
				bind_data.row_ids.clear();
				return true;
			}
		}
		return false;
	});
}

InsertionOrderPreservingMap<string> TableScanToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	auto &bind_data = input.bind_data->Cast<TableScanBindData>();
	result["Table"] = bind_data.table.name;
	return result;
}

static void TableScanSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                               const TableFunction &function) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	serializer.WriteProperty(100, "catalog", bind_data.table.schema.catalog.GetName());
	serializer.WriteProperty(101, "schema", bind_data.table.schema.name);
	serializer.WriteProperty(102, "table", bind_data.table.name);
	serializer.WriteProperty(103, "is_index_scan", bind_data.is_index_scan);
	serializer.WriteProperty(104, "is_create_index", bind_data.is_create_index);
	serializer.WriteProperty(105, "result_ids", bind_data.row_ids);
}

static unique_ptr<FunctionData> TableScanDeserialize(Deserializer &deserializer, TableFunction &function) {
	auto catalog = deserializer.ReadProperty<string>(100, "catalog");
	auto schema = deserializer.ReadProperty<string>(101, "schema");
	auto table = deserializer.ReadProperty<string>(102, "table");
	auto &catalog_entry =
	    Catalog::GetEntry<TableCatalogEntry>(deserializer.Get<ClientContext &>(), catalog, schema, table);
	if (catalog_entry.type != CatalogType::TABLE_ENTRY) {
		throw SerializationException("Cant find table for %s.%s", schema, table);
	}
	auto result = make_uniq<TableScanBindData>(catalog_entry.Cast<DuckTableEntry>());
	deserializer.ReadProperty(103, "is_index_scan", result->is_index_scan);
	deserializer.ReadProperty(104, "is_create_index", result->is_create_index);
	deserializer.ReadProperty(105, "result_ids", result->row_ids);
	return std::move(result);
}

TableFunction TableScanFunction::GetIndexScanFunction() {
	TableFunction scan_function("index_scan", {}, IndexScanFunction);
	scan_function.init_local = nullptr;
	scan_function.init_global = IndexScanInitGlobal;
	scan_function.statistics = TableScanStatistics;
	scan_function.dependency = TableScanDependency;
	scan_function.cardinality = TableScanCardinality;
	scan_function.pushdown_complex_filter = nullptr;
	scan_function.to_string = TableScanToString;
	scan_function.table_scan_progress = nullptr;
	scan_function.get_partition_data = nullptr;
	scan_function.projection_pushdown = true;
	scan_function.filter_pushdown = false;
	scan_function.get_bind_info = TableScanGetBindInfo;
	scan_function.serialize = TableScanSerialize;
	scan_function.deserialize = TableScanDeserialize;
	return scan_function;
}

TableFunction TableScanFunction::GetFunction() {
	TableFunction scan_function("seq_scan", {}, TableScanFunc);
	scan_function.init_local = TableScanInitLocal;
	scan_function.init_global = TableScanInitGlobal;
	scan_function.statistics = TableScanStatistics;
	scan_function.dependency = TableScanDependency;
	scan_function.cardinality = TableScanCardinality;
	scan_function.pushdown_complex_filter = TableScanPushdownComplexFilter;
	scan_function.to_string = TableScanToString;
	scan_function.table_scan_progress = TableScanProgress;
	scan_function.get_partition_data = TableScanGetPartitionData;
	scan_function.get_bind_info = TableScanGetBindInfo;
	scan_function.projection_pushdown = true;
	scan_function.filter_pushdown = true;
	scan_function.filter_prune = true;
	scan_function.sampling_pushdown = true;
	scan_function.serialize = TableScanSerialize;
	scan_function.deserialize = TableScanDeserialize;
	return scan_function;
}

void TableScanFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunctionSet table_scan_set("seq_scan");
	table_scan_set.AddFunction(GetFunction());
	set.AddFunction(std::move(table_scan_set));

	set.AddFunction(GetIndexScanFunction());
}

void BuiltinFunctions::RegisterTableScanFunctions() {
	TableScanFunction::RegisterFunction(*this);
}

} // namespace duckdb
