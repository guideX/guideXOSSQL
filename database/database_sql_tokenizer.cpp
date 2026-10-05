#include "database_sql_tokenizer.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace gxos {
namespace db {

namespace {

bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

bool isAsciiAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isWordStart(unsigned char c) {
    return isAsciiAlpha(static_cast<char>(c)) || c == '_' || c == '$' || c >= 0x80u;
}

bool isWordContinue(unsigned char c) {
    return isAsciiAlpha(static_cast<char>(c)) || isAsciiDigit(static_cast<char>(c)) ||
           c == '_' || c == '$' || c >= 0x80u;
}

bool isHexDigit(char c) {
    return isAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

uint8_t hexValue(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    return static_cast<uint8_t>(c - 'A' + 10);
}

// Parses a base-10 integer literal (with optional leading '-') into int64 with
// explicit overflow detection. Returns false when the value does not fit.
bool parseIntegerLiteral(const std::string& text, int64_t& out) {
    if (text.empty()) {
        return false;
    }
    size_t i = 0;
    bool negative = false;
    if (text[i] == '-') {
        negative = true;
        ++i;
    }
    if (i >= text.size()) {
        return false;
    }
    const uint64_t limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;
    uint64_t magnitude = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (!isAsciiDigit(c)) {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (magnitude > (limit - digit) / 10ull) {
            return false;
        }
        magnitude = magnitude * 10ull + digit;
    }
    if (negative) {
        if (magnitude == 9223372036854775808ULL) {
            out = std::numeric_limits<int64_t>::min();
        } else {
            out = -static_cast<int64_t>(magnitude);
        }
    } else {
        out = static_cast<int64_t>(magnitude);
    }
    return true;
}

SqlTokenKind keywordKind(const std::string& upper) {
    if (upper == "CREATE") return SqlTokenKind::Create;
    if (upper == "TABLE") return SqlTokenKind::Table;
    if (upper == "INSERT") return SqlTokenKind::Insert;
    if (upper == "INTO") return SqlTokenKind::Into;
    if (upper == "VALUES") return SqlTokenKind::Values;
    if (upper == "SELECT") return SqlTokenKind::Select;
    if (upper == "FROM") return SqlTokenKind::From;
    if (upper == "BEGIN") return SqlTokenKind::Begin;
    if (upper == "COMMIT") return SqlTokenKind::Commit;
    if (upper == "ROLLBACK") return SqlTokenKind::Rollback;
    if (upper == "NOT") return SqlTokenKind::Not;
    if (upper == "NULL") return SqlTokenKind::Null;
    if (upper == "BOOLEAN") return SqlTokenKind::Boolean;
    if (upper == "BOOL") return SqlTokenKind::Bool;
    if (upper == "INT") return SqlTokenKind::Int;
    if (upper == "INT32") return SqlTokenKind::Int32;
    if (upper == "INT64") return SqlTokenKind::Int64;
    if (upper == "BIGINT") return SqlTokenKind::BigInt;
    if (upper == "DOUBLE") return SqlTokenKind::Double;
    if (upper == "FLOAT64") return SqlTokenKind::Float64;
    if (upper == "TEXT") return SqlTokenKind::Text;
    if (upper == "BLOB") return SqlTokenKind::Blob;
    if (upper == "TRUE") return SqlTokenKind::True;
    if (upper == "FALSE") return SqlTokenKind::False;
    if (upper == "WHERE") return SqlTokenKind::Where;
    if (upper == "UPDATE") return SqlTokenKind::Update;
    if (upper == "SET") return SqlTokenKind::Set;
    if (upper == "DELETE") return SqlTokenKind::Delete;
    if (upper == "ORDER") return SqlTokenKind::Order;
    if (upper == "BY") return SqlTokenKind::By;
    if (upper == "ASC") return SqlTokenKind::Asc;
    if (upper == "DESC") return SqlTokenKind::Desc;
    if (upper == "LIMIT") return SqlTokenKind::Limit;
    if (upper == "OFFSET") return SqlTokenKind::Offset;
    if (upper == "AND") return SqlTokenKind::And;
    if (upper == "OR") return SqlTokenKind::Or;
    if (upper == "IS") return SqlTokenKind::Is;
    if (upper == "INDEX") return SqlTokenKind::Index;
    if (upper == "UNIQUE") return SqlTokenKind::Unique;
    if (upper == "PRIMARY") return SqlTokenKind::Primary;
    if (upper == "KEY") return SqlTokenKind::Key;
    if (upper == "ON") return SqlTokenKind::On;
    if (upper == "AS") return SqlTokenKind::As;
    if (upper == "JOIN") return SqlTokenKind::Join;
    if (upper == "INNER") return SqlTokenKind::Inner;
    if (upper == "LEFT") return SqlTokenKind::Left;
    if (upper == "OUTER") return SqlTokenKind::Outer;
    if (upper == "DISTINCT") return SqlTokenKind::Distinct;
    if (upper == "COUNT") return SqlTokenKind::Count;
    if (upper == "SUM") return SqlTokenKind::Sum;
    if (upper == "AVG") return SqlTokenKind::Avg;
    if (upper == "MIN") return SqlTokenKind::Min;
    if (upper == "MAX") return SqlTokenKind::Max;
    if (upper == "GROUP") return SqlTokenKind::Group;
    if (upper == "DEFAULT") return SqlTokenKind::Default;
    if (upper == "FOREIGN") return SqlTokenKind::Foreign;
    if (upper == "REFERENCES") return SqlTokenKind::References;
    if (upper == "DROP") return SqlTokenKind::Drop;
    if (upper == "ALTER") return SqlTokenKind::Alter;
    if (upper == "ADD") return SqlTokenKind::Add;
    if (upper == "COLUMN") return SqlTokenKind::Column;
    return SqlTokenKind::Identifier;
}

class Lexer {
public:
    Lexer(const std::string& input, std::vector<SqlToken>& out)
        : _input(input), _out(out), _index(0), _line(1), _column(1) {}

    bool run(SqlError& error) {
        _out.clear();
        if (_input.size() > kSqlMaxInputBytes) {
            error.code = SqlErrorCode::ResourceLimit;
            error.message = "SQL input exceeds the maximum allowed size";
            setLocation(error, 0, 1, 1);
            return false;
        }

        while (!atEnd()) {
            if (!skipTrivia(error)) {
                return false;
            }
            if (atEnd()) {
                break;
            }
            if (_out.size() >= kSqlMaxTokens) {
                return fail(error, SqlErrorCode::ResourceLimit,
                            "token count exceeds the maximum allowed", _index, _line,
                            _column);
            }
            const uint32_t startOffset = _index;
            const uint32_t startLine = _line;
            const uint32_t startColumn = _column;
            const char c = peek(0);
            if (c == ',') {
                advance();
                emit(SqlTokenKind::Comma, startOffset, startLine, startColumn);
            } else if (c == '(') {
                advance();
                emit(SqlTokenKind::LeftParen, startOffset, startLine, startColumn);
            } else if (c == ')') {
                advance();
                emit(SqlTokenKind::RightParen, startOffset, startLine, startColumn);
            } else if (c == ';') {
                advance();
                emit(SqlTokenKind::Semicolon, startOffset, startLine, startColumn);
            } else if (c == '*') {
                advance();
                emit(SqlTokenKind::Star, startOffset, startLine, startColumn);
            } else if (c == '=') {
                advance();
                emit(SqlTokenKind::Eq, startOffset, startLine, startColumn);
            } else if (c == '<') {
                advance();
                if (peek(0) == '>') {
                    advance();
                    emit(SqlTokenKind::Ne, startOffset, startLine, startColumn);
                } else if (peek(0) == '=') {
                    advance();
                    emit(SqlTokenKind::Le, startOffset, startLine, startColumn);
                } else {
                    emit(SqlTokenKind::Lt, startOffset, startLine, startColumn);
                }
            } else if (c == '>') {
                advance();
                if (peek(0) == '=') {
                    advance();
                    emit(SqlTokenKind::Ge, startOffset, startLine, startColumn);
                } else {
                    emit(SqlTokenKind::Gt, startOffset, startLine, startColumn);
                }
            } else if (c == '!') {
                if (peek(1) != '=') {
                    return fail(error, SqlErrorCode::TokenizerError,
                                "unexpected character '!' (did you mean '!='?)",
                                startOffset, startLine, startColumn);
                }
                advance();
                advance();
                emit(SqlTokenKind::Ne, startOffset, startLine, startColumn);
            } else if (c == '\'') {
                if (!lexString(error, startOffset, startLine, startColumn)) {
                    return false;
                }
            } else if (isAsciiDigit(c) || (c == '.' && isAsciiDigit(peek(1))) ||
                       (c == '-' &&
                        (isAsciiDigit(peek(1)) ||
                         (peek(1) == '.' && isAsciiDigit(peek(2)))))) {
                if (!lexNumber(error, startOffset, startLine, startColumn)) {
                    return false;
                }
            } else if (c == '.') {
                advance();
                emit(SqlTokenKind::Dot, startOffset, startLine, startColumn);
            } else if (isWordStart(static_cast<unsigned char>(c))) {
                if (!lexWord(error, startOffset, startLine, startColumn)) {
                    return false;
                }
            } else {
                return fail(error, SqlErrorCode::TokenizerError,
                            std::string("unexpected character '") + c + "'", startOffset,
                            startLine, startColumn);
            }
        }

        SqlToken end;
        end.kind = SqlTokenKind::EndOfInput;
        end.offset = _index;
        end.line = _line;
        end.column = _column;
        _out.push_back(end);
        return true;
    }

private:
    bool atEnd() const { return _index >= _input.size(); }

    char peek(size_t lookahead) const {
        const size_t at = _index + lookahead;
        return at < _input.size() ? _input[at] : '\0';
    }

    char advance() {
        const char c = _input[_index++];
        if (c == '\n') {
            ++_line;
            _column = 1;
        } else {
            ++_column;
        }
        return c;
    }

    void emit(SqlTokenKind kind, uint32_t offset, uint32_t line, uint32_t column) {
        SqlToken token;
        token.kind = kind;
        token.offset = offset;
        token.line = line;
        token.column = column;
        _out.push_back(token);
    }

    static void setLocation(SqlError& error, uint32_t offset, uint32_t line,
                            uint32_t column) {
        error.offset = offset;
        error.line = line;
        error.column = column;
        error.hasLocation = true;
    }

    static bool fail(SqlError& error, SqlErrorCode code, const std::string& message,
                     uint32_t offset, uint32_t line, uint32_t column) {
        error.code = code;
        error.message = message;
        setLocation(error, offset, line, column);
        return false;
    }

    bool skipTrivia(SqlError& error) {
        while (!atEnd()) {
            const char c = peek(0);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
                continue;
            }
            if (c == '-' && peek(1) == '-') {
                advance();
                advance();
                while (!atEnd() && peek(0) != '\n') {
                    advance();
                }
                continue;
            }
            if (c == '/' && peek(1) == '*') {
                const uint32_t startOffset = _index;
                const uint32_t startLine = _line;
                const uint32_t startColumn = _column;
                advance();
                advance();
                bool closed = false;
                while (!atEnd()) {
                    if (peek(0) == '*' && peek(1) == '/') {
                        advance();
                        advance();
                        closed = true;
                        break;
                    }
                    advance();
                }
                if (!closed) {
                    return fail(error, SqlErrorCode::TokenizerError,
                                "unterminated block comment", startOffset, startLine,
                                startColumn);
                }
                continue;
            }
            break;
        }
        return true;
    }

    bool lexString(SqlError& error, uint32_t offset, uint32_t line, uint32_t column) {
        advance(); // opening quote
        std::string value;
        while (!atEnd()) {
            const char c = advance();
            if (c == '\'') {
                if (peek(0) == '\'') {
                    value.push_back('\'');
                    advance();
                } else {
                    SqlToken token;
                    token.kind = SqlTokenKind::StringLiteral;
                    token.text = value;
                    token.offset = offset;
                    token.line = line;
                    token.column = column;
                    _out.push_back(token);
                    return true;
                }
            } else {
                value.push_back(c);
            }
            if (value.size() > kSqlMaxStringLiteralBytes) {
                return fail(error, SqlErrorCode::ResourceLimit,
                            "string literal exceeds the maximum allowed length", offset,
                            line, column);
            }
        }
        return fail(error, SqlErrorCode::TokenizerError, "unterminated string literal",
                    offset, line, column);
    }

    bool lexBlob(SqlError& error, uint32_t offset, uint32_t line, uint32_t column) {
        advance(); // opening quote
        std::string hex;
        while (!atEnd() && peek(0) != '\'') {
            const uint32_t charOffset = _index;
            const uint32_t charLine = _line;
            const uint32_t charColumn = _column;
            const char c = advance();
            if (!isHexDigit(c)) {
                return fail(error, SqlErrorCode::TokenizerError,
                            std::string("invalid hexadecimal digit '") + c +
                                "' in blob literal",
                            charOffset, charLine, charColumn);
            }
            hex.push_back(c);
            if (hex.size() > 2u * kSqlMaxBlobLiteralBytes) {
                return fail(error, SqlErrorCode::ResourceLimit,
                            "blob literal exceeds the maximum allowed length", offset,
                            line, column);
            }
        }
        if (atEnd()) {
            return fail(error, SqlErrorCode::TokenizerError, "unterminated blob literal",
                        offset, line, column);
        }
        advance(); // closing quote
        if (hex.size() % 2u != 0u) {
            return fail(error, SqlErrorCode::TokenizerError,
                        "blob literal must contain an even number of hex digits",
                        offset, line, column);
        }
        SqlToken token;
        token.kind = SqlTokenKind::BlobLiteral;
        token.offset = offset;
        token.line = line;
        token.column = column;
        token.bytes.reserve(hex.size() / 2u);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            token.bytes.push_back(
                static_cast<uint8_t>((hexValue(hex[i]) << 4) | hexValue(hex[i + 1])));
        }
        _out.push_back(token);
        return true;
    }

