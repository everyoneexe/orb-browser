#include "blocker/filter_rules.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>

namespace {

// Shorter tokens are too common to be selective.
constexpr size_t kMinTokenLength = 4;

inline bool IsTokenChar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// EasyList's '^' separator class: everything except letters, digits and
// '-', '.', '%', '_'.
inline bool IsSeparator(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return false;
    return !(c == '-' || c == '.' || c == '%' || c == '_');
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// Host of a URL, plus the offset just past it.
std::string_view HostOf(std::string_view url, size_t* host_end) {
    auto scheme = url.find("://");
    size_t start = (scheme == std::string_view::npos) ? 0 : scheme + 3;
    auto end = url.find_first_of(":/?#", start);
    if (end == std::string_view::npos) end = url.size();
    if (host_end) *host_end = end;
    return url.substr(start, end - start);
}

// host == suffix, or host is a subdomain of suffix.
bool HostMatches(std::string_view host, std::string_view suffix) {
    if (host == suffix) return true;
    if (host.size() > suffix.size()) {
        const size_t pos = host.size() - suffix.size();
        return host[pos - 1] == '.' && host.compare(pos, suffix.size(), suffix) == 0;
    }
    return false;
}

bool IsThirdParty(std::string_view host, std::string_view page_domain) {
    // An unknown document origin (the top-level navigation itself) must not be
    // treated as third-party, or $third-party rules would cancel the page.
    if (page_domain.empty()) return false;
    return !HostMatches(host, page_domain) && !HostMatches(page_domain, host);
}

// Element hiding and friends: ##, #@#, #?#, #$#, #%#. These are not network
// rules and must never reach the URL matcher.
bool IsCosmeticRule(const std::string& line) {
    for (auto h = line.find('#'); h != std::string::npos; h = line.find('#', h + 1)) {
        if (h + 1 >= line.size()) break;
        const char a = line[h + 1];
        if (a == '#') return true;
        if (h + 2 < line.size() && line[h + 2] == '#' &&
            (a == '@' || a == '?' || a == '$' || a == '%'))
            return true;
    }
    return false;
}

void CollectTokens(std::string_view s, std::vector<std::string_view>& out) {
    size_t i = 0;
    while (i < s.size()) {
        if (!IsTokenChar(static_cast<unsigned char>(s[i]))) {
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < s.size() && IsTokenChar(static_cast<unsigned char>(s[j]))) j++;
        if (j - i >= kMinTokenLength) out.push_back(s.substr(i, j - i));
        i = j;
    }
}

// Longest alphanumeric run in the pattern that is bounded on both sides. Only
// such a run is guaranteed to appear as a whole token in every URL the pattern
// matches, which is what makes the token index lossless (no false negatives).
std::string PickIndexToken(const std::string& pattern, bool anchor_start, bool anchor_end) {
    std::string best;
    size_t i = 0;
    while (i < pattern.size()) {
        if (!IsTokenChar(static_cast<unsigned char>(pattern[i]))) {
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < pattern.size() && IsTokenChar(static_cast<unsigned char>(pattern[j]))) j++;

        const bool left_bounded  = (i == 0) ? anchor_start : (pattern[i - 1] != '*');
        const bool right_bounded = (j == pattern.size()) ? anchor_end : (pattern[j] != '*');
        if (left_bounded && right_bounded && (j - i) >= kMinTokenLength &&
            (j - i) > best.size()) {
            best.assign(pattern, i, j - i);
        }
        i = j;
    }
    return best;
}

// Sequential matcher honouring '*' (any run), '^' (exactly one separator or
// end of URL) and the start/end anchors. `start` is where matching may begin,
// which for ||host rules is the offset just past the host.
bool MatchPattern(std::string_view url, const FilterRule& rule, size_t start) {
    const std::string& pat = rule.pattern;
    if (pat.empty()) return true;
    if (start > url.size()) return false;

    size_t pos = rule.anchor_start ? 0 : start;
    bool floating = !rule.anchor_start;
    size_t i = 0;

    while (i < pat.size()) {
        const char c = pat[i];
        if (c == '*') {
            floating = true;
            i++;
            continue;
        }
        if (c == '^') {
            if (floating) {
                size_t k = pos;
                while (k < url.size() && !IsSeparator(static_cast<unsigned char>(url[k]))) k++;
                pos = (k >= url.size()) ? url.size() : k + 1;
            } else if (pos < url.size()) {
                if (!IsSeparator(static_cast<unsigned char>(url[pos]))) return false;
                pos++;
            }
            // End of URL satisfies a trailing separator.
            floating = false;
            i++;
            continue;
        }

        size_t j = i;
        while (j < pat.size() && pat[j] != '*' && pat[j] != '^') j++;
        const std::string_view part(pat.data() + i, j - i);
        if (floating) {
            const size_t found = url.find(part, pos);
            if (found == std::string_view::npos) return false;
            pos = found + part.size();
            floating = false;
        } else {
            if (pos + part.size() > url.size()) return false;
            if (url.compare(pos, part.size(), part) != 0) return false;
            pos += part.size();
        }
        i = j;
    }

    if (rule.anchor_end && pos != url.size()) return false;
    return true;
}

}  // namespace

FilterRuleSet::FilterRuleSet() {}
FilterRuleSet::~FilterRuleSet() {}

void FilterRuleSet::Load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[Blocker] Cannot open filter list: " << path << std::endl;
        return;
    }

    const size_t before = total_rules_;
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                 line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) lead++;
        if (lead) line.erase(0, lead);

        if (line.empty()) continue;
        if (line[0] == '!' || line[0] == '[') continue;
        if (IsCosmeticRule(line)) continue;

        ParseLine(line);
    }

    std::cerr << "[Blocker] Loaded " << (total_rules_ - before) << " rules from " << path
              << " (total: " << total_rules_ << ")" << std::endl;
}

