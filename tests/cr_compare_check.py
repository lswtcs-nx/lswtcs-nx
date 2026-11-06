"""Check workspace-tools/tune_tcs360_context.py's float compare against the generator's form on the PC.

Builds one host program holding both bodies, taken from the tool's own OLD and NEW text, and compares the four CR bits
for every pair of 40 special values (NaNs of both signs and kinds, infinities, signed zeros, denormals, extremes,
values one ulp apart) and 20 million random bit patterns (half of them pairs with equal or adjacent values).
Usage: python tests/cr_compare_check.py --compiler <clang++>
"""
import argparse, importlib.util, pathlib, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("tune", ROOT / "workspace-tools" / "tune_tcs360_context.py")
tune = importlib.util.module_from_spec(spec); spec.loader.exec_module(tune)


def body(text, name):
    return text.replace("inline void compare(double left, double right) noexcept", f"void {name}(double left, double right)")


PROGRAM = r"""
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
struct CR {
    uint8_t lt, gt, eq, un;
%OLD%
%NEW%
};
static double bits(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
static bool same(double a, double b) {
    CR x{}, y{};
    x.old_form(a, b); y.new_form(a, b);
    return x.lt == y.lt && x.gt == y.gt && x.eq == y.eq && x.un == y.un;
}
int main() {
    const uint64_t special[] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x8000000000000001ull,
        0x000FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x8010000000000000ull,
        0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x3FF0000000000001ull, 0x3FEFFFFFFFFFFFFFull,
        0x7FEFFFFFFFFFFFFFull, 0xFFEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
        0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF0000000000001ull, 0xFFF0000000000001ull,
        0x7FF4000000000000ull, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0x7FF8000000000001ull,
        0x4000000000000000ull, 0xC000000000000000ull, 0x3E70000000000000ull, 0x47EFFFFFE0000000ull,
        0x36A0000000000000ull, 0x3810000000000000ull, 0x380FFFFFFFFFFFFFull, 0x4330000000000000ull,
        0x433FFFFFFFFFFFFFull, 0xC330000000000000ull, 0x3FB999999999999Aull, 0xBFB999999999999Aull,
        0x7FF0000000000002ull, 0x0008000000000000ull, 0x8008000000000000ull, 0x3FF8000000000000ull};
    const int n = sizeof special / sizeof special[0];
    long bad = 0, checked = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) { ++checked; if (!same(bits(special[i]), bits(special[j]))) { ++bad; if (bad < 10) std::printf("special %016llx %016llx differ\n", (unsigned long long)special[i], (unsigned long long)special[j]); } }
    uint64_t s = 0x9E3779B97F4A7C15ull;
    auto next = [&] { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    for (long k = 0; k < 20000000; ++k) {
        uint64_t a = next(), b = (k & 1) ? next() : a + (int64_t)(next() % 5) - 2;
        if ((k & 7) == 2) b ^= 0x8000000000000000ull;                // signs, including +0 against -0
        ++checked;
        if (!same(bits(a), bits(b))) { ++bad; if (bad < 10) std::printf("random %016llx %016llx differ\n", (unsigned long long)a, (unsigned long long)b); }
    }
    std::printf("%ld pairs checked, %ld differ\n", checked, bad);
    return bad != 0;
}
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--compiler", required=True)
    a = ap.parse_args()
    src = PROGRAM.replace("%OLD%", body(tune.OLD, "old_form")).replace("%NEW%", body(tune.NEW, "new_form"))
    with tempfile.TemporaryDirectory(prefix="cr-compare-") as d:
        c = pathlib.Path(d) / "check.cpp"; exe = pathlib.Path(d) / "check.exe"
        c.write_text(src)
        for opt in ("-O0", "-O2"):
            subprocess.run([a.compiler, opt, "-std=c++17", str(c), "-o", str(exe)], check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            print(opt, r.stdout.strip())
            if r.returncode: sys.exit(1)


if __name__ == "__main__":
    main()
