#include <iostream>
#include <thread>
#include <chrono>

#include <arrow/api.h>
#include <arrow/compute/exec/expression.h>
#include <arrow/dataset/api.h>
#include <arrow/filesystem/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/util/checked_cast.h>
#include <arrow/util/iterator.h>
#include "arrow/array/array_base.h"
#include "arrow/array/array_nested.h"
#include "arrow/array/data.h"
#include "arrow/array/util.h"
#include "arrow/testing/random.h"
#include "arrow/util/key_value_metadata.h"

#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <thallium.hpp>

#include "payload.h"


class MeasureExecutionTime{
  private:
      const std::chrono::steady_clock::time_point begin;
      const std::string caller;
  public:
      MeasureExecutionTime(const std::string& caller):caller(caller),begin(std::chrono::steady_clock::now()){}
      ~MeasureExecutionTime(){
          const auto duration=std::chrono::steady_clock::now()-begin;
          std::cout << (double)std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()/1000<<std::endl;
      }
};


#ifndef MEASURE_FUNCTION_EXECUTION_TIME
#define MEASURE_FUNCTION_EXECUTION_TIME const MeasureExecutionTime measureExecutionTime(__FUNCTION__);
#endif


namespace tl = thallium;
namespace cp = arrow::compute;


arrow::Result<ScanReq> GetScanRequest(std::string path,
                                      cp::Expression filter, 
                                      std::shared_ptr<arrow::Schema> projection_schema,
                                      std::shared_ptr<arrow::Schema> dataset_schema) {
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> filter_buff, arrow::compute::Serialize(filter));
    ARROW_ASSIGN_OR_RAISE(auto projection_schema_buff, arrow::ipc::SerializeSchema(*projection_schema));
    ARROW_ASSIGN_OR_RAISE(auto dataset_schema_buff, arrow::ipc::SerializeSchema(*dataset_schema));
    ScanReqRPCStub stub(
        path,
        const_cast<uint8_t*>(filter_buff->data()), filter_buff->size(), 
        const_cast<uint8_t*>(dataset_schema_buff->data()), dataset_schema_buff->size(),
        const_cast<uint8_t*>(projection_schema_buff->data()), projection_schema_buff->size()
    );
    ScanReq req;
    req.stub = stub;
    req.schema = projection_schema;
    return req;
}

ConnCtx Init(std::string host) {
    ConnCtx ctx;
    tl::engine engine("verbs://ibp130s0", THALLIUM_SERVER_MODE, true);
    tl::endpoint endpoint = engine.lookup(host);
    ctx.engine = engine;
    ctx.endpoint = endpoint;
    tl::remote_procedure clear = engine.define("clear");
    clear.on(endpoint)();
    return ctx;
}