void FilterRuleSet::ParseLine(const std::string& line) {
    auto rule = std::make_unique<FilterRule>();
    std::string input = line;

    if (input.rfind("@@", 0) == 0) {
        rule->is_exception = true;
        input.erase(0, 2);
    }

    bool document_exception = false;
    const auto dollar = input.rfind('$');
    if (dollar != std::string::npos) {
        if (!ParseOptions(input.substr(dollar + 1), *rule, document_exception))
            return;  // option we do not implement: dropping beats over-applying
        input.erase(dollar);
    }

    // Regex rules are not supported; treating them as substrings never matches.
    if (input.size() > 1 && input.front() == '/' && input.back() == '/') return;

    if (document_exception) {
        std::string host = input;
        if (host.rfind("||", 0) == 0) host.erase(0, 2);
        const auto sep = host.find_first_of("/^*|");
        if (sep != std::string::npos) host.erase(sep);
        if (!host.empty()) document_allowlist_.insert(ToLower(host));
        return;
    }

    if (input.rfind("||", 0) == 0) {
        input.erase(0, 2);
        const auto sep = input.find_first_of("/^*");
        if (sep == std::string::npos) {
            rule->domain = ToLower(input);
        } else {
            rule->domain = ToLower(input.substr(0, sep));
            rule->pattern = ToLower(input.substr(sep));
            // The host boundary is already guaranteed by the domain index.
            if (!rule->pattern.empty() && rule->pattern[0] == '^')
                rule->pattern.erase(0, 1);
        }
    } else {
        if (!input.empty() && input.front() == '|') {
            rule->anchor_start = true;
            input.erase(0, 1);
        }
        if (!input.empty() && input.back() == '|') {
            rule->anchor_end = true;
            input.pop_back();
        }
        rule->pattern = ToLower(input);
    }

    // A rule with neither a host nor a pattern matches every URL in existence.
    // That is never what a list author meant — and an exception shaped like
    // that whitelists the entire web.
    if (rule->domain.empty() && rule->pattern.empty()) return;

    total_rules_++;
    const FilterRule* ptr = rule.get();
    storage_.push_back(std::move(rule));
    IndexRule(ptr, ptr->is_exception ? exception_ : block_);
}

bool FilterRuleSet::ParseOptions(const std::string& options, FilterRule& rule,
                                 bool& document_exception) {
    uint32_t positive = 0;
    uint32_t negative = 0;

    size_t start = 0;
    while (start <= options.size()) {
        const auto comma = options.find(',', start);
        std::string opt = (comma == std::string::npos) ? options.substr(start)
                                                       : options.substr(start, comma - start);
        start = (comma == std::string::npos) ? options.size() + 1 : comma + 1;
        if (opt.empty()) continue;

        if (opt.rfind("domain=", 0) == 0) {
            const std::string value = opt.substr(7);
            size_t p = 0;
            while (p < value.size()) {
                const auto bar = value.find('|', p);
                std::string d = (bar == std::string::npos) ? value.substr(p)
                                                           : value.substr(p, bar - p);
                p = (bar == std::string::npos) ? value.size() : bar + 1;
                if (d.empty()) continue;
                if (d[0] == '~') rule.exclude_domains.push_back(ToLower(d.substr(1)));
                else rule.include_domains.push_back(ToLower(d));
            }
            continue;
        }

        bool negated = false;
        if (opt[0] == '~') {
            negated = true;
            opt.erase(0, 1);
        }

        if (opt == "third-party") {
            if (negated) rule.first_party_only = true;
            else rule.third_party = true;
            continue;
        }

        uint32_t type = 0;
        if (opt == "script") type = ZRT_SCRIPT;
        else if (opt == "image") type = ZRT_IMAGE;
        else if (opt == "stylesheet" || opt == "css") type = ZRT_STYLESHEET;
        else if (opt == "xmlhttprequest" || opt == "xhr") type = ZRT_XHR;
        else if (opt == "subdocument" || opt == "frame") type = ZRT_SUBDOCUMENT;
        else if (opt == "media") type = ZRT_MEDIA;
        else if (opt == "font") type = ZRT_FONT;
        else if (opt == "websocket") type = ZRT_WEBSOCKET;
        else if (opt == "other" || opt == "ping" || opt == "beacon" ||
                 opt == "object" || opt == "object-subrequest") type = ZRT_OTHER;
        if (type) {
            if (negated) negative |= type;
            else positive |= type;
            continue;
        }

        if (opt == "all") continue;         // every type, which is the default
        if (opt == "match-case") continue;  // we compare lowercased throughout
        if (opt == "important") continue;   // rule priority is not modelled

        if (rule.is_exception && opt == "document") {
            document_exception = true;
            continue;
        }

        // csp=, redirect=, removeparam=, rewrite=, popup, generichide,
        // elemhide, denyallow, header=, ... — applying these as plain network
        // rules would block far more than the author asked for.
        return false;
    }

    if (positive) {
        rule.resource_types = positive;
        rule.has_resource_types = true;
    } else if (negative) {
        rule.resource_types = ZRT_ALL & ~negative;
        rule.has_resource_types = true;
    }
    return true;
}

