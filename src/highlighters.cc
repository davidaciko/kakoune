#include "highlighters.hh"

#include "assert.hh"
#include "buffer_utils.hh"
#include "context.hh"
#include "display_buffer.hh"
#include "face_registry.hh"
#include "highlighter_group.hh"
#include "line_modification.hh"
#include "option_types.hh"
#include "parameters_parser.hh"
#include "register_manager.hh"
#include "string.hh"
#include "utf8.hh"
#include "utf8_iterator.hh"

#include <sstream>
#include <locale>

namespace Kakoune
{

using namespace std::placeholders;

using RegexIterator = boost::regex_iterator<BufferIterator>;

template<typename T>
void highlight_range(DisplayBuffer& display_buffer,
                     ByteCoord begin, ByteCoord end,
                     bool skip_replaced, T func)
{
    if (begin == end or end <= display_buffer.range().first
                     or begin >= display_buffer.range().second)
        return;

    for (auto& line : display_buffer.lines())
    {
        auto& range = line.range();
        if (range.second <= begin or  end < range.first)
            continue;

        for (auto atom_it = line.begin(); atom_it != line.end(); ++atom_it)
        {
            bool is_replaced = atom_it->type() == DisplayAtom::ReplacedBufferRange;

            if (not atom_it->has_buffer_range() or
                (skip_replaced and is_replaced))
                continue;

            if (end <= atom_it->begin() or begin >= atom_it->end())
                continue;

            if (not is_replaced and begin > atom_it->begin())
                atom_it = ++line.split(atom_it, begin);

            if (not is_replaced and end < atom_it->end())
            {
                atom_it = line.split(atom_it, end);
                func(*atom_it);
                ++atom_it;
            }
            else
                func(*atom_it);
        }
    }
}

template<typename T>
void apply_highlighter(const Context& context,
                       HighlightFlags flags,
                       DisplayBuffer& display_buffer,
                       ByteCoord begin, ByteCoord end,
                       T&& highlighter)
{
    using LineIterator = DisplayBuffer::LineList::iterator;
    LineIterator first_line;
    std::vector<DisplayLine::iterator> insert_pos;
    auto line_end = display_buffer.lines().end();

    DisplayBuffer region_display;
    auto& region_lines = region_display.lines();
    for (auto line_it = display_buffer.lines().begin(); line_it != line_end; ++line_it)
    {
        auto& line = *line_it;
        auto& range = line.range();
        if (range.second <= begin or end <= range.first)
            continue;

        if (region_lines.empty())
            first_line = line_it;
        region_lines.emplace_back();
        insert_pos.emplace_back();

        if (range.first < begin or range.second > end)
        {
            size_t beg_idx = 0;
            size_t end_idx = line.atoms().size();

            for (auto atom_it = line.begin(); atom_it != line.end(); ++atom_it)
            {
                if (not atom_it->has_buffer_range() or end <= atom_it->begin() or begin >= atom_it->end())
                    continue;

                bool is_replaced = atom_it->type() == DisplayAtom::ReplacedBufferRange;
                if (atom_it->begin() <= begin)
                {
                    if (is_replaced or atom_it->begin() == begin)
                        beg_idx = atom_it - line.begin();
                    else
                    {
                        atom_it = ++line.split(atom_it, begin);
                        beg_idx = atom_it - line.begin();
                        ++end_idx;
                    }
                }

                if (atom_it->end() >= end)
                {
                    if (is_replaced or atom_it->end() == end)
                        end_idx = atom_it - line.begin() + 1;
                    else
                    {
                        atom_it = ++line.split(atom_it, end);
                        end_idx = atom_it - line.begin();
                    }
                }
            }
            std::move(line.begin() + beg_idx, line.begin() + end_idx,
                      std::back_inserter(region_lines.back()));
            insert_pos.back() = line.erase(line.begin() + beg_idx, line.begin() + end_idx);
        }
        else
        {
            region_lines.back() = std::move(line);
            insert_pos.back() = line.begin();
        }
    }

    region_display.compute_range();
    highlighter(context, flags, region_display);

    for (size_t i = 0; i < region_lines.size(); ++i)
    {
        auto& line = *(first_line + i);
        auto pos = insert_pos[i];
        for (auto& atom : region_lines[i])
            pos = ++line.insert(pos, std::move(atom));
    }
    display_buffer.compute_range();
}

auto apply_face = [](const Face& face)
{
    return [&face](DisplayAtom& atom) {
        if (face.fg != Colors::Default)
            atom.face.fg = face.fg;
        if (face.bg != Colors::Default)
            atom.face.bg = face.bg;
        if (face.attributes != Attribute::Normal)
            atom.face.attributes |= face.attributes;
    };
};

using FacesSpec = std::vector<String>;

HighlighterAndId fill_factory(HighlighterParameters params)
{
    if (params.size() != 1)
        throw runtime_error("wrong parameter count");

    const String& facespec = params[0];
    get_face(facespec); // validate param

    auto fill = [facespec](const Context& context, HighlightFlags flags,
                            DisplayBuffer& display_buffer)
    {
        auto range = display_buffer.range();
        highlight_range(display_buffer, range.first, range.second, true,
                        apply_face(get_face(facespec)));
    };
    return HighlighterAndId("fill_" + params[0], fill);
}

template<typename T>
struct BufferSideCache
{
    BufferSideCache() : m_id{ValueId::get_free_id()} {}

