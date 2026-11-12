// Ablation verdict rule: truth table shared with tests/test_verdict_parity.py through
// tests/verdict_cases.json, plus direct checks of the rule's building blocks.

#include "hydra/stats.hpp"
#include "test_harness.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef BLITZ_VERDICT_CASES_PATH
#error "BLITZ_VERDICT_CASES_PATH must point at tests/verdict_cases.json"
#endif

namespace hydra::test
{
    namespace
    {
        using stats::AblationInput;
        using stats::Verdict;

        // Minimal JSON reader: objects, arrays, strings (with simple escapes), numbers, bools and
        // null. Enough for the fixture; throws std::runtime_error on anything malformed.
        struct Json
        {
            enum class Kind
            {
                Null,
                Bool,
                Number,
                String,
                Array,
                Object
            };
            Kind kind = Kind::Null;
            bool b = false;
            double num = 0.0;
            std::string str;
            std::vector<Json> arr;
            std::map<std::string, Json> obj;

            [[nodiscard]] const Json &at(const std::string &key) const
            {
                const auto it = obj.find(key);
                if (kind != Kind::Object || it == obj.end())
                {
                    throw std::runtime_error("missing key: " + key);
                }
                return it->second;
            }
            [[nodiscard]] bool has(const std::string &key) const
            {
                return kind == Kind::Object && obj.count(key) != 0;
            }
        };

        class JsonParser
        {
        public:
            explicit JsonParser(const std::string &text) : s_(text) {}

            Json parse()
            {
                Json v = value();
                skip_ws();
                if (pos_ != s_.size())
                {
                    fail("trailing characters");
                }
                return v;
            }

        private:
            const std::string &s_;
            std::size_t pos_ = 0;

