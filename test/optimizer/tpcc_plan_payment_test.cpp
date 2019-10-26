#include <memory>
#include <string>

#include "util/test_harness.h"
#include "util/tpcc/tpcc_plan_test.h"

namespace terrier {

struct TpccPlanPaymentTests : public TpccPlanTest {};

// NOLINTNEXTLINE
TEST_F(TpccPlanPaymentTests, UpdateWarehouse) {
  auto check = [](TpccPlanTest *test, catalog::table_oid_t tbl_oid, std::unique_ptr<planner::AbstractPlanNode> plan) {};

  std::string query = "UPDATE WAREHOUSE SET W_YTD = W_YTD + 1 WHERE W_ID = 2";
  OptimizeUpdate(query, tbl_warehouse_, check);
}

// NOLINTNEXTLINE
TEST_F(TpccPlanPaymentTests, GetWarehouse) {
  std::string query = "SELECT W_STREET_1, W_STREET_2, W_CITY, W_STATE, W_ZIP, W_NAME FROM WAREHOUSE WHERE W_ID=1";
  OptimizeQuery(query, tbl_warehouse_, TpccPlanTest::CheckIndexScan);
}

// NOLINTNEXTLINE
TEST_F(TpccPlanPaymentTests, UpdateDistrict) {
  auto check = [](TpccPlanTest *test, catalog::table_oid_t tbl_oid, std::unique_ptr<planner::AbstractPlanNode> plan) {};

  std::string query = "UPDATE DISTRICT SET D_YTD = D_YTD + 1 WHERE D_W_ID = 2 AND D_ID = 3";
  OptimizeUpdate(query, tbl_district_, check);
}

// NOLINTNEXTLINE
TEST_F(TpccPlanPaymentTests, GetDistrict) {
  std::string query =
      "SELECT D_STREET_1, D_STREET_2, D_CITY, D_STATE, D_ZIP, D_NAME FROM DISTRICT WHERE D_W_ID=1 AND D_ID=2";
  OptimizeQuery(query, tbl_district_, TpccPlanTest::CheckIndexScan);
}

// NOLINTNEXTLINE
TEST_F(TpccPlanPaymentTests, GetCustomer) {
  std::string query =
      "SELECT C_FIRST, C_MIDDLE, C_LAST, C_STREET_1, C_STREET_2, C_CITY, "
      "C_STATE, C_ZIP, C_PHONE, C_CREDIT, C_CREDIT_LIM, C_DISCOUNT, C_BALANCE, "
      "C_YTD_PAYMENT, C_PAYMENT_CNT, C_SINCE "
      "FROM CUSTOMER WHERE C_W_ID=1 AND C_D_ID=2 AND C_ID=3";
  OptimizeQuery(query, tbl_customer_, TpccPlanTest::CheckIndexScan);
}

// NOLINTNEXTLINE
TEST_F(TpccPlanPaymentTests, GetCustomerCData) {
  std::string query = "SELECT C_DATA FROM CUSTOMER WHERE C_W_ID=1 AND C_D_ID=2 AND C_ID=3";
  OptimizeQuery(query, tbl_customer_, TpccPlanTest::CheckIndexScan);
}

// NOLINTNEXTLINE
TEST_F(TpccPlanPaymentTests, UpdateCustomerBalance) {
  auto check = [](TpccPlanTest *test, catalog::table_oid_t tbl_oid, std::unique_ptr<planner::AbstractPlanNode> plan) {};

  std::string query =
      "UPDATE CUSTOMER SET C_BALANCE = 1, C_YTD_PAYMENT = 2,"
      "C_PAYMENT_CNT = 3, C_DATA = '4' WHERE C_W_ID = 1 AND C_D_ID = 2 AND C_ID = 3";
  OptimizeUpdate(query, tbl_customer_, check);
}

// NOLINTNEXTLINE
TEST_F(TpccPlanPaymentTests, InsertHistory) {
  std::string query =
      "INSERT INTO HISTORY "
      "(H_C_D_ID, H_C_W_ID, H_C_ID, H_D_ID, H_W_ID, H_DATE, H_AMOUNT, H_DATA) "
      "VALUES (1,2,3,4,5,0,7,'data')";
  OptimizeInsert(query, tbl_history_);
}

// NOLINTNEXTLINE
TEST_F(TpccPlanPaymentTests, CustomerByName) {
  std::string query =
      "SELECT C_FIRST, C_MIDDLE, C_ID, C_STREET_1, C_STREET_2, C_CITY, "
      "C_STATE, C_ZIP, C_PHONE, C_CREDIT, C_CREDIT_LIM, C_DISCOUNT, C_BALANCE, "
      "C_YTD_PAYMENT, C_PAYMENT_CNT, C_SINCE "
      "FROM CUSTOMER WHERE C_W_ID=1 AND C_D_ID=2 AND C_LAST='page' "
      "ORDER BY C_FIRST";
  OptimizeQuery(query, tbl_customer_, TpccPlanTest::CheckIndexScan);
}

}  // namespace terrier