    T& get(const Buffer& buffer)
    {
        Value& cache_val = buffer.values()[m_id];
        if (not cache_val)
            cache_val = Value(T{});
        return cache_val.as<T>();
    }
private:
    ValueId m_id;
};

class RegexHighlighter
{
public:
    RegexHighlighter(Regex regex, FacesSpec faces)
        : m_regex{std::move(regex)}, m_faces{std::move(faces)}
    {
    }

    void operator()(const Context& context, HighlightFlags flags, DisplayBuffer& display_buffer)
    {
        if (flags != HighlightFlags::Highlight)
            return;

        std::vector<Optional<Face>> faces(m_faces.size());
        auto& cache = update_cache_ifn(context.buffer(), display_buffer.range());
        for (auto& match : cache.m_matches)
        {
            for (size_t n = 0; n < match.size(); ++n)
            {
                if (n >= m_faces.size() or m_faces[n].empty())
                    continue;
                if (not faces[n])
                    faces[n] = get_face(m_faces[n]);

                highlight_range(display_buffer, match[n].first, match[n].second, true,
                                apply_face(*faces[n]));
            }
        }
    }

private:
    struct Cache
    {
        std::pair<LineCount, LineCount> m_range;
        size_t m_timestamp = 0;
        std::vector<std::vector<std::pair<ByteCoord, ByteCoord>>> m_matches;
    };
    BufferSideCache<Cache> m_cache;

    Regex     m_regex;
    FacesSpec m_faces;

    Cache& update_cache_ifn(const Buffer& buffer, const BufferRange& range)
    {
        Cache& cache = m_cache.get(buffer);

        LineCount first_line = range.first.line;
        LineCount last_line = std::min(buffer.line_count()-1, range.second.line);

        if (buffer.timestamp() == cache.m_timestamp and
            first_line >= cache.m_range.first and
            last_line <= cache.m_range.second)
           return cache;

        cache.m_range.first  = std::max(0_line, first_line - 10);
        cache.m_range.second = std::min(buffer.line_count()-1, last_line+10);
        cache.m_timestamp = buffer.timestamp();

        cache.m_matches.clear();
        RegexIterator re_it{buffer.iterator_at(cache.m_range.first),
                            buffer.iterator_at(cache.m_range.second+1), m_regex};
        RegexIterator re_end;
        for (; re_it != re_end; ++re_it)
        {
            cache.m_matches.emplace_back();
            auto& match = cache.m_matches.back();
            for (auto& sub : *re_it)
                match.emplace_back(sub.first.coord(), sub.second.coord());
        }
        return cache;
    }
};

HighlighterAndId highlight_regex_factory(HighlighterParameters params)
{
    if (params.size() < 2)
        throw runtime_error("wrong parameter count");

    try
    {
        static Regex face_spec_ex(R"((\d+):(.*))");
        FacesSpec faces;
        for (auto it = params.begin() + 1;  it != params.end(); ++it)
        {
            boost::smatch res;
            if (not boost::regex_match(it->begin(), it->end(), res, face_spec_ex))
                throw runtime_error("wrong face spec: '" + *it +
                                     "' expected <capture>:<facespec>");
            get_face(res[2].str()); // throw if wrong face spec
            int capture = str_to_int(res[1].str());
            if (capture >= faces.size())
                faces.resize(capture+1);
            faces[capture] = res[2].str();
        }

        String id = "hlregex'" + params[0] + "'";

        Regex ex{params[0].begin(), params[0].end(), Regex::optimize};

        return HighlighterAndId(id, RegexHighlighter(std::move(ex),
                                                   std::move(faces)));
    }
    catch (boost::regex_error& err)
    {
        throw runtime_error(String("regex error: ") + err.what());
    }
}

template<typename RegexGetter, typename FaceGetter>
class DynamicRegexHighlighter
{
public:
    DynamicRegexHighlighter(RegexGetter regex_getter, FaceGetter face_getter)
        : m_regex_getter(std::move(regex_getter)),
          m_face_getter(std::move(face_getter)),
          m_highlighter(Regex(), FacesSpec{}) {}