            [[noreturn]] void fail(const char *what) const
            {
                throw std::runtime_error(std::string("JSON parse error at offset ") +
                                         std::to_string(pos_) + ": " + what);
            }
            void skip_ws()
            {
                while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_])))
                {
                    ++pos_;
                }
            }
            char peek()
            {
                skip_ws();
                if (pos_ >= s_.size())
                {
                    fail("unexpected end");
                }
                return s_[pos_];
            }
            void expect(char c)
            {
                if (peek() != c)
                {
                    fail("unexpected character");
                }
                ++pos_;
            }
            bool literal(const char *word)
            {
                const std::string w(word);
                if (s_.compare(pos_, w.size(), w) == 0)
                {
                    pos_ += w.size();
                    return true;
                }
                return false;
            }
            std::string string_body()
            {
                expect('"');
                std::string out;
                while (pos_ < s_.size() && s_[pos_] != '"')
                {
                    char c = s_[pos_++];
                    if (c == '\\')
                    {
                        if (pos_ >= s_.size())
                        {
                            fail("bad escape");
                        }
                        const char e = s_[pos_++];
                        switch (e)
                        {
                        case 'n':
                            c = '\n';
                            break;
                        case 't':
                            c = '\t';
                            break;
                        case '"':
                        case '\\':
                        case '/':
                            c = e;
                            break;
                        default:
                            fail("unsupported escape");
                        }
                    }
                    out.push_back(c);
                }
                expect('"');
                return out;
            }
            Json value()
            {
                Json v;
                const char c = peek();
                if (c == '{')
                {
                    ++pos_;
                    v.kind = Json::Kind::Object;
                    if (peek() == '}')
                    {
                        ++pos_;
                        return v;
                    }
                    for (;;)
                    {
                        peek();
                        std::string key = string_body();
                        expect(':');
                        v.obj.emplace(std::move(key), value());
                        if (peek() == ',')
                        {
                            ++pos_;
                            continue;
                        }
                        expect('}');
                        return v;
                    }
                }
                if (c == '[')
                {
                    ++pos_;
                    v.kind = Json::Kind::Array;
                    if (peek() == ']')
                    {
                        ++pos_;
                        return v;
                    }
                    for (;;)
                    {
                        v.arr.push_back(value());
                        if (peek() == ',')
                        {
                            ++pos_;
                            continue;
                        }
                        expect(']');
                        return v;
                    }
                }
                if (c == '"')
                {
                    v.kind = Json::Kind::String;
                    v.str = string_body();
                    return v;
                }
                if (literal("true"))
                {
                    v.kind = Json::Kind::Bool;
                    v.b = true;
                    return v;
                }
                if (literal("false"))
                {
                    v.kind = Json::Kind::Bool;
                    return v;
                }
                if (literal("null"))
                {
                    return v;
                }
                const char *begin = s_.c_str() + pos_;
                char *end = nullptr;
                v.num = std::strtod(begin, &end);
                if (end == begin)
                {
                    fail("expected a value");
                }
                v.kind = Json::Kind::Number;
                pos_ += static_cast<std::size_t>(end - begin);
                return v;
            }
        };

        std::vector<double> numbers(const Json &array)
        {
            std::vector<double> out;
            for (const Json &x : array.arr)
            {
                out.push_back(x.num);
            }
            return out;
        }

        // Fixture gate name -> RunGates field; the names are the ones in scripts/run_ablations.sh.
        stats::RunGates gates_with_failure(const std::string &failed)
        {
            stats::RunGates g;
            if (failed.empty())
            {
                return g;
            }
            if (failed == "valid")
            {
                g.valid = false;
            }
            else if (failed == "determinism")
            {
                g.determinism_ok = false;
            }
            else if (failed == "dropped_events")
            {
                g.no_dropped_events = false;
            }
            else if (failed == "arena_fallback")
            {
                g.no_arena_fallback = false;
            }
            else if (failed == "pool_exhaustion")
            {
                g.no_pool_exhaustion = false;
            }
            else
            {
                throw std::runtime_error("unknown gate in fixture: " + failed);
            }
            return g;
        }

        AblationInput make(std::vector<double> baseline, std::vector<double> ablated)
        {
            AblationInput in;
            in.baseline_p999_ns = std::move(baseline);
            in.ablated_p999_ns = std::move(ablated);
            return in;
        }

        Json load_fixture()
        {
            std::ifstream f(BLITZ_VERDICT_CASES_PATH);
            if (!f)
            {
                throw std::runtime_error(std::string("cannot open ") + BLITZ_VERDICT_CASES_PATH);
            }
            std::stringstream ss;
            ss << f.rdbuf();
            const std::string text = ss.str();
            return JsonParser(text).parse();
        }

        // Every case of the shared fixture; the Python classifier runs the same cases.
        void test_fixture_truth_table()
        {
            const Json fixture = load_fixture();
            HYDRA_CHECK_EQ(stats::MIN_EFFECT_PERCENT, fixture.at("min_effect_percent").num);
            HYDRA_CHECK_EQ(static_cast<double>(stats::MIN_TRIALS), fixture.at("min_trials").num);

            const auto &cases = fixture.at("cases").arr;
            HYDRA_CHECK(!cases.empty());
            int mismatches = 0;
            for (const Json &c : cases)
            {
                AblationInput in = make(numbers(c.at("baseline")), numbers(c.at("ablated")));
                in.baseline = gates_with_failure(c.has("baseline_fail") ? c.at("baseline_fail").str
                                                                        : std::string());
                in.ablated = gates_with_failure(c.has("ablated_fail") ? c.at("ablated_fail").str
                                                                      : std::string());
                const std::string got = stats::verdict_name(stats::classify_ablation(in));
                if (got != c.at("expected").str)
                {
                    std::fprintf(stderr, "    case \"%s\": got %s, expected %s\n",
                                 c.at("name").str.c_str(), got.c_str(),
                                 c.at("expected").str.c_str());
                    ++mismatches;
                }
            }
            std::fprintf(stderr, "    %zu fixture cases, %d mismatches\n", cases.size(),
                         mismatches);
            HYDRA_CHECK_EQ(mismatches, 0);
        }

        // The cases the verdict claims rest on, spelled out in code as well as in the fixture.
        void test_real_data_cases()
        {
            const std::vector<double> base{1563, 1636, 1696, 1616, 1596};
            // Unpinned (79befc0): tail-only damage, 13144..264689 vs 1563..1696.
            HYDRA_CHECK(stats::classify_ablation(
                            make(base, {99555, 62720, 17060, 264689, 13144})) == Verdict::PROVEN);
            // Flat layout: median 1535 vs 1616 is -5%, inside the band although ranges separate.
            HYDRA_CHECK(stats::classify_ablation(make(base, {1454, 1556, 1535, 1539, 1534})) ==
                        Verdict::NO_MEASURABLE_EFFECT);
            // Mutex queue.
            HYDRA_CHECK(stats::classify_ablation(make(base, {6736, 6210, 6681, 6720, 6808})) ==
                        Verdict::PROVEN);
        }

        void test_boundaries_are_exact_in_integer_percent()
        {
            const std::vector<double> hundred(5, 100.0);
            // Exactly +10% counts as material; 1.10 as a double would not be exact.
            HYDRA_CHECK(stats::classify_ablation(make(hundred, std::vector<double>(5, 110.0))) ==
                        Verdict::PROVEN);
            HYDRA_CHECK(stats::classify_ablation(make(hundred, std::vector<double>(5, 109.9))) ==
                        Verdict::NO_MEASURABLE_EFFECT);
            // Exactly -10% (100 vs 110) counts as material on the faster side.
            HYDRA_CHECK(stats::classify_ablation(make(std::vector<double>(5, 110.0), hundred)) ==
                        Verdict::ABLATED_FASTER);
        }

        void test_min_trials_and_validity_precedence()
        {
            HYDRA_CHECK(stats::classify_ablation(make({1, 1, 1, 1}, {9, 9, 9, 9})) ==
                        Verdict::INCONCLUSIVE);
            // A failed gate beats the trial-count rule.
            AblationInput in = make({1, 1}, {9, 9});
            in.ablated.no_dropped_events = false;
            HYDRA_CHECK(stats::classify_ablation(in) == Verdict::INVALID);
        }

        void test_verdict_names()
        {
            HYDRA_CHECK_EQ(std::string(stats::verdict_name(Verdict::PROVEN)),
                           std::string("PROVEN"));
            HYDRA_CHECK_EQ(std::string(stats::verdict_name(Verdict::ABLATED_FASTER)),
                           std::string("ABLATED_FASTER"));
            HYDRA_CHECK_EQ(std::string(stats::verdict_name(Verdict::NO_MEASURABLE_EFFECT)),
                           std::string("NO_MEASURABLE_EFFECT"));
            HYDRA_CHECK_EQ(std::string(stats::verdict_name(Verdict::INCONCLUSIVE)),
                           std::string("INCONCLUSIVE"));
            HYDRA_CHECK_EQ(std::string(stats::verdict_name(Verdict::INVALID)),
                           std::string("INVALID"));
        }

        void test_complete_separation_probability()
        {
            HYDRA_CHECK(std::fabs(stats::complete_separation_p(5) - 1.0 / 252.0) < 1e-12);
            HYDRA_CHECK(std::fabs(stats::complete_separation_p(4) - 1.0 / 70.0) < 1e-12);
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_fixture_truth_table);
    RUN_TEST(test_real_data_cases);
    RUN_TEST(test_boundaries_are_exact_in_integer_percent);
    RUN_TEST(test_min_trials_and_validity_precedence);
    RUN_TEST(test_verdict_names);
    RUN_TEST(test_complete_separation_probability);

    return report_and_exit_code();
}
