#include "command_manager.hh"

#include "alias_registry.hh"
#include "assert.hh"
#include "buffer_utils.hh"
#include "context.hh"
#include "flags.hh"
#include "optional.hh"
#include "ranges.hh"
#include "register_manager.hh"
#include "shell_manager.hh"
#include "utils.hh"

#include <algorithm>

namespace Kakoune
{

bool CommandManager::command_defined(StringView command_name) const
{
    return m_commands.find(command_name) != m_commands.end();
}

void CommandManager::register_command(String command_name,
                                      CommandFunc func,
                                      String docstring,
                                      ParameterDesc param_desc,
                                      CommandFlags flags,
                                      CommandHelper helper,
                                      CommandCompleter completer)
{
    m_commands[command_name] = { std::move(func),
                                 std::move(docstring),
                                 std::move(param_desc),
                                 flags,
                                 std::move(helper),
                                 std::move(completer) };
}

struct parse_error : runtime_error
{
    parse_error(StringView error)
        : runtime_error{format("parse error: {}", error)} {}
};

Codepoint Reader::operator*() const
{
    kak_assert(pos < str.end());
    return utf8::codepoint(pos, str.end());
}

Reader& Reader::operator++()
{
    kak_assert(pos < str.end());
    if (*pos == '\n')
        ++line;
    utf8::to_next(pos, str.end());
    return *this;
}

namespace
{

bool is_command_separator(Codepoint c)
{
    return c == ';' or c == '\n';
}

template<typename Func, typename UnescapeFunc>
String get_until_delimiter(Reader& reader, Func is_delimiter,
                           UnescapeFunc unescape = [](Codepoint) { return false; })
{
    auto beg = reader.pos;
    String str;
    bool was_antislash = false;

    while (reader)
    {
        const Codepoint c = *reader;
        if (is_delimiter(c) or (was_antislash and unescape(c)))
        {
            str += reader.substr_from(beg);
            if (was_antislash)
            {
                str.back() = c;
                beg = reader.pos+1;
            }
            else
                return str;
        }
        was_antislash = c == '\\';
        ++reader;
    }
    if (beg < reader.str.end())
        str += reader.substr_from(beg);
    return str;
}

[[gnu::always_inline]]
inline String get_until_delimiter(Reader& reader, Codepoint c)
{
    return get_until_delimiter(reader, [c](Codepoint ch) { return c == ch; }, [](Codepoint) { return false; });
}

StringView get_until_closing_delimiter(Reader& reader, Codepoint opening_delimiter,
                                       Codepoint closing_delimiter)
{
    kak_assert(utf8::codepoint(utf8::previous(reader.pos, reader.str.begin()),
                               reader.str.end()) == opening_delimiter);
    int level = 0;
    auto start = reader.pos;
    while (reader)
    {
        const Codepoint c = *reader;
        if (c == opening_delimiter)
            ++level;
        else if (c == closing_delimiter)
        {
            if (level > 0)
                --level;
            else
                break;
        }
        ++reader;
    }
    return reader.substr_from(start);
}

Token::Type token_type(StringView type_name, bool throw_on_invalid)
{
    if (type_name == "")
        return Token::Type::RawQuoted;
    else if (type_name == "sh")
        return Token::Type::ShellExpand;
    else if (type_name == "reg")
        return Token::Type::RegisterExpand;
    else if (type_name == "opt")
        return Token::Type::OptionExpand;
    else if (type_name == "val")
        return Token::Type::ValExpand;
    else if (type_name == "arg")
        return Token::Type::ArgExpand;
    else if (throw_on_invalid)
        throw parse_error{format("unknown expand '{}'", type_name)};
    else
        return Token::Type::RawQuoted;
}

void skip_blanks_and_comments(Reader& reader)
{
    while (reader)
    {
        const Codepoint c = *reader;
        if (is_horizontal_blank(c))
            ++reader;
        else if (c == '\\' and reader.pos + 1 != reader.str.end() and
                 *(reader.pos + 1) == '\n')
            ++(++reader);
        else if (c == '#')
        {
            while (reader and *reader != '\n')
                ++reader;
        }
        else
            break;
    }
}

Token parse_percent_token(Reader& reader, bool throw_on_unterminated)
{
    kak_assert(*reader == '%');
    ++reader;
    const auto type_start = reader.pos;
    while (reader and iswalpha(*reader))
        ++reader;
    StringView type_name = reader.substr_from(type_start);

    if (not reader or is_blank(*reader))
    {
        if (throw_on_unterminated)
            throw parse_error{format("expected a string delimiter after '%{}'",
                                     type_name)};
        return {};
    }

    Token::Type type = token_type(type_name, throw_on_unterminated);

    constexpr struct CharPair { Codepoint opening; Codepoint closing; } matching_pairs[] = {
        { '(', ')' }, { '[', ']' }, { '{', '}' }, { '<', '>' }
    };

    const Codepoint opening_delimiter = *reader;
    auto coord = reader.coord();
    ++reader;
    auto start = reader.pos;

    auto it = find_if(matching_pairs, [opening_delimiter](const CharPair& cp)
                      { return opening_delimiter == cp.opening; });

    const auto str_beg = reader.str.begin();
    if (it != std::end(matching_pairs))
    {
        const Codepoint closing_delimiter = it->closing;
        auto token = get_until_closing_delimiter(reader, opening_delimiter,
                                                 closing_delimiter);
        if (throw_on_unterminated and not reader)
            throw parse_error{format("{}:{}: unterminated string '%{}{}...{}'",
                                     coord.line, coord.column, type_name,
                                     opening_delimiter, closing_delimiter)};

        return {type, start - str_beg, coord, token.str()};
    }
    else
    {
        String token = get_until_delimiter(reader, opening_delimiter);

        if (throw_on_unterminated and not reader)
            throw parse_error{format("{}:{}: unterminated string '%{}{}...{}'",
                                     coord.line, coord.column, type_name,
                                     opening_delimiter, opening_delimiter)};

        return {type, start - str_beg, coord, std::move(token)};
    }
}

String expand_token(const Token& token, const Context& context,
                    const ShellContext& shell_context)
{
    auto& content = token.content;
    switch (token.type)
    {
    case Token::Type::ShellExpand:
    {
        auto str = ShellManager::instance().eval(
            content, context, {}, ShellManager::Flags::WaitForStdout,
            shell_context).first;

        int trailing_eol_count = 0;
        for (auto c : str | reverse())
        {
            if (c != '\n')
                break;
            ++trailing_eol_count;
        }
        str.resize(str.length() - trailing_eol_count, 0);
        return str;
    }
    case Token::Type::RegisterExpand:
        return context.main_sel_register_value(content).str();
    case Token::Type::OptionExpand:
        return context.options()[content].get_as_string();
    case Token::Type::ValExpand:
    {
        auto it = shell_context.env_vars.find(content);
        if (it != shell_context.env_vars.end())
            return it->value;
        return ShellManager::instance().get_val(content, context);
    }
    case Token::Type::ArgExpand:
    {
        auto& params = shell_context.params;
        if (content == '@')
            return join(params, ' ');

        const int arg = str_to_int(content)-1;
        if (arg < 0)
            throw runtime_error("invalid argument index");
        return arg < params.size() ? params[arg] : String{};
    }
    case Token::Type::RawEval:
        return expand(content, context, shell_context);
    case Token::Type::Raw:
    case Token::Type::RawQuoted:
        return content;
    default: kak_assert(false);
    }
    return {};
}

}

CommandParser::CommandParser(StringView command_line) : m_reader{command_line} {}

Optional<Token> CommandParser::read_token(bool throw_on_unterminated)
{
    skip_blanks_and_comments(m_reader);
    if (not m_reader)
        return {};

    const StringView line = m_reader.str;
    const char* start = m_reader.pos;
    auto coord = m_reader.coord();

    const Codepoint c = *m_reader;
    if (c == '"' or c == '\'')
    {
        start = (++m_reader).pos;
        String token = get_until_delimiter(m_reader, c);
        if (throw_on_unterminated and not m_reader)
            throw parse_error{format("unterminated string {0}...{0}", c)};
        if (m_reader)
            ++m_reader;
        return Token{c == '"' ? Token::Type::RawEval
                              : Token::Type::RawQuoted,
                     start - line.begin(), coord, std::move(token)};
    }
    else if (c == '%')
    {
        auto token = parse_percent_token(m_reader, throw_on_unterminated);
        if (m_reader)
            ++m_reader;
        return token;
    }
    else if (is_command_separator(*m_reader))
    {
        ++m_reader;
        return Token{Token::Type::CommandSeparator,
                     m_reader.pos - line.begin(), coord, {}};
    }
    else
    {
        String str = get_until_delimiter(m_reader, [](Codepoint c) {
            return is_command_separator(c) or is_horizontal_blank(c);
        }, [](Codepoint c) { return c == '%'; });
        return Token{Token::Type::Raw, start - line.begin(),
                     coord, std::move(str)};
    }
    return {};
}

template<typename Postprocess>
String expand_impl(StringView str, const Context& context,
                   const ShellContext& shell_context,
                   Postprocess postprocess)
{
    Reader reader{str};
    String res;
    auto beg = str.begin();
    while (reader)
    {
        Codepoint c = *reader;
        if (c == '\\')
        {
            if (not (++reader))
                throw parse_error{"unterminated escape"};
            c = *reader;
            if (c == '%' or c == '\\')
            {
                res += reader.substr_from(beg);
                res.back() = c;
                beg = (++reader).pos;
            }
        }
        else if (c == '%')
        {
            res += reader.substr_from(beg);
            res += postprocess(expand_token(parse_percent_token(reader, true),
                                            context, shell_context));
            beg = (++reader).pos;
        }
        else
            ++reader;
    }
    res += reader.substr_from(beg);
    return res;
}

String expand(StringView str, const Context& context,
              const ShellContext& shell_context)
{
    return expand_impl(str, context, shell_context, [](String s){ return s; });
}

String expand(StringView str, const Context& context,
              const ShellContext& shell_context,
              const std::function<String (String)>& postprocess)
{
    return expand_impl(str, context, shell_context,
                       [&](String s) { return postprocess(std::move(s)); });
}

struct command_not_found : runtime_error
{
    command_not_found(StringView name)
        : runtime_error(name + " : no such command") {}
};

CommandManager::CommandMap::const_iterator
CommandManager::find_command(const Context& context, StringView name) const
{
    auto alias = context.aliases()[name];
    StringView cmd_name = alias.empty() ? name : alias;

    return m_commands.find(cmd_name);
}

void CommandManager::execute_single_command(CommandParameters params,
                                            Context& context,
                                            const ShellContext& shell_context,
                                            BufferCoord pos)
{
    if (params.empty())
        return;

    constexpr int max_command_depth = 100;
    if (m_command_depth > max_command_depth)
        throw runtime_error("maximum nested command depth hit");

    ++m_command_depth;
    auto pop_cmd = on_scope_end([this] { --m_command_depth; });

    ParameterList param_view(params.begin()+1, params.end());
    auto command_it = find_command(context, params[0]);
    if (command_it == m_commands.end())
        throw command_not_found(params[0]);

    const DebugFlags debug_flags = context.options()["debug"].get<DebugFlags>();
    if (debug_flags & DebugFlags::Commands)
    {
        String repr_parameters;

        for (auto repr_param : param_view)
            repr_parameters += " " + repr_param;
        write_to_debug_buffer(format("command {}{}", params[0], repr_parameters));
    }

    try
    {
        ParametersParser parameter_parser(param_view,
                                          command_it->value.param_desc);
        command_it->value.func(parameter_parser, context, shell_context);
    }
    catch (runtime_error& error)
    {
        throw runtime_error(format("{}:{}: '{}' {}", pos.line+1, pos.column+1,
                                   params[0], error.what()));
    }
}

void CommandManager::execute(StringView command_line,
                             Context& context, const ShellContext& shell_context)
{
    CommandParser parser(command_line);
    struct ShellParser {
        ShellParser(String&& str) : output{std::move(str)}, parser{output} {}
        String output;
        CommandParser parser;
    };
    Vector<ShellParser> shell_parser_stack;

    auto next_token = [&] {
        while (not shell_parser_stack.empty())
        {
            if (auto shell_token = shell_parser_stack.back().parser.read_token(true))
                return shell_token;
            shell_parser_stack.pop_back();
        }
        return parser.read_token(true);
    };

    BufferCoord command_coord;
    Vector<String> params;
    while (Optional<Token> token_opt = next_token())
    {
        auto& token = *token_opt;
        if (params.empty())
            command_coord = token.coord;

        if (token.type == Token::Type::CommandSeparator)
        {
            execute_single_command(params, context, shell_context, command_coord);
            params.clear();
        }
        // Shell expand are retokenized
        else if (token.type == Token::Type::ShellExpand)
            shell_parser_stack.emplace_back(expand_token(token, context, shell_context));
        else if (token.type == Token::Type::ArgExpand and token.content == '@')
            params.insert(params.end(), shell_context.params.begin(),
                          shell_context.params.end());
        else
            params.push_back(expand_token(token, context, shell_context));
    }
    execute_single_command(params, context, shell_context, command_coord);
}

Optional<CommandInfo> CommandManager::command_info(const Context& context, StringView command_line) const
{
    CommandParser parser{command_line};
    Vector<Token> tokens;
    while (auto token = parser.read_token(false))
    {
        if (token->type == Token::Type::CommandSeparator)
            tokens.clear();
        else
            tokens.push_back(std::move(*token));
    }

    if (tokens.empty() or
        (tokens.front().type != Token::Type::Raw and
         tokens.front().type != Token::Type::RawQuoted))
        return {};

    auto cmd = find_command(context, tokens.front().content);
    if (cmd == m_commands.end())
        return {};

    CommandInfo res;
    res.name = cmd->key;
    if (not cmd->value.docstring.empty())
        res.info += cmd->value.docstring + "\n";

    if (cmd->value.helper)
    {
        Vector<String> params;
        for (auto it = tokens.begin() + 1; it != tokens.end(); ++it)
        {
            if (it->type == Token::Type::Raw or
                it->type == Token::Type::RawQuoted or
                it->type == Token::Type::RawEval)
                params.push_back(it->content);
        }
        String helpstr = cmd->value.helper(context, params);
        if (not helpstr.empty())
            res.info += format("{}\n", helpstr);
    }

    String aliases;
    for (auto& alias : context.aliases().aliases_for(cmd->key))
        aliases += " " + alias;
    if (not aliases.empty())
        res.info += format("Aliases:{}\n", aliases);

    auto& switches = cmd->value.param_desc.switches;
    if (not switches.empty())
        res.info += format("Switches:\n{}", indent(generate_switches_doc(switches)));

    return res;
}

Completions CommandManager::complete_command_name(const Context& context, StringView query) const
{
    auto commands = m_commands
            | filter([](const CommandMap::Item& cmd) { return not (cmd.value.flags & CommandFlags::Hidden); })
            | transform(&CommandMap::Item::key);

    auto aliases = context.aliases().flatten_aliases()
            | transform(&HashItem<String, String>::key)
            | filter([](auto& alias) { return alias.length() > 3; });

    return {0, query.length(), Kakoune::complete(query, query.length(), concatenated(commands, aliases))};
}

Completions CommandManager::complete(const Context& context,
                                     CompletionFlags flags,
                                     StringView command_line,
                                     ByteCount cursor_pos)
{
    CommandParser parser{command_line};
    const char* cursor = command_line.begin() + (int)cursor_pos;
    Vector<Token> tokens;

    bool is_last_token = true;
    while (auto token = parser.read_token(false))
    {
        if (token->type == Token::Type::CommandSeparator)
        {
            tokens.clear();
            continue;
        }

        tokens.push_back(std::move(*token));
        if (parser.pos() >= cursor)
        {
            is_last_token = false;
            break;
        }
    }

    if (is_last_token)
        tokens.push_back({Token::Type::Raw, command_line.length(), parser.coord(), {}});
    kak_assert(not tokens.empty());
    const auto& token = tokens.back();

    // command name completion
    if (tokens.size() == 1 and (token.type == Token::Type::Raw or
                                token.type == Token::Type::RawQuoted))
    {
        auto cmd_start = token.pos;
        StringView query = command_line.substr(cmd_start, cursor_pos - cmd_start);
        return offset_pos(complete_command_name(context, query), cmd_start);
    }

    const ByteCount start = token.pos;
    const ByteCount cursor_pos_in_token = cursor_pos - start;

    switch (token.type)
    {
    case Token::Type::OptionExpand:
        return {start , cursor_pos,
                GlobalScope::instance().option_registry().complete_option_name(
                    token.content, cursor_pos_in_token) };

    case Token::Type::ShellExpand:
        return offset_pos(shell_complete(context, flags, token.content,
                                         cursor_pos_in_token), start);

    case Token::Type::ValExpand:
        return {start , cursor_pos,
                ShellManager::instance().complete_env_var(
                    token.content, cursor_pos_in_token) };

    case Token::Type::Raw:
    case Token::Type::RawQuoted:
    case Token::Type::RawEval:
    {
        if (token.type != Token::Type::Raw and token.type != Token::Type::RawQuoted)
            return Completions{};

        StringView command_name = tokens.front().content;
        if (command_name != m_last_complete_command)
        {
            m_last_complete_command = command_name.str();
            flags |= CompletionFlags::Start;
        }

        auto command_it = find_command(context, command_name);
        if (command_it == m_commands.end() or
            not command_it->value.completer)
            return Completions();

        Vector<String> params;
        for (auto it = tokens.begin() + 1; it != tokens.end(); ++it)
            params.push_back(it->content);
        Completions completions = offset_pos(command_it->value.completer(
            context, flags, params, tokens.size() - 2,
            cursor_pos_in_token), start);

        if (token.type != Token::Type::RawQuoted)
        {
            StringView to_escape = token.type == Token::Type::Raw ? "% \t;" : "%";
            for (auto& candidate : completions.candidates)
                candidate = escape(candidate, to_escape, '\\');
        }

        return completions;
    }
    default:
        break;
    }
    return Completions{};
}

Completions CommandManager::complete(const Context& context,
                                     CompletionFlags flags,
                                     CommandParameters params,
                                     size_t token_to_complete,
                                     ByteCount pos_in_token)
{
    StringView prefix = params[token_to_complete].substr(0, pos_in_token);

    if (token_to_complete == 0)
        return complete_command_name(context, prefix);
    else
    {
        StringView command_name = params[0];
        if (command_name != m_last_complete_command)
        {
            m_last_complete_command = command_name.str();
            flags |= CompletionFlags::Start;
        }

        auto command_it = find_command(context, command_name);
        if (command_it != m_commands.end() and command_it->value.completer)
            return command_it->value.completer(
                context, flags, params.subrange(1),
                token_to_complete-1, pos_in_token);
    }
    return Completions{};
}

}
