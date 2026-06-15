#include "shelf.h"

#include <cctype>
#include <map>

namespace p4gw {

namespace {

// Splits "depotFile0" into ("depotFile", 0). Returns false when the key has
// no trailing digits (a global field like "change" or "desc").
bool splitIndexedKey(const std::string& key, std::string& base, int& index) {
    size_t digitStart = key.size();
    while (digitStart > 0 && std::isdigit(static_cast<unsigned char>(
                                 key[digitStart - 1]))) {
        --digitStart;
    }
    if (digitStart == key.size() || digitStart == 0) return false;
    base = key.substr(0, digitStart);
    index = std::stoi(key.substr(digitStart));
    return true;
}

}  // namespace

ShelveAction parseShelveAction(const std::string& action) {
    if (action == "edit") return ShelveAction::Edit;
    if (action == "add") return ShelveAction::Add;
    if (action == "delete") return ShelveAction::Delete;
    if (action == "move/add") return ShelveAction::MoveAdd;
    if (action == "move/delete") return ShelveAction::MoveDelete;
    return ShelveAction::Other;
}

bool isBinaryType(const std::string& type) {
    // Text-ish p4 types start with these stems; everything else (binary,
    // ubinary, apple, resource, ...) is byte content a line merge would wreck.
    return !(type.starts_with("text") || type.starts_with("xtext") ||
             type.starts_with("ktext") || type.starts_with("kxtext") ||
             type.starts_with("unicode") || type.starts_with("utf") ||
             type.starts_with("symlink"));
}

std::expected<ShelfInfo, std::string> parseShelveDescribe(
    const std::string& out) {
    // First pass: collapse tagged lines (and their continuation lines) into
    // (key, value) fields. A line "... key value" starts a field; a line that
    // does not start with "... " continues the previous field's value.
    struct Field {
        std::string key;
        std::string value;
    };
    std::vector<Field> fields;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t end = out.find('\n', pos);
        if (end == std::string::npos) end = out.size();
        std::string line = out.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.starts_with("... ")) {
            const std::string rest = line.substr(4);
            const size_t space = rest.find(' ');
            Field field;
            if (space == std::string::npos) {
                field.key = rest;
            } else {
                field.key = rest.substr(0, space);
                field.value = rest.substr(space + 1);
            }
            fields.push_back(std::move(field));
        } else if (!fields.empty()) {
            // Continuation of a multi-line value (e.g. the description).
            fields.back().value += '\n';
            fields.back().value += line;
        }
    }

    ShelfInfo info;
    std::map<int, ShelvedFile> byIndex;
    for (const auto& field : fields) {
        std::string base;
        int index = 0;
        if (splitIndexedKey(field.key, base, index)) {
            ShelvedFile& file = byIndex[index];
            if (base == "depotFile") file.depotFile = field.value;
            else if (base == "action") file.action = parseShelveAction(field.value);
            else if (base == "type") file.type = field.value;
            else if (base == "rev") file.rev = field.value;
        } else if (field.key == "change") {
            info.change = field.value;
        } else if (field.key == "desc") {
            info.description = field.value;
        }
    }

    for (auto& [index, file] : byIndex) {
        (void)index;
        if (!file.depotFile.empty()) {
            info.files.push_back(std::move(file));
        }
    }
    return info;
}

}  // namespace p4gw
