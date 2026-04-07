#include "blocker/request_filter.h"
#include "include/internal/cef_types.h"

RequestFilter::RequestFilter(std::shared_ptr<FilterRuleSet> rules)
    : rules_(rules) {}

CefResourceRequestHandler::ReturnValue RequestFilter::OnBeforeResourceLoad(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefCallback> callback) {

    std::string url = request->GetURL().ToString();

    // Don't filter internal URLs
    if (url.find("orb://") == 0 || url.find("devtools://") == 0 ||
        url.find("chrome://") == 0) {
        return RV_CONTINUE;
    }

    // Determine page domain from the frame
    std::string page_domain;
    if (frame) {
        CefRefPtr<CefFrame> top_frame = frame;
        while (top_frame->GetParent()) {
            top_frame = top_frame->GetParent();
        }
        std::string page_url = top_frame->GetURL().ToString();
        auto scheme_end = page_url.find("://");
        if (scheme_end != std::string::npos) {
            auto domain_start = scheme_end + 3;
            auto domain_end = page_url.find_first_of(":/", domain_start);
            if (domain_end == std::string::npos) domain_end = page_url.size();
            page_domain = page_url.substr(domain_start, domain_end - domain_start);
        }
    }

    uint32_t rt = MapResourceType(request->GetResourceType());

    if (rules_ && rules_->ShouldBlock(url, page_domain, rt)) {
        return RV_CANCEL;
    }

    return RV_CONTINUE;
}

uint32_t RequestFilter::MapResourceType(cef_resource_type_t type) {
    switch (type) {
        case RT_SCRIPT:        return ZRT_SCRIPT;
        case RT_IMAGE:         return ZRT_IMAGE;
        case RT_STYLESHEET:    return ZRT_STYLESHEET;
        case RT_XHR:           return ZRT_XHR;
        case RT_SUB_FRAME:     return ZRT_SUBDOCUMENT;
        case RT_MEDIA:         return ZRT_MEDIA;
        case RT_FONT_RESOURCE: return ZRT_FONT;
        default:               return ZRT_OTHER;
    }
}
