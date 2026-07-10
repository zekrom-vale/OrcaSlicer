#ifndef slic3r_GCode_PostProcessor_hpp_
#define slic3r_GCode_PostProcessor_hpp_

#include <string>
#include <vector>
#include <optional>

#include "../libslic3r.h"
#include "../PrintConfig.hpp"

#include <boost/regex.hpp>

namespace Slic3r {

// Forward declaration for Print — used by run_post_process to derive config.
class Print;

// Block type for substitution rules — controls the scope of the substitution.
// None = layer block (default), Color = color/toolhead block.
enum class GCodeSubBlockType : uint8_t
{
    None  = 0,  // layer block (default)
    Color = 2,  // C flag — block spans lines within a color/toolhead section
};

// Parsed substitution rule — applied via chunked full-file processing.
struct GCodeSubRule
{
    bool        is_regex;
    GCodeSubBlockType block_type = GCodeSubBlockType::None;
    std::string find;
    std::string replace;
    // Flags needed at runtime for literal substitution and format bitmask.
    bool case_insensitive  = false;
    bool format_first_only = false;
    // Pre-compiled regex — set during parsing to avoid per-chunk compilation.
    std::optional<boost::regex> compiled_regex;
    // Macro metadata — populated at parse time.
    struct MacroMeta {
        bool has_macros = false;  // quick check: does replacement contain {var}?
    };
    MacroMeta macro_meta;
    // M flag — metadata-only mode (preamble/suffix only).
    bool metadata_only = false;
    // P flag — preprint-only mode (PrePrint section only, between EXECUTABLE_BLOCK_START and first LAYER_CHANGE).
    bool preprint_only = false;
    // R flag — run/don't run condition (boolean expression evaluated per chunk via PlaceholderParser).
    std::optional<std::string> condition;
};

// Parse the raw substitution config options into a vector of GCodeSubRule.
// Returns all defined substitution rules as a single flat vector.
// Returns an empty vector when no rules are defined.
extern std::vector<GCodeSubRule> parse_gcode_substitution_rules(const ConfigBase &config);

// Run post processing script / scripts if defined.
// Returns true if a post-processing script was executed.
// Returns false if no post-processing script was defined.
// Throws an exception on error.
// host is one of "File", "PrusaLink", "Repetier", "SL1Host", "OctoPrint", "FlashAir", "Duet", "AstroBox" ...
// output_name is the final name of the G-code on SD card or when uploaded to PrusaLink or OctoPrint.
// If uploading to PrusaLink or OctoPrint, then the file will be renamed to output_name first on the target host.
// The post-processing script may change the output_name.
extern bool run_post_process_scripts(std::string &src_path, const std::string &host, std::string &output_name, const DynamicPrintConfig &config);

// Apply sed-like regex/literal substitutions from in_path to out_path.
// Each line: s/find/replace/flags  (regex) or  l/find/replace/flags  (literal)
// Delimiter can be any character after s/l.
// Syntax Flags: i = case-insensitive, n = no-sub-match, c = collate, m = multiline.
// Inline Modifier: s = match-newline (?s).
// Format Flags: f = format-first-only (replace first match only).
// Reads from in_path line-by-line, applies block-level substitutions per chunk,
// and writes directly to out_path (no full-file accumulation in memory).
// Must be called before run_post_process_scripts() so external scripts
// see the substituted content.
// Returns true if substitutions were applied.
// Returns false if no gcode_substitutions were defined.
// Throws an exception on error.
extern bool apply_gcode_substitutions(const std::string &in_path, const std::string &out_path, std::vector<GCodeSubRule> &&all_rules, const DynamicPrintConfig &config);

// Combined post-processor: applies substitutions then runs scripts.
// If make_copy and either feature is active, creates a .pp copy to protect
// the memory-mapped previewer handle. Returns true if any post-processing
// work was done (caller must delete the .pp temp file when make_copy=true).
// Throws an exception on error.
// When print is provided, full_print_config() is derived from it and merged
// with PrintStatistics::config() so all runtime variables (initial_extruder,
// print_time, used_filament, etc.) are available to PlaceholderParser macros
// and exported as environment variables via setenv_().
extern bool run_post_process(std::string &src_path, bool make_copy, const std::string &host, std::string &output_name, const Print *print);

// Overload for backwards compatibility — takes config directly (no runtime
// variables from PrintStatistics).
extern bool run_post_process(std::string &src_path, bool make_copy, const std::string &host, std::string &output_name, const DynamicPrintConfig &config);

// BBS
extern void gcode_add_line_number(const std::string &path, const DynamicPrintConfig &config);

} // namespace Slic3r

#endif /* slic3r_GCode_PostProcessor_hpp_ */
