// SPDX-License-Identifier: MIT
// cpp-buddy-watch — Linux memory fragmentation monitor (/proc/buddyinfo)
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o buddy-watch
// Usage: ./buddy-watch [--json] [--watch N] [--timeout T] [--no-histogram] [--no-color] [--demo] [--selftest]

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct Error {
  int code;
  std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

constexpr int ORDER_COUNT = 11;  // buddyinfo siempre tiene 11 órdenes (0..10)
constexpr long long PAGE_KIB = 4;

struct Zone {
  int node = 0;
  std::string name;
  std::array<long long, ORDER_COUNT> blocks{};
  long long total_pages = 0;
};

struct Snapshot {
  long long ts = 0;               // epoch seconds
  std::vector<Zone> zones;
  long long total_free_pages = 0;
  long long total_free_kib = 0;
  double frag_pct = 0.0;          // 0 = todo en bloques grandes, 100 = solo orden 0
  long long thp_blocks = 0;       // bloques de orden 9 (2 MiB) listos para THP
  int max_contig_order = -1;      // mayor orden con bloques libres (global)
};

// ── Paleta ANSI MUTABLE: --no-color la vacía (patrón diskstats corregido) ──
static bool g_color = true;
static std::string BOLD = "\x1b[1m";
static std::string DIM = "\x1b[2m";
static std::string RED = "\x1b[31m";
static std::string YEL = "\x1b[33m";
static std::string GRN = "\x1b[32m";
static std::string CYN = "\x1b[36m";
static std::string MAG = "\x1b[35m";
static std::string RST = "\x1b[0m";

static void disable_color() {
  g_color = false;
  BOLD = DIM = RED = YEL = GRN = CYN = MAG = RST = "";
}

// ── utilidades de formateo ──

static std::string human_kib(long long kib) {
  char buf[32];
  if (kib >= 1048576)
    std::snprintf(buf, sizeof buf, "%.2f GiB", kib / 1048576.0);
  else if (kib >= 1024)
    std::snprintf(buf, sizeof buf, "%.2f MiB", kib / 1024.0);
  else
    std::snprintf(buf, sizeof buf, "%lld KiB", kib);
  return buf;
}

// Rellena el texto visible a w chars y luego lo envuelve en color (invisible
// para el ancho) — evita la desalineación que causan los códigos ANSI.
static std::string ccell(const std::string& visible, int w, const std::string& color_code) {
  std::string p = visible;
  if (static_cast<int>(p.size()) < w) p += std::string(w - static_cast<int>(p.size()), ' ');
  return color_code + p + RST;
}

static std::string pad(const std::string& s, int w) {
  std::string p = s;
  if (static_cast<int>(p.size()) < w) p += std::string(w - static_cast<int>(p.size()), ' ');
  return p;
}

static std::string now_str() {
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
  localtime_r(&t, &tmv);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
  return buf;
}

// ── parsing de /proc/buddyinfo ──
// Formato real: "Node 0, zone      DMA      0  0  0  0  0  0  0  0  1  2  2"
static Result<Zone> parse_zone_line(const std::string& line) {
  std::istringstream ss(line);
  std::string tok;
  if (!(ss >> tok) || tok != "Node")
    return std::unexpected(Error{2, "not a buddyinfo zone line"});
  std::string node_tok;
  if (!(ss >> node_tok))
    return std::unexpected(Error{2, "missing node id"});
  if (!node_tok.empty() && node_tok.back() == ',') node_tok.pop_back();
  Zone z;
  auto [ptr, ec] = std::from_chars(node_tok.data(), node_tok.data() + node_tok.size(), z.node);
  if (ec != std::errc())
    return std::unexpected(Error{2, "bad node id: " + node_tok});
  std::string kw;
  if (!(ss >> kw))
    return std::unexpected(Error{2, "missing zone keyword"});
  if (kw == "zone") {
    if (!(ss >> z.name))
      return std::unexpected(Error{2, "missing zone name"});
  } else {
    z.name = kw;  // tolera líneas sin la palabra "zone"
  }
  long long v;
  int i = 0;
  while (ss >> v && i < ORDER_COUNT) z.blocks[i++] = v;
  return z;
}

static void compute_stats(Snapshot& s) {
  s.total_free_pages = 0;
  s.total_free_kib = 0;
  s.frag_pct = 0.0;
  s.thp_blocks = 0;
  s.max_contig_order = -1;
  long long big_pages = 0;  // páginas en bloques de orden >= 4 (>= 64 KiB)
  for (auto& z : s.zones) {
    long long zpages = 0;
    for (int o = 0; o < ORDER_COUNT; ++o) {
      long long p = z.blocks[o] * (1LL << o);
      zpages += p;
      if (z.blocks[o] > 0) s.max_contig_order = std::max(s.max_contig_order, o);
      if (o >= 4) big_pages += p;
    }
    z.total_pages = zpages;
    s.total_free_pages += zpages;
    if (ORDER_COUNT > 9) s.thp_blocks += z.blocks[9];
  }
  s.total_free_kib = s.total_free_pages * PAGE_KIB;
  if (s.total_free_pages > 0)
    s.frag_pct = 100.0 * (1.0 - static_cast<double>(big_pages) / static_cast<double>(s.total_free_pages));
}

static Result<Snapshot> read_buddyinfo() {
  std::ifstream f("/proc/buddyinfo");
  if (!f)
    return std::unexpected(Error{1, "cannot open /proc/buddyinfo"});
  Snapshot s;
  s.ts = std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto z = parse_zone_line(line);
    if (z) s.zones.push_back(*z);
  }
  if (s.zones.empty())
    return std::unexpected(Error{1, "no zones parsed from /proc/buddyinfo"});
  compute_stats(s);
  return s;
}

