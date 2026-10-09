#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum OrbResourceType : uint32_t {
    ZRT_SCRIPT      = 1 << 0,
    ZRT_IMAGE       = 1 << 1,
    ZRT_STYLESHEET  = 1 << 2,
    ZRT_XHR         = 1 << 3,
    ZRT_SUBDOCUMENT = 1 << 4,
    ZRT_MEDIA       = 1 << 5,
    ZRT_FONT        = 1 << 6,
    ZRT_WEBSOCKET   = 1 << 7,
    ZRT_OTHER       = 1 << 8,
    ZRT_ALL         = 0xFFFFFFFF,
};

struct FilterRule {
    std::string pattern;        // lowercased; '*' and '^' keep wildcard meaning
    std::string domain;         // host of a ||host rule, lowercased
    std::vector<std::string> include_domains;  // $domain=a.com|b.com
    std::vector<std::string> exclude_domains;  // $domain=~a.com
    bool is_exception = false;  // @@
    bool third_party = false;   // $third-party
    bool first_party_only = false;  // $~third-party
    bool anchor_start = false;  // leading |
    bool anchor_end = false;    // trailing |
    uint32_t resource_types = ZRT_ALL;
    bool has_resource_types = false;
};

// Parsed EasyList-style rule set.
//
// Threading: Load() mutates and is not thread-safe; it must finish before the
// first request is filtered. ShouldBlock() is const and is called concurrently
// from the CEF IO thread.
class FilterRuleSet {
public:
    FilterRuleSet();
    ~FilterRuleSet();

    void Load(const std::string& path);
    bool ShouldBlock(const std::string& url, const std::string& page_domain,
                     uint32_t resource_type) const;

    size_t RuleCount() const { return total_rules_; }

private:
    // Heterogeneous lookup so the request path can probe with string_view and
    // allocate nothing per request.
    struct StringHash {
        using is_transparent = void;
        size_t operator()(std::string_view sv) const {
            return std::hash<std::string_view>{}(sv);
        }
    };
    using RuleList = std::vector<const FilterRule*>;
    using RuleMap = std::unordered_map<std::string, RuleList, StringHash, std::equal_to<>>;

    // One side of the decision (blocking rules or exception rules).
    struct Index {
        RuleMap by_domain;   // ||host rules, keyed by host
        RuleMap by_token;    // generic rules, keyed by a required URL token
        RuleList untokenized;  // generic rules with no usable token
    };

    struct MatchContext {
        std::string_view url;          // lowercased full URL
        std::string_view host;         // lowercased request host
        std::string_view page_domain;  // lowercased document host
        size_t host_end = 0;           // offset just past the host in url
        uint32_t resource_type = ZRT_ALL;
    };

    void ParseLine(const std::string& line);
    bool ParseOptions(const std::string& options, FilterRule& rule,
                      bool& document_exception);
    void IndexRule(const FilterRule* rule, Index& index);
    bool MatchesIndex(const Index& index, const MatchContext& ctx) const;

    std::vector<std::unique_ptr<FilterRule>> storage_;
    Index block_;
    Index exception_;
    // @@||host^$document — blocking is disabled for the whole document.
    std::unordered_set<std::string, StringHash, std::equal_to<>> document_allowlist_;

    size_t total_rules_ = 0;
};
