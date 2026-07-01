#include "PostProcessor.hpp"

#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/I18N.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

// BBS
#include <iostream>
#include <fstream>

#ifdef WIN32

// The standard Windows includes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

// https://blogs.msdn.microsoft.com/twistylittlepassagesallalike/2011/04/23/everyone-quotes-command-line-arguments-the-wrong-way/
// This routine appends the given argument to a command line such that CommandLineToArgvW will return the argument string unchanged.
// Arguments in a command line should be separated by spaces; this function does not add these spaces.
// Argument    - Supplies the argument to encode.
// CommandLine - Supplies the command line to which we append the encoded argument string.
static void quote_argv_winapi(const std::wstring &argument, std::wstring &commmand_line_out)
{
	// Don't quote unless we actually need to do so --- hopefully avoid problems if programs won't parse quotes properly.
	if (argument.empty() == false && argument.find_first_of(L" \t\n\v\"") == argument.npos)
		commmand_line_out.append(argument);
	else {
		commmand_line_out.push_back(L'"');
		for (auto it = argument.begin(); ; ++ it) {
			unsigned number_backslashes = 0;
			while (it != argument.end() && *it == L'\\') {
				++ it;
				++ number_backslashes;
			}
			if (it == argument.end()) {
				// Escape all backslashes, but let the terminating double quotation mark we add below be interpreted as a metacharacter.
				commmand_line_out.append(number_backslashes * 2, L'\\');
				break;
			} else if (*it == L'"') {
				// Escape all backslashes and the following double quotation mark.
				commmand_line_out.append(number_backslashes * 2 + 1, L'\\');
				commmand_line_out.push_back(*it);
			} else {
				// Backslashes aren't special here.
				commmand_line_out.append(number_backslashes, L'\\');
				commmand_line_out.push_back(*it);
			}
		}
		commmand_line_out.push_back(L'"');
	}
}

static DWORD execute_process_winapi(const std::wstring &command_line)
{
    // Extract the current environment to be passed to the child process.
	std::wstring envstr;
	{
		wchar_t *env = GetEnvironmentStrings();
		assert(env != nullptr);
		const wchar_t* var = env;
		size_t totallen = 0;
		size_t len;
		while ((len = wcslen(var)) > 0) {
			totallen += len + 1;
			var += len + 1;
		}
		envstr = std::wstring(env, totallen);
		FreeEnvironmentStrings(env);
	}

	STARTUPINFOW startup_info;
	memset(&startup_info, 0, sizeof(startup_info));
	startup_info.cb			 = sizeof(STARTUPINFO);
#if 0
	startup_info.dwFlags	 = STARTF_USESHOWWINDOW;
	startup_info.wShowWindow = SW_HIDE;
#endif
	PROCESS_INFORMATION process_info;
	if (! ::CreateProcessW(
            nullptr /* lpApplicationName */, (LPWSTR)command_line.c_str(), nullptr /* lpProcessAttributes */, nullptr /* lpThreadAttributes */, false /* bInheritHandles */,
			CREATE_UNICODE_ENVIRONMENT /* | CREATE_NEW_CONSOLE */ /* dwCreationFlags */, (LPVOID)envstr.c_str(), nullptr /* lpCurrentDirectory */, &startup_info, &process_info))
		throw Slic3r::RuntimeError(std::string("Failed starting the script ") + boost::nowide::narrow(command_line) + ", Win32 error: " + std::to_string(int(::GetLastError())));
	::WaitForSingleObject(process_info.hProcess, INFINITE);
	ULONG rc = 0;
	::GetExitCodeProcess(process_info.hProcess, &rc);
	::CloseHandle(process_info.hThread);
	::CloseHandle(process_info.hProcess);
	return rc;
}