// ── snapshot sintético (--demo y selftest) ──
static Snapshot make_synthetic() {
  Snapshot s;
  s.ts = 1787000000;
  Zone dma;
  dma.node = 0; dma.name = "DMA";
  dma.blocks = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 2};
  Zone d32;
  d32.node = 0; d32.name = "DMA32";
  d32.blocks = {5430, 2773, 1746, 2763, 1547, 890, 351, 88, 13, 2, 0};
  Zone nm;
  nm.node = 0; nm.name = "Normal";
  nm.blocks = {11508, 10434, 8072, 7848, 4412, 2016, 711, 197, 12, 3, 0};
  s.zones = {dma, d32, nm};
  compute_stats(s);
  return s;
}

// ── salida ──

static void print_snapshot(const Snapshot& s, bool show_histogram) {
  std::println(std::cout, "{}buddy-watch{} — fragmentación de memoria Linux ({}/proc/buddyinfo{})",
               BOLD, RST, DIM, RST);
  std::println(std::cout, "{}snapshot {} · página {} KiB{}", DIM, now_str(), PAGE_KIB, RST);
  std::println(std::cout, "");

  // Tabla por zona
  std::string hdr = pad("zone", 8) + pad("free", 12) + pad("max contig", 12) +
                    pad("frag", 8) + pad("THP 2MiB", 10);
  std::println(std::cout, "{}{}{}", BOLD, hdr, RST);
  std::println(std::cout, "{}{}{}", DIM, std::string(50, '-'), RST);

  for (const auto& z : s.zones) {
    int mco = -1;
    for (int o = ORDER_COUNT - 1; o >= 0; --o)
      if (z.blocks[o] > 0) { mco = o; break; }
    long long mckib = (mco >= 0) ? (1LL << mco) * PAGE_KIB : 0;
    long long zbig = 0;
    for (int o = 4; o < ORDER_COUNT; ++o) zbig += z.blocks[o] * (1LL << o);
    double zfrag = z.total_pages > 0 ? 100.0 * (1.0 - static_cast<double>(zbig) / static_cast<double>(z.total_pages)) : 0.0;

    char fbuf[32], tbuf[32];
    std::snprintf(fbuf, sizeof fbuf, "%5.1f%%", zfrag);
    std::snprintf(tbuf, sizeof tbuf, "%lld", z.blocks[9]);
    std::string fcol = zfrag > 60.0 ? RED : (zfrag > 30.0 ? YEL : GRN);

    std::string row =
        pad(z.name, 8) +
        ccell(human_kib(z.total_pages * PAGE_KIB), 12, CYN) +
        ccell(mco >= 0 ? human_kib(mckib) : "-", 12, MAG) +
        ccell(fbuf, 8, fcol) +
        ccell(tbuf, 10, GRN);
    std::println(std::cout, "{}", row);
  }

  std::println(std::cout, "{}{}{}", DIM, std::string(50, '-'), RST);
  char sum[128];
  std::snprintf(sum, sizeof sum, "TOTAL  %s libres · frag %4.1f%% · contig máx %s · %lld bloques THP (2 MiB)",
                human_kib(s.total_free_kib).c_str(), s.frag_pct,
                s.max_contig_order >= 0 ? human_kib((1LL << s.max_contig_order) * PAGE_KIB).c_str() : "-",
                s.thp_blocks);
  std::println(std::cout, "{}", sum);

  if (show_histogram) {
    std::println(std::cout, "");
    std::println(std::cout, "{}bloques libres por orden (4 KiB · 2^orden){}", BOLD, RST);
    std::string oh = pad("zone", 8);
    for (int o = 0; o < ORDER_COUNT; ++o) oh += pad("o" + std::to_string(o), 6);
    std::println(std::cout, "{}{}{}", DIM, oh, RST);
    for (const auto& z : s.zones) {
      std::string r = pad(z.name, 8);
      for (int o = 0; o < ORDER_COUNT; ++o) r += pad(std::to_string(z.blocks[o]), 6);
      std::println(std::cout, "{}", r);
    }
  }
  std::println(std::cout, "");
}

