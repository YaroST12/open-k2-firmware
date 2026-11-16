# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/stack_depth.py over small hand-written call graphs (the .ci text GCC writes with -fcallgraph-info=su,da)."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import stack_depth as sd  # noqa: E402


def ci(frames, edges):
    """.ci text: frames {name: bytes}, edges [(caller, callee)]; "__indirect_call" is GCC's indirect-call placeholder."""
    out = [f'node: {{ title: "{n}" label: "{n}\\nsrc/x.cpp:1:1\\n{b} bytes (static)\\n0 dynamic objects" }}'
           for n, b in frames.items()]
    out += [f'edge: {{ sourcename: "{s}" targetname: "{t}" label: "src/x.cpp:2:3" }}' for s, t in edges]
    return "\n".join(out)


def graph(frames, edges, **kw):
    return sd.Graph(*sd.load_text([ci(frames, edges)]), **kw)


ENV_HEADER = """
namespace k2::charger_test {
struct Options { int a; };
struct Env {
    virtual pd::Err request_pd(pd::Choice) = 0;
    virtual fc::Fail request_fc(fc::Proto, int mv, bool retarget, int ma) = 0;
    // a comment that names virtual void not_a_method() in prose
    virtual void phase(Phase, size_t index) { (void)index; }
    virtual void record(const Step&) = 0;
protected:
    ~Env() = default;
};
struct Other { virtual void unrelated() = 0; };
}
"""


class StaleRule(unittest.TestCase):
    def test_a_rule_that_matches_the_caller_but_no_function_leaves_the_call_unresolved(self):
        # the charger-test seam rule kept naming ChargerTestPage after the Env moved to ChargerTestSession: the caller
        # matched, nothing resolved, and the chain silently stopped at the Runner
        g = graph({"k2::Runner::request(int)": 400, "k2::Session::request_pd(int)": 64},
                  [("k2::Runner::request(int)", "__indirect_call")],
                  rules=[(r"k2::Runner::", r"k2::Page::request_pd\(", ("env",))])
        self.assertIn("k2::Runner::request(int)", g.unresolved)
        self.assertEqual(g.worst("k2::Runner::request(int)")[0], 400)


class Interfaces(unittest.TestCase):
    def test_virtual_methods_are_read_from_the_named_struct_only(self):
        self.assertEqual(sd.interface_methods(ENV_HEADER, "Env"), ["request_pd", "request_fc", "phase", "record"])

    def test_an_interface_resolves_to_the_implementers_methods_of_the_same_names(self):
        frames = {"k2::Runner::request(int)": 400, "k2::Session::request_pd(int)": 64,
                  "k2::Session::record(k2::Step const&)": 88, "k2::Session::poll(unsigned long)": 900,
                  "k2::Page::request_pd(int)": 2000}
        rules = sd.interface_rules([(r"k2::Runner::", ENV_HEADER, "Env", r"k2::Session::", ("env",))])
        g = graph(frames, [("k2::Runner::request(int)", "__indirect_call")], rules=rules)
        self.assertNotIn("k2::Runner::request(int)", g.unresolved)
        d, chain = g.worst("k2::Runner::request(int)")
        self.assertEqual(d, 400 + 88)       # record, not poll (no Env method) and not the Page's request_pd
        self.assertEqual(chain[-1], "k2::Session::record(k2::Step const&)")


class Scenario(unittest.TestCase):
    FR = {"main": 8, "k2::Ui::tick(unsigned long)": 100, "k2::HomePage::update(unsigned long)": 3000,
          "k2::ChargerTestPage::update(unsigned long)": 500, "k2::ChargerTestPage::on_event(k2::Event)": 2500,
          "k2::Runner::poll()": 300, "k2::Runner::request(int)": 200, "k2::fs::write()": 100}
    ED = [("main", "k2::Ui::tick(unsigned long)"), ("k2::Ui::tick(unsigned long)", "__indirect_call"),
          ("k2::Ui::tick(unsigned long)", "k2::Runner::poll()"), ("k2::Runner::poll()", "k2::Runner::request(int)"),
          ("main", "k2::fs::write()")]
    RULES = [(r"k2::Ui::tick", sd.pv("update", "on_event"), ("page", "*virt"))]

    def test_via_keeps_only_chains_through_the_named_function(self):
        g = graph(self.FR, self.ED, rules=self.RULES, via=r"Runner::request")
        d, chain = g.worst("main")
        self.assertEqual(d, 8 + 100 + 300 + 200)
        self.assertEqual(chain[-1], "k2::Runner::request(int)")

    def test_via_with_no_chain_through_it_reports_none(self):
        self.assertIsNone(graph(self.FR, self.ED, rules=self.RULES, via=r"nowhere").worst("main"))

    def test_page_and_no_events_model_a_run_on_one_page_without_presses(self):
        g = graph(self.FR, self.ED, rules=self.RULES)
        self.assertEqual(g.worst("main")[0], 8 + 100 + 3000)            # any page: Home's update
        g = graph(self.FR, self.ED, rules=self.RULES, page="ChargerTestPage")
        self.assertEqual(g.worst("main")[0], 8 + 100 + 2500)            # its on_event
        g = graph(self.FR, self.ED, rules=self.RULES, page="ChargerTestPage", events=False)
        self.assertEqual(g.worst("main")[0], 8 + 100 + 500)             # the page's update only


if __name__ == "__main__":
    unittest.main()