    void operator()(const Context& context, HighlightFlags flags, DisplayBuffer& display_buffer)
    {
        if (flags != HighlightFlags::Highlight)
            return;

        Regex regex = m_regex_getter(context);
        FacesSpec face = m_face_getter(context);
        if (regex != m_last_regex or face != m_last_face)
        {
            m_last_regex = regex;
            m_last_face = face;
            if (not m_last_regex.empty())
                m_highlighter= RegexHighlighter{m_last_regex, face};
        }
        if (not m_last_regex.empty() and not m_last_face.empty())
            m_highlighter(context, flags, display_buffer);
    }

private:
    Regex       m_last_regex;
    RegexGetter m_regex_getter;

    FacesSpec   m_last_face;
    FaceGetter  m_face_getter;

    RegexHighlighter m_highlighter;
};

template<typename RegexGetter, typename FaceGetter>
DynamicRegexHighlighter<RegexGetter, FaceGetter>
make_dynamic_regex_highlighter(RegexGetter regex_getter, FaceGetter face_getter)
{
    return DynamicRegexHighlighter<RegexGetter, FaceGetter>(
        std::move(regex_getter), std::move(face_getter));
}


HighlighterAndId highlight_search_factory(HighlighterParameters params)
{
    if (params.size() != 0)
        throw runtime_error("wrong parameter count");
        auto get_face = [](const Context& context){
            return FacesSpec{ { "Search" } };
        };
        auto get_regex = [](const Context&){
            auto s = Context().main_sel_register_value("/");
            try
            {
                return s.empty() ? Regex{} : Regex{s.begin(), s.end()};
            }
            catch (boost::regex_error& err)
            {
                return Regex{};
            }
        };
        return {"hlsearch", make_dynamic_regex_highlighter(get_regex, get_face)};
}

HighlighterAndId highlight_regex_option_factory(HighlighterParameters params)
{
    if (params.size() != 2)
        throw runtime_error("wrong parameter count");

    String facespec = params[1];
    auto get_face = [=](const Context&){
        return FacesSpec{ { facespec } };
    };

    String option_name = params[0];
    // verify option type now
    GlobalOptions::instance()[option_name].get<Regex>();

    auto get_regex = [option_name](const Context& context){
        return context.options()[option_name].get<Regex>();
    };
    return {"hloption_" + option_name, make_dynamic_regex_highlighter(get_regex, get_face)};
}

HighlighterAndId highlight_line_option_factory(HighlighterParameters params)
{
    if (params.size() != 2)
        throw runtime_error("wrong parameter count");

    String facespec = params[1];
    get_face(facespec); // validate facespec

    String option_name = params[0];
    // verify option type now
    GlobalOptions::instance()[option_name].get<int>();

    auto highlighter = [=](const Context& context, HighlightFlags flags,
                           DisplayBuffer& display_buffer)
    {
        int line = context.options()[option_name].get<int>();
        highlight_range(display_buffer, {line-1, 0}, {line, 0}, false,
                        apply_face(get_face(facespec)));
    };

    return {"hlline_" + option_name, std::move(highlighter)};
}

void expand_tabulations(const Context& context, HighlightFlags flags, DisplayBuffer& display_buffer)
{
    const int tabstop = context.options()["tabstop"].get<int>();
    auto& buffer = context.buffer();
    for (auto& line : display_buffer.lines())
    {
        for (auto atom_it = line.begin(); atom_it != line.end(); ++atom_it)
        {
            if (atom_it->type() != DisplayAtom::BufferRange)
                continue;

            auto begin = buffer.iterator_at(atom_it->begin());
            auto end = buffer.iterator_at(atom_it->end());
            for (BufferIterator it = begin; it != end; ++it)
            {
                if (*it == '\t')
                {
                    if (it != begin)
                        atom_it = ++line.split(atom_it, it.coord());
                    if (it+1 != end)
                        atom_it = line.split(atom_it, (it+1).coord());

                    int column = (int)get_column(buffer, tabstop, it.coord());
                    int count = tabstop - (column % tabstop);
                    String padding;
                    for (int i = 0; i < count; ++i)
                        padding += ' ';
                    atom_it->replace(padding);
                    break;
                }
            }
        }
    }
}

void show_whitespaces(const Context& context, HighlightFlags flags, DisplayBuffer& display_buffer)
{
    const int tabstop = context.options()["tabstop"].get<int>();
    auto& buffer = context.buffer();
    for (auto& line : display_buffer.lines())
    {
        for (auto atom_it = line.begin(); atom_it != line.end(); ++atom_it)
        {
            if (atom_it->type() != DisplayAtom::BufferRange)
                continue;

            auto begin = buffer.iterator_at(atom_it->begin());
            auto end = buffer.iterator_at(atom_it->end());
            for (BufferIterator it = begin; it != end; ++it)
            {
                auto c = *it;
                if (c == '\t' or c == ' ' or c == '\n')
                {
                    if (it != begin)
                        atom_it = ++line.split(atom_it, it.coord());
                    if (it+1 != end)
                        atom_it = line.split(atom_it, (it+1).coord());

                    if (c == '\t')
                    {
                        int column = (int)get_column(buffer, tabstop, it.coord());
                        int count = tabstop - (column % tabstop);
                        String padding = "→";
                        for (int i = 0; i < count-1; ++i)
                            padding += ' ';
                        atom_it->replace(padding);
                    }
                    else if (c == ' ')
                        atom_it->replace("·");
                    else if (c == '\n')
                        atom_it->replace("¬");
                    break;
                }
            }
        }
    }
}

void show_line_numbers(const Context& context, HighlightFlags flags, DisplayBuffer& display_buffer)
{
    LineCount last_line = context.buffer().line_count();
    int digit_count = 0;
    for (LineCount c = last_line; c > 0; c /= 10)
        ++digit_count;

    char format[] = "%?d│";
    format[1] = '0' + digit_count;
    const Face face = get_face("LineNumbers");
    for (auto& line : display_buffer.lines())
    {
        char buffer[10];
        snprintf(buffer, 10, format, (int)line.range().first.line + 1);
        DisplayAtom atom{buffer};
        atom.face = face;
        line.insert(line.begin(), std::move(atom));
    }
}

void show_matching_char(const Context& context, HighlightFlags flags, DisplayBuffer& display_buffer)
{
    const Face face = get_face("MatchingChar");
    using CodepointPair = std::pair<Codepoint, Codepoint>;
    static const CodepointPair matching_chars[] = { { '(', ')' }, { '{', '}' }, { '[', ']' }, { '<', '>' } };
    const auto range = display_buffer.range();
    const auto& buffer = context.buffer();
    for (auto& sel : context.selections())
    {
        auto pos = sel.cursor();
        if (pos < range.first or pos >= range.second)
            continue;
        auto c = buffer.byte_at(pos);
        for (auto& pair : matching_chars)
        {
            int level = 1;
            if (c == pair.first)
            {
                auto it = buffer.iterator_at(pos)+1;
                auto end = buffer.iterator_at(range.second);
                skip_while(it, end, [&](char c) {
                    if (c == pair.first)
                        ++level;
                    else if (c == pair.second and --level == 0)
                        return false;
                    return true;
                });
                if (it != end)
                    highlight_range(display_buffer, it.coord(), (it+1).coord(), false,
                                    apply_face(face));
                break;
            }
            else if (c == pair.second and pos > range.first)
            {
                auto it = buffer.iterator_at(pos)-1;
                auto end = buffer.iterator_at(range.first);
                skip_while_reverse(it, end, [&](char c) {
                    if (c == pair.second)
                        ++level;
                    else if (c == pair.first and --level == 0)
                        return false;
                    return true;
                });
                if (it != end or (*end == pair.first and level == 1))
                    highlight_range(display_buffer, it.coord(), (it+1).coord(), false,
                                    apply_face(face));
                break;
            }
        }
    }
}

void highlight_selections(const Context& context, HighlightFlags flags, DisplayBuffer& display_buffer)
{
    if (flags != HighlightFlags::Highlight)
        return;
    const auto& buffer = context.buffer();
    for (size_t i = 0; i < context.selections().size(); ++i)
    {
        auto& sel = context.selections()[i];
        const bool forward = sel.anchor() <= sel.cursor();
        ByteCoord begin = forward ? sel.anchor() : buffer.char_next(sel.cursor());
        ByteCoord end   = forward ? (ByteCoord)sel.cursor() : buffer.char_next(sel.anchor());

        const bool primary = (i == context.selections().main_index());
        Face sel_face = get_face(primary ? "PrimarySelection" : "SecondarySelection");
        highlight_range(display_buffer, begin, end, false,
                        apply_face(sel_face));
    }
    for (size_t i = 0; i < context.selections().size(); ++i)
    {
        auto& sel = context.selections()[i];
        const bool primary = (i == context.selections().main_index());
        Face cur_face = get_face(primary ? "PrimaryCursor" : "SecondaryCursor");
        highlight_range(display_buffer, sel.cursor(), buffer.char_next(sel.cursor()), false,
                        apply_face(cur_face));
    }
}

void expand_unprintable(const Context& context, HighlightFlags flags, DisplayBuffer& display_buffer)
{
    auto& buffer = context.buffer();
    for (auto& line : display_buffer.lines())
    {
        for (auto atom_it = line.begin(); atom_it != line.end(); ++atom_it)
        {
            if (atom_it->type() == DisplayAtom::BufferRange)
            {
                for (auto it  = buffer.iterator_at(atom_it->begin()),
                          end = buffer.iterator_at(atom_it->end()); it < end;)
                {
                    Codepoint cp = utf8::codepoint<utf8::InvalidPolicy::Pass>(it, end);
                    auto next = utf8::next(it, end);
                    if (cp != '\n' and not iswprint(cp))
                    {
                        std::ostringstream oss;
                        oss << "U+" << std::hex << cp;
                        String str = oss.str();
                        if (it.coord() != atom_it->begin())
                            atom_it = ++line.split(atom_it, it.coord());
                        if (next.coord() < atom_it->end())
                            atom_it = line.split(atom_it, next.coord());
                        atom_it->replace(str);
                        atom_it->face = { Colors::Red, Colors::Black };
                        break;
                    }
                    it = next;
                }
            }
        }
    }
}

HighlighterAndId flag_lines_factory(HighlighterParameters params)
{
    if (params.size() != 2)
        throw runtime_error("wrong parameter count");

    const String& option_name = params[1];
    Color bg = str_to_color(params[0]);

    // throw if wrong option type
    GlobalOptions::instance()[option_name].get<std::vector<LineAndFlag>>();

    return {"hlflags_" + params[1],
            [=](const Context& context, HighlightFlags flags, DisplayBuffer& display_buffer)
            {
                auto& lines_opt = context.options()[option_name];
                auto& lines = lines_opt.get<std::vector<LineAndFlag>>();

                CharCount width = 0;
                for (auto& l : lines)
                     width = std::max(width, std::get<2>(l).char_length());
                const String empty{' ', width};
                for (auto& line : display_buffer.lines())
                {
                    int line_num = (int)line.range().first.line + 1;
                    auto it = find_if(lines,
                                      [&](const LineAndFlag& l)
                                      { return std::get<0>(l) == line_num; });
                    String content = it != lines.end() ? std::get<2>(*it) : empty;
                    content += String(' ', width - content.char_length());
                    DisplayAtom atom{std::move(content)};
                    atom.face = { it != lines.end() ? std::get<1>(*it) : Colors::Default , bg };
                    line.insert(line.begin(), std::move(atom));
                }
            }};
}

template<void (*highlighter_func)(const Context&, HighlightFlags, DisplayBuffer&)>
class SimpleHighlighterFactory
{
public:
    SimpleHighlighterFactory(const String& id) : m_id(id) {}

