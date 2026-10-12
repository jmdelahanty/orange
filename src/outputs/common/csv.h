#pragma once

// One CSV reader and writer for Orange outputs (RFC 4180): fields separated by ',', a field
// containing ',', '"', CR or LF is quoted with '"' and its quotes doubled. A line's trailing CR
// (CRLF endings) is dropped; an empty last field is kept ("a,b," has three fields).

#include <cstddef>
#include <string>
#include <vector>

namespace orange::outputs {

// Parses one CSV line (no embedded line breaks). Returns false on an unterminated quoted field
// or a quote inside an unquoted field, leaving *fields with what was parsed.
inline bool parse_csv_row(const std::string& line, std::vector<std::string>* fields,
                          std::string* error = nullptr) {
    fields->clear();
    std::size_t end = line.size();
    if (end > 0 && line[end - 1] == '\r') {
        --end;
    }
    std::string field;
    bool quoted = false;
    bool after_quote = false;
    for (std::size_t i = 0; i < end; ++i) {
        const char ch = line[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < end && line[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    quoted = false;
                    after_quote = true;
                }
            } else {
                field += ch;
            }
        } else if (ch == ',') {
            fields->push_back(std::move(field));
            field.clear();
            after_quote = false;
        } else if (ch == '"' && field.empty() && !after_quote) {
            quoted = true;
        } else if (ch == '"' || after_quote) {
            if (error) *error = "unexpected character after or inside an unquoted field at column " + std::to_string(i);
            fields->push_back(std::move(field));
            return false;
        } else {
            field += ch;
        }
    }
    fields->push_back(std::move(field));
    if (quoted) {
        if (error) *error = "unterminated quoted field";
        return false;
    }
    return true;
}

// Convenience form: the fields, best effort on malformed input.
inline std::vector<std::string> split_csv_row(const std::string& line) {
    std::vector<std::string> fields;
    parse_csv_row(line, &fields);
    return fields;
}

inline std::string csv_field(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) {
        return value;
    }
    std::string quoted = "\"";
    for (const char ch : value) {
        quoted += ch;
        if (ch == '"') {
            quoted += '"';
        }
    }
    return quoted + "\"";
}

// Fields joined with ',', each escaped; no line terminator.
inline std::string csv_row(const std::vector<std::string>& fields) {
    std::string row;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            row += ',';
        }
        row += csv_field(fields[i]);
    }
    return row;
}

// Column position of `name` in a header row, or npos.
inline std::size_t csv_column(const std::vector<std::string>& header, const std::string& name) {
    for (std::size_t i = 0; i < header.size(); ++i) {
        if (header[i] == name) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

}  // namespace orange::outputs
