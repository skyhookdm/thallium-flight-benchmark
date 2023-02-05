#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>

#include <thallium.hpp>
namespace tl = thallium;


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


size_t GetNextBatch(ConnCtx &conn_ctx) {
    std::function<void(const tl::request&, tl::bulk&)> f =
        [&conn_ctx](const tl::request& req, tl::bulk& b) {
            
            std::vector<std::pair<void*,std::size_t>> segments(1);            
            {
                MeasureExecutionTime m("memory_allocate");
                segments[0].first = (uint8_t*)malloc(buff_size);
                segments[0].second = buff_size;
            }

            tl::bulk local;
            
            {
                MeasureExecutionTime m("client_expose");
                local = conn_ctx.engine.expose(segments, tl::bulk_mode::write_only);
            }

            {
                MeasureExecutionTime m("RDMA");
                b.on(req.get_endpoint()) >> local;
            }

            return req.respond(0);
        };
    conn_ctx.engine.define("do_rdma", f);
    tl::remote_procedure get_next = conn_ctx.engine.define("get_next");

    int e = get_next.on(conn_ctx.endpoint)();
    if (e == 0) {
        return buff_size;
    } else {
        return 0;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cout << "./tc [uri] [protocol]" << std::endl;
        exit(1);
    }

    std::string uri = argv[1];
    std::string protocol = argv[2];

    tl::engine engine(protocol, THALLIUM_SERVER_MODE, true);
    tl::endpoint endpoint = engine.lookup(uri);
    
    tl::remote_procedure scan = conn_ctx.engine.define("scan");
    int e = scan.on(conn_ctx.endpoint)();

    size_t bytes_read = 0;
    size_t total_bytes_read = 0;
    {
        MEASURE_FUNCTION_EXECUTION_TIME
        while ((bytes_read = GetNext(conn_ctx)) > 0) {
            total_bytes_read += bytes_read;
        }
    }

    conn_ctx.engine.finalize();
    return 0;
}