void Scan(ConnCtx &conn_ctx, ScanReq &scan_req) {
    tl::remote_procedure scan = conn_ctx.engine.define("scan");
    scan.disable_response();
    ScanCtx scan_ctx;
    scan.on(conn_ctx.endpoint)(scan_req.stub);
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> GetNextBatch(ConnCtx &conn_ctx, std::shared_ptr<arrow::Schema> schema, bool last) {
    std::shared_ptr<arrow::RecordBatch> batch;
    std::function<void(const tl::request&, int64_t&, std::vector<int64_t>&, std::vector<int64_t>&, tl::bulk&)> f =
        [&conn_ctx, &schema, &batch, &last](const tl::request& req, int64_t& num_rows, std::vector<int64_t>& data_buff_sizes, std::vector<int64_t>& offset_buff_sizes, tl::bulk& b) {
            int num_cols = schema->num_fields();
            
            std::vector<std::shared_ptr<arrow::Array>> columns;
            std::vector<std::unique_ptr<arrow::Buffer>> data_buffs(num_cols);
            std::vector<std::unique_ptr<arrow::Buffer>> offset_buffs(num_cols);
            std::vector<std::pair<void*,std::size_t>> segments(num_cols*2);
            
            for (int64_t i = 0; i < num_cols; i++) {
                data_buffs[i] = arrow::AllocateBuffer(data_buff_sizes[i]).ValueOrDie();
                offset_buffs[i] = arrow::AllocateBuffer(offset_buff_sizes[i]).ValueOrDie();

                segments[i*2].first = (void*)data_buffs[i]->mutable_data();
                segments[i*2].second = data_buff_sizes[i];

                segments[(i*2)+1].first = (void*)offset_buffs[i]->mutable_data();
                segments[(i*2)+1].second = offset_buff_sizes[i];
            }

            tl::bulk local = conn_ctx.engine.expose(segments, tl::bulk_mode::write_only);
            b.on(req.get_endpoint()) >> local;

            for (int64_t i = 0; i < num_cols; i++) {
                std::shared_ptr<arrow::DataType> type = schema->field(i)->type();  
                if (is_binary_like(type->id())) {
                    std::shared_ptr<arrow::Array> col_arr = std::make_shared<arrow::StringArray>(num_rows, std::move(offset_buffs[i]), std::move(data_buffs[i]));
                    columns.push_back(col_arr);
                } else {
                    std::shared_ptr<arrow::Array> col_arr = std::make_shared<arrow::PrimitiveArray>(type, num_rows, std::move(data_buffs[i]));
                    columns.push_back(col_arr);
                }
            }

            batch = arrow::RecordBatch::Make(schema, num_rows, columns);
            return req.respond(0);
        };
    conn_ctx.engine.define("do_rdma", f);
    tl::remote_procedure get_next_batch = conn_ctx.engine.define("get_next_batch");

    int e = get_next_batch.on(conn_ctx.endpoint)(last);

    if (e == 0) {
        return batch;
    } else {
        return nullptr;
    }
}

arrow::Status Main(char **argv) {
    // connection info
    std::string uri_base = "ofi+verbs;ofi_rxm://10.0.2.50:";
    std::string uri = uri_base + argv[1];
    std::string selectivity = argv[2];

    // query params
    auto filter = 
        cp::greater(cp::field_ref("total_amount"), cp::literal(-200));
    if (selectivity == "10") {
        filter = cp::greater(cp::field_ref("total_amount"), cp::literal(27));
    } else if (selectivity == "1") {
        filter = cp::greater(cp::field_ref("total_amount"), cp::literal(69));
    }

    auto schema = arrow::schema({
        arrow::field("VendorID", arrow::int64()),
        arrow::field("tpep_pickup_datetime", arrow::timestamp(arrow::TimeUnit::MICRO)),
        arrow::field("tpep_dropoff_datetime", arrow::timestamp(arrow::TimeUnit::MICRO)),
        arrow::field("passenger_count", arrow::int64()),
        arrow::field("trip_distance", arrow::float64()),
        arrow::field("RatecodeID", arrow::int64()),
        arrow::field("store_and_fwd_flag", arrow::utf8()),
        arrow::field("PULocationID", arrow::int64()),
        arrow::field("DOLocationID", arrow::int64()),
        arrow::field("payment_type", arrow::int64()),
        arrow::field("fare_amount", arrow::float64()),
        arrow::field("extra", arrow::float64()),
        arrow::field("mta_tax", arrow::float64()),
        arrow::field("tip_amount", arrow::float64()),
        arrow::field("tolls_amount", arrow::float64()),
        arrow::field("improvement_surcharge", arrow::float64()),
        arrow::field("total_amount", arrow::float64())
    });

    // scan
    ConnCtx conn_ctx = Init(uri);
    int64_t total_rows = 0;
    std::shared_ptr<arrow::RecordBatch> batch;

    {
        MEASURE_FUNCTION_EXECUTION_TIME
        for (int i = 1; i <= 200; i++) {
            std::string filepath = "/mnt/cephfs/dataset/16MB.uncompressed.parquet." + std::to_string(i);
            std::cout << filepath << std::endl;
            ARROW_ASSIGN_OR_RAISE(auto scan_req, GetScanRequest(filepath, filter, schema, schema));
            Scan(conn_ctx, scan_req);
            bool last = (i == 200);
            while ((batch = GetNextBatch(conn_ctx, schema, last).ValueOrDie()) != nullptr) {
                total_rows += batch->num_rows();
                std::cout << batch->ToString() << std::endl;
            }
        }
    }
    
    std::cout << "Total rows read: " << total_rows << std::endl;
    conn_ctx.engine.finalize();

    return arrow::Status::OK();
}

int main(int argc, char** argv) {
    Main(argv);
}