    HighlighterAndId operator()(HighlighterParameters params) const
    {
        return HighlighterAndId(m_id, HighlighterFunc(highlighter_func));
    }
private:
    String m_id;
};

HighlighterAndId highlighter_group_factory(HighlighterParameters params)
{
    if (params.size() != 1)
        throw runtime_error("wrong parameter count");

    return HighlighterAndId(params[0], HighlighterGroup());
}

HighlighterAndId reference_factory(HighlighterParameters params)
{
    if (params.size() != 1)
        throw runtime_error("wrong parameter count");

    const String& name = params[0];

    // throw if not found
    //DefinedHighlighters::instance().get_group(name, '/');

    return {name,
            [name](const Context& context, HighlightFlags flags, DisplayBuffer& display_buffer)
            {
                try
                {
                    DefinedHighlighters::instance().get_highlighter(name)(context, flags, display_buffer);
                }
                catch (group_not_found&)
                {
                }
            }};
}

struct RegexMatch
{
    size_t timestamp;
    LineCount line;
    ByteCount begin;
    ByteCount end;

    ByteCoord begin_coord() const { return { line, begin }; }
    ByteCoord end_coord() const { return { line, end }; }
};
using RegexMatchList = std::vector<RegexMatch>;

void find_matches(const Buffer& buffer, RegexMatchList& matches, const Regex& regex)
{
    const size_t buf_timestamp = buffer.timestamp();
    for (auto line = 0_line, end = buffer.line_count(); line < end; ++line)
    {
        auto& l = buffer[line];
        for (boost::regex_iterator<String::const_iterator> it{l.begin(), l.end(), regex}, end{}; it != end; ++it)
        {
            ByteCount b = (int)((*it)[0].first - l.begin());
            ByteCount e = (int)((*it)[0].second - l.begin());
            matches.push_back({ buf_timestamp, line, b, e });
        }
    }
}

void update_matches(const Buffer& buffer, memoryview<LineModification> modifs,
                    RegexMatchList& matches, const Regex& regex)
{
    const size_t buf_timestamp = buffer.timestamp();
    // remove out of date matches and update line for others
    auto ins_pos = matches.begin();
    for (auto it = ins_pos; it != matches.end(); ++it)
    {
        auto modif_it = std::lower_bound(modifs.begin(), modifs.end(), it->line,
                                         [](const LineModification& c, const LineCount& l)
                                         { return c.old_line < l; });

        bool erase = (modif_it != modifs.end() and modif_it->old_line == it->line);
        if (not erase and modif_it != modifs.begin())
        {
            auto& prev = *(modif_it-1);
            erase = it->line <= prev.old_line + prev.num_removed;
            it->line += prev.diff();
        }
        erase = erase or (it->line >= buffer.line_count());

        if (not erase)
        {
            it->timestamp = buf_timestamp;
            kak_assert(buffer.is_valid(it->begin_coord()) or
                       buffer[it->line].length() == it->begin);
            kak_assert(buffer.is_valid(it->end_coord()) or
                       buffer[it->line].length() == it->end);

            if (ins_pos != it)
                *ins_pos = std::move(*it);
            ++ins_pos;
        }
    }
    matches.erase(ins_pos, matches.end());
    size_t pivot = matches.size();

    // try to find new matches in each updated lines
    for (auto& modif : modifs)
    {
        for (auto line = modif.new_line;
             line < modif.new_line + modif.num_added+1 and
             line < buffer.line_count(); ++line)
        {
            auto& l = buffer[line];
            for (boost::regex_iterator<String::const_iterator> it{l.begin(), l.end(), regex}, end{}; it != end; ++it)
            {
                ByteCount b = (int)((*it)[0].first - l.begin());
                ByteCount e = (int)((*it)[0].second - l.begin());
                matches.push_back({ buf_timestamp, line, b, e });
            }
        }
    }
    std::inplace_merge(matches.begin(), matches.begin() + pivot, matches.end(),
                       [](const RegexMatch& lhs, const RegexMatch& rhs) {
                           return lhs.begin_coord() < rhs.begin_coord();
                       });
}

struct RegionMatches
{
    RegexMatchList begin_matches;
    RegexMatchList end_matches;
    RegexMatchList recurse_matches;

