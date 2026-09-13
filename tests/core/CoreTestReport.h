#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace core_test {
struct Result {
    std::string name;
    double seconds = 0;
    int checks = 0;
    int failures = 0;
    std::string skip;
    std::string output;
};

// XML 1.0 and HTML share these escapes. Replace forbidden ASCII controls.
inline std::string Escape(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:
            if (c < 32 && c != '\n' && c != '\r' && c != '\t') out += "?";
            else out += static_cast<char>(c);
        }
    }
    return out;
}

class Report {
public:
    std::vector<Result> results;
    Result* current = nullptr;

    int Print(const char* format, va_list args) {
        va_list copy;
        va_copy(copy, args);
        const int count = std::vsnprintf(nullptr, 0, format, copy);
        va_end(copy);
        if (count < 0) throw std::runtime_error("Cannot format test output");
        std::vector<char> buffer(static_cast<size_t>(count) + 1);
        std::vsnprintf(buffer.data(), buffer.size(), format, args);
        if (current) current->output.append(buffer.data(), static_cast<size_t>(count));
        return std::printf("%s", buffer.data());
    }

    template<class F>
    void Run(const char* name, int& checks, int& failures, F test) {
        Result result;
        result.name = name;
        const int beforeChecks = checks, beforeFailures = failures;
        const auto start = std::chrono::steady_clock::now();
        current = &result;
        try { test(); }
        catch (const std::exception& e) {
            ++failures;
            result.output += std::string("Unhandled exception: ") + e.what() + "\n";
            std::fprintf(stderr, "%s", result.output.c_str());
        }
        catch (...) {
            ++failures;
            result.output += "Unhandled non-standard exception\n";
            std::fprintf(stderr, "%s", result.output.c_str());
        }
        current = nullptr;
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        result.checks = checks - beforeChecks;
        result.failures = failures - beforeFailures;
        results.push_back(std::move(result));
    }

    void Skip(const char* name, const char* reason) {
        if (current && current->name == name) current->skip = reason;
        else {
            Result result;
            result.name = name;
            result.skip = reason;
            results.push_back(std::move(result));
        }
    }

    void Write(const std::filesystem::path& directory, const char* arch) const {
        int failed = 0, skipped = 0, checks = 0;
        double seconds = 0;
        for (const auto& r : results) {
            failed += r.failures != 0;
            skipped += r.failures == 0 && !r.skip.empty();
            checks += r.checks;
            seconds += r.seconds;
        }
        std::ostringstream xml, html;
        xml.imbue(std::locale::classic());
        html.imbue(std::locale::classic());
        xml << std::fixed << std::setprecision(6);
        html << std::fixed << std::setprecision(3);
        xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites><testsuite name=\"StirHex core\" tests=\""
            << results.size() << "\" failures=\"" << failed << "\" errors=\"0\" skipped=\"" << skipped
            << "\" time=\"" << seconds << "\"><properties><property name=\"arch\" value=\""
            << Escape(arch) << "\"/><property name=\"checks\" value=\"" << checks << "\"/></properties>\n";
        html << "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><title>StirHex core tests</title>"
             << "<style>body{font-family:system-ui;margin:2em}table{border-collapse:collapse;width:100%}"
             << "th,td{border:1px solid #bbb;padding:.5em;text-align:left}pre{white-space:pre-wrap;overflow-wrap:anywhere}"
             << ".FAIL{background:#ffe0e0}.SKIP{background:#fff3cd}</style><h1>StirHex core tests</h1><p>"
             << Escape(arch) << " | " << results.size() << " tests | " << failed << " failed | " << skipped
             << " skipped | " << checks << " checks | " << seconds << " seconds</p>"
             << "<table><thead><tr><th>Test</th><th>Result</th><th>Seconds</th><th>Checks</th><th>Details</th></tr></thead><tbody>\n";
        for (const auto& r : results) {
            const char* status = r.failures ? "FAIL" : r.skip.empty() ? "PASS" : "SKIP";
            xml << "<testcase classname=\"StirHex.core\" name=\"" << Escape(r.name) << "\" time=\"" << r.seconds << "\">";
            if (r.failures) xml << "<failure message=\"" << r.failures << " failed checks\">" << Escape(r.output) << "</failure>";
            else if (!r.skip.empty()) xml << "<skipped message=\"" << Escape(r.skip) << "\"/>";
            if (!r.output.empty()) xml << "<system-out>" << Escape(r.output) << "</system-out>";
            xml << "</testcase>\n";
            html << "<tr class=\"" << status << "\"><td>" << Escape(r.name) << "</td><td>" << status
                 << "</td><td>" << r.seconds << "</td><td>" << r.checks << "</td><td>" << Escape(r.skip);
            if (!r.output.empty()) html << "<details><summary>Output</summary><pre>" << Escape(r.output) << "</pre></details>";
            html << "</td></tr>\n";
        }
        xml << "</testsuite></testsuites>\n";
        html << "</tbody></table></html>\n";
        std::filesystem::create_directories(directory);
        Save(directory / "report.xml", xml.str());
        Save(directory / "report.html", html.str());
    }
private:
    static void Save(const std::filesystem::path& path, const std::string& text) {
        std::ofstream out;
        out.exceptions(std::ios::failbit | std::ios::badbit);
        try {
            out.open(path, std::ios::binary | std::ios::trunc);
            out << text;
            out.close();
        } catch (const std::exception&) {
            throw std::runtime_error("Cannot write core test report: " + path.u8string());
        }
    }
};
} // namespace core_test
