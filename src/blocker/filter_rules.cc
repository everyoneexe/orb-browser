#include "blocker/filter_rules.h"

#include <algorithm>
#include <fstream>
#include <iostream>

FilterRuleSet::FilterRuleSet() {}
FilterRuleSet::~FilterRuleSet() {}

void FilterRuleSet::Load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[Blocker] Cannot open filter list: " << path << std::endl;
        return;
    }

    std::string line;
    size_t count = 0;
    while (std::getline(file, line)) {
        // Trim whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        if (line.empty()) continue;

        // Skip comments and headers
        if (line[0] == '!' || line[0] == '[') continue;

        // Skip element hiding rules (contain ## or #@#)
        if (line.find("##") != std::string::npos || line.find("#@#") != std::string::npos)
            continue;

        ParseLine(line);
        count++;
    }

    total_rules_ += count;
    std::cerr << "[Blocker] Loaded " << count << " rules from " << path
              << " (total: " << total_rules_ << ")" << std::endl;
}

void FilterRuleSet::ParseLine(const std::string& line) {
    auto rule = std::make_unique<FilterRule>();
    std::string input = line;

    // Check for exception
    if (input.substr(0, 2) == "@@") {
        rule->is_exception = true;
        input = input.substr(2);
    }

    // Check for options ($)
    auto dollar = input.rfind('$');
    if (dollar != std::string::npos) {
        std::string options = input.substr(dollar + 1);
        input = input.substr(0, dollar);
        ParseOptions(options, *rule);
    }

    // Check for domain anchor (||)
    if (input.substr(0, 2) == "||") {
        input = input.substr(2);
        // Extract domain part (up to first / or ^)
        auto sep = input.find_first_of("/^*");
        if (sep != std::string::npos) {
            rule->domain = input.substr(0, sep);
            rule->pattern = input.substr(sep);
        } else {
            rule->domain = input;
            rule->pattern = "";
        }
    } else {
        // Strip leading | (start anchor)
        if (!input.empty() && input[0] == '|') input = input.substr(1);
        // Strip trailing | (end anchor)
        if (!input.empty() && input.back() == '|') input.pop_back();
        rule->pattern = input;
    }

    // Store rule
    if (rule->is_exception) {
        exception_rules_.push_back(std::move(rule));
    } else if (!rule->domain.empty()) {
        std::string domain = rule->domain;
        // Lowercase
        std::transform(domain.begin(), domain.end(), domain.begin(), ::tolower);
        domain_rules_[domain].push_back(std::move(rule));
    } else {
        generic_rules_.push_back(std::move(rule));
    }
}

void FilterRuleSet::ParseOptions(const std::string& options, FilterRule& rule) {
    size_t start = 0;
    while (start < options.size()) {
        auto comma = options.find(',', start);
        std::string opt;
        if (comma != std::string::npos) {
            opt = options.substr(start, comma - start);
            start = comma + 1;
        } else {
            opt = options.substr(start);
            start = options.size();
        }

        if (opt == "third-party") {
            rule.third_party = true;
        } else if (opt == "~third-party") {
            rule.first_party_only = true;
        } else if (opt == "script") {
            rule.has_resource_types = true;
            rule.resource_types = ZRT_SCRIPT;
        } else if (opt == "image") {
            rule.has_resource_types = true;
            rule.resource_types = ZRT_IMAGE;
        } else if (opt == "stylesheet") {
            rule.has_resource_types = true;
            rule.resource_types = ZRT_STYLESHEET;
        } else if (opt == "xmlhttprequest") {
            rule.has_resource_types = true;
            rule.resource_types = ZRT_XHR;
        } else if (opt == "subdocument") {
            rule.has_resource_types = true;
            rule.resource_types = ZRT_SUBDOCUMENT;
        } else if (opt == "media") {
            rule.has_resource_types = true;
            rule.resource_types = ZRT_MEDIA;
        } else if (opt == "font") {
            rule.has_resource_types = true;
            rule.resource_types = ZRT_FONT;
        } else if (opt == "websocket") {
            rule.has_resource_types = true;
            rule.resource_types = ZRT_WEBSOCKET;
        }
        // Ignore unknown options
    }
}