    static bool compare_to_begin(const RegexMatch& lhs, ByteCoord rhs)
    {
        return lhs.begin_coord() < rhs;
    }

    RegexMatchList::const_iterator find_next_begin(ByteCoord pos) const
    {
        return std::lower_bound(begin_matches.begin(), begin_matches.end(),
                                pos, compare_to_begin);
    }

    RegexMatchList::const_iterator find_matching_end(ByteCoord beg_pos) const
    {
        auto end_it = end_matches.begin();
        auto rec_it = recurse_matches.begin();
        int recurse_level = 0;
        while (true)
        {
            end_it = std::lower_bound(end_it, end_matches.end(),
                                      beg_pos, compare_to_begin);
            rec_it = std::lower_bound(rec_it, recurse_matches.end(),
                                      beg_pos, compare_to_begin);

            if (end_it == end_matches.end())
                return end_it;

            while (rec_it != recurse_matches.end() and
                   rec_it->end_coord() < end_it->begin_coord())
            {
                ++recurse_level;
                ++rec_it;
            }

            if (recurse_level == 0)
                return end_it;

            --recurse_level;
            beg_pos = end_it->end_coord();
        }
    }
};

struct RegionDesc
{
    Regex m_begin;
    Regex m_end;
    Regex m_recurse;