// Run the script. If it is a perl script, run it through the bundled perl interpreter.
// If it is a batch file, run it through the cmd.exe.
// Otherwise run it directly.
static int run_script(const std::string &script, const std::string &gcode, std::string &/*std_err*/)
{
    // Unpack the argument list provided by the user.
    int     nArgs;
    LPWSTR *szArglist = CommandLineToArgvW(boost::nowide::widen(script).c_str(), &nArgs);
    if (szArglist == nullptr || nArgs <= 0) {
        // CommandLineToArgvW failed. Maybe the command line escapment is invalid?
		throw Slic3r::RuntimeError(std::string("Post processing script ") + script + " on file " + gcode + " failed. CommandLineToArgvW() refused to parse the command line path.");
    }

    std::wstring command_line;
    std::wstring command = szArglist[0];
	if (! boost::filesystem::exists(boost::filesystem::path(command)))
		throw Slic3r::RuntimeError(std::string("The configured post-processing script does not exist: ") + boost::nowide::narrow(command));
    if (boost::iends_with(command, L".pl")) {
        // This is a perl script. Run it through the perl interpreter.
        // The current process may be slic3r.exe or slic3r-console.exe.
        // Find the path of the process:
        wchar_t wpath_exe[_MAX_PATH + 1];
        ::GetModuleFileNameW(nullptr, wpath_exe, _MAX_PATH);
        boost::filesystem::path path_exe(wpath_exe);
        boost::filesystem::path path_perl = path_exe.parent_path() / "perl" / "perl.exe";
        if (! boost::filesystem::exists(path_perl)) {
			LocalFree(szArglist);
			throw Slic3r::RuntimeError(std::string("Perl interpreter ") + path_perl.string() + " does not exist.");
        }
        // Replace it with the current perl interpreter.
        quote_argv_winapi(boost::nowide::widen(path_perl.string()), command_line);
        command_line += L" ";
    } else if (boost::iends_with(command, ".bat")) {
        // Run a batch file through the command line interpreter.
        command_line = L"cmd.exe /C ";
    }

    for (int i = 0; i < nArgs; ++ i) {
        quote_argv_winapi(szArglist[i], command_line);
        command_line += L" ";
    }
    LocalFree(szArglist);
	quote_argv_winapi(boost::nowide::widen(gcode), command_line);
    return (int)execute_process_winapi(command_line);
}

#else
    // POSIX

#include <cstdlib>   // getenv()
#include <sstream>
#include <boost/process.hpp>

namespace process = boost::process;

static int run_script(const std::string &script, const std::string &gcode, std::string &std_err)
{
    // Try to obtain user's default shell
    const char *shell = ::getenv("SHELL");
    if (shell == nullptr) { shell = "/bin/sh"; }

    // Quote and escape the gcode path argument
    std::string command { script };
    command.append(" '");
    for (char c : gcode) {
        if (c == '\'') { command.append("'\\''"); }
        else { command.push_back(c); }
    }
    command.push_back('\'');

    BOOST_LOG_TRIVIAL(debug) << boost::format("Executing script, shell: %1%, command: %2%") % shell % command;

    process::ipstream istd_err;
    process::child child(shell, "-c", command, process::std_err > istd_err);

    std_err.clear();
    std::string line;

    while (child.running() && std::getline(istd_err, line)) {
        std_err.append(line);
        std_err.push_back('\n');
    }

    child.wait();
    return child.exit_code();
}

#endif

