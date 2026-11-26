#ifndef OPENCMR2_NET_JOIN_CODE_H
#define OPENCMR2_NET_JOIN_CODE_H

#include "net/lobby_wire.h"

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace net {
namespace joincode {

constexpr char kAlphabet[33] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr size_t kMaxInput = 64;
constexpr size_t kTextSize = lobby::kCodeLength + 2;

inline int SymbolValue(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c == 'O')
        return 0;
    if (c == 'I' || c == 'L')
        return 1;
    for (int i = 0; i < 32; i++) {
        if (kAlphabet[i] == c)
            return i;
    }
    return -1;
}

inline bool IsSeparator(char c)
{
    return c == '-' || c == ' ' || c == '\t';
}

inline bool Parse(const char *text, lobby::Code &code)
{
    if (text == nullptr)
        return false;
    lobby::Code parsed = {};
    size_t count = 0;
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (i == kMaxInput)
            return false;
        if (IsSeparator(text[i]))
            continue;
        int value = SymbolValue(text[i]);
        if (value < 0 || count == lobby::kCodeLength)
            return false;
        parsed[count++] = kAlphabet[value];
    }
    if (count != lobby::kCodeLength)
        return false;
    code = parsed;
    return true;
}

inline bool IsCanonical(const lobby::Code &code)
{
    for (char c : code) {
        int value = SymbolValue(c);
        if (value < 0 || kAlphabet[value] != c)
            return false;
    }
    return true;
}

inline lobby::Code FromBits(uint64_t bits)
{
    lobby::Code code = {};
    for (size_t i = 0; i < lobby::kCodeLength; i++)
        code[i] = kAlphabet[(bits >> (5 * (lobby::kCodeLength - 1 - i))) & 31];
    return code;
}

inline std::string Format(const lobby::Code &code)
{
    std::string text(code.data(), lobby::kCodeLength / 2);
    text += '-';
    text.append(code.data() + lobby::kCodeLength / 2, lobby::kCodeLength / 2);
    return text;
}

}
}

#endif
