#pragma once

#include "include/cef_resource_handler.h"
#include "include/cef_scheme.h"

#include <string>

class OrbSchemeHandlerFactory : public CefSchemeHandlerFactory {
public:
    explicit OrbSchemeHandlerFactory(const std::string& ui_dir);

    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                          CefRefPtr<CefFrame> frame,
                                          const CefString& scheme_name,
                                          CefRefPtr<CefRequest> request) override;

private:
    std::string ui_dir_;

    IMPLEMENT_REFCOUNTING(OrbSchemeHandlerFactory);
    DISALLOW_COPY_AND_ASSIGN(OrbSchemeHandlerFactory);
};

class OrbResourceHandler : public CefResourceHandler {
public:
    OrbResourceHandler(const std::string& file_path);

    // Set inline HTML data instead of reading from file
    void SetInlineData(const std::string& data, const std::string& mime) {
        data_ = data;
        mime_type_ = mime;
        inline_ = true;
    }

    bool Open(CefRefPtr<CefRequest> request,
              bool& handle_request,
              CefRefPtr<CefCallback> callback) override;
    void GetResponseHeaders(CefRefPtr<CefResponse> response,
                           int64_t& response_length,
                           CefString& redirect_url) override;
    bool Read(void* data_out,
              int bytes_to_read,
              int& bytes_read,
              CefRefPtr<CefResourceReadCallback> callback) override;
    void Cancel() override;

private:
    std::string file_path_;
    std::string data_;
    size_t offset_ = 0;
    std::string mime_type_;
    bool inline_ = false;

    static std::string GetMimeType(const std::string& path);

    IMPLEMENT_REFCOUNTING(OrbResourceHandler);
    DISALLOW_COPY_AND_ASSIGN(OrbResourceHandler);
};
