#include "http_client.h"
#include <iostream>
#include <fstream>
#include <cstring>

#ifdef HAS_CURL
#include <curl/curl.h>
#endif

namespace dev_sys {

struct HttpClient::Impl {
    std::string ca_cert;
    std::string client_cert;
    std::string client_key;
    bool tls_configured = false;

#ifdef HAS_CURL
    CURL* curl = nullptr;

    void ensure_curl() {
        if (!curl) {
            curl = curl_easy_init();
            if (curl) {
                // Defaults
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            }
        }
    }
#endif
};

// ============================================================
// libcurl write callback — append to std::string
// ============================================================
#ifdef HAS_CURL
namespace {
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t total = size * nmemb;
        auto* str = static_cast<std::string*>(userp);
        str->append(static_cast<char*>(contents), total);
        return total;
    }

    static size_t file_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t total = size * nmemb;
        auto* fp = static_cast<std::ofstream*>(userp);
        if (fp && fp->is_open()) {
            fp->write(static_cast<char*>(contents), total);
            return total;
        }
        return 0;
    }

    // Progress callback for download
    static int progress_callback(void* /*clientp*/,
                                  curl_off_t dltotal, curl_off_t dlnow,
                                  curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
        if (dltotal > 0) {
            int pct = static_cast<int>((dlnow * 100) / dltotal);
            // Log every 10%
            static int last_pct = -1;
            if (pct / 10 != last_pct / 10) {
                last_pct = pct;
                std::cout << "[HttpClient] Download progress: " << pct << "% ("
                          << dlnow << "/" << dltotal << " bytes)" << std::endl;
            }
        }
        return 0; // continue
    }
} // namespace
#endif

HttpClient::HttpClient()
    : impl_(std::make_unique<Impl>()) {
#ifdef HAS_CURL
    impl_->ensure_curl();
#endif
}

HttpClient::~HttpClient() {
#ifdef HAS_CURL
    if (impl_->curl) {
        curl_easy_cleanup(impl_->curl);
        impl_->curl = nullptr;
    }
#endif
}

StatusCode HttpClient::set_tls(const std::string& ca_cert_path,
                                const std::string& client_cert_path,
                                const std::string& client_key_path) {
    impl_->ca_cert     = ca_cert_path;
    impl_->client_cert = client_cert_path;
    impl_->client_key  = client_key_path;
    impl_->tls_configured = true;

#ifdef HAS_CURL
    impl_->ensure_curl();
    if (!impl_->curl) return StatusCode::ERROR;

    if (!ca_cert_path.empty()) {
        curl_easy_setopt(impl_->curl, CURLOPT_CAINFO, ca_cert_path.c_str());
    }
    if (!client_cert_path.empty()) {
        curl_easy_setopt(impl_->curl, CURLOPT_SSLCERT, client_cert_path.c_str());
    }
    if (!client_key_path.empty()) {
        curl_easy_setopt(impl_->curl, CURLOPT_SSLKEY, client_key_path.c_str());
    }
#endif
    return StatusCode::OK;
}

