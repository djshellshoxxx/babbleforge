#pragma once
// Speaker identity sources (docs/CORPUS.md §4), in priority order:
//   1. speakers.csv (file -> speakerId, language, optional labels)
//   2. folder convention <root>/<speaker>/<files>
//   3. filename regex (group 1 = speaker id)
//   4. one speaker per file, flagged speaker.unknown
#include <filesystem>
#include <map>
#include <regex>
#include <string>

namespace bf::ingest {

struct SpeakerAssignment {
    std::string externalId;
    std::string language;    // BCP-47 or empty
    std::string labelsJson;  // free-form, optional
    bool unknown = false;    // speaker.unknown
};

class SpeakerResolver {
public:
    // csvPath may be empty. Returns false (and fills error) if the CSV cannot be read.
    bool init(const std::filesystem::path& csvPath, const std::string& filenameRegex, std::string* error);
    // relPath uses '/' separators, relative to the import root.
    SpeakerAssignment resolve(const std::string& relPath) const;

private:
    struct CsvEntry { std::string speaker, language, labels; };
    std::map<std::string, CsvEntry> byPath_, byName_;
    std::regex re_;
    bool haveRe_ = false;
};

// Splits one CSV line (RFC 4180 quoting, comma separator).
std::vector<std::string> splitCsvLine(const std::string& line);

}  // namespace bf::ingest
