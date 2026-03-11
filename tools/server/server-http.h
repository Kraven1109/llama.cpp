#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

struct common_params;

// generator-like API for HTTP response generation
// this object response with one of the 2 modes:
// 1) normal response: `data` contains the full response body
// 2) streaming response: each call to next(output) generates the next chunk
//    when next(output) returns false, no more data after the current chunk
//    note: some chunks can be empty, in which case no data is sent for that chunk
struct server_http_res {
    std::string content_type = "application/json; charset=utf-8";
    int status = 200;
    std::string data;
    std::map<std::string, std::string> headers;

    // TODO: move this to a virtual function once we have proper polymorphism support
    std::function<bool(std::string &)> next = nullptr;
    bool is_stream() const {
        return next != nullptr;
    }

    virtual ~server_http_res() = default;
};

// unique pointer, used by set_chunked_content_provider
// httplib requires the stream provider to be stored in heap
using server_http_res_ptr = std::unique_ptr<server_http_res>;

struct server_http_req {
    std::map<std::string, std::string> params; // path_params + query_params
    std::map<std::string, std::string> headers; // used by MCP proxy
    std::string path;
    std::string query_string; // query parameters string (e.g. "action=save")
    std::string body;
    const std::function<bool()> & should_stop;

    std::string get_param(const std::string & key, const std::string & def = "") const {
        auto it = params.find(key);
        if (it != params.end()) {
            return it->second;
        }
        return def;
    }
};

struct server_http_context {
    class Impl;
    std::unique_ptr<Impl> pimpl;

    std::thread thread; // server thread
    std::atomic<bool> is_ready = false;

    std::string path_prefix;
    std::string hostname;
    int port;

    server_http_context();
    ~server_http_context();

    bool init(const common_params & params);
    bool start();
    void stop() const;

    // note: the handler should never throw exceptions
    using handler_t = std::function<server_http_res_ptr(const server_http_req & req)>;

    void get(const std::string & path, const handler_t & handler) const;
    void post(const std::string & path, const handler_t & handler) const;

    // Streaming multipart upload — files are streamed to disk (not buffered in memory)
    struct uploaded_file {
        std::string field_name;    // form field name (e.g. "video")
        std::string filename;      // original filename from client
        std::string content_type;  // MIME type
        std::string tmp_path;      // path to temp file on disk
        size_t      size = 0;      // bytes written
    };

    struct multipart_req {
        std::map<std::string, std::string> params;
        std::map<std::string, std::string> headers;
        std::map<std::string, std::string> fields;  // text form fields
        std::vector<uploaded_file> files;             // uploaded files (on disk)
        std::string path;
        std::string query_string;
        std::function<bool()> is_closed;
    };

    using multipart_handler_t = std::function<server_http_res_ptr(const multipart_req & req)>;

    // max_file_size: maximum bytes per file (0 = 2GB default)
    void post_multipart(const std::string & path, const multipart_handler_t & handler, size_t max_file_size = 0) const;

    // for debugging
    std::string listening_address;
};