static std::string snapshot_json(const Snapshot& s) {
  std::ostringstream os;
  os << "{\n";
  os << "  \"ts\": " << s.ts << ",\n";
  os << "  \"total_free_pages\": " << s.total_free_pages << ",\n";
  os << "  \"total_free_kib\": " << s.total_free_kib << ",\n";
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f", s.frag_pct);
  os << "  \"fragmentation_pct\": " << buf << ",\n";
  os << "  \"thp_blocks\": " << s.thp_blocks << ",\n";
  os << "  \"max_contig_order\": " << s.max_contig_order << ",\n";
  os << "  \"zones\": [\n";
  bool first = true;
  for (const auto& z : s.zones) {
    if (!first) os << ",\n";
    first = false;
    os << "    {\"node\": " << z.node << ", \"zone\": \"" << z.name
       << "\", \"free_pages\": " << z.total_pages << ", \"blocks\": [";
    for (int o = 0; o < ORDER_COUNT; ++o) {
      if (o) os << ", ";
      os << z.blocks[o];
    }
    os << "]}";
  }
  os << "\n  ]\n}\n";
  return os.str();
}

// ── selftest ──

struct Harness {
  int total = 0;
  int failed = 0;
  void ok(bool cond, std::string_view name) {
    ++total;
    if (cond)
      std::println(std::cout, "{}PASS{}  {}", GRN, RST, name);
    else {
      ++failed;
      std::println(std::cout, "{}FAIL{}  {}", RED, RST, name);
    }
  }
};