    RegionMatches find_matches(const Buffer& buffer) const
    {
        RegionMatches res;
        Kakoune::find_matches(buffer, res.begin_matches, m_begin);
        Kakoune::find_matches(buffer, res.end_matches, m_end);
        if (not m_recurse.empty())
            Kakoune::find_matches(buffer, res.recurse_matches, m_recurse);
        return res;
    }

    void update_matches(const Buffer& buffer,
                        memoryview<LineModification> modifs,
                        RegionMatches& matches) const
    {
        Kakoune::update_matches(buffer, modifs, matches.begin_matches, m_begin);
        Kakoune::update_matches(buffer, modifs, matches.end_matches, m_end);
        if (not m_recurse.empty())
            Kakoune::update_matches(buffer, modifs, matches.recurse_matches, m_recurse);
    }
};

struct RegionsHighlighter
{
public:
    using NamedRegionDescList = std::vector<std::pair<String, RegionDesc>>;

    RegionsHighlighter(NamedRegionDescList regions, String default_group)
        : m_regions{std::move(regions)}, m_default_group{std::move(default_group)}
    {
        if (m_regions.empty())
            throw runtime_error("at least one region must be defined");

        for (auto& region : m_regions)
            if (region.second.m_begin.empty() or region.second.m_end.empty())
                throw runtime_error("invalid regex for region highlighter");
    }