    bool lexNumber(SqlError& error, uint32_t offset, uint32_t line, uint32_t column) {
        std::string text;
        bool isFloat = false;
        bool sawDigit = false;
        if (peek(0) == '-') {
            text.push_back(advance());
        }
        while (!atEnd() && isAsciiDigit(peek(0))) {
            text.push_back(advance());
            sawDigit = true;
        }
        if (!atEnd() && peek(0) == '.') {
            isFloat = true;
            text.push_back(advance());
            while (!atEnd() && isAsciiDigit(peek(0))) {
                text.push_back(advance());
                sawDigit = true;
            }
        }
        if (!atEnd() && (peek(0) == 'e' || peek(0) == 'E')) {
            isFloat = true;
            text.push_back(advance());
            if (!atEnd() && (peek(0) == '+' || peek(0) == '-')) {
                text.push_back(advance());
            }
            bool exponentDigit = false;
            while (!atEnd() && isAsciiDigit(peek(0))) {
                text.push_back(advance());
                exponentDigit = true;
            }
            if (!exponentDigit) {
                return fail(error, SqlErrorCode::TokenizerError,
                            "malformed numeric literal exponent", offset, line, column);
            }
        }
        if (!sawDigit) {
            return fail(error, SqlErrorCode::TokenizerError, "malformed numeric literal",
                        offset, line, column);
        }
        if (text.size() > kSqlMaxNumericLiteralBytes) {
            return fail(error, SqlErrorCode::ResourceLimit,
                        "numeric literal exceeds the maximum allowed length", offset,
                        line, column);
        }

        SqlToken token;
        token.offset = offset;
        token.line = line;
        token.column = column;
        if (isFloat) {
            const double value = std::strtod(text.c_str(), nullptr);
            if (!std::isfinite(value)) {
                return fail(error, SqlErrorCode::TokenizerError,
                            "float literal is out of range", offset, line, column);
            }
            token.kind = SqlTokenKind::FloatLiteral;
            token.float64Value = value;
        } else {
            int64_t value = 0;
            if (!parseIntegerLiteral(text, value)) {
                return fail(error, SqlErrorCode::TokenizerError,
                            "integer literal overflow", offset, line, column);
            }
            token.kind = SqlTokenKind::IntegerLiteral;
            token.int64Value = value;
        }
        _out.push_back(token);
        return true;
    }

