#include "word_db.hh"

#include "utils.hh"
#include "line_modification.hh"
#include "utf8_iterator.hh"

namespace Kakoune
{

static std::vector<InternedString> get_words(StringView content)
{
    std::vector<InternedString> res;
    using Iterator = utf8::iterator<const char*, utf8::InvalidPolicy::Pass>;
    const char* word_start = content.begin();
    bool in_word = false;
    for (Iterator it{word_start}, end{content.end()}; it != end; ++it)
    {
        Codepoint c = *it;
        const bool word = is_word(c);
        if (not in_word and word)
        {
            word_start = it.base();
            in_word = true;
        }
        else if (in_word and not word)
        {
            res.push_back(StringView{word_start, it.base()});
            in_word = false;
        }
    }
    return res;
}

static void add_words(WordDB::WordList& wl, const std::vector<InternedString>& words)
{
    for (auto& w : words)
        ++wl[w];
}

static void remove_words(WordDB::WordList& wl, const std::vector<InternedString>& words)
{
    for (auto& w : words)
    {
        auto it = wl.find(w);
        kak_assert(it != wl.end() and it->second > 0);
        if (--it->second == 0)
            wl.erase(it);
    }
}

WordDB::WordDB(const Buffer& buffer)
    : m_buffer{&buffer}, m_timestamp{buffer.timestamp()}
{
    m_line_to_words.reserve((int)buffer.line_count());
    for (auto line = 0_line, end = buffer.line_count(); line < end; ++line)
    {
        m_line_to_words.push_back(get_words(buffer[line]));
        add_words(m_words, m_line_to_words.back());
    }
}

void WordDB::update_db()
{
    auto& buffer = *m_buffer;

    auto modifs = compute_line_modifications(buffer, m_timestamp);
    m_timestamp = buffer.timestamp();

    if (modifs.empty())
        return;


    LineToWords new_lines;
    new_lines.reserve((int)buffer.line_count());

    auto old_line = 0_line;
    for (auto& modif : modifs)
    {
        kak_assert(0_line <= modif.new_line and modif.new_line < buffer.line_count());
        kak_assert(old_line <= modif.old_line);
        while (old_line < modif.old_line)
            new_lines.push_back(std::move(m_line_to_words[(int)old_line++]));

        kak_assert((int)new_lines.size() == (int)modif.new_line);

        while (old_line <= modif.old_line + modif.num_removed)
        {
            kak_assert(old_line < m_line_to_words.size());
            remove_words(m_words, m_line_to_words[(int)old_line++]);
        }

        for (auto l = 0_line; l <= modif.num_added; ++l)
        {
            if (modif.new_line + l >= buffer.line_count())
                break;

            new_lines.push_back(get_words(buffer[modif.new_line + l]));
            add_words(m_words, new_lines.back());
        }
    }
    while (old_line != (int)m_line_to_words.size())
        new_lines.push_back(std::move(m_line_to_words[(int)old_line++]));

    m_line_to_words = std::move(new_lines);
}

std::vector<InternedString> WordDB::find_prefix(StringView prefix)
{
    update_db();

    std::vector<InternedString> res;
    for (auto it = m_words.lower_bound(prefix); it != m_words.end(); ++it)
    {
        if (not prefix_match(it->first, prefix))
            break;
        res.push_back(it->first);
    }
    return res;
}

std::vector<InternedString> WordDB::find_subsequence(StringView subsequence)
{
    update_db();

    std::vector<InternedString> res;
    for (auto it = m_words.begin(); it != m_words.end(); ++it)
    {
        if (subsequence_match(it->first, subsequence))
            res.push_back(it->first);
    }
    return res;
}

int WordDB::get_word_occurences(StringView word) const
{
    auto it = m_words.find(word);
    if (it != m_words.end())
        return it->second;
    return 0;
}

}