StatusCode HttpClient::get(const std::string& url,
                            std::string& response_body,
                            int timeout_sec) {
#ifdef HAS_CURL
    impl_->ensure_curl();
    if (!impl_->curl) return StatusCode::ERROR;

    response_body.clear();

    curl_easy_setopt(impl_->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(impl_->curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(impl_->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(impl_->curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(impl_->curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));

    // Headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(impl_->curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(impl_->curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        std::cerr << "[HttpClient] GET failed: " << curl_easy_strerror(res)
                  << " (url=" << url << ")" << std::endl;
        long http_code = 0;
        curl_easy_getinfo(impl_->curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 400) {
            return StatusCode::COMM_DISCONNECTED;
        }
        return StatusCode::COMM_TIMEOUT;
    }

    // Check HTTP status
    long http_code = 0;
    curl_easy_getinfo(impl_->curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 400) {
        std::cerr << "[HttpClient] GET returned HTTP " << http_code
                  << " (url=" << url << ")" << std::endl;
        return StatusCode::COMM_DISCONNECTED;
    }

    return StatusCode::OK;
#else
    std::cerr << "[HttpClient] WARNING: Built without libcurl. HTTP GET not available." << std::endl;
    return StatusCode::ERROR;
#endif
}

StatusCode HttpClient::post(const std::string& url,
                             const std::string& body,
                             const std::map<std::string, std::string>& headers,
                             std::string& response_body,
                             int timeout_sec) {
#ifdef HAS_CURL
    impl_->ensure_curl();
    if (!impl_->curl) return StatusCode::ERROR;

    response_body.clear();

    curl_easy_setopt(impl_->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(impl_->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(impl_->curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(impl_->curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(impl_->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(impl_->curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(impl_->curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));

    // Build header list
    struct curl_slist* header_list = nullptr;
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
    header_list = curl_slist_append(header_list, "Accept: application/json");
    for (const auto& [key, value] : headers) {
        std::string hdr = key + ": " + value;
        header_list = curl_slist_append(header_list, hdr.c_str());
    }
    curl_easy_setopt(impl_->curl, CURLOPT_HTTPHEADER, header_list);

    CURLcode res = curl_easy_perform(impl_->curl);
    curl_slist_free_all(header_list);

    if (res != CURLE_OK) {
        std::cerr << "[HttpClient] POST failed: " << curl_easy_strerror(res)
                  << " (url=" << url << ")" << std::endl;
        return StatusCode::COMM_TIMEOUT;
    }

    long http_code = 0;
    curl_easy_getinfo(impl_->curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 400) {
        std::cerr << "[HttpClient] POST returned HTTP " << http_code << std::endl;
        return StatusCode::COMM_DISCONNECTED;
    }

    return StatusCode::OK;
#else
    std::cerr << "[HttpClient] WARNING: Built without libcurl. HTTP POST not available." << std::endl;
    return StatusCode::ERROR;
#endif
}

StatusCode HttpClient::download_file(const std::string& url,
                                      const std::string& output_path,
                                      bool resume) {
#ifdef HAS_CURL
    impl_->ensure_curl();
    if (!impl_->curl) return StatusCode::ERROR;

    // Open output file
    std::ios::openmode mode = std::ios::out | std::ios::binary;
    if (resume) {
        mode |= std::ios::app; // append if resuming
    }
    std::ofstream outfile(output_path, mode);
    if (!outfile.is_open()) {
        std::cerr << "[HttpClient] Failed to open output file: " << output_path << std::endl;
        return StatusCode::STORAGE_WRITE_ERROR;
    }

    // Get current file size for resume
    curl_off_t resume_from = 0;
    if (resume) {
        outfile.seekp(0, std::ios::end);
        resume_from = static_cast<curl_off_t>(outfile.tellp());
        if (resume_from > 0) {
            std::cout << "[HttpClient] Resuming download from byte " << resume_from << std::endl;
        }
    }

    curl_easy_setopt(impl_->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(impl_->curl, CURLOPT_WRITEFUNCTION, file_write_callback);
    curl_easy_setopt(impl_->curl, CURLOPT_WRITEDATA, &outfile);
    curl_easy_setopt(impl_->curl, CURLOPT_TIMEOUT, 0L); // no timeout for large downloads
    curl_easy_setopt(impl_->curl, CURLOPT_CONNECTTIMEOUT, 30L);

    // Progress tracking
    curl_easy_setopt(impl_->curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(impl_->curl, CURLOPT_XFERINFOFUNCTION, progress_callback);

    // Resume support
    if (resume_from > 0) {
        curl_easy_setopt(impl_->curl, CURLOPT_RESUME_FROM, resume_from);
    }

    // Fail on HTTP errors
    curl_easy_setopt(impl_->curl, CURLOPT_FAILONERROR, 1L);

    // Follow redirects
    curl_easy_setopt(impl_->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(impl_->curl, CURLOPT_MAXREDIRS, 5L);

    CURLcode res = curl_easy_perform(impl_->curl);
    outfile.close();

    if (res != CURLE_OK) {
        std::cerr << "[HttpClient] Download failed: " << curl_easy_strerror(res)
                  << " (url=" << url << ")" << std::endl;
        // Don't delete partial file — may be resumed later
        return StatusCode::OTA_DOWNLOAD_FAILED;
    }

    long http_code = 0;
    curl_easy_getinfo(impl_->curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_off_t downloaded = 0;
    curl_easy_getinfo(impl_->curl, CURLINFO_SIZE_DOWNLOAD_T, &downloaded);

    std::cout << "[HttpClient] Download complete: " << downloaded << " bytes"
              << " (HTTP " << http_code << ") → " << output_path << std::endl;

    return StatusCode::OK;
#else
    std::cerr << "[HttpClient] WARNING: Built without libcurl. File download not available." << std::endl;
    return StatusCode::ERROR;
#endif
}

} // namespace dev_sys
