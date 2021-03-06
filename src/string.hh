#ifndef string_hh_INCLUDED
#define string_hh_INCLUDED

#include "memoryview.hh"
#include "units.hh"
#include "utf8.hh"

#include <string>
#include <boost/regex.hpp>

namespace Kakoune
{

using Regex = boost::regex;

class StringView;

class String : public std::string
{
public:
    String() {}
    String(const char* content) : std::string(content) {}
    String(std::string content) : std::string(std::move(content)) {}
    explicit String(char content, CharCount count = 1) : std::string((size_t)(int)count, content) {}
    explicit String(Codepoint cp, CharCount count = 1)
    {
        while (count-- > 0)
            utf8::dump(back_inserter(*this), cp);
    }
    template<typename Iterator>
    String(Iterator begin, Iterator end) : std::string(begin, end) {}

    std::string& stdstr() { return *this; }
    const std::string& stdstr() const { return *this; }

    [[gnu::always_inline]]
    char      operator[](ByteCount pos) const { return std::string::operator[]((int)pos); }
    [[gnu::always_inline]]
    char&     operator[](ByteCount pos) { return std::string::operator[]((int)pos); }
    Codepoint operator[](CharCount pos) { return utf8::codepoint(utf8::advance(begin(), end(), pos), end()); }

    [[gnu::always_inline]]
    ByteCount length() const { return ByteCount{(int)std::string::length()}; }
    CharCount char_length() const { return utf8::distance(begin(), end()); }
    ByteCount byte_count_to(CharCount count) const { return utf8::advance(begin(), end(), (int)count) - begin(); }
    CharCount char_count_to(ByteCount count) const { return utf8::distance(begin(), begin() + (int)count); }

    String  operator+(const String& other) const { return String{stdstr() + other.stdstr()}; }
    String& operator+=(const String& other) { std::string::operator+=(other); return *this; }
    String  operator+(const char* other) const { return String{stdstr() + other}; }
    String& operator+=(const char* other) { std::string::operator+=(other); return *this; }
    String  operator+(char other) const { return String{stdstr() + other}; }
    String& operator+=(char other) { std::string::operator+=(other); return *this; }
    String  operator+(Codepoint cp) const { String res = *this; utf8::dump(back_inserter(res), cp); return res; }
    String& operator+=(Codepoint cp) { utf8::dump(back_inserter(*this), cp); return *this; }

    StringView substr(ByteCount pos, ByteCount length = INT_MAX) const;
    StringView substr(CharCount pos, CharCount length = INT_MAX) const;
};

class StringView
{
public:
    constexpr StringView() : m_data{nullptr}, m_length{0} {}
    constexpr StringView(const char* data, ByteCount length)
        : m_data{data}, m_length{length} {}
    StringView(const char* data) : m_data{data}, m_length{(int)strlen(data)} {}
    constexpr StringView(const char* begin, const char* end) : m_data{begin}, m_length{(int)(end - begin)} {}
    StringView(const std::string& str) : m_data{str.data()}, m_length{(int)str.length()} {}

    bool operator==(StringView other) const;
    bool operator!=(StringView other) const;

    [[gnu::always_inline]]
    const char* data() const { return m_data; }

    using iterator = const char*;
    using reverse_iterator = std::reverse_iterator<const char*>;

    iterator begin() const { return m_data; }
    iterator end() const { return m_data + (int)m_length; }

    reverse_iterator rbegin() const { return reverse_iterator{m_data + (int)m_length}; }
    reverse_iterator rend() const { return reverse_iterator{m_data}; }

    char front() const { return *m_data; }
    char back() const { return m_data[(int)m_length - 1]; }

    [[gnu::always_inline]]
    char operator[](ByteCount pos) const { return m_data[(int)pos]; }
    Codepoint operator[](CharCount pos) { return utf8::codepoint(utf8::advance(begin(), end(), pos), end()); }

    [[gnu::always_inline]]
    ByteCount length() const { return m_length; }
    CharCount char_length() const { return utf8::distance(begin(), end()); }

    [[gnu::always_inline]]
    bool empty() { return m_length == 0_byte; }

    ByteCount byte_count_to(CharCount count) const;
    CharCount char_count_to(ByteCount count) const;

    StringView substr(ByteCount from, ByteCount length = INT_MAX) const;
    StringView substr(CharCount from, CharCount length = INT_MAX) const;

    String str() const { return String{begin(), end()}; }

    operator String() const { return str(); } // to remove

    struct ZeroTerminatedString
    {
        ZeroTerminatedString(const char* begin, const char* end)
        {
            if (*end == '\0')
                unowned = begin;
            else
                owned = std::string(begin, end);
        }
        operator const char*() const { return unowned ? unowned : owned.c_str(); }

