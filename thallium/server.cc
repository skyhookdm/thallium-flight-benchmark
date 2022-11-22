#include <iostream>
#include <unordered_map>

#include <arrow/api.h>
#include <arrow/compute/exec/expression.h>
#include <arrow/dataset/api.h>
#include <arrow/dataset/plan.h>
#include <arrow/filesystem/api.h>
#include <arrow/io/api.h>
#include <arrow/util/checked_cast.h>
#include <arrow/util/iterator.h>

#include <arrow/array/array_base.h>
#include <arrow/array/array_nested.h>
#include <arrow/array/data.h>
#include <arrow/array/util.h>
#include <arrow/testing/random.h>
#include <arrow/util/key_value_metadata.h>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <thallium.hpp>
#include <bake-client.hpp>
#include <bake-server.hpp>

#include <yokan/cxx/server.hpp>
#include <yokan/cxx/admin.hpp>
#include <yokan/cxx/client.hpp>

#include "ace.h"

namespace tl = thallium;
namespace bk = bake;
namespace cp = arrow::compute;
namespace yk = yokan;

static char* read_input_file(const char* filename) {
    size_t ret;
    FILE*  fp = fopen(filename, "r");
    if (fp == NULL) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(-1);
    }
    fseek(fp, 0, SEEK_END);
    size_t sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* buf = (char*)calloc(1, sz + 1);
    ret       = fread(buf, 1, sz, fp);
    if (ret != sz && ferror(fp)) {
        free(buf);
        perror("read_input_file");
        buf = NULL;
    }
    fclose(fp);
    return buf;
}