bool FilterRuleSet::ShouldBlock(const std::string& url, const std::string& page_domain,
                                 uint32_t resource_type) const {
    // Check exception rules first
    for (const auto& rule : exception_rules_) {
        if (rule->has_resource_types && !(rule->resource_types & resource_type))
            continue;
        if (!rule->domain.empty() && !MatchDomain(ExtractDomain(url), rule->domain))
            continue;
        if (MatchPattern(url, rule->pattern))
            return false;  // Whitelisted
    }

    // Check domain-anchored rules
    std::string url_domain = ExtractDomain(url);
    std::string lower_domain = url_domain;
    std::transform(lower_domain.begin(), lower_domain.end(), lower_domain.begin(), ::tolower);

    // Try matching against the domain and its parent domains
    std::string check_domain = lower_domain;
    while (!check_domain.empty()) {
        auto it = domain_rules_.find(check_domain);
        if (it != domain_rules_.end()) {
            for (const auto& rule : it->second) {
                if (rule->has_resource_types && !(rule->resource_types & resource_type))
                    continue;
                if (rule->third_party && !IsThirdParty(url, page_domain))
                    continue;
                if (rule->first_party_only && IsThirdParty(url, page_domain))
                    continue;
                // Domain already matched by map key, check pattern part
                if (rule->pattern.empty() || MatchPattern(url, rule->pattern))
                    return true;
            }
        }

        // Try parent domain
        auto dot = check_domain.find('.');
        if (dot == std::string::npos) break;
        check_domain = check_domain.substr(dot + 1);
    }

    // Check generic rules
    for (const auto& rule : generic_rules_) {
        if (rule->has_resource_types && !(rule->resource_types & resource_type))
            continue;
        if (rule->third_party && !IsThirdParty(url, page_domain))
            continue;
        if (rule->first_party_only && IsThirdParty(url, page_domain))
            continue;
        if (MatchPattern(url, rule->pattern))
            return true;
    }

    return false;
}

std::string FilterRuleSet::ExtractDomain(const std::string& url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return url;

    auto domain_start = scheme_end + 3;
    auto domain_end = url.find_first_of(":/", domain_start);
    if (domain_end == std::string::npos) domain_end = url.size();

    return url.substr(domain_start, domain_end - domain_start);
}

bool FilterRuleSet::MatchPattern(const std::string& url, const std::string& pattern) {
    if (pattern.empty()) return true;

    // Handle wildcard patterns
    if (pattern.find('*') != std::string::npos) {
        // Split by * and match each part sequentially
        size_t url_pos = 0;
        size_t pat_start = 0;
        while (pat_start < pattern.size()) {
            auto star = pattern.find('*', pat_start);
            std::string part;
            if (star != std::string::npos) {
                part = pattern.substr(pat_start, star - pat_start);
                pat_start = star + 1;
            } else {
                part = pattern.substr(pat_start);
                pat_start = pattern.size();
            }

            if (part.empty()) continue;

            // Replace ^ with separator matching
            std::string search_part;
            for (char c : part) {
                if (c == '^') continue;  // simplified: skip separator chars
                search_part += c;
            }

            if (search_part.empty()) continue;

            auto found = url.find(search_part, url_pos);
            if (found == std::string::npos) return false;
            url_pos = found + search_part.length();
        }
        return true;
    }

    // Handle ^ separator char — replace with simple substring match
    std::string clean_pattern;
    for (char c : pattern) {
        if (c == '^') continue;
        clean_pattern += c;
    }

    if (clean_pattern.empty()) return true;
    return url.find(clean_pattern) != std::string::npos;
}

bool FilterRuleSet::IsSeparatorChar(char c) {
    if (c >= 'a' && c <= 'z') return false;
    if (c >= 'A' && c <= 'Z') return false;
    if (c >= '0' && c <= '9') return false;
    if (c == '-' || c == '.' || c == '%') return false;
    return true;
}

bool FilterRuleSet::MatchDomain(const std::string& url_domain, const std::string& rule_domain) {
    if (url_domain == rule_domain) return true;
    // Check if url_domain ends with .rule_domain
    if (url_domain.length() > rule_domain.length()) {
        auto pos = url_domain.length() - rule_domain.length();
        return url_domain[pos - 1] == '.' && url_domain.substr(pos) == rule_domain;
    }
    return false;
}

bool FilterRuleSet::IsThirdParty(const std::string& url, const std::string& page_domain) {
    std::string url_domain = ExtractDomain(url);
    // Simple: different domains = third party
    if (url_domain == page_domain) return false;
    // Check if one is subdomain of the other
    if (url_domain.length() > page_domain.length()) {
        auto pos = url_domain.length() - page_domain.length();
        if (url_domain[pos - 1] == '.' && url_domain.substr(pos) == page_domain) return false;
    }
    if (page_domain.length() > url_domain.length()) {
        auto pos = page_domain.length() - url_domain.length();
        if (page_domain[pos - 1] == '.' && page_domain.substr(pos) == url_domain) return false;
    }
    return true;
}