    private:
        std::string owned;
        const char* unowned = nullptr;

    };
    ZeroTerminatedString zstr() const { return ZeroTerminatedString{begin(), end()}; }

private:
    const char* m_data;
    ByteCount m_length;
};

inline bool StringView::operator==(StringView other) const
{
    return m_length == other.m_length and memcmp(m_data, other.m_data, (int)m_length) == 0;
}

inline bool StringView::operator!=(StringView other) const
{
    return !this->operator==(other);
}

bool operator<(StringView lhs, StringView rhs);

inline bool operator==(const char* lhs, StringView rhs)
{
    return StringView{lhs} == rhs;
}

inline bool operator!=(const char* lhs, StringView rhs)
{
    return StringView{lhs} != rhs;
}

inline bool operator==(const std::string& lhs, StringView rhs)
{
    return StringView{lhs} == rhs;
}

inline bool operator!=(const std::string& lhs, StringView rhs)
{
    return StringView{lhs} != rhs;
}

inline ByteCount StringView::byte_count_to(CharCount count) const
{
    return utf8::advance(begin(), end(), (int)count) - begin();
}
inline CharCount StringView::char_count_to(ByteCount count) const
{
    return utf8::distance(begin(), begin() + (int)count);
}

inline StringView StringView::substr(ByteCount from, ByteCount length) const
{
    if (length < 0)
        length = INT_MAX;
    return StringView{ m_data + (int)from, std::min(m_length - from, length) };
}

inline StringView StringView::substr(CharCount from, CharCount length) const
{
    if (length < 0)
        length = INT_MAX;
    auto beg = utf8::advance(begin(), end(), (int)from);
    return StringView{ beg, utf8::advance(beg, end(), length) };
}

inline StringView String::substr(ByteCount pos, ByteCount length) const
{
    return StringView{*this}.substr(pos, length);
}

inline StringView String::substr(CharCount pos, CharCount length) const
{
    return StringView{*this}.substr(pos, length);
}

inline const char* begin(StringView str)
{
    return str.begin();
}

inline const char* end(StringView str)
{
    return str.end();
}

inline String operator+(const char* lhs, const String& rhs)
{
    return String(lhs) + rhs;
}

inline String operator+(const std::string& lhs, const String& rhs)
{
    return String(lhs) + rhs;
}

inline String operator+(const String& lhs, const std::string& rhs)
{
    return lhs + String(rhs);
}

inline String& operator+=(String& lhs, StringView rhs)
{
    lhs.append(rhs.data(), (size_t)(int)rhs.length());
    return lhs;
}

inline String operator+(const char* lhs, StringView rhs)
{
    String res = lhs;
    res += rhs;
    return res;
}

inline String operator+(const String& lhs, StringView rhs)
{
    String res = lhs;
    res += rhs;
    return res;
}

inline String operator+(StringView lhs, const String& rhs)
{
    String res{lhs.begin(), lhs.end()};
    res.append(rhs);
    return res;
}

inline String operator+(StringView lhs, const char* rhs)
{
    String res{lhs.begin(), lhs.end()};
    res.append(rhs);
    return res;
}

inline String operator+(char lhs, const String& rhs)
{
    return String(lhs) + rhs;
}

inline String operator+(Codepoint lhs, const String& rhs)
{
    return String(lhs) + rhs;
}

inline String operator+(StringView lhs, StringView rhs)
{
    String res{lhs.begin(), lhs.end()};
    res += rhs;
    return res;
}

std::vector<String> split(StringView str, char separator, char escape = 0);
String escape(StringView str, char character, char escape);
String escape(StringView str, StringView characters, char escape);

inline String operator"" _str(const char* str, size_t)
{
    return String(str);
}

inline String codepoint_to_str(Codepoint cp)
{
    std::string str;
    utf8::dump(back_inserter(str), cp);
    return String(str);
}

String option_to_string(const Regex& re);
void option_from_string(StringView str, Regex& re);

int str_to_int(StringView str);

String to_string(int val);

template<typename RealType, typename ValueType>
String to_string(const StronglyTypedNumber<RealType, ValueType>& val)
{
    return to_string((ValueType)val);
}

bool prefix_match(StringView str, StringView prefix);
bool subsequence_match(StringView str, StringView subseq);

String expand_tabs(StringView line, CharCount tabstop, CharCount col = 0);

size_t hash_data(const char* data, size_t len);

}

namespace std
{
    template<>
    struct hash<Kakoune::String> : hash<std::string>
    {
        size_t operator()(const Kakoune::String& str) const
        {
            return hash<std::string>::operator()(str);
        }
    };

    template<>
    struct hash<Kakoune::StringView>
    {
        size_t operator()(Kakoune::StringView str) const
        {
            return Kakoune::hash_data(str.data(), (int)str.length());
        }
    };
}

#endif // string_hh_INCLUDED