    void operator()(HierachicalHighlighter::GroupMap groups, const Context& context,
                    HighlightFlags flags, DisplayBuffer& display_buffer)
    {
        if (flags != HighlightFlags::Highlight)
            return;

        auto range = display_buffer.range();
        const auto& buffer = context.buffer();
        auto& regions = update_cache_ifn(buffer);

        auto begin = std::lower_bound(regions.begin(), regions.end(), range.first,
                                      [](const Region& r, ByteCoord c) { return r.end < c; });
        auto end = std::lower_bound(begin, regions.end(), range.second,
                                    [](const Region& r, ByteCoord c) { return r.begin < c; });
        auto correct = [&](ByteCoord c) -> ByteCoord {
            if (buffer[c.line].length() == c.column)
                return {c.line+1, 0};
            return c;
        };

        auto default_group_it = groups.find(m_default_group);
        const bool apply_default = default_group_it != groups.end();

        auto last_begin = range.first;
        for (; begin != end; ++begin)
        {
            if (apply_default and last_begin < begin->begin)
                apply_highlighter(context, flags, display_buffer,
                                  correct(last_begin), correct(begin->begin),
                                  default_group_it->second);

            auto it = groups.find(begin->group);
            if (it == groups.end())
                continue;
            apply_highlighter(context, flags, display_buffer,
                              correct(begin->begin), correct(begin->end),
                              it->second);
            last_begin = begin->end;
        }
        if (apply_default and last_begin < range.second)
            apply_highlighter(context, flags, display_buffer,
                              correct(last_begin), range.second,
                              default_group_it->second);

    }
private:
    const NamedRegionDescList m_regions;
    const String m_default_group;

    struct Region
    {
        ByteCoord begin;
        ByteCoord end;
        StringView group;
    };
    using RegionList = std::vector<Region>;

    struct Cache
    {
        size_t timestamp = 0;
        std::vector<RegionMatches> matches;
        RegionList regions;
    };
    BufferSideCache<Cache> m_cache;

    using RegionAndMatch = std::pair<size_t, RegexMatchList::const_iterator>;

    // find the begin closest to pos in all matches
    RegionAndMatch find_next_begin(const Cache& cache, ByteCoord pos) const
    {
        RegionAndMatch res{0, cache.matches[0].find_next_begin(pos)};
        for (size_t i = 1; i < cache.matches.size(); ++i)
        {
            const auto& matches = cache.matches[i];
            auto it = matches.find_next_begin(pos);
            if (it != matches.begin_matches.end() and
                (res.second == cache.matches[res.first].begin_matches.end() or
                 it->begin_coord() < res.second->begin_coord()))
                res = RegionAndMatch{i, it};
        }
        return res;
    }