int main(int argc, char** argv) {

    if (argc < 4) {
        std::cout << "./ts [selectivity] [backend] [protocol]" << std::endl;
        exit(1);
    }

    std::string selectivity = argv[1];
    std::string backend = argv[2];
    std::string protocol = argv[3];

    tl::engine engine(protocol, THALLIUM_SERVER_MODE, true);
    margo_instance_id mid = engine.get_margo_instance();
    hg_addr_t svr_addr;
    hg_return_t hret = margo_addr_self(mid, &svr_addr);
    if (hret != HG_SUCCESS) {
        std::cerr << "Error: margo_addr_lookup()\n";
        margo_finalize(mid);
        return -1;
    }

    char *bake_config = read_input_file("bake_config.json");
    bk::provider *bp = bk::provider::create(
        mid, 0, ABT_POOL_NULL, std::string(bake_config, strlen(bake_config) + 1), ABT_IO_INSTANCE_NULL, NULL, NULL
    );

    char *yokan_config = read_input_file("yokan_config.json");
    yk::Provider yp(mid, 0, "ABCD", yokan_config, ABT_POOL_NULL, nullptr);
    yk::Client ycl(mid);
    yk::Admin admin(mid);
    yk_database_id_t db_id = admin.openDatabase(svr_addr, 0, "ABCD", "rocksdb", yokan_config);
    yk::Database db(ycl.handle(), svr_addr, 0, db_id);

    tl::remote_procedure do_rdma = engine.define("do_rdma");

    std::unordered_map<std::string, std::shared_ptr<arrow::RecordBatchReader>> reader_map;
    bk::client bcl(mid);
    bk::provider_handle bph(bcl, svr_addr, 0);
    bph.set_eager_limit(0);
    bk::target tid = bp->list_targets()[0];

    std::vector<std::pair<void*,std::size_t>> segments(1);
    uint8_t *segment_buffer = (uint8_t*)malloc(32*1024*1024);
    segments[0].first = (void*)segment_buffer;
    segments[0].second = 32*1024*1024;
    tl::bulk arrow_bulk = engine.expose(segments, tl::bulk_mode::read_write);

    std::function<void(const tl::request&, const ScanReqRPCStub&)> scan = 
        [&reader_map, &mid, &svr_addr, &bp, &bcl, &bph, &tid, &db, &backend, &selectivity](const tl::request &req, const ScanReqRPCStub& stub) {
            arrow::dataset::internal::Initialize();
            std::shared_ptr<arrow::RecordBatchReader> reader;

            if (backend == "dataset" || backend == "dataset+mem") {
                cp::ExecContext exec_ctx;
                reader = ScanDataset(exec_ctx, stub, backend, selectivity).ValueOrDie();
            } else if (backend == "file" || backend == "file+mmap") {
                reader = ScanFile(stub, backend, selectivity).ValueOrDie();
            } else if (backend == "bake") {
                size_t value_size = 28;
                void *value_buf = malloc(28);
                
                db.get((void*)stub.path.c_str(), stub.path.length(), value_buf, &value_size);
                bk::region rid(std::string((char*)value_buf, value_size));

                uint8_t *ptr = (uint8_t*)bcl.get_data(bph, tid, rid);
                reader = ScanBake(stub, ptr, selectivity).ValueOrDie();
            }

            std::string uuid = boost::uuids::to_string(boost::uuids::random_generator()());
            reader_map[uuid] = reader;
            
            return req.respond(uuid);
        };

    int32_t total_rows_written = 0;
    std::function<void(const tl::request&, const std::string&)> get_next_batch = 
        [&mid, &svr_addr, &engine, &do_rdma, &reader_map, &total_rows_written, &segment_buffer, &segments, &arrow_bulk](const tl::request &req, const std::string& uuid) {
            std::shared_ptr<arrow::RecordBatchReader> reader = reader_map[uuid];
            std::shared_ptr<arrow::RecordBatch> batch;

            if (reader->ReadNext(&batch).ok() && batch != nullptr) {
                int32_t num_rows = batch->num_rows();
                total_rows_written += num_rows;

                int32_t curr_pos = 0;
                int32_t total_size = 0;

                std::vector<int32_t> data_offsets;
                std::vector<int32_t> data_sizes;

                std::vector<int32_t> off_offsets;
                std::vector<int32_t> off_sizes;

                std::string null_buff = "xx";

                for (int32_t i = 0; i < batch->num_columns(); i++) {
                    std::shared_ptr<arrow::Array> col_arr = batch->column(i);
                    arrow::Type::type type = col_arr->type_id();
                    int32_t null_count = col_arr->null_count();

                    if (is_binary_like(type)) {
                        std::shared_ptr<arrow::Buffer> data_buff = 
                            std::static_pointer_cast<arrow::BinaryArray>(col_arr)->value_data();
                        std::shared_ptr<arrow::Buffer> offset_buff = 
                            std::static_pointer_cast<arrow::BinaryArray>(col_arr)->value_offsets();

                        int32_t data_size = data_buff->size();
                        int32_t offset_size = offset_buff->size();

                        data_offsets.emplace_back(curr_pos);
                        data_sizes.emplace_back(data_size);                        
                        memcpy(segment_buffer + curr_pos, data_buff->data(), data_size);
                        curr_pos += data_size;

                        off_offsets.emplace_back(curr_pos);
                        off_sizes.emplace_back(offset_size);
                        memcpy(segment_buffer + curr_pos, offset_buff->data(), offset_size);
                        curr_pos += offset_size;

                        total_size += (data_size + offset_size);
                    } else {
                        std::shared_ptr<arrow::Buffer> data_buff = 
                            std::static_pointer_cast<arrow::PrimitiveArray>(col_arr)->values();

                        int32_t data_size = data_buff->size();
                        int32_t offset_size = null_buff.size() + 1; 

                        data_offsets.emplace_back(curr_pos);
                        data_sizes.emplace_back(data_size);
                        memcpy(segment_buffer + curr_pos, data_buff->data(), data_size);
                        curr_pos += data_size;

                        off_offsets.emplace_back(curr_pos);
                        off_sizes.emplace_back(offset_size);
                        memcpy(segment_buffer + curr_pos, (uint8_t*)null_buff.c_str(), offset_size);
                        curr_pos += offset_size;

                        total_size += (data_size + offset_size);
                    }
                }

                segments[0].second = total_size;

                do_rdma.on(req.get_endpoint())(num_rows, data_offsets, data_sizes, off_offsets, off_sizes, total_size, arrow_bulk);
                return req.respond(0);
            } else {
                reader_map.erase(uuid);
                return req.respond(1);
            }
        };
    
    engine.define("scan", scan);
    engine.define("get_next_batch", get_next_batch);

    std::cout << "Server running at address " << engine.self() << std::endl;    
    engine.wait_for_finalize();        
};
