#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "catalog/catalog_accessor.h"
#include "common/macros.h"
#include "common/managed_pointer.h"
#include "optimizer/logical_operators.h"
#include "optimizer/operator_expression.h"
#include "optimizer/query_to_operator_transformer.h"
#include "parser/expression/column_value_expression.h"
#include "parser/expression/comparison_expression.h"
#include "parser/expression/operator_expression.h"
#include "parser/expression/subquery_expression.h"
#include "parser/expression_util.h"
#include "parser/statements.h"
#include "planner/plannodes/plan_node_defs.h"

namespace terrier::optimizer {

QueryToOperatorTransformer::QueryToOperatorTransformer(std::unique_ptr<catalog::CatalogAccessor> catalog_accessor)
    : accessor_(std::move(catalog_accessor)) {
  output_expr_ = nullptr;
}

OperatorExpression *QueryToOperatorTransformer::ConvertToOpExpression(common::ManagedPointer<parser::SQLStatement> op,
                                                                      parser::ParseResult *parse_result) {
  output_expr_ = nullptr;
  op->Accept(this, parse_result);
  return output_expr_;
}

void QueryToOperatorTransformer::Visit(parser::SelectStatement *op, parser::ParseResult *parse_result) {
  // We do not visit the select list of a base table because the column
  // information is derived before the plan generation, at this step we
  // don't need to derive that
  auto pre_predicates = std::move(predicates_);

  if (op->GetSelectTable() != nullptr) {
    // SELECT with FROM
    op->GetSelectTable()->Accept(this, parse_result);
  } else {
    // SELECT without FROM
    output_expr_ = new OperatorExpression(LogicalGet::Make(), {});
  }

  if (op->GetSelectCondition() != nullptr) {
    CollectPredicates(op->GetSelectCondition(), parse_result, &predicates_);
  }

  if (!predicates_.empty()) {
    auto filter_expr = new OperatorExpression(LogicalFilter::Make(std::move(predicates_)), {output_expr_});
    // TODO(Ling): Do something after the predicates are moved to make the vector valid?
    predicates_.clear();
    output_expr_ = filter_expr;
  }

  if (QueryToOperatorTransformer::RequireAggregation(common::ManagedPointer(op))) {
    // Plain aggregation
    OperatorExpression *agg_expr;
    if (op->GetSelectGroupBy() == nullptr) {
      // TODO(boweic): aggregation without groupby could still have having clause
      agg_expr = new OperatorExpression(LogicalAggregateAndGroupBy::Make(), {output_expr_});
      output_expr_ = agg_expr;
    } else {
      size_t num_group_by_cols = op->GetSelectGroupBy()->GetColumns().size();
      auto group_by_cols = std::vector<common::ManagedPointer<parser::AbstractExpression>>(num_group_by_cols);
      for (size_t i = 0; i < num_group_by_cols; i++) {
        group_by_cols[i] = common::ManagedPointer<parser::AbstractExpression>(op->GetSelectGroupBy()->GetColumns()[i]);
      }
      agg_expr = new OperatorExpression(LogicalAggregateAndGroupBy::Make(std::move(group_by_cols)), {output_expr_});
      output_expr_ = agg_expr;

      std::vector<AnnotatedExpression> having;
      if (op->GetSelectGroupBy()->GetHaving() != nullptr) {
        CollectPredicates(op->GetSelectGroupBy()->GetHaving(), parse_result, &having);
      }
      if (!having.empty()) {
        auto filter_expr = new OperatorExpression(LogicalFilter::Make(std::move(having)), {output_expr_});
        output_expr_ = filter_expr;
      }
    }
  }

  if (op->IsSelectDistinct()) {
    auto distinct_expr = new OperatorExpression(LogicalDistinct::Make(), {output_expr_});
    output_expr_ = distinct_expr;
  }

  if (op->GetSelectLimit() != nullptr && op->GetSelectLimit()->GetLimit() != -1) {
    std::vector<common::ManagedPointer<parser::AbstractExpression>> sort_exprs;
    std::vector<optimizer::OrderByOrderingType> sort_direction;

    if (op->GetSelectOrderBy() != nullptr) {
      const auto &order_info = op->GetSelectOrderBy();
      for (auto &expr : order_info->GetOrderByExpressions()) {
        sort_exprs.push_back(expr);
      }
      for (auto &type : order_info->GetOrderByTypes()) {
        if (type == parser::kOrderAsc)
          sort_direction.push_back(optimizer::OrderByOrderingType::ASC);
        else
          sort_direction.push_back(optimizer::OrderByOrderingType::DESC);
      }
    }
    auto limit_expr =
        new OperatorExpression(LogicalLimit::Make(op->GetSelectLimit()->GetOffset(), op->GetSelectLimit()->GetLimit(),
                                                  std::move(sort_exprs), std::move(sort_direction)),
                               {output_expr_});
    output_expr_ = limit_expr;
  }

  predicates_ = std::move(pre_predicates);
}

void QueryToOperatorTransformer::Visit(parser::JoinDefinition *node, parser::ParseResult *parse_result) {
  // Get left operator
  node->GetLeftTable()->Accept(this, parse_result);
  auto left_expr = output_expr_;

  // Get right operator
  node->GetRightTable()->Accept(this, parse_result);
  auto right_expr = output_expr_;

  // Construct join operator
  OperatorExpression *join_expr;
  switch (node->GetJoinType()) {
    case parser::JoinType::INNER: {
      CollectPredicates(node->GetJoinCondition(), parse_result, &predicates_);
      join_expr = new OperatorExpression(LogicalInnerJoin::Make(), {left_expr, right_expr});
      break;
    }
    case parser::JoinType::OUTER: {
      join_expr = new OperatorExpression(LogicalOuterJoin::Make(common::ManagedPointer(node->GetJoinCondition())),
                                         {left_expr, right_expr});
      break;
    }
    case parser::JoinType::LEFT: {
      join_expr = new OperatorExpression(LogicalLeftJoin::Make(common::ManagedPointer(node->GetJoinCondition())),
                                         {left_expr, right_expr});
      break;
    }
    case parser::JoinType::RIGHT: {
      join_expr = new OperatorExpression(LogicalRightJoin::Make(common::ManagedPointer(node->GetJoinCondition())),
                                         {left_expr, right_expr});
      break;
    }
    case parser::JoinType::SEMI: {
      join_expr = new OperatorExpression(LogicalSemiJoin::Make(common::ManagedPointer(node->GetJoinCondition())),
                                         {left_expr, right_expr});
      break;
    }
    default:
      throw OPTIMIZER_EXCEPTION("Join type invalid");
  }

  output_expr_ = join_expr;
}

void QueryToOperatorTransformer::Visit(parser::TableRef *node, parser::ParseResult *parse_result) {
  if (node->GetSelect() != nullptr) {
    // Store previous context

    // Construct query derived table predicates
    // i.e. the mapping from column name to the underlying expression in the sub-query.
    // This is needed to generate input/output information for subqueries
    auto table_alias = node->GetAlias();
    std::transform(table_alias.begin(), table_alias.end(), table_alias.begin(), ::tolower);

    auto alias_to_expr_map = ConstructSelectElementMap(node->GetSelect()->GetSelectColumns());

    node->GetSelect()->Accept(this, parse_result);

    auto child_expr = output_expr_;
    output_expr_ =
        new OperatorExpression(LogicalQueryDerivedGet::Make(table_alias, std::move(alias_to_expr_map)), {child_expr});
  } else if (node->GetJoin() != nullptr) {
    // Explicit Join
    node->GetJoin()->Accept(this, parse_result);
  } else if (node->GetList().size() > 1) {
    // Multiple tables (Implicit Join)
    // Create a join operator between the first two tables
    node->GetList().at(0)->Accept(this, parse_result);
    auto prev_expr = output_expr_;
    // Build a left deep join tree
    for (auto &list_elem : node->GetList()) {
      list_elem->Accept(this, parse_result);
      auto join_expr = new OperatorExpression(LogicalInnerJoin::Make(), {prev_expr, output_expr_});
      TERRIER_ASSERT(join_expr->GetChildren().size() == 2, "The join expr should have exactly 2 elements");
      prev_expr = join_expr;
    }
    output_expr_ = prev_expr;
  } else {
    // Single table
    if (node->GetList().size() == 1) node = node->GetList().at(0).Get();

    // TODO(Ling): how should we determine the value of `is_for_update` field of logicalGet constructor?
    output_expr_ = new OperatorExpression(
        LogicalGet::Make(accessor_->GetDatabaseOid(node->GetDatabaseName()), accessor_->GetDefaultNamespace(),
                         accessor_->GetTableOid(node->GetTableName()), {}, node->GetAlias(), false),
        {});
  }
}

void QueryToOperatorTransformer::Visit(parser::GroupByDescription *node, parser::ParseResult *parse_result) {}
void QueryToOperatorTransformer::Visit(parser::OrderByDescription *node, parser::ParseResult *parse_result) {}
void QueryToOperatorTransformer::Visit(parser::LimitDescription *node, parser::ParseResult *parse_result) {}
void QueryToOperatorTransformer::Visit(UNUSED_ATTRIBUTE parser::CreateFunctionStatement *op,
                                       parser::ParseResult *parse_result) {}

void QueryToOperatorTransformer::Visit(UNUSED_ATTRIBUTE parser::CreateStatement *op,
                                       UNUSED_ATTRIBUTE parser::ParseResult *parse_result) {}
void QueryToOperatorTransformer::Visit(parser::InsertStatement *op, parser::ParseResult *parse_result) {
  auto target_table = op->GetInsertionTable();
  auto target_table_id = accessor_->GetTableOid(target_table->GetTableName());
  auto target_db_id = accessor_->GetDatabaseOid(target_table->GetDatabaseName());
  auto target_ns_id = accessor_->GetDefaultNamespace();

  if (op->GetInsertType() == parser::InsertType::SELECT) {
    auto insert_expr =
        new OperatorExpression(LogicalInsertSelect::Make(target_db_id, target_ns_id, target_table_id), {});
    op->GetSelect()->Accept(this, parse_result);
    insert_expr->PushChild(output_expr_);
    output_expr_ = insert_expr;
    return;
  }

  // column_objects represents the columns for the current table as defined in its schema
  auto column_objects = accessor_->GetSchema(target_table_id).GetColumns();

  // vector of column oids
  std::vector<catalog::col_oid_t> col_ids;

  // INSERT INTO table_name VALUES (val1, val2, ...), (val_a, val_b, ...), ...
  if (op->GetInsertColumns()->empty()) {
    for (const auto &values : *(op->GetValues())) {
      if (values.size() > column_objects.size()) {
        throw CATALOG_EXCEPTION("ERROR:  INSERT has more expressions than target columns");
      }
      if (values.size() < column_objects.size()) {
        for (auto i = values.size(); i != column_objects.size(); ++i) {
          // check whether null values or default values can be used in the rest of the columns
          if (!column_objects[i].Nullable() && column_objects[i].StoredExpression() == nullptr) {
            throw CATALOG_EXCEPTION(
                ("ERROR:  null value in column \"" + column_objects[i].Name() + "\" violates not-null constraint")
                    .c_str());
          }
        }
      }
    }
    for (const auto &col : column_objects) col_ids.push_back(col.Oid());

  } else {
    // INSERT INTO table_name (col1, col2, ...) VALUES (val1, val2, ...), ...
    auto num_columns = op->GetInsertColumns()->size();
    for (const auto &tuple : *(op->GetValues())) {  // check size of each tuple
      if (tuple.size() > num_columns) {
        throw CATALOG_EXCEPTION("ERROR:  INSERT has more expressions than target columns");
      }
      if (tuple.size() < num_columns) {
        throw CATALOG_EXCEPTION("ERROR:  INSERT has more target columns than expressions");
      }
    }

    // set below contains names of columns mentioned in the insert statement
    std::unordered_set<catalog::col_oid_t> specified;
    auto schema = accessor_->GetSchema(target_table_id);

    for (const auto &col : *(op->GetInsertColumns())) {
      try {
        const auto &column_object = schema.GetColumn(col);
        specified.insert(column_object.Oid());
      } catch (const std::out_of_range &oor) {
        throw CATALOG_EXCEPTION(
            ("ERROR:  column \"" + col + "\" of relation \"" + target_table->GetTableName() + "\" does not exist")
                .c_str());
      }
    }

    for (const auto &column : schema.GetColumns()) {
      // this loop checks not null constraint for unspecified columns
      if (specified.find(column.Oid()) == specified.end() && !column.Nullable() &&
          column.StoredExpression() == nullptr) {
        // TODO(peloton): Add check for default value's existence for the current column
        throw CATALOG_EXCEPTION(
            ("ERROR: null value in column \"" + column.Name() + "\" violates not-null constraint").c_str());
      }
    }

    col_ids.insert(col_ids.end(), specified.begin(), specified.end());
  }

  auto insert_expr = new OperatorExpression(
      LogicalInsert::Make(target_db_id, target_ns_id, target_table_id, std::move(col_ids), op->GetValues()), {});
  output_expr_ = insert_expr;
}

void QueryToOperatorTransformer::Visit(parser::DeleteStatement *op, parser::ParseResult *parse_result) {
  auto target_table = op->GetDeletionTable();
  auto target_db_id = accessor_->GetDatabaseOid(target_table->GetDatabaseName());
  auto target_table_id = accessor_->GetTableOid(target_table->GetTableName());
  auto target_ns_id = accessor_->GetDefaultNamespace();
  auto target_table_alias = target_table->GetAlias();

  auto delete_expr = new OperatorExpression(LogicalDelete::Make(target_db_id, target_ns_id, target_table_id), {});

  OperatorExpression *table_scan;
  if (op->GetDeleteCondition() != nullptr) {
    std::vector<AnnotatedExpression> predicates;
    QueryToOperatorTransformer::ExtractPredicates(op->GetDeleteCondition(), &predicates);
    table_scan = new OperatorExpression(
        LogicalGet::Make(target_db_id, target_ns_id, target_table_id, predicates, target_table_alias, true), {});
  } else {
    table_scan = new OperatorExpression(
        LogicalGet::Make(target_db_id, target_ns_id, target_table_id, {}, target_table_alias, true), {});
  }
  delete_expr->PushChild(table_scan);

  output_expr_ = delete_expr;
}

void QueryToOperatorTransformer::Visit(UNUSED_ATTRIBUTE parser::DropStatement *op, parser::ParseResult *parse_result) {}
void QueryToOperatorTransformer::Visit(UNUSED_ATTRIBUTE parser::PrepareStatement *op,
                                       UNUSED_ATTRIBUTE parser::ParseResult *parse_result) {}
void QueryToOperatorTransformer::Visit(UNUSED_ATTRIBUTE parser::ExecuteStatement *op,
                                       UNUSED_ATTRIBUTE parser::ParseResult *parse_result) {}
void QueryToOperatorTransformer::Visit(UNUSED_ATTRIBUTE parser::TransactionStatement *op,
                                       UNUSED_ATTRIBUTE parser::ParseResult *parse_result) {}

void QueryToOperatorTransformer::Visit(parser::UpdateStatement *op, parser::ParseResult *parse_result) {
  auto target_table = op->GetUpdateTable();
  auto target_db_id = accessor_->GetDatabaseOid(target_table->GetDatabaseName());
  auto target_table_id = accessor_->GetTableOid(target_table->GetTableName());
  auto target_ns_id = accessor_->GetDefaultNamespace();
  auto target_table_alias = target_table->GetAlias();

  OperatorExpression *table_scan;

  auto update_expr = new OperatorExpression(
      LogicalUpdate::Make(target_db_id, target_ns_id, target_table_alias, target_table_id, op->GetUpdateClauses()), {});

  if (op->GetUpdateCondition() != nullptr) {
    std::vector<AnnotatedExpression> predicates;
    QueryToOperatorTransformer::ExtractPredicates(op->GetUpdateCondition(), &predicates);
    table_scan = new OperatorExpression(
        LogicalGet::Make(target_db_id, target_ns_id, target_table_id, predicates, target_table_alias, true), {});
  } else {
    table_scan = new OperatorExpression(
        LogicalGet::Make(target_db_id, target_ns_id, target_table_id, {}, target_table_alias, true), {});
  }
  update_expr->PushChild(table_scan);

  output_expr_ = update_expr;
}

void QueryToOperatorTransformer::Visit(parser::CopyStatement *op, parser::ParseResult *parse_result) {
  if (op->IsFrom()) {
    // The copy statement is reading from a file into a table. We construct a
    // logical external-file get operator as the leaf, and an insert operator
    // as the root.

    // TODO(Ling): filename? copy statement only has file path
    auto get_op = new OperatorExpression(
        LogicalExternalFileGet::Make(op->GetExternalFileFormat(), op->GetFilePath(), op->GetDelimiter(),
                                     op->GetQuoteChar(), op->GetEscapeChar()),
        {});

    auto target_table = op->GetCopyTable();

    auto insert_op =
        new OperatorExpression(LogicalInsertSelect::Make(accessor_->GetDatabaseOid(target_table->GetDatabaseName()),
                                                         accessor_->GetDefaultNamespace(),
                                                         accessor_->GetTableOid(target_table->GetTableName())),
                               {get_op});

    output_expr_ = insert_op;

  } else {
    if (op->GetSelectStatement() != nullptr) {
      op->GetSelectStatement()->Accept(this, parse_result);
    } else {
      op->GetCopyTable()->Accept(this, parse_result);
    }
    auto export_op = new OperatorExpression(
        LogicalExportExternalFile::Make(op->GetExternalFileFormat(), op->GetFilePath(), op->GetDelimiter(),
                                        op->GetQuoteChar(), op->GetEscapeChar()),
        {output_expr_});

    output_expr_ = export_op;
  }
}

void QueryToOperatorTransformer::Visit(UNUSED_ATTRIBUTE parser::AnalyzeStatement *op,
                                       parser::ParseResult *parse_result) {}

void QueryToOperatorTransformer::Visit(parser::ComparisonExpression *expr, parser::ParseResult *parse_result) {
  auto expr_type = expr->GetExpressionType();
  if (expr->GetExpressionType() == parser::ExpressionType::COMPARE_IN) {
    if (GenerateSubqueryTree(expr, 1, parse_result, false)) {
      // TODO(boweic): Should use IN to preserve the semantic, for now we do not
      //  have semi-join so use = to transform into inner join
      // TODO(Ling): now we have semi-join operators. Are we supporting it?
      expr->SetExpressionType(parser::ExpressionType::COMPARE_EQUAL);
    }

  } else if (expr_type == parser::ExpressionType::COMPARE_EQUAL ||
             expr_type == parser::ExpressionType::COMPARE_GREATER_THAN ||
             expr_type == parser::ExpressionType::COMPARE_GREATER_THAN_OR_EQUAL_TO ||
             expr_type == parser::ExpressionType::COMPARE_LESS_THAN ||
             expr_type == parser::ExpressionType::COMPARE_LESS_THAN_OR_EQUAL_TO) {
    if (expr->GetChild(0)->GetExpressionType() == parser::ExpressionType::ROW_SUBQUERY &&
        expr->GetChild(1)->GetExpressionType() == parser::ExpressionType::ROW_SUBQUERY) {
      throw NOT_IMPLEMENTED_EXCEPTION("Do not support comparison between sub-select");
    }
    // Transform if either child is sub-query
    GenerateSubqueryTree(expr, 0, parse_result, true) || GenerateSubqueryTree(expr, 1, parse_result, true);
  }
  expr->AcceptChildren(this, parse_result);
}

void QueryToOperatorTransformer::Visit(parser::OperatorExpression *expr, parser::ParseResult *parse_result) {
  // TODO(boweic): We may want to do the rewrite (exist -> in) in the binder
  if (expr->GetExpressionType() == parser::ExpressionType::OPERATOR_EXISTS) {
    if (GenerateSubqueryTree(expr, 0, parse_result, false)) {
      // Already reset the child to column, we need to transform exist to not-null to preserve semantic
      expr->SetExpressionType(parser::ExpressionType::OPERATOR_IS_NOT_NULL);
    }
  }

  expr->AcceptChildren(this, parse_result);
}

bool QueryToOperatorTransformer::RequireAggregation(common::ManagedPointer<parser::SelectStatement> op) {
  if (op->GetSelectGroupBy() != nullptr) {
    return true;
  }
  // Check plain aggregation
  bool has_aggregation = false;
  bool has_other_exprs = false;

  for (auto &expr : op->GetSelectColumns()) {
    std::vector<common::ManagedPointer<parser::AggregateExpression>> aggr_exprs;
    // we need to use get method of managed pointer because the function we are calling will recursivly get aggreate
    // expressions from the current expression and its children; children are of unique pointers
    parser::ExpressionUtil::GetAggregateExprs(&aggr_exprs, expr);
    if (!aggr_exprs.empty())
      has_aggregation = true;
    else
      has_other_exprs = true;
  }
  // TODO(peloton): Should be handled in the binder
  // Syntax error when there are mixture of aggregation and other exprs when group by is absent
  if (has_aggregation && has_other_exprs) {
    throw OPTIMIZER_EXCEPTION(
        "Non aggregation expression must appear in the GROUP BY clause or be used in an aggregate function");
  }
  return has_aggregation;
}

void QueryToOperatorTransformer::CollectPredicates(common::ManagedPointer<parser::AbstractExpression> expr,
                                                   parser::ParseResult *parse_result,
                                                   std::vector<AnnotatedExpression> *predicates) {
  // First check if all conjunctive predicates are supported before transforming
  // predicate with sub-select into regular predicates
  std::vector<common::ManagedPointer<parser::AbstractExpression>> predicate_ptrs;
  QueryToOperatorTransformer::SplitPredicates(expr, &predicate_ptrs);
  for (const auto &pred : predicate_ptrs) {
    if (!QueryToOperatorTransformer::IsSupportedConjunctivePredicate(pred)) {
      throw NOT_IMPLEMENTED_EXCEPTION("Predicate type not supported yet");
    }
  }
  // Accept will change the expression, e.g. (a in (select b from test)) into
  // (a IN test.b), after the rewrite, we can extract the table aliases
  // information correctly
  expr->Accept(this, parse_result);
  QueryToOperatorTransformer::ExtractPredicates(expr, predicates);
}

bool QueryToOperatorTransformer::IsSupportedConjunctivePredicate(
    common::ManagedPointer<parser::AbstractExpression> expr) {
  // Currently support : 1. expr without subquery
  // 2. subquery without disjunction. Since the expr is already one of the
  // conjunctive exprs, we'll only need to check if the root level is an
  // operator with subquery
  if (!expr->HasSubquery()) {
    return true;
  }
  auto expr_type = expr->GetExpressionType();
  // Subquery with IN
  if (expr_type == parser::ExpressionType::COMPARE_IN &&
      expr->GetChild(0)->GetExpressionType() != parser::ExpressionType::ROW_SUBQUERY &&
      expr->GetChild(1)->GetExpressionType() == parser::ExpressionType::ROW_SUBQUERY) {
    return true;
  }
  // Subquery with EXIST
  if (expr_type == parser::ExpressionType::OPERATOR_EXISTS &&
      expr->GetChild(0)->GetExpressionType() == parser::ExpressionType::ROW_SUBQUERY) {
    return true;
  }
  // Subquery with other operator
  if (expr_type == parser::ExpressionType::COMPARE_EQUAL || expr_type == parser::ExpressionType::COMPARE_GREATER_THAN ||
      expr_type == parser::ExpressionType::COMPARE_GREATER_THAN_OR_EQUAL_TO ||
      expr_type == parser::ExpressionType::COMPARE_LESS_THAN ||
      expr_type == parser::ExpressionType::COMPARE_LESS_THAN_OR_EQUAL_TO) {
    // Supported if one child is subquery and the other is not
    if ((!expr->GetChild(0)->HasSubquery() &&
         expr->GetChild(1)->GetExpressionType() == parser::ExpressionType::ROW_SUBQUERY) ||
        (!expr->GetChild(1)->HasSubquery() &&
         expr->GetChild(0)->GetExpressionType() == parser::ExpressionType::ROW_SUBQUERY)) {
      return true;
    }
  }
  return false;
}

bool QueryToOperatorTransformer::IsSupportedSubSelect(common::ManagedPointer<parser::SelectStatement> op) {
  // Supported if 1. No aggregation. 2. With aggregation and WHERE clause only
  // have correlated columns in conjunctive predicates in the form of
  // "outer_relation.a = ..."
  // TODO(boweic): Add support for arbitary expressions, this would require
  //  the support for mark join & some special operators, see Hyper's unnesting arbitary query slides
  if (!QueryToOperatorTransformer::RequireAggregation(op)) return true;

  std::vector<common::ManagedPointer<parser::AbstractExpression>> predicates;
  QueryToOperatorTransformer::SplitPredicates(op->GetSelectCondition(), &predicates);
  for (const auto &pred : predicates) {
    // Depth is used to detect correlated subquery, it is set in the binder,
    // if a TVE has depth less than the depth of the current operator, then it is correlated predicate
    if (pred->GetDepth() < op->GetDepth()) {
      if (pred->GetExpressionType() != parser::ExpressionType::COMPARE_EQUAL) {
        return false;
      }
      // Check if in the form of "outer_relation.a = (expr with only columns in inner relation)"
      if (!((pred->GetChild(1)->GetDepth() == op->GetDepth() &&
             pred->GetChild(0)->GetExpressionType() == parser::ExpressionType::COLUMN_VALUE) ||
            (pred->GetChild(0)->GetDepth() == op->GetDepth() &&
             pred->GetChild(1)->GetExpressionType() == parser::ExpressionType::COLUMN_VALUE))) {
        return false;
      }
    }
  }
  return true;
}

bool QueryToOperatorTransformer::GenerateSubqueryTree(parser::AbstractExpression *expr, int child_id,
                                                      parser::ParseResult *parse_result, bool single_join) {
  // TODO(Ling): See if we could or should move this function, which expands subquery to list of columns, in binder
  // Get potential subquery
  auto subquery_expr = expr->GetChild(child_id);
  if (subquery_expr->GetExpressionType() != parser::ExpressionType::ROW_SUBQUERY) return false;

  auto sub_select = subquery_expr.CastManagedPointerTo<parser::SubqueryExpression>()->GetSubselect();
  if (!QueryToOperatorTransformer::IsSupportedSubSelect(sub_select))
    throw NOT_IMPLEMENTED_EXCEPTION("Sub-select not supported");
  // We only support subselect with single row
  if (sub_select->GetSelectColumns().size() != 1) throw NOT_IMPLEMENTED_EXCEPTION("Array in predicates not supported");

  std::vector<parser::AbstractExpression *> select_list;
  // Construct join
  OperatorExpression *op_expr;
  if (single_join) {
    op_expr = new OperatorExpression(LogicalSingleJoin::Make(), {output_expr_});
  } else {
    op_expr = new OperatorExpression(LogicalMarkJoin::Make(), {output_expr_});
  }

  sub_select->Accept(this, parse_result);

  // Push subquery output
  op_expr->PushChild(output_expr_);

  output_expr_ = op_expr;

  // Convert subquery to the selected column in the sub-select
  expr->SetChild(child_id, sub_select->GetSelectColumns().at(0));
  return true;
}

void QueryToOperatorTransformer::ExtractPredicates(common::ManagedPointer<parser::AbstractExpression> expr,
                                                   std::vector<AnnotatedExpression> *annotated_predicates) {
  // Split a complex predicate into a set of predicates connected by AND.
  std::vector<common::ManagedPointer<parser::AbstractExpression>> predicates;
  QueryToOperatorTransformer::SplitPredicates(expr, &predicates);

  for (auto predicate : predicates) {
    std::unordered_set<std::string> table_alias_set;
    QueryToOperatorTransformer::GenerateTableAliasSet(predicate, &table_alias_set);

    // Deep copy expression to avoid memory leak
    annotated_predicates->push_back(AnnotatedExpression(predicate, std::move(table_alias_set)));
  }
}

void QueryToOperatorTransformer::GenerateTableAliasSet(const common::ManagedPointer<parser::AbstractExpression> expr,
                                                       std::unordered_set<std::string> *table_alias_set) {
  if (expr->GetExpressionType() == parser::ExpressionType::COLUMN_VALUE) {
    table_alias_set->insert(expr.CastManagedPointerTo<const parser::ColumnValueExpression>()->GetTableName());
  } else {
    for (const auto &child : expr->GetChildren())
      QueryToOperatorTransformer::GenerateTableAliasSet(child, table_alias_set);
  }
}

void QueryToOperatorTransformer::SplitPredicates(
    common::ManagedPointer<parser::AbstractExpression> expr,
    std::vector<common::ManagedPointer<parser::AbstractExpression>> *predicates) {
  if (expr == nullptr) {
    return;
  }

  if (expr->GetExpressionType() == parser::ExpressionType::CONJUNCTION_AND) {
    // Traverse down the expression tree along conjunction
    for (const auto &child : expr->GetChildren()) {
      QueryToOperatorTransformer::SplitPredicates(common::ManagedPointer(child), predicates);
    }
  } else {
    // Find an expression that is the child of conjunction expression
    predicates->push_back(expr);
  }
}

std::unordered_map<std::string, common::ManagedPointer<parser::AbstractExpression>>
QueryToOperatorTransformer::ConstructSelectElementMap(
    const std::vector<common::ManagedPointer<parser::AbstractExpression>> &select_list) {
  std::unordered_map<std::string, common::ManagedPointer<parser::AbstractExpression>> res;
  for (auto &expr : select_list) {
    std::string alias;
    if (!expr->GetAlias().empty()) {
      alias = expr->GetAlias();
    } else if (expr->GetExpressionType() == parser::ExpressionType::COLUMN_VALUE) {
      auto tv_expr = expr.CastManagedPointerTo<parser::ColumnValueExpression>();
      alias = tv_expr->GetColumnName();
    } else {
      continue;
    }
    std::transform(alias.begin(), alias.end(), alias.begin(), ::tolower);
    res[alias] = common::ManagedPointer<parser::AbstractExpression>(expr);
  }
  return res;
}

}  // namespace terrier::optimizer