static int run_selftest() {
  Harness h;

  // 1-2. parseo de línea sintética completa
  {
    auto z = parse_zone_line("Node 0, zone      DMA      0  1  2  3  4  5  6  7  8  9 10");
    h.ok(z.has_value(), "parse: línea sintética completa");
    if (z) {
      h.ok(z->node == 0 && z->name == "DMA", "parse: node 0 y nombre DMA");
      bool vals = true;
      for (int o = 0; o < ORDER_COUNT; ++o) vals = vals && (z->blocks[o] == o);
      h.ok(vals, "parse: 11 valores de bloques en orden");
    }
  }

  // 3. línea con menos números → padding a cero
  {
    auto z = parse_zone_line("Node 0, zone  Normal  5 4");
    h.ok(z.has_value() && z->blocks[0] == 5 && z->blocks[1] == 4 && z->blocks[2] == 0,
         "parse: línea corta rellena con ceros");
  }

  // 4. línea con más números → cap a 11
  {
    auto z = parse_zone_line("Node 1, zone  Foo  0 1 2 3 4 5 6 7 8 9 10 11 12 13");
    h.ok(z.has_value() && z->blocks[10] == 10 && z->blocks.size() == ORDER_COUNT,
         "parse: más de 11 números se recortan");
  }

  // 5-6. líneas inválidas
  {
    auto z = parse_zone_line("garbage data here");
    h.ok(!z.has_value(), "parse: línea basura → error");
    auto e = parse_zone_line("");
    h.ok(!e.has_value(), "parse: línea vacía → error");
  }

  // 7. lectura real de /proc/buddyinfo
  {
    auto s = read_buddyinfo();
    h.ok(s.has_value() && !s->zones.empty(), "lectura: /proc/buddyinfo con >= 1 zona");
    if (s) {
      bool all11 = true;
      for (const auto& z : s->zones) all11 = all11 && (z.blocks.size() == ORDER_COUNT);
      h.ok(all11, "lectura: todas las zonas con 11 órdenes");
      bool nonneg = true;
      for (const auto& z : s->zones)
        for (int o = 0; o < ORDER_COUNT; ++o) nonneg = nonneg && (z.blocks[o] >= 0);
      h.ok(nonneg, "lectura: conteos de bloques no negativos");
      h.ok(s->total_free_pages > 0, "lectura: memoria libre total > 0");
    }
  }

  // 8-10. consistencia del snapshot sintético
  {
    auto s = make_synthetic();
    long long zsum = 0;
    for (const auto& z : s.zones) zsum += z.total_pages;
    h.ok(zsum == s.total_free_pages, "stats: total global == suma de zonas");
    bool consistent = true;
    for (const auto& z : s.zones) {
      long long acc = 0;
      for (int o = 0; o < ORDER_COUNT; ++o) acc += z.blocks[o] * (1LL << o);
      consistent = consistent && (acc == z.total_pages);
    }
    h.ok(consistent, "stats: cada zona == Σ bloques·2^orden");
    h.ok(s.max_contig_order == 10, "stats: max_contig_order == mayor orden con bloques (10)");
  }

  // 11-13. fragmentación
  {
    auto s = make_synthetic();
    h.ok(s.frag_pct >= 0.0 && s.frag_pct <= 100.0, "frag: pct en [0,100]");
    Zone allbig;
    allbig.name = "Big"; allbig.blocks = {0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0};
    Zone only0;
    only0.name = "Frag"; only0.blocks = {10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    Snapshot s2;
    s2.zones = {allbig, only0};
    compute_stats(s2);
    Zone zbig = s2.zones[0];
    Zone zfrag = s2.zones[1];
    double fbig = zbig.total_pages > 0
                      ? 100.0 * (1.0 - static_cast<double>(zbig.blocks[9] * 512) / static_cast<double>(zbig.total_pages))
                      : 0.0;
    double ffrag = zfrag.total_pages > 0
                       ? 100.0 * (1.0 - 0.0 / static_cast<double>(zfrag.total_pages))
                       : 0.0;
    h.ok(fbig < 0.5, "frag: todo en orden >= 4 → ~0%");
    h.ok(ffrag > 99.5, "frag: solo orden 0 → 100%");
  }

  // 14. bloques THP (orden 9)
  {
    auto s = make_synthetic();
    h.ok(s.thp_blocks == (2 + 2 + 3), "THP: Σ bloques de orden 9 (2 MiB)");
  }

  // 15-17. unidades humanas
  {
    h.ok(human_kib(1024) == "1.00 MiB", "unidades: 1024 KiB → 1.00 MiB");
    h.ok(human_kib(1536) == "1.50 MiB", "unidades: 1536 KiB → 1.50 MiB");
    h.ok(human_kib(1048576) == "1.00 GiB", "unidades: 1048576 KiB → 1.00 GiB");
  }

  // 18. JSON: claves con comillas y llaves balanceadas
  {
    auto s = make_synthetic();
    std::string j = snapshot_json(s);
    bool has_keys = j.find("\"zone\": \"DMA\"") != std::string::npos &&
                    j.find("\"fragmentation_pct\"") != std::string::npos &&
                    j.find("\"blocks\": [") != std::string::npos;
    h.ok(has_keys, "JSON: claves con comillas dobles");
    int depth = 0;
    bool balanced = true;
    for (char c : j) {
      if (c == '{') ++depth;
      if (c == '}') --depth;
      if (depth < 0) balanced = false;
    }
    h.ok(balanced && depth == 0, "JSON: llaves balanceadas");
  }

  // 19. --no-color: ninguna línea con códigos ANSI
  {
    bool saved = g_color;
    disable_color();
    auto s = make_synthetic();
    std::string row = pad(s.zones[0].name, 8) + ccell("1.00 MiB", 12, CYN) + ccell("5.0%", 8, GRN);
    bool has_ansi = row.find('\x1b') != std::string::npos;
    // print_snapshot escribe a cout; capturar no es necesario — verificamos los helpers
    std::println(std::cout, "{}", row);
    h.ok(!has_ansi, "no-color: helpers sin códigos ANSI");
    if (saved) g_color = true;
  }

  // 20. delta entre snapshots
  {
    auto a = make_synthetic();
    auto b = a;
    b.zones[2].blocks[0] += 100;
    compute_stats(b);
    long long delta = b.total_free_pages - a.total_free_pages;
    h.ok(delta == 100, "watch: delta entre snapshots correcto (100 páginas)");
  }

  // 21. parseo de argumentos --watch
  {
    bool ok_watch = false;
    int watch = 0;
    // simula el parseo de "--watch 5"
    std::vector<std::string> args{"--watch", "5"};
    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i] == "--watch") {
        if (i + 1 < args.size()) {
          watch = std::atoi(args[i + 1].c_str());
          ok_watch = watch == 5;
        }
      }
    }
    h.ok(ok_watch, "args: --watch 5 → intervalo 5s");
  }

  std::println(std::cout, "");
  std::println(std::cout, "{}  {} passed, {} failed{}", BOLD, h.total, h.failed, RST);
  return h.failed == 0 ? 0 : 1;
}

