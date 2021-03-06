#ifndef highlighter_hh_INCLUDED
#define highlighter_hh_INCLUDED

#include "function_registry.hh"
#include "memoryview.hh"
#include "string.hh"
#include "utils.hh"

#include <functional>

namespace Kakoune
{

class DisplayBuffer;
class Context;

enum class HighlightFlags
{
    Highlight,
    MoveOnly
};

// An Highlighter is a function which mutates a DisplayBuffer in order to
// change the visual representation of a file. It could be changing text
// color, adding information text (line numbering for example) or replacing
// buffer content (folding for example)

using HighlighterFunc = std::function<void (const Context& context, HighlightFlags flags, DisplayBuffer& display_buffer)>;
using HighlighterAndId = std::pair<String, HighlighterFunc>;
using HighlighterParameters = memoryview<String>;
using HighlighterFactory = std::function<HighlighterAndId (HighlighterParameters params)>;

struct HighlighterRegistry : FunctionRegistry<HighlighterFactory>,
                             Singleton<HighlighterRegistry>
{};

}

#endif // highlighter_hh_INCLUDED
