#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
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
    std::string pattern;        // URL substring pattern
    std::string domain;         // For domain-anchored rules (||domain^)
    bool is_exception = false;  // @@
    bool third_party = false;
    bool first_party_only = false;
    uint32_t resource_types = ZRT_ALL;
    bool has_resource_types = false;
};

class FilterRuleSet {
public:
    FilterRuleSet();
    ~FilterRuleSet();

    void Load(const std::string& path);
    bool ShouldBlock(const std::string& url, const std::string& page_domain,
                     uint32_t resource_type) const;

    size_t RuleCount() const { return total_rules_; }

private:
    void ParseLine(const std::string& line);
    void ParseOptions(const std::string& options, FilterRule& rule);
    static std::string ExtractDomain(const std::string& url);
    static bool MatchPattern(const std::string& url, const std::string& pattern);
    static bool IsSeparatorChar(char c);
    static bool MatchDomain(const std::string& url_domain, const std::string& rule_domain);
    static bool IsThirdParty(const std::string& url, const std::string& page_domain);

    // Domain-anchored rules: keyed by domain
    std::unordered_map<std::string, std::vector<std::unique_ptr<FilterRule>>> domain_rules_;
    // Generic rules (not domain-anchored)
    std::vector<std::unique_ptr<FilterRule>> generic_rules_;
    // Exception rules
    std::vector<std::unique_ptr<FilterRule>> exception_rules_;

    size_t total_rules_ = 0;
};
