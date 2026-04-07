#pragma once

#include "blocker/filter_rules.h"
#include "include/cef_request_handler.h"

#include <memory>

class RequestFilter : public CefResourceRequestHandler {
public:
    explicit RequestFilter(std::shared_ptr<FilterRuleSet> rules);

    ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefFrame> frame,
                                      CefRefPtr<CefRequest> request,
                                      CefRefPtr<CefCallback> callback) override;

private:
    std::shared_ptr<FilterRuleSet> rules_;

    static uint32_t MapResourceType(cef_resource_type_t type);

    IMPLEMENT_REFCOUNTING(RequestFilter);
    DISALLOW_COPY_AND_ASSIGN(RequestFilter);
};
