#include <catch2/catch_all.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"

using namespace Slic3r;

// Helper: create a temporary file with given content, return the path.
static std::string create_temp_file(const std::string &content)
{
    static int counter = 0;
    std::string path = "/tmp/gcode_sub_test_" + std::to_string(++counter) + ".gcode";
    std::ofstream ofs(path);
    ofs << content;
    ofs.close();
    return path;
}

// Helper: read entire file content.
static std::string read_file_content(const std::string &path)
{
    std::ifstream ifs(path);
    return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

// Helper: build a DynamicPrintConfig with gcode_substitutions set.
static DynamicPrintConfig make_config(const std::string &subs)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{ "gcode_substitutions", subs }});
    return config;
}

// Helper: build a DynamicPrintConfig with both gcode_substitutions and printer_gcode_substitutions.
static DynamicPrintConfig make_config(const std::string &print_subs, const std::string &printer_subs)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "gcode_substitutions", print_subs },
        { "printer_gcode_substitutions", printer_subs }
    });
    return config;
}

SCENARIO("GCode Substitution: parse_gcode_substitution_rules", "[PostProcessor]") {
    GIVEN("empty substitutions") {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        auto rules = parse_gcode_substitution_rules(config);
        THEN("no rules are returned") {
            REQUIRE(rules.empty());
        }
    }

    GIVEN("a simple regex rule") {
        auto config = make_config("s/OLD/NEW/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("one rule is returned") {
            REQUIRE(rules.size() == 1);
        }
        AND_THEN("rule is regex type") {
            REQUIRE(rules[0].is_regex == true);
        }
        AND_THEN("find and replace are correct") {
            REQUIRE(rules[0].find == "OLD");
            REQUIRE(rules[0].replace == "NEW");
        }
    }

    GIVEN("a simple literal rule") {
        auto config = make_config("l/OLD/NEW/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("one rule is returned") {
            REQUIRE(rules.size() == 1);
        }
        AND_THEN("rule is literal type") {
            REQUIRE(rules[0].is_regex == false);
        }
    }

    GIVEN("a rule with case-insensitive flag") {
        auto config = make_config("s/old/new/i");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("rule has case_insensitive set") {
            REQUIRE(rules[0].case_insensitive == true);
        }
    }

    GIVEN("a rule with first-only flag") {
        auto config = make_config("s/old/new/f");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("rule has format_first_only set") {
            REQUIRE(rules[0].format_first_only == true);
        }
    }

    GIVEN("a rule with color block flag") {
        auto config = make_config("s/old/new/C");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("rule has color block type") {
            REQUIRE(rules[0].block_type == GCodeSubBlockType::Color);
        }
    }

    GIVEN("a rule with M flag (metadata-only)") {
        auto config = make_config("s/old/new/M");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("rule has metadata_only set") {
            REQUIRE(rules[0].metadata_only == true);
        }
    }

    GIVEN("a rule without M flag") {
        auto config = make_config("s/old/new/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("rule has metadata_only false") {
            REQUIRE(rules[0].metadata_only == false);
        }
    }

    GIVEN("a rule with macro variable in replacement") {
        auto config = make_config("s/old/new_{layer_num}/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("macro_meta.has_macros is true") {
            REQUIRE(rules[0].macro_meta.has_macros == true);
        }
    }

    GIVEN("a rule without macro variable in replacement") {
        auto config = make_config("s/old/new_value/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("macro_meta.has_macros is false") {
            REQUIRE(rules[0].macro_meta.has_macros == false);
        }
    }

    GIVEN("a rule with ${1} back-reference (not a macro)") {
        auto config = make_config("s/(A)/${1}B/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("macro_meta.has_macros is false for back-references") {
            REQUIRE(rules[0].macro_meta.has_macros == false);
        }
    }

    GIVEN("a rule with escaped brace \\{ (not a macro)") {
        auto config = make_config("s/old/new\\{literal/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("macro_meta.has_macros is false for escaped braces") {
            REQUIRE(rules[0].macro_meta.has_macros == false);
        }
    }

    GIVEN("multiple rules separated by newlines") {
        auto config = make_config("s/A/B/\ns/C/D/\nl/E/F/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("three rules are returned") {
            REQUIRE(rules.size() == 3);
        }
    }

    GIVEN("both print and printer substitutions") {
        auto config = make_config("s/A/B/", "s/C/D/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("both sets of rules are returned") {
            REQUIRE(rules.size() == 2);
        }
        AND_THEN("print rules come first") {
            REQUIRE(rules[0].find == "A");
            REQUIRE(rules[1].find == "C");
        }
    }

    GIVEN("empty find pattern") {
        auto config = make_config("s//NEW/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("rule is skipped") {
            REQUIRE(rules.empty());
        }
    }

    GIVEN("invalid regex pattern") {
        auto config = make_config("s/[invalid/NEW/");
        THEN("RuntimeError is thrown") {
            REQUIRE_THROWS_AS(parse_gcode_substitution_rules(config), Slic3r::RuntimeError);
        }
    }

    GIVEN("lines not starting with s or l are ignored") {
        auto config = make_config("this is a comment\ns/A/B/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("only the valid rule is returned") {
            REQUIRE(rules.size() == 1);
        }
    }

    GIVEN("whitespace-only lines are ignored") {
        auto config = make_config("   \ns/A/B/\n\n");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("only the valid rule is returned") {
            REQUIRE(rules.size() == 1);
        }
    }

    GIVEN("custom delimiter") {
        auto config = make_config("s|A|B|");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("rule is parsed correctly") {
            REQUIRE(rules.size() == 1);
            REQUIRE(rules[0].find == "A");
            REQUIRE(rules[0].replace == "B");
        }
    }

    GIVEN("escape sequences in literal find") {
        auto config = make_config("l/foo\\nbar/baz/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("newline is processed in literal find") {
            REQUIRE(rules.size() == 1);
            REQUIRE(rules[0].find == "foo\nbar");
        }
    }

    GIVEN("escape sequences in replace string") {
        auto config = make_config("s/foo/bar\\nbaz/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("newline is processed in replace") {
            REQUIRE(rules.size() == 1);
            REQUIRE(rules[0].replace == "bar\nbaz");
        }
    }

    GIVEN("escaped braces in regex find") {
        auto config = make_config("s/\\{/[/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("backslash-brace is preserved for regex engine") {
            REQUIRE(rules.size() == 1);
            REQUIRE(rules[0].find == "\\{");
        }
    }

    GIVEN("escaped braces in replace string") {
        auto config = make_config("s/foo/\\{/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("brace is emitted without backslash") {
            REQUIRE(rules.size() == 1);
            REQUIRE(rules[0].replace == "{");
        }
    }
}