    bool lexWord(SqlError& error, uint32_t offset, uint32_t line, uint32_t column) {
        std::string word;
        while (!atEnd() && isWordContinue(static_cast<unsigned char>(peek(0)))) {
            word.push_back(advance());
        }

        // Blob literal: the token 'X' immediately followed by a single quote.
        if ((word == "X" || word == "x") && peek(0) == '\'') {
            return lexBlob(error, offset, line, column);
        }

        std::string upper;
        upper.reserve(word.size());
        bool asciiOnly = true;
        for (size_t i = 0; i < word.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(word[i]);
            if (c >= 0x80u) {
                asciiOnly = false;
                break;
            }
            if (c >= 'a' && c <= 'z') {
                upper.push_back(static_cast<char>(c - 'a' + 'A'));
            } else {
                upper.push_back(static_cast<char>(c));
            }
        }

        if (asciiOnly) {
            const SqlTokenKind kind = keywordKind(upper);
            if (kind != SqlTokenKind::Identifier) {
                SqlToken token;
                token.kind = kind;
                token.text = word;
                token.offset = offset;
                token.line = line;
                token.column = column;
                _out.push_back(token);
                return true;
            }
        }

        if (word.size() > kSqlMaxIdentifierBytes) {
            return fail(error, SqlErrorCode::ResourceLimit,
                        "identifier exceeds the maximum allowed length", offset, line,
                        column);
        }
        SqlToken token;
        token.kind = SqlTokenKind::Identifier;
        token.text = word;
        token.offset = offset;
        token.line = line;
        token.column = column;
        _out.push_back(token);
        return true;
    }

    const std::string& _input;
    std::vector<SqlToken>& _out;
    size_t _index;
    uint32_t _line;
    uint32_t _column;
};

} // namespace

SqlTokenizer::SqlTokenizer() {}

bool SqlTokenizer::tokenize(const std::string& input, std::vector<SqlToken>& out,
                            SqlError& error) {
    error = SqlError();
    Lexer lexer(input, out);
    return lexer.run(error);
}

} // namespace db
} // namespace gxos