void FilterRuleSet::IndexRule(const FilterRule* rule, Index& index) {
    if (!rule->domain.empty()) {
        index.by_domain[rule->domain].push_back(rule);
        return;
    }
    std::string token = PickIndexToken(rule->pattern, rule->anchor_start, rule->anchor_end);
    if (token.empty()) index.untokenized.push_back(rule);
    else index.by_token[std::move(token)].push_back(rule);
}

namespace {

bool RuleMatches(const FilterRule& rule, std::string_view url, std::string_view host,
                 std::string_view page_domain, uint32_t resource_type, size_t start) {
    if (rule.has_resource_types && !(rule.resource_types & resource_type)) return false;

    if (rule.third_party || rule.first_party_only) {
        const bool third = IsThirdParty(host, page_domain);
        if (rule.third_party && !third) return false;
        if (rule.first_party_only && third) return false;
    }

    if (!rule.include_domains.empty()) {
        bool included = false;
        for (const auto& d : rule.include_domains) {
            if (HostMatches(page_domain, d)) {
                included = true;
                break;
            }
        }
        if (!included) return false;
    }
    for (const auto& d : rule.exclude_domains) {
        if (HostMatches(page_domain, d)) return false;
    }

    return MatchPattern(url, rule, start);
}

}  // namespace

bool FilterRuleSet::MatchesIndex(const Index& index, const MatchContext& ctx) const {
    // Host-anchored rules for the host itself and each parent domain.
    std::string_view d = ctx.host;
    while (!d.empty()) {
        const auto it = index.by_domain.find(d);
        if (it != index.by_domain.end()) {
            for (const FilterRule* rule : it->second) {
                if (RuleMatches(*rule, ctx.url, ctx.host, ctx.page_domain,
                                ctx.resource_type, ctx.host_end))
                    return true;
            }
        }
        const auto dot = d.find('.');
        if (dot == std::string_view::npos) break;
        d = d.substr(dot + 1);
    }

    // Generic rules, reached only through the tokens the URL actually contains.
    if (!index.by_token.empty()) {
        std::vector<std::string_view> tokens;
        tokens.reserve(24);
        CollectTokens(ctx.url, tokens);
        for (std::string_view token : tokens) {
            const auto it = index.by_token.find(token);
            if (it == index.by_token.end()) continue;
            for (const FilterRule* rule : it->second) {
                if (RuleMatches(*rule, ctx.url, ctx.host, ctx.page_domain,
                                ctx.resource_type, 0))
                    return true;
            }
        }
    }

    for (const FilterRule* rule : index.untokenized) {
        if (RuleMatches(*rule, ctx.url, ctx.host, ctx.page_domain, ctx.resource_type, 0))
            return true;
    }
    return false;
}

bool FilterRuleSet::ShouldBlock(const std::string& url, const std::string& page_domain,
                                uint32_t resource_type) const {
    if (url.empty()) return false;

    const std::string lower_url = ToLower(url);
    const std::string lower_page = ToLower(page_domain);

    MatchContext ctx;
    ctx.url = lower_url;
    ctx.host = HostOf(lower_url, &ctx.host_end);
    ctx.page_domain = lower_page;
    ctx.resource_type = resource_type;

    // Site-wide allowlist first: @@||host^$document disables blocking here.
    if (!document_allowlist_.empty()) {
        std::string_view d = ctx.page_domain;
        while (!d.empty()) {
            if (document_allowlist_.find(d) != document_allowlist_.end())
                return false;
            const auto dot = d.find('.');
            if (dot == std::string_view::npos) break;
            d = d.substr(dot + 1);
        }
    }

    if (MatchesIndex(exception_, ctx)) return false;
    return MatchesIndex(block_, ctx);
}
