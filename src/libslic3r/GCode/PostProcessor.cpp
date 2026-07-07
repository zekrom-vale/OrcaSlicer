#include "PostProcessor.hpp"

#include "GCodeProcessor.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/PlaceholderParser.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Print.hpp"

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
                    result.push_back(src[++i]);
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
    const auto *print_subs   = config.option<ConfigOptionString>("gcode_substitutions");
    const auto *printer_subs = config.option<ConfigOptionString>("printer_gcode_substitutions");

    bool has_print_subs   = print_subs != nullptr && !print_subs->value.empty();
    bool has_printer_subs = printer_subs != nullptr && !printer_subs->value.empty();

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

    auto expand_rules = [&rules, &protect_escaped_braces, &restore_escaped_braces](const ConfigOptionString* subs) {
        std::vector<std::string> lines;
        boost::split(lines, subs->value, boost::is_any_of("\r\n"), boost::token_compress_on);
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

                // Parse M flag — metadata-only mode (skip preamble/suffix).
                bool has_M = flags.find('M') != std::string::npos;
                rule.metadata_only = has_M;

                // Warn on unknown flags — known flags: i, n, c, m, s, f, C, M.
                for (char fc : flags) {
                    if (fc != 'i' && fc != 'n' && fc != 'c' && fc != 'm' &&
                        fc != 's' && fc != 'f' && fc != 'C' && fc != 'M') {
                        BOOST_LOG_TRIVIAL(warning) << "GCode substitution: unknown flag '" << fc
                            << "' in rule: " << line;
                    }
                }

                // Detect macro variables in replacement string: {identifier} pattern.
                // Scan for '{' followed by a letter or underscore, where '{' is NOT
                // preceded by '\' (escaped brace), '$' (regex back-reference), or
                // '\g' (boost regex named group reference).
                {
                    bool found_macro = false;
                    for (size_t i = 0; i < rule.replace.size(); ++i) {
                        if (rule.replace[i] == '{') {
                            // Check this is not an escaped brace or regex syntax.
                            bool is_escaped = (i > 0 && rule.replace[i - 1] == '\\');
                            bool is_backref = (i > 0 && rule.replace[i - 1] == '$');
                            bool is_named_group = (i > 1 && rule.replace[i - 1] == 'g' && rule.replace[i - 2] == '\\');
                            if (!is_escaped && !is_backref && !is_named_group && i + 1 < rule.replace.size()) {
                                char next = rule.replace[i + 1];
                                if (std::isalpha(static_cast<unsigned char>(next)) || next == '_') {
                                    found_macro = true;
                                    break;
                                }
                            }
                        }
                    }
                    rule.macro_meta.has_macros = found_macro;
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
                    std::string pattern = rule.find;
                    if (match_newline)  pattern = "(?s)" + pattern;
                    if (has_m_flag)     pattern = "(?m)" + pattern;
                    boost::regex::flag_type syntax_flags = boost::regex::normal;
                    if (case_insensitive) syntax_flags |= boost::regex_constants::icase;
                    if (no_sub_match)     syntax_flags |= boost::regex_constants::nosubs;
                    if (collate)          syntax_flags |= boost::regex_constants::collate;
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
            auto range = boost::make_iterator_range(src.begin() + pos, src.end());
            auto it = boost::ifind_first(range, rule.find);
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

// ---------------------------------------------------------------------------
// Brace protection/restoration for macro evaluation
// ---------------------------------------------------------------------------

// Protect literal braces in a replacement string before PlaceholderParser evaluation.
// Single-pass algorithm:
//   1. Shield regex syntax: ${...} and \g{...} patterns (sentinel both braces)
//   2. Shield remaining literal braces: '{' not followed by letter/underscore
static std::string protect_literal_braces(const std::string& input)
{
    std::string result;
    result.reserve(input.size());

    bool in_macro = false; // true when inside a {macro} that should be left as-is
    size_t i = 0;
    while (i < input.size()) {
        // Check for ${...} pattern — regex back-reference.
        if (i + 1 < input.size() && input[i] == '$' && input[i + 1] == '{') {
            result.push_back('$');
            result.push_back(OPEN_BRACE_SENTINEL);
            i += 2; // skip ${
            while (i < input.size() && input[i] != '}')
                result.push_back(input[i++]);
            if (i < input.size()) {
                result.push_back(CLOSE_BRACE_SENTINEL);
                ++i; // skip }
            }
            continue;
        }
        // Check for \g{...} pattern — named group reference.
        if (i + 2 < input.size() && input[i] == '\\' && input[i + 1] == 'g' && input[i + 2] == '{') {
            result.push_back('\\');
            result.push_back('g');
            result.push_back(OPEN_BRACE_SENTINEL);
            i += 3; // skip \g{
            while (i < input.size() && input[i] != '}')
                result.push_back(input[i++]);
            if (i < input.size()) {
                result.push_back(CLOSE_BRACE_SENTINEL);
                ++i; // skip }
            }
            continue;
        }
        // '{' followed by letter/underscore — start of macro variable, leave as-is.
        if (input[i] == '{' && i + 1 < input.size()) {
            char next = input[i + 1];
            if (std::isalpha(static_cast<unsigned char>(next)) || next == '_') {
                in_macro = true;
                result.push_back('{');
                ++i;
                continue;
            }
        }
        // '{' not followed by letter/underscore — literal brace, shield it.
        if (input[i] == '{') {
            result.push_back(OPEN_BRACE_SENTINEL);
            ++i;
            continue;
        }
        // '}' — if inside a macro, it closes the macro. Otherwise it's a literal brace.
        if (input[i] == '}') {
            if (in_macro) {
                in_macro = false;
                result.push_back('}');
            } else {
                result.push_back(CLOSE_BRACE_SENTINEL);
            }
            ++i;
            continue;
        }
        // Regular character.
        result.push_back(input[i++]);
    }

    return result;
}

// Restore sentinel characters back to literal braces after PlaceholderParser evaluation.
static std::string restore_literal_braces(const std::string& input)
{
    std::string result;
    result.reserve(input.size());
    for (char c : input) {
        if (c == OPEN_BRACE_SENTINEL)
            result.push_back('{');
        else if (c == CLOSE_BRACE_SENTINEL)
            result.push_back('}');
        else
            result.push_back(c);
    }
    return result;
}

// ---------------------------------------------------------------------------
// G-code header parsing and layer Z/height extraction
// ---------------------------------------------------------------------------

// Parse the G-code header block to extract global variables.
// Collects lines between "; HEADER_BLOCK_START" and "; HEADER_BLOCK_END".
// Returns a map of variable name → value (vectorized values stored as comma-separated strings).
// Tolerant of spacing, order, and missing fields for compatibility flexibility.
// Only recognized keys are extracted; unknown keys are ignored.

// Cached tag variants for layer height parsing — computed once, reused every call.
static const std::pair<std::string_view, std::string_view> get_cached_height_tags()
{
    auto [bbl, compat] = GCodeProcessor::reserved_tag_variants(GCodeProcessor::ETags::Height);
    std::string_view bbl_sv = bbl;
    std::string_view compat_sv = compat;
    while (!bbl_sv.empty() && bbl_sv.front() == ' ')
        bbl_sv.remove_prefix(1);
    while (!compat_sv.empty() && compat_sv.front() == ' ')
        compat_sv.remove_prefix(1);
    return {bbl_sv, compat_sv};
}
static const auto [cached_height_bbl, cached_height_compat] = get_cached_height_tags();

// Parse layer height from a comment line using tag variants from GCodeProcessor.
// BBL: "; LAYER_HEIGHT: X"  →  extract X as layer_height
// Compatible: ";HEIGHT:X"   →  extract X as layer_height
static std::optional<double> parse_layer_height(const std::string& line)
{
    // Strip leading spaces once.
    size_t start = 0;
    while (start < line.size() && line[start] == ' ')
        ++start;
    if (start >= line.size() || line[start] != ';')
        return std::nullopt;

    std::string_view sv(line.data() + start, line.size() - start);

    // Try BBL format: "; LAYER_HEIGHT: X"
    if (sv.compare(0, cached_height_bbl.size(), cached_height_bbl) == 0) {
        std::string val{sv.substr(cached_height_bbl.size())};
        try { return std::stod(val); }
        catch (...) {}
    }

    // Try compatible format: ";HEIGHT:X"
    if (sv.compare(0, cached_height_compat.size(), cached_height_compat) == 0) {
        std::string val{sv.substr(cached_height_compat.size())};
        try { return std::stod(val); }
        catch (...) {}
    }

    return std::nullopt;
}

// Parse layer Z from a comment line. G-code uses ";Z:X" for Z position.
static std::optional<double> parse_layer_z(const std::string& line)
{
    // Strip leading spaces.
    size_t start = 0;
    while (start < line.size() && line[start] == ' ')
        ++start;
    if (start >= line.size() || line[start] != ';')
        return std::nullopt;

    std::string_view sv(line.data() + start, line.size() - start);

    // Try ";Z:X" format.
    if (sv.size() > 2 && sv[0] == ';' && sv[1] == 'Z' && sv[2] == ':') {
        std::string val{sv.substr(3)};
        try { return std::stod(val); }
        catch (...) {}
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// MacroScopeManager — manages PlaceholderParser config for scope transitions
// ---------------------------------------------------------------------------

// Lightweight scope manager for PlaceholderParser.
// Manages adding/removing scoped variables as we enter/exit chunks.
class MacroScopeManager
{
public:
    MacroScopeManager(PlaceholderParser& parser, int total_layers)
        : m_parser(parser), m_total_layers(total_layers) {}

    // Enter preamble or suffix (global scope — no layer/color vars).
    void enter_global() { m_in_layer = false; }

    // Enter a layer chunk. Layer vars become available.
    // Layer indexing is 0-based to match G-code layer markers (layer 0 = first layer).
    void enter_layer(int layer_num, double layer_z, double layer_height)
    {
        m_in_layer = true;
        m_layer_num = layer_num;
        m_parser.set("layer_num", layer_num);
        m_parser.set("layer_z", layer_z);
        m_parser.set("layer_height", layer_height);
        m_parser.set("first_layer", layer_num == 0);
        m_parser.set("last_layer", layer_num == m_total_layers - 1);
    }

    // Update Z and height after the Height tag is parsed.
    // Called when the Height tag appears after enter_layer() was already called.
    void update_layer_z(double layer_z, double layer_height)
    {
        m_parser.set("layer_z", layer_z);
        m_parser.set("layer_height", layer_height);
    }

    // Enter a color block within a layer. Color vars become available.
    void enter_color(int color_chunk_num, int tool_num)
    {
        m_color_chunk_num = color_chunk_num;
        m_parser.set("color_chunk_num", color_chunk_num);
        m_parser.set("tool_num", tool_num);
    }

    // Exit a color block. Color vars are removed.
    void exit_color()
    {
        m_parser.config_writable().erase("color_chunk_num");
        m_parser.config_writable().erase("tool_num");
    }

    // Exit a layer chunk. Layer vars are removed.
    void exit_layer()
    {
        m_parser.config_writable().erase("layer_num");
        m_parser.config_writable().erase("layer_z");
        m_parser.config_writable().erase("layer_height");
        m_parser.config_writable().erase("first_layer");
        m_parser.config_writable().erase("last_layer");
        m_in_layer = false;
    }

    // Check if we are currently inside a layer chunk.
    bool in_layer() const { return m_in_layer; }

private:
    PlaceholderParser& m_parser;
    int m_total_layers;
    bool m_in_layer = false;
    int m_layer_num = 0;
    int m_color_chunk_num = 0;
};

// Evaluate a replacement string through PlaceholderParser with brace protection.
// Returns the resolved string. On error, logs a warning and returns the original
// unresolved replacement (graceful degradation).
static std::string evaluate_replacement(
    const std::string& raw_replacement,
    PlaceholderParser& parser)
{
    // Protect literal braces (two-phase: shield regex syntax first, then generic).
    std::string protected_replacement = protect_literal_braces(raw_replacement);

    // Evaluate through PlaceholderParser.
    std::string resolved;
    try {
        resolved = parser.process(protected_replacement, 0);
    } catch (const Slic3r::PlaceholderParserError& e) {
        // Graceful degradation — log warning, return original replacement unresolved.
        BOOST_LOG_TRIVIAL(warning) << "GCode macro evaluation failed: " << e.what()
            << "\nReplacement: " << raw_replacement;
        return raw_replacement;
    }

    // Restore literal braces.
    return restore_literal_braces(resolved);
}

// Fast string prefix checks for layer/color marker detection.
// Avoids regex overhead on every line of a multi-million-line G-code file.

// Check if a line starts with a layer change marker.
// Uses reserved_tag_variants() to detect both BBL and compatible tag formats.
// Cached tag variants for layer marker detection — computed once, reused every call.
static const std::pair<std::string_view, std::string_view> get_cached_layer_tags()
{
    auto [bbl, compat] = GCodeProcessor::reserved_tag_variants(GCodeProcessor::ETags::Layer_Change);
    std::string_view bbl_sv = bbl;
    std::string_view compat_sv = compat;
    if (bbl_sv.size() > 1 && bbl_sv[0] == ' ')
        bbl_sv.remove_prefix(1);
    if (compat_sv.size() > 1 && compat_sv[0] == ' ')
        compat_sv.remove_prefix(1);
    return {bbl_sv, compat_sv};
}
static const auto [cached_layer_bbl, cached_layer_compat] = get_cached_layer_tags();

// BBL: "; CHANGE_LAYER", Compatible: ";LAYER_CHANGE"
static bool is_layer_marker(const std::string& line)
{
    if (line.empty() || line[0] != ';')
        return false;

    // Skip the semicolon and any optional spaces
    size_t pos = 1;
    while (pos < line.size() && line[pos] == ' ')
        ++pos;

    std::string_view sv(line.data() + pos, line.size() - pos);
    return sv.compare(0, cached_layer_bbl.size(), cached_layer_bbl) == 0
        || sv.compare(0, cached_layer_compat.size(), cached_layer_compat) == 0;
}

// Detect bare T commands (T0, T1, T2, ...) — tool changes / color boundaries.
static bool is_bare_t_command(const std::string& line)
{
    if (line.empty())
        return false;

    size_t pos = 0;
    while (pos < line.size() && line[pos] == ' ')
        ++pos;
    return pos < line.size() && line[pos] == 'T' && pos + 1 < line.size() &&
        std::isdigit(static_cast<unsigned char>(line[pos + 1]));
}

// Detect the EXECUTABLE_BLOCK_START marker — separates preamble from first layer.
static bool is_executable_block_start(const std::string& line)
{
    return line.find("EXECUTABLE_BLOCK_START") != std::string::npos;
}

// Detect the EXECUTABLE_BLOCK_END marker — separates last layer from suffix.
static bool is_executable_block_end(const std::string& line)
{
    return line.find("EXECUTABLE_BLOCK_END") != std::string::npos;
}

// Apply a single substitution rule to a string (either regex or literal).
// If the rule has macros and a PlaceholderParser is available, evaluates the
// replacement through the parser before applying the substitution.
// Regex replacement syntax (${1}, \g{name}, & for entire match) is preserved
// and NOT escaped, allowing user control over replacement formatting.
// Returns true if the string was modified.
static bool apply_rule_to_string(GCodeSubRule &rule, std::string &src,
    PlaceholderParser* parser)
{
    // If rule has macros and parser is available, evaluate replacement lazily.
    if (rule.macro_meta.has_macros && parser) {
        std::string resolved = evaluate_replacement(rule.replace, *parser);
        // Swap replacement temporarily. String move is O(1) — no allocation.
        // Wrap in try/catch to guarantee restoration if apply_substitution_rule throws.
        std::string tmp = std::move(rule.replace);
        rule.replace = std::move(resolved);
        try {
            bool changed = apply_substitution_rule(rule, src);
            rule.replace = std::move(tmp);
            return changed;
        } catch (...) {
            // Restore original replacement on exception to prevent state corruption.
            rule.replace = std::move(tmp);
            throw;
        }
    }
    return apply_substitution_rule(rule, src);
}

// Apply rules to a string buffer. Updates modified flag if any rule matched.
// M-flagged (metadata_only) rules run ONLY on preamble/suffix (in_layer=false).
// Non-M rules run ONLY on layer/color chunks (in_layer=true).
// When is_layer_rule is true, color-specific macros (color_chunk_num, tool_num) are
// temporarily hidden so layer rules cannot reference color variables.
static void apply_rules_to_string(
    std::string &buf,
    const std::vector<GCodeSubRule*> &rules,
    bool &modified,
    PlaceholderParser* parser,
    bool in_layer,
    bool is_layer_rule)
{
    if (buf.empty() || rules.empty())
        return;

    // When applying layer rules, temporarily hide color-specific macros.
    if (is_layer_rule) {
        auto &cfg = parser->config_writable();
        const ConfigOption *color_chunk_num_opt = cfg.option("color_chunk_num");
        const ConfigOption *tool_num_opt = cfg.option("tool_num");
        bool had_color_chunk_num = color_chunk_num_opt != nullptr;
        bool had_tool_num = tool_num_opt != nullptr;
        std::string color_chunk_num_val = had_color_chunk_num ? color_chunk_num_opt->serialize() : "";
        std::string tool_num_val = had_tool_num ? tool_num_opt->serialize() : "";
        if (had_color_chunk_num)
            cfg.erase("color_chunk_num");
        if (had_tool_num)
            cfg.erase("tool_num");

        for (auto *rule : rules) {
            // M-flagged rules: run ONLY on preamble/suffix (skip when in a layer).
            if (rule->metadata_only && in_layer)
                continue;
            // Non-M rules: run ONLY on layer/color chunks (skip preamble/suffix).
            if (!rule->metadata_only && !in_layer)
                continue;
            if (apply_rule_to_string(*rule, buf, parser))
                modified = true;
        }

        // Restore color macros.
        if (had_color_chunk_num)
            parser->set("color_chunk_num", color_chunk_num_val);
        if (had_tool_num)
            parser->set("tool_num", tool_num_val);
    }
    else {
        for (auto *rule : rules) {
            // M-flagged rules: run ONLY on preamble/suffix (skip when in a layer).
            if (rule->metadata_only && in_layer)
                continue;
            // Non-M rules: run ONLY on layer/color chunks (skip preamble/suffix).
            if (!rule->metadata_only && !in_layer)
                continue;
            if (apply_rule_to_string(*rule, buf, parser))
                modified = true;
        }
    }
}

// Flush a color chunk: apply color rules, then append to destination.
// Clears color_chunk on return. If color_rules is empty, the chunk passes
// through unchanged (no substitution applied).
static void flush_color_chunk(
    std::string &color_chunk,
    const std::vector<GCodeSubRule*> &color_rules,
    std::string &dest,
    bool &modified,
    PlaceholderParser* parser,
    bool in_layer)
{
    if (color_chunk.empty())
        return;

    apply_rules_to_string(color_chunk, color_rules, modified, parser, in_layer, false);
    dest.append(color_chunk);
    color_chunk.clear();
}

// Flush a layer chunk: apply layer rules, then write directly to output file.
// Clears layer_content on return.
static void flush_layer_chunk(
    std::string &layer_content,
    const std::vector<GCodeSubRule*> &layer_rules,
    FILE *out,
    bool &modified,
    PlaceholderParser* parser,
    bool in_layer)
{
    if (layer_content.empty())
        return;

    apply_rules_to_string(layer_content, layer_rules, modified, parser, in_layer, true);
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
bool apply_gcode_substitutions(const std::string &in_path, const std::string &out_path, std::vector<GCodeSubRule> &&all_rules, const DynamicPrintConfig &config)
{
    if (all_rules.empty())
        return false;

    try {
        // Categorize rules by block type and check for macros in a single pass.
        // Non-block rules (no L/C flag) are treated as layer-level rules
        // so they are applied within the layer chunk.
        // Cross layer regex is not supported.
        std::vector<GCodeSubRule*> layer_rules;  // L flag or no flag — apply per layer chunk
        std::vector<GCodeSubRule*> color_rules;  // C flag — apply per color chunk
        bool any_macros = false;

        for (auto &rule : all_rules) {
            if (rule.block_type == GCodeSubBlockType::Color)
                color_rules.push_back(&rule);
            else
                layer_rules.push_back(&rule);
            if (rule.macro_meta.has_macros)
                any_macros = true;
        }

        if (layer_rules.empty() && color_rules.empty())
            return false;

        // Create PlaceholderParser with the print config as external config.
        // This gives macros access to all print/filament/printer config variables
        // (nozzle_temperature, filament_type, total_layer_count, max_z_height,
        // filament_density, filament_diameter, etc.) without needing to extract
        // them from the G-code header.
        PlaceholderParser parser(&config);
        PlaceholderParser* parser_ptr = any_macros ? &parser : nullptr;

        bool modified = false;

        // --- Chunked processing (L/C rules) ---
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

            // Macro scope manager — only active when macros are present.
            int total_layers = 0;
            if (any_macros) {
                auto it = parser.option("total_layer_count");
                if (it) {
                    try {
                        total_layers = std::stoi(it->serialize());
                    } catch (...) {
                        BOOST_LOG_TRIVIAL(warning) << "GCode macro: invalid total_layer_count '"
                            << it->serialize() << "'. {last_layer} will always be false.";
                    }
                }
            }
            MacroScopeManager scope(parser, total_layers);

            std::string line;
            int layer_count = 0;
            int color_count = 0;
            double current_layer_z = 0.0;
            double current_layer_height = 0.0;
            // Track whether we've seen the Z and HEIGHT tags for the current layer yet.
            bool layer_z_set = false;
            bool layer_height_set = false;
            // Track whether we've skipped the first ;LAYER_CHANGE after EXECUTABLE_BLOCK_START.
            // This first marker is part of layer 0, not a layer boundary.
            bool skipped_first_layer_marker = false;
            // Lookahead counter to limit parse_layer_z calls. The Height tag is
            // practically guaranteed to be within the first few lines of a layer.
            // After 20 lines without finding it, give up to avoid per-line overhead.
            int height_lookahead = 0;
            const int max_height_lookahead = 20;
            char buf[4096];
            while (fgets(buf, sizeof(buf), in.f)) {
                line.assign(buf);
                // Strip trailing \r from \r\n line endings (file is opened in binary mode).
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                // Strip trailing \n — fgets includes it, and we add it back with push_back('\n') below.
                if (!line.empty() && line.back() == '\n')
                    line.pop_back();

                // EXECUTABLE_BLOCK_START — start of first layer (layer 0).
                // Content between EXECUTABLE_BLOCK_START and ;LAYER_CHANGE includes
                // actual G-code that belongs to layer 0, not the preamble.
                if (is_executable_block_start(line)) {
                    if (!in_layer) {
                        // Preamble — enter global scope.
                        if (any_macros) scope.enter_global();
                        // Flush any color chunk that accumulated in the preamble.
                        flush_color_chunk(color_chunk, color_rules, layer_content, modified, parser_ptr, in_layer);
                        BOOST_LOG_TRIVIAL(debug) << "GCode substitution: flushing preamble (" << layer_content.size() << " bytes)";
                        // Flush preamble to output.
                        flush_layer_chunk(layer_content, layer_rules, out.f, modified, parser_ptr, in_layer);
                    }
                    // Start layer 0 with this marker.
                    if (any_macros) {
                        scope.enter_layer(0, 0.0, 0.0);
                        layer_z_set = false;
                        layer_height_set = false;
                        height_lookahead = max_height_lookahead;
                    }
                    in_layer = true;
                    color_count = 0;
                    color_chunk.append(line).push_back('\n');
                // ;LAYER_CHANGE — layer boundary.
                // When EXECUTABLE_BLOCK_START started layer 0, the first ;LAYER_CHANGE
                // is skipped because the content before it belongs to layer 0.
                // The Height tag (;Z:...) for layer 0 appears after this skipped marker.
                } else if (is_layer_marker(line)) {
                    if (!in_layer) {
                        // Fallback: no EXECUTABLE_BLOCK_START found, treat first
                        // ;LAYER_CHANGE as layer 0 start (backward compatibility).
                        if (any_macros) scope.enter_global();
                        flush_color_chunk(color_chunk, color_rules, layer_content, modified, parser_ptr, in_layer);
                        BOOST_LOG_TRIVIAL(debug) << "GCode substitution: flushing preamble (" << layer_content.size() << " bytes)";
                        flush_layer_chunk(layer_content, layer_rules, out.f, modified, parser_ptr, in_layer);
                        if (any_macros) {
                            scope.enter_layer(0, 0.0, 0.0);
                            layer_z_set = false;
                            layer_height_set = false;
                            height_lookahead = max_height_lookahead;
                        }
                        in_layer = true;
                        color_count = 0;
                        color_chunk.append(line).push_back('\n');
                    } else if (skipped_first_layer_marker == false && layer_count == 0) {
                        // First ;LAYER_CHANGE after EXECUTABLE_BLOCK_START — skip it.
                        // Layer 0's Height tag appears after this marker.
                        // Append to current color chunk (part of layer 0).
                        skipped_first_layer_marker = true;
                        color_chunk.append(line).push_back('\n');
                    } else {
                        // Subsequent layer — flush previous layer.
                        BOOST_LOG_TRIVIAL(debug) << "GCode substitution: flushing layer " << layer_count
                            << " (layer_content=" << layer_content.size() << " bytes)";
                        flush_color_chunk(color_chunk, color_rules, layer_content, modified, parser_ptr, in_layer);
                        // Don't exit_color here — the ;LAYER_CHANGE line is the first line
                        // of the new color chunk, and color_chunk_num/tool_num should carry over
                        // from the previous layer until a new T command is encountered.
                        flush_layer_chunk(layer_content, layer_rules, out.f, modified, parser_ptr, in_layer);
                        if (any_macros) scope.exit_layer();
                        // Start new layer.
                        ++layer_count;
                        if (any_macros) {
                            scope.enter_layer(layer_count, 0.0, 0.0);
                            layer_z_set = false;
                            layer_height_set = false;
                            height_lookahead = max_height_lookahead;
                        }
                        color_count = 0;
                        color_chunk.append(line).push_back('\n');
                    }
                // Bare T command (T0, T1, T2, ...) — tool change / color boundary
                } else if (is_bare_t_command(line)) {
                    // Flush previous color chunk within current layer.
                    BOOST_LOG_TRIVIAL(debug) << "GCode substitution: flushing color chunk " << color_count
                        << " (color_chunk=" << color_chunk.size() << " bytes)";
                    // Flush color chunk BEFORE exiting color scope (so color-scoped vars are available).
                    flush_color_chunk(color_chunk, color_rules, layer_content, modified, parser_ptr, in_layer);
                    // Exit previous color scope.
                    if (any_macros) scope.exit_color();
                    // Parse the actual tool number from the T command.
                    {
                        size_t tpos = 0;
                        while (tpos < line.size() && line[tpos] == ' ')
                            ++tpos;
                        int tool_num = 0;
                        if (tpos < line.size() && line[tpos] == 'T') {
                            size_t dpos = tpos + 1;
                            while (dpos < line.size() && std::isdigit(static_cast<unsigned char>(line[dpos])))
                                tool_num = tool_num * 10 + (line[dpos++] - '0');
                        }
                        ++color_count;
                        if (any_macros)
                            scope.enter_color(color_count, tool_num);
                    }
                    color_chunk.append(line).push_back('\n');
                // EXECUTABLE_BLOCK_END — end of last layer, start of suffix
                } else if (is_executable_block_end(line)) {
                    // Flush the last layer's color chunk and layer chunk.
                    BOOST_LOG_TRIVIAL(debug) << "GCode substitution: EXECUTABLE_BLOCK_END — flushing last layer";
                    flush_color_chunk(color_chunk, color_rules, layer_content, modified, parser_ptr, in_layer);
                    if (any_macros) scope.exit_color();
                    flush_layer_chunk(layer_content, layer_rules, out.f, modified, parser_ptr, in_layer);
                    if (any_macros) scope.exit_layer();
                    // Exit layer mode — subsequent lines are suffix.
                    in_layer = false;
                    // Append this line to the suffix.
                    layer_content.append(line).push_back('\n');
                } else {
                    // Regular line — check for Z/HEIGHT tags before appending.
                    // G-code provides both ";Z:X" and ";HEIGHT:X" directly.
                    // Limit parsing to the first max_height_lookahead lines to avoid
                    // per-line overhead when tags are absent.
                    if (any_macros && in_layer && height_lookahead > 0 && (!layer_z_set || !layer_height_set)) {
                        --height_lookahead;
                        if (!layer_z_set) {
                            auto z = parse_layer_z(line);
                            if (z) {
                                current_layer_z = *z;
                                scope.update_layer_z(current_layer_z, current_layer_height);
                                layer_z_set = true;
                            }
                        }
                        if (!layer_height_set) {
                            auto h = parse_layer_height(line);
                            if (h) {
                                current_layer_height = *h;
                                scope.update_layer_z(current_layer_z, current_layer_height);
                                layer_height_set = true;
                            }
                        }
                    }
                    // Append to current section.
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
            flush_color_chunk(color_chunk, color_rules, layer_content, modified, parser_ptr, in_layer);
            flush_layer_chunk(layer_content, layer_rules, out.f, modified, parser_ptr, in_layer);

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
// Internal: core post-processing logic using enriched config.
static bool run_post_process_impl(std::string &src_path, bool make_copy, const std::string &host,
    std::string &output_name, const DynamicPrintConfig &enriched_config)
{
    const auto *post_process = enriched_config.opt<ConfigOptionStrings>("post_process");

    // Check for _DEBUG_MACRO master key — if gcode_substitutions contains only
    // "_DEBUG_MACRO" (with optional whitespace), trigger debug mode instead.
    const auto *print_subs = enriched_config.option<ConfigOptionString>("gcode_substitutions");
    bool debug_macro = false;
    if (print_subs) {
        std::string trimmed = print_subs->value;
        boost::trim(trimmed);
        debug_macro = (trimmed == "_DEBUG_MACRO");
    }

    // Parse all substitution rules — applied via chunked post-processing.
    auto sub_rules = parse_gcode_substitution_rules(enriched_config);
    bool has_scripts = post_process != nullptr && !post_process->values.empty();
    // Capture emptiness before std::move(sub_rules) invalidates the vector.
    bool had_sub_rules = !sub_rules.empty();

    if (!had_sub_rules && !has_scripts && !debug_macro)
        return false;

    BOOST_LOG_TRIVIAL(debug) << "run_post_process: make_copy=" << make_copy
        << " sub_rules_count=" << sub_rules.size()
        << " has_scripts=" << has_scripts
        << " debug_macro=" << debug_macro;

    // Determine output path: .pp for isolated copy (make_copy), original for in-place.
    std::string tmp_path = make_copy || had_sub_rules || debug_macro ? (src_path + ".pp") : src_path;

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
            apply_gcode_substitutions(src_path, tmp_path, std::move(sub_rules), enriched_config);
        }
        // --- Step 1b: Debug macro — prepend resolved macro values as comments ---
        else if (debug_macro) {
            // Copy source to tmp_path first.
            std::string error_message;
            if (copy_file(src_path, tmp_path, error_message, false) != SUCCESS)
                throw Slic3r::RuntimeError(Slic3r::format("Failed copying G-code file %1%: %2%", src_path, error_message));

            // Resolve all config keys and prepend as comments.
            PlaceholderParser parser(&enriched_config);
            std::string debug_header = "G4 P0 ; DEBUG_MACRO: resolved macro values\n";
            for (const auto &key : enriched_config.keys()) {
                std::string macro = "{" + key + "}";
                try {
                    std::string resolved = parser.process(macro);
                    debug_header += "G4 P0 ; DEBUG_MACRO: " + key + " = " + resolved + "\n";
                } catch (...) {
                    debug_header += "G4 P0 ; DEBUG_MACRO: " + key + " = <error>\n";
                }
            }
            debug_header += "G4 P0 ; DEBUG_MACRO: end\n";

            // Prepend debug header using streaming — write header to a new temp file,
            // then stream the original file content using a small fixed-size buffer to
            // avoid loading the entire G-code file into memory.
            {
                std::string streaming_tmp = tmp_path + ".debug_tmp";
                {
                    // Open original file for reading.
                    FilePtr fin{ boost::nowide::fopen(tmp_path.c_str(), "rb") };
                    // Open new file for writing.
                    FilePtr fout{ boost::nowide::fopen(streaming_tmp.c_str(), "wb") };
                    if (fin.f && fout.f) {
                        // Write debug header first.
                        fwrite(debug_header.data(), 1, debug_header.size(), fout.f);
                        // Stream file content with a small fixed-size buffer.
                        char buffer[4096];
                        size_t bytes_read;
                        while ((bytes_read = fread(buffer, 1, sizeof(buffer), fin.f)) > 0) {
                            fwrite(buffer, 1, bytes_read, fout.f);
                        }
                    }
                }
                // Replace original with the new file containing the header.
                boost::filesystem::remove(tmp_path);
                boost::filesystem::rename(streaming_tmp, tmp_path);
            }

            BOOST_LOG_TRIVIAL(info) << "GCode substitution: prepended DEBUG_MACRO header with "
                << enriched_config.keys().size() << " resolved variables";
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
            run_post_process_scripts(tmp_path, host, output_name, enriched_config);
        }

        // --- Step 4: Finalize ---
        if (make_copy) {
            // In-place: move the src path
            src_path = std::move(tmp_path);
        }
        else if (had_sub_rules || debug_macro) {
            // In-place: rename .tmp over original.
            boost::filesystem::rename(tmp_path, src_path);
        }
    } catch (...) {
        // Clean up temp file on error.
        if (had_sub_rules || debug_macro || make_copy) {
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

// work was done (caller must delete the .pp temp file when make_copy=true).
bool run_post_process(std::string &src_path, bool make_copy, const std::string &host, std::string &output_name, const Print *print)
{
    // Build enriched config from Print: merge full_print_config() with
    // PrintStatistics::config() so all runtime variables (initial_extruder,
    // print_time, used_filament, total_toolchanges, etc.) are available to
    // PlaceholderParser macros and exported as environment variables via setenv_().
    DynamicPrintConfig enriched_config = print->full_print_config();
    enriched_config += print->print_statistics().config();
    return run_post_process_impl(src_path, make_copy, host, output_name, enriched_config);
}

// Overload for backwards compatibility — takes config directly (no runtime
// variables from PrintStatistics).
bool run_post_process(std::string &src_path, bool make_copy, const std::string &host, std::string &output_name, const DynamicPrintConfig &config)
{
    return run_post_process_impl(src_path, make_copy, host, output_name, config);
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
