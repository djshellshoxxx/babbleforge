#include "core/corpus/ingest/SpeakerIdentity.h"

#include <algorithm>
#include <fstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace bf::ingest {

std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (q) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else q = false;
            } else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    out.push_back(cur);
    return out;
}

namespace {
std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
std::string norm(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.rfind("./", 0) == 0) p.erase(0, 2);
    return p;
}
}  // namespace

bool SpeakerResolver::init(const std::filesystem::path& csvPath, const std::string& filenameRegex, std::string* error) {
    byPath_.clear();
    byName_.clear();
    try {
        re_ = std::regex(filenameRegex);
        haveRe_ = !filenameRegex.empty();
    } catch (const std::regex_error&) {
        if (error) *error = "invalid speaker filename regex";
        return false;
    }
    if (csvPath.empty()) return true;
    std::ifstream f(csvPath);
    if (!f) {
        if (error) *error = "cannot read " + csvPath.string();
        return false;
    }
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (trim(line).empty()) continue;
        auto cols = splitCsvLine(line);
        for (auto& c : cols) c = trim(c);
        if (first) {
            first = false;
            const auto h = lower(cols[0]);
            if (h == "file" || h == "filename" || h == "path") continue;  // header
        }
        if (cols.size() < 2 || cols[0].empty() || cols[1].empty()) continue;
        CsvEntry e{cols[1], cols.size() > 2 ? cols[2] : "", cols.size() > 3 ? cols[3] : ""};
        byPath_[norm(cols[0])] = e;
        byName_.emplace(std::filesystem::path(norm(cols[0])).filename().string(), e);
    }
    return true;
}

SpeakerAssignment SpeakerResolver::resolve(const std::string& relPath) const {
    SpeakerAssignment a;
    auto fillFrom = [&](const CsvEntry& e) {
        a.externalId = e.speaker;
        a.language = e.language;
        if (!e.labels.empty()) {
            // Labels are stored as JSON; a plain string becomes {"labels": "..."}.
            const auto j = nlohmann::json::parse(e.labels, nullptr, false);
            a.labelsJson = j.is_discarded() ? nlohmann::json{{"labels", e.labels}}.dump() : j.dump();
        }
    };
    const std::string rel = norm(relPath);
    if (auto it = byPath_.find(rel); it != byPath_.end()) { fillFrom(it->second); return a; }
    const std::string name = std::filesystem::path(rel).filename().string();
    if (auto it = byName_.find(name); it != byName_.end()) { fillFrom(it->second); return a; }
    if (const auto slash = rel.find('/'); slash != std::string::npos) {
        a.externalId = rel.substr(0, slash);
        return a;
    }
    if (haveRe_) {
        std::smatch m;
        if (std::regex_search(name, m, re_) && m.size() > 1 && m[1].matched && m[1].length() > 0) {
            a.externalId = m[1].str();
            return a;
        }
    }
    a.externalId = rel;
    a.unknown = true;
    return a;
}

}  // namespace bf::ingest