    const RegionList& update_cache_ifn(const Buffer& buffer)
    {
        Cache& cache = m_cache.get(buffer);
        const size_t buf_timestamp = buffer.timestamp();
        if (cache.timestamp == buf_timestamp)
            return cache.regions;

        if (cache.timestamp == 0)
        {
            cache.matches.resize(m_regions.size());
            for (size_t i = 0; i < m_regions.size(); ++i)
                cache.matches[i] = m_regions[i].second.find_matches(buffer);
        }
        else
        {
            auto modifs = compute_line_modifications(buffer, cache.timestamp);
            for (size_t i = 0; i < m_regions.size(); ++i)
                m_regions[i].second.update_matches(buffer, modifs, cache.matches[i]);
        }

        cache.regions.clear();

        for (auto begin = find_next_begin(cache, ByteCoord{-1, 0}),
                  end = RegionAndMatch{ 0, cache.matches[0].begin_matches.end() };
             begin != end; )
        {
            const RegionMatches& matches = cache.matches[begin.first];
            auto& named_region = m_regions[begin.first];
            auto beg_it = begin.second;
            auto end_it = matches.find_matching_end(beg_it->end_coord());

            if (end_it == matches.end_matches.end())
            {
                cache.regions.push_back({ {beg_it->line, beg_it->begin},
                                           buffer.end_coord(),
                                           named_region.first });
                break;
            }
            else
            {
                cache.regions.push_back({ beg_it->begin_coord(),
                                          end_it->end_coord(),
                                          named_region.first });
                auto end_coord = end_it->end_coord();

                // With empty begin and end matches (for example if the regexes
                // are /"\K/ and /(?=")/), that case can happen, and would
                // result in an infinite loop.
                if (end_coord == beg_it->begin_coord())
                {
                    kak_assert(beg_it->begin_coord() == beg_it->end_coord() and
                               end_it->begin_coord() == end_it->end_coord());
                    ++end_coord.column;
                }
                begin = find_next_begin(cache, end_coord);
            }
        }
        cache.timestamp = buf_timestamp;
        return cache.regions;
    }
};

HighlighterAndId regions_factory(HighlighterParameters params)
{
    try
    {
        static const ParameterDesc param_desc{
            SwitchMap{ { "default", { true, "" } } },
            ParameterDesc::Flags::SwitchesOnlyAtStart,
            5};

        ParametersParser parser{params, param_desc};
        if ((parser.positional_count() % 4) != 1)
            throw runtime_error("wrong parameter count, expect <id> (<group name> <begin> <end> <recurse>)+");

        RegionsHighlighter::NamedRegionDescList regions;
        id_map<HighlighterGroup> groups;
        for (size_t i = 1; i < parser.positional_count(); i += 4)
        {
            if (parser[i].empty() or parser[i+1].empty() or parser[i+2].empty())
                throw runtime_error("group id, begin and end must not be empty");

            Regex begin{parser[i+1], Regex::nosubs | Regex::optimize };
            Regex end{parser[i+2], Regex::nosubs | Regex::optimize };
            Regex recurse;
            if (not parser[i+3].empty())
                recurse = Regex{parser[i+3], Regex::nosubs | Regex::optimize };

            regions.push_back({ parser[i], {std::move(begin), std::move(end), std::move(recurse)} });
            groups.append({ parser[i], HighlighterGroup{} });
        }
        String default_group;
        if (parser.has_option("default"))
        {
            default_group = parser.option_value("default");
            groups.append({ default_group, HighlighterGroup{} });
        }

        return {parser[0],
                HierachicalHighlighter(
                    RegionsHighlighter(std::move(regions), std::move(default_group)), std::move(groups))};
    }
    catch (boost::regex_error& err)
    {
        throw runtime_error(String("regex error: ") + err.what());
    }
}

void register_highlighters()
{
    HighlighterRegistry& registry = HighlighterRegistry::instance();

    registry.register_func("number_lines", SimpleHighlighterFactory<show_line_numbers>("number_lines"));
    registry.register_func("show_matching", SimpleHighlighterFactory<show_matching_char>("show_matching"));
    registry.register_func("show_whitespaces", SimpleHighlighterFactory<show_whitespaces>("show_whitespaces"));
    registry.register_func("fill", fill_factory);
    registry.register_func("regex", highlight_regex_factory);
    registry.register_func("regex_option", highlight_regex_option_factory);
    registry.register_func("search", highlight_search_factory);
    registry.register_func("group", highlighter_group_factory);
    registry.register_func("flag_lines", flag_lines_factory);
    registry.register_func("line_option", highlight_line_option_factory);
    registry.register_func("ref", reference_factory);
    registry.register_func("regions", regions_factory);
}

}
