#ifndef slic3r_GCode_PostProcessor_hpp_
#define slic3r_GCode_PostProcessor_hpp_

#include <string>

#include "../libslic3r.h"
#include "../PrintConfig.hpp"

namespace Slic3r {

// Run post processing script / scripts if defined.
// Returns true if a post-processing script was executed.
// Returns false if no post-processing script was defined.
// Throws an exception on error.
// host is one of "File", "PrusaLink", "Repetier", "SL1Host", "OctoPrint", "FlashAir", "Duet", "AstroBox" ...
// If make_copy, then a temp file will be created for src_path by adding a ".pp" suffix and src_path will be updated.
// In that case the caller is responsible to delete the temp file created.
// output_name is the final name of the G-code on SD card or when uploaded to PrusaLink or OctoPrint.
// If uploading to PrusaLink or OctoPrint, then the file will be renamed to output_name first on the target host.
// The post-processing script may change the output_name.
extern bool run_post_process_scripts(std::string &src_path, bool make_copy, const std::string &host, std::string &output_name, const DynamicPrintConfig &config);

inline bool run_post_process_scripts(std::string &src_path, const DynamicPrintConfig &config)
{
	std::string src_path_name = src_path;
	return run_post_process_scripts(src_path, false, "File", src_path_name, config);
}

// Apply sed-like regex/literal substitutions to the G-code file in-place.
// Each line: s/find/replace/flags  (regex) or  l/find/replace/flags  (literal)
// Delimiter can be any character after s/l.
// Syntax Flags: i = case-insensitive, n = no-sub-match, c = collate, m = multiline.
// Inline Modifier: s = match-newline (?s).
// Format Flags: f = format-first-only (replace first match only).
// Reads the whole file, applies substitutions, writes back.
// Must be called before run_post_process_scripts() so external scripts
// see the substituted content.
// Returns true if substitutions were applied.
// Returns false if no gcode_substitutions were defined.
// Throws an exception on error.
extern bool apply_gcode_substitutions(std::string &src_path, const DynamicPrintConfig &config);

// Combined post-processor: applies substitutions then runs scripts.
// If make_copy and either feature is active, creates a .pp copy to protect
// the memory-mapped previewer handle. Returns true if any post-processing
// work was done (caller must delete the .pp temp file when make_copy=true).
// Throws an exception on error.
extern bool run_post_process(std::string &src_path, bool make_copy, const std::string &host, std::string &output_name, const DynamicPrintConfig &config);

// BBS
extern void gcode_add_line_number(const std::string &path, const DynamicPrintConfig &config);

} // namespace Slic3r

#endif /* slic3r_GCode_PostProcessor_hpp_ */