SCENARIO("GCode Substitution: apply_gcode_substitutions — basic", "[PostProcessor]") {
    GIVEN("simple regex replacement in layer chunk") {
        std::string input = ";LAYER_CHANGE\nG1 E10\nG1 E20\nG1 E30\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("substitution was applied") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
            REQUIRE(output.find("E10") == std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("literal replacement in layer chunk") {
        std::string input = ";LAYER_CHANGE\nG1 E10\nG1 E20\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("l/E10/E100/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("literal substitution was applied") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("case-insensitive regex replacement in layer chunk") {
        std::string input = ";LAYER_CHANGE\nG1 E10\ng1 e10\nG1 e10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/e10/E100/i");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("all case variations are replaced") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
            REQUIRE(output.find("e10") == std::string::npos);
            REQUIRE(output.find("E10") == std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("first-only replacement in layer chunk") {
        std::string input = ";LAYER_CHANGE\nG1 E10\nG1 E10\nG1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/f");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("only first match is replaced") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            size_t count = 0, pos = 0;
            while ((pos = output.find("E10", pos)) != std::string::npos) {
                ++count;
                ++pos;
            }
            REQUIRE(count == 2);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("capture group replacement with ${1} in layer chunk") {
        std::string input = ";LAYER_CHANGE\nG1 E10 F1000\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/(E[0-9]+) (F[0-9]+)/${2} ${1}/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("capture groups are swapped") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("F1000 E10") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("no rules defined") {
        std::string input = "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("no substitution is applied") {
            REQUIRE(modified == false);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: Complex \\G anchor with filament_colour_type", "[PostProcessor]") {
    // Real-world use case: convert "; filament_colour_type = 1;1;1;1"
    // to "; filament_colour_type = 1,1,1,1" using the \G anchor.

    GIVEN("filament_colour_type with semicolon-separated values") {
        std::string input =
            "; filament_colour_type = 1;1;1;1\n"
            "G1 E10\n"
            "; filament_colour_type = 2;3;4\n"
            "G1 E20\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        // Rule 1: Iteratively replace ; between digits with , using \G anchor.
        // Rule 2: Remove trailing comma before non-digit.
        // M flag needed since this is preamble (no layer markers).
        std::string subs =
            "s/(?:(^;\\s*filament_colour_type\\s*=\\s*)|\\G\\D)(\\d+)/${1}${2},/M\n"
            "s/(^;\\s*filament_colour_type\\s*=\\s*(?:,?\\d+)+)[^\\n\\d]/${1}/M";

        auto config = make_config(subs);
        auto rules = parse_gcode_substitution_rules(config);

        THEN("rules are parsed without error") {
            REQUIRE(rules.size() == 2);
        }

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("semicolons are replaced with commas") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("filament_colour_type = 1,1,1,1") != std::string::npos);
            REQUIRE(output.find("filament_colour_type = 2,3,4") != std::string::npos);
            REQUIRE(output.find("filament_colour_type = 1;1;1;1") == std::string::npos);
            REQUIRE(output.find("filament_colour_type = 2;3;4") == std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("filament_colour_type with single value") {
        std::string input =
            "; filament_colour_type = 1\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        std::string subs =
            "s/(?:(^;\\s*filament_colour_type\\s*=\\s*)|\\G\\D)(\\d+)/${1}${2},/M\n"
            "s/(^;\\s*filament_colour_type\\s*=\\s*(?:,?\\d+)+)[^\\n\\d]/${1}/M";

        auto config = make_config(subs);
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("single value is handled correctly") {
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("filament_colour_type = 1") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: Chunking — no layer markers", "[PostProcessor]") {
    // When there are no LAYER_CHANGE markers, the entire file is one preamble chunk.
    // Non-M rules do NOT match in preamble — use M flag for preamble matching.

    GIVEN("file with no layer markers and M-flagged rule") {
        std::string input =
            "G28\n"
            "G1 E10\n"
            "G1 E20\n"
            "G91\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/M");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("M-flagged rule matches in preamble") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
            REQUIRE(output.find("E10") == std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("file with no layer markers and non-M rule should NOT match") {
        std::string input =
            "G28\n"
            "G1 E10\n"
            "G1 E20\n"
            "G91\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("non-M rule does not match in preamble") {
            REQUIRE(modified == false);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") == std::string::npos);
            REQUIRE(output.find("E10") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: Chunking — rules applied per layer", "[PostProcessor]") {
    GIVEN("file with two layers and a rule that matches in both") {
        std::string input =
            "G28\n"
            ";LAYER_CHANGE\n"
            "G1 E10\n"
            ";LAYER_CHANGE\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("both layers are substituted") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            size_t count = 0, pos = 0;
            while ((pos = output.find("E100", pos)) != std::string::npos) {
                ++count;
                ++pos;
            }
            REQUIRE(count == 2);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("file with two layers and a rule that only matches in first") {
        std::string input =
            "G28\n"
            ";LAYER_CHANGE\n"
            "G1 E10\n"
            ";LAYER_CHANGE\n"
            "G1 E20\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("only first layer is substituted") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
            REQUIRE(output.find("G1 E20") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: Chunking — color rules isolated per color chunk", "[PostProcessor]") {
    GIVEN("file with two color chunks split by bare T commands and C-flag rule") {
        std::string input =
            ";LAYER_CHANGE\n"
            "T0\n"
            "G1 E10\n"
            "T1\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/C");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("both color chunks are substituted") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            size_t count = 0, pos = 0;
            while ((pos = output.find("E100", pos)) != std::string::npos) {
                ++count;
                ++pos;
            }
            REQUIRE(count == 2);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: M flag — strict chunk type separation", "[PostProcessor]") {
    // M-flagged rules: run ONLY on preamble/suffix (forbidden on layer/color chunks).
    // Non-M rules: run ONLY on layer/color chunks (forbidden on preamble/suffix).

    GIVEN("non-M rule should NOT match in preamble") {
        std::string input =
            "G28\n"
            "G1 E10\n"
            ";LAYER_CHANGE\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("only layer chunk is substituted, preamble is not") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            // Preamble E10 should remain unchanged.
            REQUIRE(output.find("G28\nG1 E10\n") != std::string::npos);
            // Layer E10 should be replaced.
            REQUIRE(output.find("E100") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("M-flagged rule should ONLY match in preamble") {
        std::string input =
            "G28\n"
            "G1 E10\n"
            ";LAYER_CHANGE\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/M");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("only preamble is substituted, layer is not") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            // Preamble E10 should be replaced.
            REQUIRE(output.find("G28\nG1 E100\n") != std::string::npos);
            // Layer E10 should remain unchanged.
            REQUIRE(output.find(";LAYER_CHANGE\nG1 E10\n") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("M-flagged rule with EXECUTABLE_BLOCK_END should match in suffix") {
        std::string input =
            "G28\n"
            ";LAYER_CHANGE\n"
            "G1 E10\n"
            "; EXECUTABLE_BLOCK_END\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/M");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("suffix is substituted, layer is not") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            // Layer E10 should remain unchanged.
            REQUIRE(output.find(";LAYER_CHANGE\nG1 E10\n") != std::string::npos);
            // Suffix E10 should be replaced.
            REQUIRE(output.find("E100") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("non-M rule with EXECUTABLE_BLOCK_END should NOT match in suffix") {
        std::string input =
            "G28\n"
            ";LAYER_CHANGE\n"
            "G1 E10\n"
            "; EXECUTABLE_BLOCK_END\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("layer is substituted, suffix is not") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            // Layer E10 should be replaced.
            REQUIRE(output.find(";LAYER_CHANGE\nG1 E100\n") != std::string::npos);
            // Suffix E10 should remain unchanged.
            REQUIRE(output.find("; EXECUTABLE_BLOCK_END\nG1 E10\n") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("both M and non-M rules on same file") {
        std::string input =
            "G28\n"
            "G1 E10\n"
            ";LAYER_CHANGE\n"
            "G1 E10\n"
            "; EXECUTABLE_BLOCK_END\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        // M rule replaces E10 with E200 in preamble/suffix.
        // Non-M rule replaces E10 with E100 in layer chunks.
        auto config = make_config("s/E10/E200/M\ns/E10/E100/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("each rule type matches only its designated chunk type") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            // Preamble: M rule applied (E10 -> E200).
            REQUIRE(output.find("G28\nG1 E200\n") != std::string::npos);
            // Layer: non-M rule applied (E10 -> E100).
            REQUIRE(output.find(";LAYER_CHANGE\nG1 E100\n") != std::string::npos);
            // Suffix: M rule applied (E10 -> E200).
            REQUIRE(output.find("; EXECUTABLE_BLOCK_END\nG1 E200\n") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: Chunking — CHANGE_LAYER also recognized", "[PostProcessor]") {
    GIVEN("file using CHANGE_LAYER marker (BBL variant)") {
        std::string input =
            "G28\n"
            ";CHANGE_LAYER\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("CHANGE_LAYER is recognized as layer boundary") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("file using LAYER_CHANGE marker (compatible variant)") {
        std::string input =
            "G28\n"
            ";LAYER_CHANGE\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("LAYER_CHANGE is recognized as layer boundary") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("file using bare T command as color boundary") {
        std::string input =
            "; CHANGE_LAYER\n"
            "T0\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/C");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("bare T command is recognized as color boundary") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("file using bare T command with LAYER_CHANGE") {
        std::string input =
            ";LAYER_CHANGE\n"
            "T1\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/C");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("bare T command is recognized as color boundary") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: Multiline flag (m)", "[PostProcessor]") {
    GIVEN("multiline regex with ^ anchor in layer chunk") {
        std::string input =
            ";LAYER_CHANGE\nG1 E10\nG1 E20\nG2 E30\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/^G1 E10/REPLACED/m");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("only lines starting with G1 E10 are replaced") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("REPLACED") != std::string::npos);
            REQUIRE(output.find("G1 E20") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: Match-newline flag (s)", "[PostProcessor]") {
    GIVEN("regex with . matching newlines in layer chunk") {
        std::string input =
            ";LAYER_CHANGE\nG1 E10\nG1 E20\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/G1 E10.*/REPLACED/s");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("dot matches across newlines") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("REPLACED") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: Named capture groups", "[PostProcessor]") {
    GIVEN("regex with named capture groups in layer chunk") {
        std::string input =
            ";LAYER_CHANGE\nG1 E10 F1000\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/(?<extr>E[0-9]+) (?<feed>F[0-9]+)/${feed} ${extr}/");
        auto rules = parse_gcode_substitution_rules(config);

        THEN("named capture groups parse without error") {
            REQUIRE(rules.size() == 1);
        }

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("named captures work in replacement") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("F1000 E10") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: no-sub-match flag (n)", "[PostProcessor]") {
    GIVEN("regex with n flag for performance in layer chunk") {
        std::string input =
            ";LAYER_CHANGE\nG1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/n");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("substitution works without sub-match overhead") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: Comment and unknown line handling", "[PostProcessor]") {
    // Lines not starting with 's' or 'l' are currently silently ignored.
    // Future: consider using '#' for explicit comments and warning on unknown lines.

    GIVEN("lines starting with # are treated as comments") {
        auto config = make_config("# This is a comment\ns/A/B/");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("comment lines are ignored and valid rules are parsed") {
            REQUIRE(rules.size() == 1);
            REQUIRE(rules[0].find == "A");
        }
    }

    GIVEN("free-form text lines are silently ignored") {
        auto config = make_config("some random text\ns/A/B/\nanother line");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("only valid rules are parsed") {
            REQUIRE(rules.size() == 1);
        }
    }

    GIVEN("empty lines are ignored") {
        auto config = make_config("\n\ns/A/B/\n\n");
        auto rules = parse_gcode_substitution_rules(config);
        THEN("only valid rules are parsed") {
            REQUIRE(rules.size() == 1);
        }
    }
}

SCENARIO("GCode Substitution: run_post_process integration", "[PostProcessor]") {
    GIVEN("post-processing with substitutions only (no scripts) in layer chunk") {
        std::string input =
            ";LAYER_CHANGE\nG1 E10\nG1 E20\n";
        std::string in_path = create_temp_file(input);
        std::string output_name = in_path;

        auto config = make_config("s/E10/E100/");

        bool result = run_post_process(in_path, false, "File", output_name, config);

        THEN("substitutions are applied") {
            REQUIRE(result == true);
            std::string output = read_file_content(in_path);
            REQUIRE(output.find("E100") != std::string::npos);
        }

        std::filesystem::remove(in_path);
    }

    GIVEN("post-processing with no substitutions and no scripts") {
        std::string input =
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string output_name = in_path;

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

        bool result = run_post_process(in_path, false, "File", output_name, config);

        THEN("no processing is done") {
            REQUIRE(result == false);
        }

        std::filesystem::remove(in_path);
    }
}

SCENARIO("GCode Substitution: current_extruder macro", "[PostProcessor]") {
    GIVEN("current_extruder is set after bare T command in color chunk") {
        std::string input =
            ";LAYER_CHANGE\n"
            "T1\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        // Use {current_extruder} in replacement.
        auto config = make_config("s/E10/E{current_extruder}00/C");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("current_extruder resolves to 1") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("tool_num is NOT recognized (no backward compatibility)") {
        std::string input =
            ";LAYER_CHANGE\n"
            "T1\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E{tool_num}00/C");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("tool_num is not resolved, substitution may fail or produce literal") {
            REQUIRE(modified == false);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: HEADER_BLOCK parsing", "[PostProcessor]") {
    GIVEN("HEADER_BLOCK contains total layer number") {
        std::string input =
            "; HEADER_BLOCK_START\n"
            "; total layer number: 42\n"
            "; max z height: 123.45\n"
            "; HEADER_BLOCK_END\n"
            ";LAYER_CHANGE\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E{total_layer_number}/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("total_layer_number resolves to 42") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E42") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("HEADER_BLOCK contains max_z_height") {
        std::string input =
            "; HEADER_BLOCK_START\n"
            "; total layer number: 42\n"
            "; max z height: 123.45\n"
            "; HEADER_BLOCK_END\n"
            ";LAYER_CHANGE\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E{max_z_height}/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("max_z_height resolves to 123.45") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E123.45") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: {last_layer} in compatible-mode (no EXECUTABLE_BLOCK_START)", "[PostProcessor]") {
    GIVEN("HEADER_BLOCK with total layer number, no EXECUTABLE_BLOCK_START, using ;LAYER_CHANGE") {
        // Compatible-mode G-code: HEADER_BLOCK provides total layer count,
        // but there is no EXECUTABLE_BLOCK_START marker. The fallback path
        // must still set total_layers so {last_layer} resolves correctly.
        std::string input =
            "; HEADER_BLOCK_START\n"
            "; total layer number: 3\n"
            "; HEADER_BLOCK_END\n"
            ";LAYER_CHANGE\n"
            "G1 E10\n"
            ";LAYER_CHANGE\n"
            "G1 E20\n"
            ";LAYER_CHANGE\n"
            "G1 E30\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        // Replace E10/E20/E30 with E{last_layer} to verify the macro resolves.
        auto config = make_config("s/E10/E{last_layer}/\ns/E20/E{last_layer}/\ns/E30/E{last_layer}/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("last_layer resolves correctly: false for layers 0 and 1, true for layer 2") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            // Layer 0 (first layer) — not last
            REQUIRE(output.find("Efalse") != std::string::npos);
            // Layer 2 (third and final layer) — is last
            REQUIRE(output.find("Etrue") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("no HEADER_BLOCK at all — total_layers defaults to 0, {last_layer} is false for layer 0") {
        // When there is no HEADER_BLOCK and no EXECUTABLE_BLOCK_START,
        // total_layers defaults to 0, so layer 0 == m_total_layers - 1 is false.
        std::string input =
            ";LAYER_CHANGE\n"
            "G1 E10\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E{last_layer}/");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("last_layer is false when total_layers is unknown (0)") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("Efalse") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}

SCENARIO("GCode Substitution: CP TOOLCHANGE robust tool splitting", "[PostProcessor]") {
    GIVEN("CP TOOLCHANGE START..END keeps bare T inside one color chunk") {
        std::string input =
            ";LAYER_CHANGE\n"
            "G1 E10\n"
            "; CP TOOLCHANGE START\n"
            "T1\n"
            "G1 E20\n"
            "; CP TOOLCHANGE END\n"
            "G1 E30\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/C\ns/E30/E300/C");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("E10 is replaced but E30 is in a separate color chunk") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
            REQUIRE(output.find("E300") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }

    GIVEN("bare T outside TOOLCHANGE sequence splits color chunk") {
        std::string input =
            ";LAYER_CHANGE\n"
            "T0\n"
            "G1 E10\n"
            "T1\n"
            "G1 E20\n";
        std::string in_path = create_temp_file(input);
        std::string out_path = in_path + ".out";

        auto config = make_config("s/E10/E100/C");
        auto rules = parse_gcode_substitution_rules(config);

        bool modified = apply_gcode_substitutions(in_path, out_path, std::move(rules), config);

        THEN("E10 is replaced in its color chunk") {
            REQUIRE(modified == true);
            std::string output = read_file_content(out_path);
            REQUIRE(output.find("E100") != std::string::npos);
        }

        std::filesystem::remove(in_path);
        std::filesystem::remove(out_path);
    }
}