namespace Slic3r {

//! macro used to mark string used at localization,
//! return same string
#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

// BBS
void gcode_add_line_number(const std::string& path, const DynamicPrintConfig& config)
{
    const ConfigOptionBool* opt = config.opt<ConfigOptionBool>("gcode_add_line_number");
    if (!opt->getBool())
        return;

    auto gcode_file = boost::filesystem::path(path);
    if (!boost::filesystem::exists(gcode_file))
        return;

    std::fstream fs;
    std::string new_gcode;
    fs.open(gcode_file.c_str(), std::fstream::in | std::fstream::out);

    size_t line_number = 1;
    std::string gcode_line;
    while (std::getline(fs, gcode_line)) {
        char num_str[128];
        memset(num_str, 0, sizeof(num_str));
        snprintf(num_str, sizeof(num_str), "%zd", line_number);
        new_gcode += std::string("N") + num_str + " " + gcode_line + "\n";
        line_number++;
    }

    fs.clear();
    fs.seekp(0, std::ios_base::beg);
    fs.write(new_gcode.c_str(), new_gcode.length());
    fs.close();
}

// ---------------------------------------------------------------------------
// Escape processing infrastructure
// ---------------------------------------------------------------------------

// Sentinel characters used to protect escaped braces during escape processing.
// These are chosen from the control character range and should not appear in
// normal GCode content.
static constexpr char OPEN_BRACE_SENTINEL  = '\x01';
static constexpr char CLOSE_BRACE_SENTINEL = '\x02';

// Resolve C-style escape sequences in a string.
// Processes: \n (newline), \r (carriage return), \t (tab), \\ (backslash),
//           \" (double quote), \' (single quote).
// Unknown escape sequences (e.g., \x) are passed through as literal characters.
static std::string process_escapes(const std::string& src)
{
    std::string result;
    result.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '\\' && i + 1 < src.size()) {
            switch (src[i + 1]) {
                case 'n':  result.push_back('\n'); ++i; break;
                case 'r':  result.push_back('\r'); ++i; break;
                case 't':  result.push_back('\t'); ++i; break;
                case '\\': result.push_back('\\'); ++i; break;
                case '"':  result.push_back('"');  ++i; break;
                case '\'': result.push_back('\''); ++i; break;
                default:
                    // Unknown escape — pass through both characters literally.
                    result.push_back(src[i]);
                    result.push_back(src[i + 1]);
                    ++i; // consume the next character too
                    break;
            }
        } else {
            result.push_back(src[i]);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Shared rule parsing and substitution
// ---------------------------------------------------------------------------

std::vector<GCodeSubRule> parse_gcode_substitution_rules(const ConfigBase &config)
{
    const auto *print_subs   = config.opt<ConfigOptionStrings>("gcode_substitutions");
    const auto *printer_subs = config.opt<ConfigOptionStrings>("printer_gcode_substitutions");

    bool has_print_subs   = print_subs != nullptr && !print_subs->values.empty();
    bool has_printer_subs = printer_subs != nullptr && !printer_subs->values.empty();

    if (!has_print_subs && !has_printer_subs)
        return {};

    std::vector<GCodeSubRule> rules;

    // Lambda: protect \{ and \} by replacing them with sentinel characters.
    // This prevents escape processing from consuming the backslash before
    // we can restore it for the regex engine.
    auto protect_escaped_braces = [](const std::string& str) -> std::string {
        std::string result;
        result.reserve(str.size());
        for (size_t i = 0; i < str.size(); ++i) {
            if (str[i] == '\\' && i + 1 < str.size()) {
                if (str[i + 1] == '{') {
                    result.push_back(OPEN_BRACE_SENTINEL);
                    ++i; // consume both \ and {
                    continue;
                }
                if (str[i + 1] == '}') {
                    result.push_back(CLOSE_BRACE_SENTINEL);
                    ++i; // consume both \ and }
                    continue;
                }
            }
            result.push_back(str[i]);
        }
        return result;
    };

    // Lambda: restore sentinel characters back to braces.
    // When keep_backslash is true (for regex find patterns), the backslash
    // is preserved so the regex engine receives \{ / \} (literal brace).
    // When keep_backslash is false (for replace strings), only the bare
    // brace is emitted.
    auto restore_escaped_braces = [](const std::string& str, bool keep_backslash) -> std::string {
        std::string result;
        result.reserve(str.size());
        for (char c : str) {
            if (c == OPEN_BRACE_SENTINEL) {
                if (keep_backslash) result.push_back('\\');
                result.push_back('{');
            } else if (c == CLOSE_BRACE_SENTINEL) {
                if (keep_backslash) result.push_back('\\');
                result.push_back('}');
            } else {
                result.push_back(c);
            }
        }
        return result;
    };

    auto expand_rules = [&rules, &protect_escaped_braces, &restore_escaped_braces](const ConfigOptionStrings* subs) {
        for (const auto& raw_value : subs->values) {
            std::vector<std::string> lines;
            boost::split(lines, raw_value, boost::is_any_of("\r\n"), boost::token_compress_on);
            for (auto &line : lines) {
                boost::trim(line);
                if (line.empty() || line.size() < 4 || (line[0] != 's' && line[0] != 'l'))
                    continue;

                GCodeSubRule rule;
                rule.is_regex  = line[0] == 's';
                char delimiter = line[1];

                // Split by delimiter using string_view to avoid small heap allocations.
                std::vector<std::string_view> parts;
                {
                    std::string_view sv(line);
                    std::string_view::size_type pos   = 2;
                    std::string_view::size_type start = pos;
                    int part_count = 0;
                    while (part_count < 3) {
                        pos = sv.find(delimiter, start);
                        if (pos == std::string_view::npos) {
                            parts.push_back(sv.substr(start));
                            break;
                        }
                        parts.push_back(sv.substr(start, pos - start));
                        start = pos + 1;
                        part_count++;
                    }
                }
                if (parts.size() < 2)
                    continue;

                rule.find    = std::string(parts[0]);
                rule.replace = std::string(parts[1]);
                std::string flags = parts.size() > 2 ? std::string(parts[2]) : "";

                if (rule.find.empty()) {
                    BOOST_LOG_TRIVIAL(warning) << "GCode substitution skipped: empty find pattern in: " << line;
                    continue;
                }

                // Parse block-type flags: C = color/toolhead block.
                // The m flag is a regex flag that works within chunks normally.
                GCodeSubBlockType block_type = GCodeSubBlockType::None;
                bool has_C = flags.find('C') != std::string::npos;
                if (has_C)
                    block_type = GCodeSubBlockType::Color;
                rule.block_type = block_type;

                // Warn on unknown flags — known flags: i, n, c, m, s, f, C.
                for (char fc : flags) {
                    if (fc != 'i' && fc != 'n' && fc != 'c' && fc != 'm' &&
                        fc != 's' && fc != 'f' && fc != 'C') {
                        BOOST_LOG_TRIVIAL(warning) << "GCode substitution: unknown flag '" << fc
                            << "' in rule: " << line;
                    }
                }

                // --- Escape processing pipeline ---
                // Order matters:
                //  1. protect_escaped_braces — convert \{ / \} to sentinels (consumes both chars)
                //  2. process_escapes — resolve \n, \r, \t, \\, \", \'
                //  3. restore_escaped_braces — convert sentinels back to braces
                //
                // For regex find patterns: keep_backslash=true so regex engine receives \{ / \}
                // For replace strings: keep_backslash=false so bare { / } is emitted

                // Step 1: Protect escaped braces in both find and replace.
                std::string protected_find    = protect_escaped_braces(rule.find);
                std::string protected_replace = protect_escaped_braces(rule.replace);

                // Step 2: Process escape sequences.
                // For regex find patterns, escape sequences are NOT processed — the regex
                // engine handles \d, \s, \n, etc. directly. For literal find patterns and
                // all replace strings, escape sequences are processed.
                std::string processed_find, processed_replace;
                if (rule.is_regex) {
                    // Regex find: do NOT process escapes (let regex engine handle them).
                    // But we still need to restore the protected braces with keep_backslash=true.
                    processed_find = protected_find;
                } else {
                    // Literal find: process escapes normally.
                    processed_find = process_escapes(protected_find);
                }
                // Replace string: always process escapes.
                processed_replace = process_escapes(protected_replace);

                // Step 3: Restore escaped braces.
                // Regex find: keep_backslash=true so \{ becomes \{ (regex literal brace).
                // Literal find: keep_backslash=false so \{ becomes { (literal brace).
                // Replace: keep_backslash=false so \{ becomes { (literal brace).
                if (rule.is_regex) {
                    rule.find    = restore_escaped_braces(processed_find, true);
                } else {
                    rule.find    = restore_escaped_braces(processed_find, false);
                }
                rule.replace = restore_escaped_braces(processed_replace, false);

                if (rule.find.empty()) {
                    BOOST_LOG_TRIVIAL(warning) << "GCode substitution skipped: empty find pattern after escape processing in: " << line;
                    continue;
                }


                bool case_insensitive  = flags.find('i') != std::string::npos;
                bool no_sub_match      = flags.find('n') != std::string::npos;
                bool collate           = flags.find('c') != std::string::npos;
                bool match_newline     = flags.find('s') != std::string::npos;
                bool format_first_only = flags.find('f') != std::string::npos;
                bool has_m_flag        = flags.find('m') != std::string::npos;

                // case_insensitive is needed at runtime for literal substitution.
                rule.case_insensitive  = case_insensitive;
                rule.format_first_only = format_first_only;

                // Pre-compile regex at parse time to avoid per-chunk compilation.
                if (rule.is_regex) {
                    std::string pattern = match_newline ? "(?s)" + rule.find : rule.find;
                    boost::regex::flag_type syntax_flags = boost::regex::normal;
                    if (case_insensitive) syntax_flags |= boost::regex::icase;
                    if (no_sub_match)     syntax_flags |= boost::regex::no_sub_match;
                    if (collate)          syntax_flags |= boost::regex::collate;
                    if (has_m_flag)       syntax_flags |= boost::regex::multiline;
                    try {
                        rule.compiled_regex = boost::regex(pattern, syntax_flags);
                    } catch (const boost::regex_error &re_err) {
                        throw Slic3r::RuntimeError(Slic3r::format(
                            "GCode substitution failed. Invalid regex in rule: %1%\nError: %2%", rule.find, re_err.what()));
                    }
                }
                BOOST_LOG_TRIVIAL(debug) << "Parsed substitution rule: is_regex=" << rule.is_regex
                    << " case_insensitive=" << rule.case_insensitive
                    << " format_first_only=" << rule.format_first_only
                    << " find=" << rule.find;
                rules.push_back(std::move(rule));
            }
        }
    };

    if (has_print_subs)   expand_rules(print_subs);
    if (has_printer_subs) expand_rules(printer_subs);

    BOOST_LOG_TRIVIAL(debug) << "parse_gcode_substitution_rules: rules_count=" << rules.size();
    return rules;
}

// Shared helper: apply a single substitution rule (regex or literal) to a string.
// Returns true if the string was modified.
static bool apply_substitution_rule(GCodeSubRule &rule, std::string &src)
{
    if (rule.is_regex) {
        if (!rule.compiled_regex)
            return false;

        if (!boost::regex_search(src, *rule.compiled_regex))
            return false;

        boost::match_flag_type format_flags = boost::regex_constants::format_default;
        if (rule.format_first_only) format_flags |= boost::regex_constants::format_first_only;

        std::string result;
        result.reserve(src.size());
        boost::regex_replace(
            std::back_inserter(result),
            src.begin(), src.end(),
            *rule.compiled_regex, rule.replace, format_flags
        );

        src = std::move(result);
        return true;
    }

    // Literal substitution — support i (case-insensitive) and f (first only) flags.
    size_t pos = 0;
    bool literal_match_found = false;
    std::string result;

    while (true) {
        size_t found_pos = std::string::npos;

        if (rule.case_insensitive) {
            auto it = boost::ifind_first(
                boost::make_iterator_range(src.begin() + pos, src.end()), rule.find);
            if (it) {
                found_pos = static_cast<size_t>(std::distance(src.begin(), it.begin()));
            }
        } else {
            found_pos = src.find(rule.find, pos);
        }

        if (found_pos == std::string::npos)
            break;

        if (!literal_match_found) {
            literal_match_found = true;
            result.reserve(src.size() + rule.replace.size());
            result.append(src, 0, found_pos);
            result.append(rule.replace);
            pos = found_pos + rule.find.length();

            if (rule.format_first_only) {
                result.append(src, pos, std::string::npos);
                src = std::move(result);
                return true;
            }
            continue;
        }

        result.append(src, pos, found_pos - pos);
        result.append(rule.replace);
        pos = found_pos + rule.find.length();
    }

    if (!literal_match_found)
        return false;

    result.append(src, pos, std::string::npos);
    src = std::move(result);
    return true;
}

// ---------------------------------------------------------------------------
// Chunked substitution — processed by layer/color boundaries
// ---------------------------------------------------------------------------

// Fast string prefix checks for layer/color marker detection.
// Avoids regex overhead on every line of a multi-million-line G-code file.

// Check if a line starts with a layer change marker.
// Accepts "; CHANGE_LAYER", ";CHANGE_LAYER", "; LAYER_CHANGE", ";LAYER_CHANGE".
static bool is_layer_marker(const std::string& line)
{
    if (line.size() < 14 || line[0] != ';')
        return false;

    // Skip the semicolon and any optional spaces
    size_t pos = 1;
    while (pos < line.size() && line[pos] == ' ')
        ++pos;

    std::string_view sv(line.data() + pos, line.size() - pos);
    return sv.starts_with("LAYER_CHANGE") || sv.starts_with("CHANGE_LAYER");
}

// Check if a line starts with a color change marker.
// Accepts "; CP TOOLCHANGE START", ";CP TOOLCHANGE START".
static bool is_color_marker(const std::string& line)
{
    if (line.size() < 21 || line[0] != ';')
        return false;

    // Skip the semicolon and any optional spaces
    size_t pos = 1;
    while (pos < line.size() && line[pos] == ' ')
        ++pos;

    std::string_view sv(line.data() + pos, line.size() - pos);
    return sv.starts_with("CP TOOLCHANGE START");
}

// Apply a single substitution rule to a string (either regex or literal).
// Returns true if the string was modified.
static bool apply_rule_to_string(GCodeSubRule &rule, std::string &src)
{
    return apply_substitution_rule(rule, src);
}

// Apply rules to a string buffer. Updates modified flag if any rule matched.
static void apply_rules_to_string(
    std::string &buf,
    const std::vector<GCodeSubRule*> &rules,
    bool &modified)
{
    if (buf.empty() || rules.empty())
        return;
    for (auto *rule : rules) {
        if (apply_rule_to_string(*rule, buf))
            modified = true;
    }
}

// Flush a color chunk: apply color rules, then append to destination.
// Clears color_chunk on return. If color_rules is empty, the chunk passes
// through unchanged (no substitution applied).
static void flush_color_chunk(
    std::string &color_chunk,
    const std::vector<GCodeSubRule*> &color_rules,
    std::string &dest,
    bool &modified)
{
    if (color_chunk.empty())
        return;

    apply_rules_to_string(color_chunk, color_rules, modified);
    dest.append(color_chunk);
    color_chunk.clear();
}

// Flush a layer chunk: apply layer rules, then write directly to output file.
// Clears layer_content on return.
static void flush_layer_chunk(
    std::string &layer_content,
    const std::vector<GCodeSubRule*> &layer_rules,
    FILE *out,
    bool &modified)
{
    if (layer_content.empty())
        return;

    apply_rules_to_string(layer_content, layer_rules, modified);
    size_t cnt_written = ::fwrite(layer_content.data(), 1, layer_content.size(), out);
    if (::ferror(out) || cnt_written != layer_content.size())
        throw Slic3r::RuntimeError(Slic3r::format("GCode substitution failed. Error writing file."));
    layer_content.clear();
}

// Apply sed-like regex/literal substitutions from in_path to out_path.
// Uses chunked processing: the file is read line-by-line and split at
// layer/color boundaries. Rules are applied per-chunk based on their
// block_type. Processed chunks are written directly to out_path — no
// full-file accumulation in memory.
//
// G-code structure:
//   preamble (before first layer marker)
//   layer 1
//     color chunk 1
//     color chunk 2
//   layer 2
//     color chunk 1
//   suffix (after last layer, separate section)
//
// Rule application:
//   - Color rules (C flag): applied per color chunk (within layers only)
//   - Layer rules (L flag): applied per layer, preamble, and suffix
//   - Preamble: layer rules only (color rules require color chunks, which only exist within layers)
//   - Suffix: layer rules only (same reason as preamble)
//
// Known limitation: Color chunks are split at layer boundaries. If a single
// color/toolhead spans multiple layers, color rules (C flag) are applied
// independently to each layer's portion of that color. A color rule matching
// content that crosses a layer boundary will not match. This is intentional:
// keeping chunks separated by layer allows layer rules (L flag) to operate
// on per-layer content, and future macro/variable support will need layer
// context to resolve height-dependent variables.
//
// Known limitation: The m (multiline) flag in regex rules changes ^ and $ to
// match at line boundaries within the chunk. When used with block rules (L/C),
// this means ^ and $ match at each line boundary within the layer/color chunk,
// not just at the chunk boundaries.
//
// Must be called before run_post_process_scripts() so external scripts
// see the substituted content.
// Returns true if substitutions were applied.
// Returns false if no gcode_substitutions were defined.
// Throws an exception on error.
bool apply_gcode_substitutions(const std::string &in_path, const std::string &out_path, std::vector<GCodeSubRule> &&all_rules)
{
    if (all_rules.empty())
        return false;

    try {
        // Categorize rules by block type.
        // Non-block rules (no L/C flag) are treated as layer-level rules
        // so they are applied within the layer chunk.
        // Cross layer regex is not supported.
        std::vector<GCodeSubRule*> layer_rules;  // L flag or no flag — apply per layer chunk
        std::vector<GCodeSubRule*> color_rules;  // C flag — apply per color chunk

        for (auto &rule : all_rules) {
            if (rule.block_type == GCodeSubBlockType::Color)
                color_rules.push_back(&rule);
            else
                layer_rules.push_back(&rule);
        }

        if (layer_rules.empty() && color_rules.empty())
            return false;

        bool modified = false;

        // --- Streaming chunked processing (L/C rules) ---
        // Hierarchical: layer → color. Read line by line, accumulate color chunks,
        // process color rules and append to layer_content, process layer rules
        // and write directly to output file.
        {
            FilePtr in{ boost::nowide::fopen(in_path.c_str(), "rb") };
            if (in.f == nullptr)
                throw Slic3r::RuntimeError(Slic3r::format("GCode substitution failed. Cannot open file for reading: %1%", in_path));

            FilePtr out{ boost::nowide::fopen(out_path.c_str(), "wb") };
            if (out.f == nullptr)
                throw Slic3r::RuntimeError(Slic3r::format("GCode substitution failed. Cannot open file for writing: %1%", out_path));

            std::string color_chunk;    // accumulates lines for current color
            std::string layer_content;  // accumulates processed color chunks (also used for preamble/suffix)
            bool in_layer = false;

            std::string line;
            int layer_count = 0;
            int color_count = 0;
            while (std::getline(std::istream(in.f), line)) {
                // Strip trailing \r from \r\n line endings (file is opened in binary mode).
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                // ;LAYER_CHANGE
                if (is_layer_marker(line)) {
                    if (!in_layer) {
                        BOOST_LOG_TRIVIAL(debug) << "GCode substitution: flushing preamble (" << layer_content.size() << " bytes)";
                    } else {
                        // Subsequent layer — flush previous layer's color chunk first.
                        BOOST_LOG_TRIVIAL(debug) << "GCode substitution: flushing layer " << layer_count
                            << " (layer_content=" << layer_content.size() << " bytes)";
                        flush_color_chunk(color_chunk, color_rules, layer_content, modified);
                    }
                    // Flush layer content (preamble on first marker, layer on subsequent).
                    flush_layer_chunk(layer_content, layer_rules, out.f, modified);
                    // Start new layer with this layer marker.
                    color_chunk.append(line).push_back('\n');
                    in_layer = true;
                    ++layer_count;
                    color_count = 0;
                // ; CP TOOLCHANGE START
                } else if (is_color_marker(line)) {
                    // Flush previous color chunk within current layer.
                    BOOST_LOG_TRIVIAL(debug) << "GCode substitution: flushing color chunk " << color_count
                        << " (color_chunk=" << color_chunk.size() << " bytes)";
                    flush_color_chunk(color_chunk, color_rules, layer_content, modified);
                    // Start new color chunk with this color marker.
                    color_chunk.append(line).push_back('\n');
                    ++color_count;
                } else {
                    // Regular line — append to current section.
                    if (in_layer)
                        color_chunk.append(line).push_back('\n');
                    else
                        layer_content.append(line).push_back('\n');
                }
            }

            // Flush remaining content.
            BOOST_LOG_TRIVIAL(debug) << "GCode substitution: flushing final color chunk ("
                << color_chunk.size() << " bytes), final layer_content ("
                << layer_content.size() << " bytes)";
            flush_color_chunk(color_chunk, color_rules, layer_content, modified);
            flush_layer_chunk(layer_content, layer_rules, out.f, modified);

            BOOST_LOG_TRIVIAL(debug) << "GCode substitution: processed " << layer_count
                << " layers";
        }

        return modified;
    } catch (const std::exception &err) {
        BOOST_LOG_TRIVIAL(error) << "Exception caught during GCode substitution: " << err.what();
        throw;
    }
}

// Combined post-processor: applies substitutions then runs scripts.
//
// I/O optimization: when multiline rules exist, apply_gcode_substitutions reads
// from the original file and writes directly to the output — no intermediate
// copy_file call. This halves disk I/O and eliminates memory accumulation.
//
// If make_copy and either feature is active, creates a .pp copy to protect
// the memory-mapped previewer handle. Returns true if any post-processing
// work was done (caller must delete the .pp temp file when make_copy=true).
bool run_post_process(std::string &src_path, bool make_copy, const std::string &host, std::string &output_name, const DynamicPrintConfig &config)
{
    const auto *post_process = config.opt<ConfigOptionStrings>("post_process");

    // Parse all substitution rules — applied via chunked post-processing.
    auto sub_rules = parse_gcode_substitution_rules(config);
    bool has_scripts = post_process != nullptr && !post_process->values.empty();
    // Capture emptiness before std::move(sub_rules) invalidates the vector.
    bool had_sub_rules = !sub_rules.empty();

    if (!had_sub_rules && !has_scripts)
        return false;

    BOOST_LOG_TRIVIAL(debug) << "run_post_process: make_copy=" << make_copy
        << " sub_rules_count=" << sub_rules.size()
        << " has_scripts=" << has_scripts;

    // Determine output path: .pp for isolated copy (make_copy), original for in-place.
    std::string tmp_path = make_copy || had_sub_rules ? (src_path + ".pp") : src_path;

    try {
        // Remove stale temp file if it exists.
        try {
            if (boost::filesystem::exists(tmp_path))
                boost::filesystem::remove(tmp_path);
        } catch (const std::exception &err) {
            BOOST_LOG_TRIVIAL(error) << Slic3r::format("Failed deleting an old temporary file %1%: %2%", tmp_path, err.what());
        }

        // --- Step 1: Apply substitutions ---
        if (had_sub_rules) {
            // Read from original, write directly to tmp_path — no intermediate copy.
            apply_gcode_substitutions(src_path, tmp_path, std::move(sub_rules));
        }
        // --- Step 2: If no rules but isolation needed, make a plain copy ---
        else if (make_copy) {
            std::string error_message;
            if (copy_file(src_path, tmp_path, error_message, false) != SUCCESS)
                throw Slic3r::RuntimeError(Slic3r::format("Failed making a temporary copy of G-code file %1%: %2%", src_path, error_message));
        }
        // else: no rules and no isolation — nothing to do for step 1/2.

        // --- Step 3: Run post-processing scripts ---
        if (has_scripts) {
            run_post_process_scripts(tmp_path, host, output_name, config);
        }

        // --- Step 4: Finalize ---
        if (make_copy) {
            // In-place: move the src path
            src_path = std::move(tmp_path);
        }
        else if (had_sub_rules) {
            // In-place: rename .tmp over original.
            boost::filesystem::rename(tmp_path, src_path);
        }
    } catch (...) {
        // Clean up temp file on error.
        if (had_sub_rules || make_copy) {
            try {
                if (boost::filesystem::exists(tmp_path))
                    boost::filesystem::remove(tmp_path);
            } catch (const std::exception &err) {
                BOOST_LOG_TRIVIAL(error) << Slic3r::format("Failed deleting temporary G-code file %1% on error: %2%", tmp_path, err.what());
            }
        }
        throw;
    }

    return true;
}


// Run post processing script / scripts if defined.
// Returns true if a post-processing script was executed.
// Returns false if no post-processing script was defined.
// Throws an exception on error.
// host is one of "File", "PrusaLink", "Repetier", "SL1Host", "OctoPrint", "FlashAir", "Duet", "AstroBox" ...
// output_name is the final name of the G-code on SD card or when uploaded to PrusaLink or OctoPrint.
// If uploading to PrusaLink or OctoPrint, then the file will be renamed to output_name first on the target host.
// The post-processing script may change the output_name.
bool run_post_process_scripts(std::string &src_path, const std::string &host, std::string &output_name, const DynamicPrintConfig &config)
{
    const auto *post_process = config.opt<ConfigOptionStrings>("post_process");
    if (// likely running in SLA mode
        post_process == nullptr ||
        // no post-processing script
        post_process->values.empty())
        return false;

    auto gcode_file = boost::filesystem::path(src_path);
    if (! boost::filesystem::exists(gcode_file))
        throw Slic3r::RuntimeError(std::string("Post-processor can't find exported gcode file"));

    // Store print configuration into environment variables.
    config.setenv_();
    // Let the post-processing script know the target host ("File", "PrusaLink", "Repetier", "SL1Host", "OctoPrint", "FlashAir", "Duet", "AstroBox" ...)
    boost::nowide::setenv("SLIC3R_PP_HOST", host.c_str(), 1);
    // Let the post-processing script know the final file name. For "File" host, it is a full path of the target file name and its location, for example pointing to an SD card.
    // For "PrusaLink" or "OctoPrint", it is a file name optionally with a directory on the target host.
    boost::nowide::setenv("SLIC3R_PP_OUTPUT_NAME", output_name.c_str(), 1);

    // Path to an optional file that the post-processing script may create and populate it with a single line containing the output_name replacement.
    std::string path_output_name = src_path + ".output_name";
    auto remove_output_name_file = [&path_output_name, &src_path]() {
        try {
            if (boost::filesystem::exists(path_output_name))
                boost::filesystem::remove(path_output_name);
        } catch (const std::exception &err) {
            BOOST_LOG_TRIVIAL(error) << Slic3r::format("Failed deleting a file %1% carrying the final name / path of a G-code file %2%: %3%", path_output_name, src_path, err.what());
        }
    };
    // Remove possible stalled path_output_name of the previous run.
    remove_output_name_file();

    try {
        for (const std::string &scripts : post_process->values) {
    		std::vector<std::string> lines;
    		boost::split(lines, scripts, boost::is_any_of("\r\n"));
            for (std::string script : lines) {
                // Ignore empty post processing script lines.
                boost::trim(script);
                if (script.empty())
                    continue;
                BOOST_LOG_TRIVIAL(info) << "Executing script " << script << " on file " << src_path;
                std::string std_err;
                const int result = run_script(script, gcode_file.string(), std_err);
                if (result != 0) {
                    const std::string msg = std_err.empty() ? (boost::format("Post-processing script %1% on file %2% failed.\nError code: %3%") % script % src_path % result).str()
                        : (boost::format("Post-processing script %1% on file %2% failed.\nError code: %3%\nOutput:\n%4%") % script % src_path % result % std_err).str();
                    BOOST_LOG_TRIVIAL(error) << msg;
                    throw Slic3r::RuntimeError(msg);
                }
                if (! boost::filesystem::exists(gcode_file)) {
                    const std::string msg = (boost::format(_(L(
                        "Post-processing script %1% failed.\n\n"
                        "The post-processing script is expected to change the G-code file %2% in place, but the G-code file was deleted and likely saved under a new name.\n"
                        "Please adjust the post-processing script to change the G-code in place and consult the manual on how to optionally rename the post-processed G-code file.\n")))
                        % script % src_path).str();
                    BOOST_LOG_TRIVIAL(error) << msg;
                    throw Slic3r::RuntimeError(msg);
                }
            }
        }
        if (boost::filesystem::exists(path_output_name)) {
            try {
                // Read a single line from path_output_name, which should contain the new output name of the post-processed G-code.
                boost::nowide::fstream f;
                f.open(path_output_name, std::ios::in);
                std::string new_output_name;
                std::getline(f, new_output_name);
                f.close();

                if (host == "File") {
                    namespace fs = boost::filesystem;
                    fs::path op(new_output_name);
                    if (op.is_relative() && op.has_filename() && op.parent_path().empty()) {
                        // Is this just a filename? Make it an absolute path.
                        auto outpath = fs::path(output_name).parent_path();
                        outpath /= op.string();
                        new_output_name = outpath.string();
                    }
                    else {
                        if (! op.is_absolute() || ! op.has_filename())
                            throw Slic3r::RuntimeError("Unable to parse desired new path from output name file");
                    }
                    if (! fs::exists(fs::path(new_output_name).parent_path()))
                        throw Slic3r::RuntimeError(Slic3r::format("Output directory does not exist: %1%",
                                                                  fs::path(new_output_name).parent_path().string()));
                }

                BOOST_LOG_TRIVIAL(trace) << "Post-processing script changed the file name from " << output_name << " to " << new_output_name;
                output_name = new_output_name;
            } catch (const std::exception &err) {
                throw Slic3r::RuntimeError(Slic3r::format("run_post_process_scripts: Failed reading a file %1% "
                                                          "carrying the final name / path of a G-code file: %2%",
                                                          path_output_name, err.what()));
            }
            remove_output_name_file();
        }
    } catch (...) {
        remove_output_name_file();
        throw;
    }

    return true;
}

} // namespace Slic3r