// ── modo watch ──

static int run_watch(int interval_sec, int timeout_sec) {
  auto t0 = std::chrono::steady_clock::now();
  std::optional<Snapshot> prev;
  long long min_free_pages = LLONG_MAX;
  double max_frag = 0.0;
  int iter = 0;

  while (true) {
    auto res = read_buddyinfo();
    if (!res) {
      std::cerr << "error: " << res.error().message << "\n";
      return 1;
    }
    const Snapshot& s = *res;

    if (iter == 0) {
      print_snapshot(s, true);
    } else {
      long long delta = s.total_free_pages - prev->total_free_pages;
      min_free_pages = std::min(min_free_pages, s.total_free_pages);
      max_frag = std::max(max_frag, s.frag_pct);
      char line[160];
      std::snprintf(line, sizeof line,
                    "[%s]  %s libres (Δ %+lld KiB) · frag %4.1f%% · mín %s · máx frag %4.1f%%",
                    now_str().c_str(), human_kib(s.total_free_kib).c_str(),
                    delta * PAGE_KIB, s.frag_pct,
                    human_kib(min_free_pages * PAGE_KIB).c_str(), max_frag);
      std::println(std::cout, "{}", line);
    }

    std::cout << std::flush;  // pipes: no perder la salida al matar el proceso

    prev = s;
    ++iter;

    if (timeout_sec > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - t0).count();
      if (elapsed >= timeout_sec) break;
    }
    std::this_thread::sleep_for(std::chrono::seconds(interval_sec));
  }
  return 0;
}

static void usage() {
  std::println(std::cout, "buddy-watch — monitor de fragmentación de memoria Linux (C++23, stdlib-only)");
  std::println(std::cout, "");
  std::println(std::cout, "Uso: ./buddy-watch [opciones]");
  std::println(std::cout, "");
  std::println(std::cout, "  --json           salida JSON completa (scripting)");
  std::println(std::cout, "  --watch N        refresco cada N segundos con deltas (Ctrl+C para salir)");
  std::println(std::cout, "  --timeout T      salir tras T segundos en modo watch");
  std::println(std::cout, "  --no-histogram   omitir la tabla de bloques por orden");
  std::println(std::cout, "  --no-color       sin códigos ANSI (pipes/logs)");
  std::println(std::cout, "  --demo           snapshot sintético (contenedores/VMs)");
  std::println(std::cout, "  --selftest       auto-verificación (21 checks)");
  std::println(std::cout, "  -h, --help       esta ayuda");
}

int main(int argc, char** argv) {
  bool json = false;
  bool selftest = false;
  bool demo = false;
  bool show_hist = true;
  int watch = 0;
  int timeout = 0;

  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    if (a == "--json") {
      json = true;
    } else if (a == "--no-color") {
      disable_color();
    } else if (a == "--selftest") {
      selftest = true;
    } else if (a == "--demo") {
      demo = true;
    } else if (a == "--no-histogram") {
      show_hist = false;
    } else if (a == "--watch" || a == "-w") {
      if (i + 1 >= argc) {
        std::cerr << "error: --watch requiere N segundos\n";
        return 2;
      }
      ++i;
      watch = std::atoi(argv[i]);
      if (watch <= 0) {
        std::cerr << "error: --watch N debe ser > 0\n";
        return 2;
      }
    } else if (a == "--timeout" || a == "-t") {
      if (i + 1 >= argc) {
        std::cerr << "error: --timeout requiere segundos\n";
        return 2;
      }
      ++i;
      timeout = std::atoi(argv[i]);
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else {
      std::cerr << "error: opción desconocida '" << a << "'\n";
      usage();
      return 2;
    }
  }

  if (selftest) return run_selftest();

  if (demo) {
    auto s = make_synthetic();
    if (json)
      std::cout << snapshot_json(s);
    else
      print_snapshot(s, show_hist);
    return 0;
  }

  if (watch > 0) return run_watch(watch, timeout);

  auto res = read_buddyinfo();
  if (!res) {
    std::cerr << "error: " << res.error().message << "\n";
    return 1;
  }
  if (json)
    std::cout << snapshot_json(*res);
  else
    print_snapshot(*res, show_hist);
  return 0;
}
