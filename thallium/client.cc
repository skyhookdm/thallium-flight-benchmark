#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>

#include <arrow/api.h>
#include <arrow/compute/exec/expression.h>
#include <arrow/dataset/api.h>
#include <arrow/filesystem/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/util/checked_cast.h>
#include <arrow/util/iterator.h>

#include <arrow/array/array_base.h>
#include <arrow/array/array_nested.h>
#include <arrow/array/data.h>
#include <arrow/array/util.h>
#include <arrow/testing/random.h>
#include <arrow/util/key_value_metadata.h>

#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <thallium.hpp>

#include "payload.h"

class MeasureExecutionTime{
    private:
        const std::chrono::steady_clock::time_point begin;
        const std::string caller;
        std::ofstream log;
    public:
        MeasureExecutionTime(const std::string& caller):caller(caller),begin(std::chrono::steady_clock::now()) {
            log.open("result_client.txt", std::ios_base::app);
        }
        
        ~MeasureExecutionTime() {
            const auto duration=std::chrono::steady_clock::now()-begin;
            std::string s = caller + " : " + std::to_string((double)std::chrono::duration_cast<std::chrono::microseconds>(duration).count()/1000) + "\n";
            std::cout << s;
            log << s;
            log.close();
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

ConnCtx Init(std::string protocol, std::string host) {
    ConnCtx ctx;
    tl::engine engine(protocol, THALLIUM_SERVER_MODE, true);
    tl::endpoint endpoint = engine.lookup(host);
    ctx.engine = engine;
    ctx.endpoint = endpoint;
    return ctx;
}

ScanCtx Scan(ConnCtx &conn_ctx, ScanReq &scan_req) {
    tl::remote_procedure scan = conn_ctx.engine.define("scan");
    ScanCtx scan_ctx;
    std::string uuid = scan.on(conn_ctx.endpoint)(scan_req.stub);
    scan_ctx.uuid = uuid;
    scan_ctx.schema = scan_req.schema;
    return scan_ctx;
}

arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> GetNextBatch(ConnCtx &conn_ctx, ScanCtx &scan_ctx) {
    // store batches
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;

    std::function<void(const tl::request&, std::vector<int32_t>&, std::vector<int32_t>&, std::vector<int32_t>&, tl::bulk&)> f =
        [&conn_ctx, &scan_ctx, &batches](const tl::request& req, std::vector<int32_t>& batch_sizes, std::vector<int32_t>& data_buff_sizes, std::vector<int32_t>& offset_buff_sizes, tl::bulk& b) {
            int num_cols = scan_ctx.schema->num_fields();
            
            // std::vector<std::shared_ptr<arrow::Array>> columns;
            std::vector<std::unique_ptr<arrow::Buffer>> data_buffs(num_cols * batch_sizes.size());
            std::vector<std::unique_ptr<arrow::Buffer>> offset_buffs(num_cols * batch_sizes.size());
            std::vector<std::pair<void*, std::size_t>> segments;
            segments.reserve(num_cols * 2 * batch_sizes.size());

            // allocate buffers for recieving data from server            
            for (int32_t i = 0; i < num_cols * batch_sizes.size(); i++) {
                data_buffs[i] = arrow::AllocateBuffer(data_buff_sizes[i]).ValueOrDie();
                offset_buffs[i] = arrow::AllocateBuffer(offset_buff_sizes[i]).ValueOrDie();

                segments.emplace_back(std::make_pair(
                    (void*)data_buffs[i]->mutable_data(),
                    data_buff_sizes[i]
                ));
                segments.emplace_back(std::make_pair(
                    (void*)offset_buffs[i]->mutable_data(),
                    offset_buff_sizes[i]
                ));
            }

            // expose local memory and pull data from server
            tl::bulk local = conn_ctx.engine.expose(segments, tl::bulk_mode::write_only);
            b.on(req.get_endpoint()) >> local;

            // materialize arrow arrays
            for (int32_t batch_idx = 0; batch_idx < batch_sizes.size(); batch_idx++) {
                int32_t num_rows = batch_sizes[batch_idx];
                std::vector<std::shared_ptr<arrow::Array>> columns;
                for (int32_t col_idx = 0; col_idx < num_cols; col_idx++) {
                    std::shared_ptr<arrow::DataType> type = scan_ctx.schema->field(col_idx)->type();  
                    int32_t buff_idx = batch_idx * num_cols + col_idx;
                    if (is_binary_like(type->id())) {
                        std::shared_ptr<arrow::Array> col_arr = std::make_shared<arrow::StringArray>(num_rows, std::move(offset_buffs[buff_idx]), std::move(data_buffs[buff_idx]));
                        columns.push_back(col_arr);
                    } else {
                        std::shared_ptr<arrow::Array> col_arr = std::make_shared<arrow::PrimitiveArray>(type, num_rows, std::move(data_buffs[buff_idxi]));
                        columns.push_back(col_arr);
                    }
                }
                std::shared_ptr<arrow::RecordBatch> batch = arrow::RecordBatch::Make(scan_ctx.schema, num_rows, columns);
                batches.push_back(batch);
            }

            return req.respond(0);
        };
    conn_ctx.engine.define("do_rdma", f);
    tl::remote_procedure get_next_batch = conn_ctx.engine.define("get_next_batch");

    int e = get_next_batch.on(conn_ctx.endpoint)(scan_ctx.uuid);
    if (e == 0) {
        return batches;
    } else {
        return std::vector<std::shared_ptr<arrow::RecordBatch>>();
    }
}

arrow::Status Main(int argc, char **argv) {
    if (argc < 4) {
        std::cout << "./tc [uri] [backend] [protocol]" << std::endl;
        exit(1);
    }

    std::string uri = argv[1];
    std::string backend = argv[2];
    std::string protocol = argv[3];

    auto filter = 
        cp::greater(cp::field_ref("total_amount"), cp::literal(-200));

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

    ConnCtx conn_ctx = Init(protocol, uri);
    int64_t total_rows = 0;

    std::string path = "/mnt/cephfs/dataset";
    ARROW_ASSIGN_OR_RAISE(auto scan_req, GetScanRequest(path, filter, schema, schema));
    ScanCtx scan_ctx = Scan(conn_ctx, scan_req);
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    {
        MEASURE_FUNCTION_EXECUTION_TIME
        while ((batches = GetNextBatch(conn_ctx, scan_ctx).ValueOrDie()).size() != 0) {
            for (auto batch : batches) {
                total_rows += batch->num_rows();
            }
        }
    }
    std::cout << "Read " << total_rows << " rows" << std::endl;

    conn_ctx.engine.finalize();
    return arrow::Status::OK();
}

int main(int argc, char** argv) {
    Main(argc, argv);
}
