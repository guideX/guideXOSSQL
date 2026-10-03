#pragma once
// guideXOS SQL -- Phase SQL4
// Bounded SQL tokenizer.
//
// The tokenizer is iterative (no recursion) and never allocates based on an
// unbounded length discovered in hostile input: every literal and identifier
// is length-checked against the SQL4 resource limits before it is stored.
//
// It always appends a single EndOfInput token, so the parser can index the
// token vector safely.

#include <string>
#include <vector>

#include "database_sql_token.h"

namespace gxos {
namespace db {

class SqlTokenizer {
public:
    SqlTokenizer();

    // Tokenizes the whole input. Returns true on success and fills `out` with
    // the token stream (terminated by EndOfInput). On failure returns false and
    // fills `error` with a location-bearing diagnostic; `out` is unspecified.
    bool tokenize(const std::string& input, std::vector<SqlToken>& out,
                  SqlError& error);
};

} // namespace db
} // namespace gxos
