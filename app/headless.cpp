#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <srz80/engine.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
fs::path utf8_path(const std::string &s) {
  return fs::path(std::u8string(s.begin(), s.end()));
}
SrzSlice slice(const std::string &s) { return {s.data(), s.size()}; }
const char *safe(const char *s) { return s ? s : ""; }
uint64_t number(const Json &v) {
  if (v.is_number_unsigned())
    return v.get<uint64_t>();
  if (v.is_number_integer() && v.get<int64_t>() >= 0)
    return v.get<uint64_t>();
  if (v.is_string()) {
    auto s = v.get<std::string>();
    if (s.empty() || s[0] == '-' || s[0] == '+' ||
        s.find_first_of(" \t\r\n") != std::string::npos)
      throw std::runtime_error("Expected unsigned integer");
    size_t used = 0;
    auto n = std::stoull(s, &used, s.substr(0, 2) == "0x" ? 16 : 10);
    if (used == s.size())
      return n;
  }
  throw std::runtime_error("Expected unsigned integer or decimal/0x string");
}
uint64_t duration(const std::string &s) {
  for (const auto &u : std::vector<std::pair<std::string, uint64_t>>{
           {"ns", 1}, {"us", 1000}, {"ms", 1000000}, {"s", 1000000000}})
    if (s.size() > u.first.size() &&
        s.compare(s.size() - u.first.size(), u.first.size(), u.first) == 0) {
      auto n = number(s.substr(0, s.size() - u.first.size()));
      if (n && n <= UINT64_MAX / u.second)
        return n * u.second;
      break;
    }
  throw std::runtime_error(
      "Duration must be a positive integer followed by ns, us, ms or s");
}
std::ofstream output(const fs::path &path, bool binary = false) {
  if (path.has_parent_path())
    fs::create_directories(path.parent_path());
  std::ofstream f(path,
                  std::ios::out | std::ios::trunc |
                      (binary ? std::ios::binary : std::ios::openmode(0)));
  f.exceptions(std::ios::failbit | std::ios::badbit);
  return f;
}
void little(std::ostream &f, uint32_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i)
    f.put(static_cast<char>(value >> (8 * i)));
}
class Wav {
  std::ofstream file;
  uint32_t rate;
  uint64_t frames = 0;
  void header() {
    file.seekp(0);
    file.write("RIFF", 4);
    little(file, 36 + static_cast<uint32_t>(frames * 4), 4);
    file.write("WAVEfmt ", 8);
    little(file, 16, 4);
    little(file, 1, 2);
    little(file, 2, 2);
    little(file, rate, 4);
    little(file, rate * 4, 4);
    little(file, 4, 2);
    little(file, 16, 2);
    file.write("data", 4);
    little(file, static_cast<uint32_t>(frames * 4), 4);
  }

public:
  Wav(const fs::path &path, uint32_t hz) : file(output(path, true)), rate(hz) {
    header();
  }
  void append(const int16_t *pcm, uint32_t count) {
    if (frames + count > (UINT32_MAX - 36) / 4)
      throw std::runtime_error("WAV exceeds RIFF size limit");
    for (uint64_t i = 0; i < uint64_t(count) * 2; ++i)
      little(file, static_cast<uint16_t>(pcm[i]), 2);
    frames += count;
  }
  uint64_t finish() {
    header();
    file.close();
    return frames;
  }
};
void bitmap(const fs::path &path, uint32_t w, uint32_t h,
            const std::vector<uint8_t> &rgba) {
  // Top-down 32-bit BI_RGB, BGRX channel order.
  auto f = output(path, true);
  f.write("BM", 2);
  little(f, 54 + static_cast<uint32_t>(rgba.size()), 4);
  little(f, 0, 4);
  little(f, 54, 4);
  little(f, 40, 4);
  little(f, w, 4);
  little(f, 0u - h, 4);
  little(f, 1, 2);
  little(f, 32, 2);
  little(f, 0, 4);
  little(f, static_cast<uint32_t>(rgba.size()), 4);
  for (int i = 0; i < 4; ++i)
    little(f, 0, 4);
  for (size_t i = 0; i < rgba.size(); i += 4) {
    f.put(static_cast<char>(rgba[i + 2]));
    f.put(static_cast<char>(rgba[i + 1]));
    f.put(static_cast<char>(rgba[i]));
    f.put(0);
  }
  f.close();
}
struct Options {
  std::string project, plugins, script, report, screenshots, wav;
  uint64_t run = 0, timeout = 60000000000ULL;
  uint32_t rate = 44100;
};
void help() {
  std::cout << R"(Usage: srz80 PROJECT [options]
  --run DURATION        Advance simulation (e.g. 500ms, 2s)
  --script FILE         Execute a version 1 JSON test script
  --json FILE|-         Write structured results, including failures
  --plugins DIRECTORY   Card library directory
  --screenshots DIR     Capture all final video surfaces as BMP
  --wav FILE            Record mixed stereo S16 PCM during run steps
  --sample-rate HZ      Mixer rate, 8000..384000 (default 44100)
  --timeout DURATION    Total execution wall-time budget (default 60s)
  -h, --help            Show help

Without run/script, inspect a paused project. --run precedes script steps.
Exit codes: 0 success, 1 simulation/assertion/capture failure, 2 argument error.
See docs/cli.md for test scripts and capture/report semantics.
)";
}
Options parse(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (++i >= argc)
        throw std::runtime_error("Missing value for " + a);
      return argv[i];
    };
    if (a == "--run")
      o.run = duration(value());
    else if (a == "--timeout")
      o.timeout = duration(value());
    else if (a == "--script")
      o.script = value();
    else if (a == "--json")
      o.report = value();
    else if (a == "--plugins")
      o.plugins = value();
    else if (a == "--screenshots")
      o.screenshots = value();
    else if (a == "--wav")
      o.wav = value();
    else if (a == "--sample-rate") {
      auto n = number(value());
      if (n < 8000 || n > 384000)
        throw std::runtime_error("Sample rate must be 8000..384000");
      o.rate = static_cast<uint32_t>(n);
    } else if (!a.empty() && a[0] != '-' && o.project.empty())
      o.project = a;
    else
      throw std::runtime_error("Unknown argument: " + a);
  }
  if (o.project.empty())
    throw std::runtime_error("PROJECT is required");
  if (!o.wav.empty() && !o.run && o.script.empty())
    throw std::runtime_error("--wav requires --run or --script");
  return o;
}
class Runner {
  SrzEngine *e = nullptr;
  SrzResult *r = nullptr;
  const Options &options;
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  std::unique_ptr<Wav> wav;
  std::vector<int16_t> pcm = std::vector<int16_t>(8192);
  uint64_t start_ns = 0;
  std::string message(uint64_t(SRZ_CALL *fn)(const SrzEngine *, char *,
                                             uint64_t)) {
    std::vector<char> b(static_cast<size_t>(fn(e, nullptr, 0)) + 1);
    fn(e, b.data(), b.size());
    return b.data();
  }
  void check(SrhStatus status, const std::string &op) {
    if (status != SRH_OK)
      throw std::runtime_error(op + " (status " + std::to_string(status) +
                               "): " + message(srz80_engine_last_error));
  }
  SrhHandle named(const std::string &name, bool card) {
    uint32_t n = 0;
    if (card) {
      check(srz80_engine_cards(e, r), "List cards");
      auto list = srz80_engine_result_cards(r, &n);
      SrhHandle found = 0;
      for (uint32_t i = 0; i < n; ++i)
        if (name == safe(list[i].name)) {
          if (found)
            throw std::runtime_error("Ambiguous card name: " + name);
          found = list[i].id;
        }
      if (found)
        return found;
    } else {
      check(srz80_engine_spaces(e, r), "List spaces");
      auto list = srz80_engine_result_spaces(r, &n);
      for (uint32_t i = 0; i < n; ++i)
        if (name == safe(list[i].name))
          return list[i].id;
    }
    throw std::runtime_error(std::string("Unknown ") +
                             (card ? "card: " : "space: ") + name);
  }
  Json properties(SrhHandle card) {
    check(srz80_engine_properties(e, card, r), "Read properties");
    uint32_t n = 0;
    auto p = srz80_engine_result_properties(r, &n);
    Json out = Json::array();
    for (uint32_t i = 0; i < n; ++i)
      out.push_back({{"name", safe(p[i].name)},
                     {"unsigned", p[i].value.unsigned_value},
                     {"signed", p[i].value.signed_value},
                     {"text", std::string(p[i].value.text)},
                     {"kind", p[i].kind}});
    return out;
  }
  std::string text(SrhHandle card) {
    uint32_t size = 0, total = 0;
    check(srz80_engine_text_query(e, card, 0, nullptr, &size, &total),
          "Size text");
    if (total > 64 * 1024 * 1024)
      throw std::runtime_error("Text exceeds 64 MiB capture limit");
    std::vector<uint8_t> bytes(total);
    size = total;
    check(srz80_engine_text_query(e, card, 0, bytes.data(), &size, &total),
          "Read text");
    return std::string(bytes.begin(), bytes.begin() + size);
  }
  void drain() {
    for (;;) {
      auto n = srz80_engine_audio_read(e, pcm.data(), 4096);
      if (!n)
        break;
      if (wav)
        wav->append(pcm.data(), n);
    }
  }

public:
  Json report = {{"schema_version", 1},
                 {"ok", false},
                 {"steps", Json::array()},
                 {"artifacts", Json::array()}};
  explicit Runner(const Options &o) : options(o) {}
  ~Runner() {
    if (r)
      srz80_engine_result_destroy(e, r);
    if (e)
      srz80_engine_destroy(e);
  }
  void load() {
    e = srz80_engine_create(slice(options.plugins));
    if (!e)
      throw std::runtime_error("Cannot create engine");
    r = srz80_engine_result_create(e);
    if (!r)
      throw std::runtime_error("Cannot create result arena");
    check(srz80_engine_load_project(e, slice(options.project),
                                    slice(options.plugins), SRZ_PAUSED),
          "Load project");
    start_ns = srz80_engine_now(e);
    srz80_engine_set_trace_capture(e, 0, 0);
    check(srz80_engine_cards(e, r), "List cards");
    uint32_t n = 0;
    auto cards = srz80_engine_result_cards(r, &n);
    for (uint32_t i = 0; i < n; ++i)
      if (*safe(cards[i].load_error))
        throw std::runtime_error("Unavailable card " +
                                 std::string(safe(cards[i].name)) + ": " +
                                 safe(cards[i].load_error));
    check(srz80_engine_audio_set_sample_rate(e, options.rate),
          "Set sample rate");
    srz80_engine_audio_set_queue_capacity(e, options.rate);
    if (!options.wav.empty())
      wav = std::make_unique<Wav>(utf8_path(options.wav), options.rate);
  }
  void run(uint64_t ns) {
    auto now = srz80_engine_now(e);
    if (ns > UINT64_MAX - now)
      throw std::runtime_error("Simulation time overflow");
    const auto target = now + ns;
    check(srz80_engine_resume(e), "Resume");
    while (srz80_engine_now(e) < target) {
      auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - started)
                         .count();
      if (static_cast<uint64_t>(elapsed) >= options.timeout)
        throw std::runtime_error("Wall-time timeout");
      now = srz80_engine_now(e);
      check(srz80_engine_run_slice(
                e, 100000, now + std::min<uint64_t>(target - now, 1000000),
                2000000),
            "Run");
      drain();
      auto reason = message(srz80_engine_stop_reason);
      if (srz80_engine_run_state(e) != SRZ_RUNNING || reason != "Running")
        throw std::runtime_error("Simulation stopped: " + reason);
    }
    srz80_engine_pause(e);
  }
  void screenshots(const fs::path &directory, const std::string &owner = "") {
    check(srz80_engine_video_surfaces(e, r), "List video");
    uint32_t n = 0;
    auto ptr = srz80_engine_result_video_surfaces(r, &n);
    std::vector<SrzVideoSurface> surfaces;
    for (uint32_t i = 0; i < n; ++i)
      surfaces.push_back(ptr[i]);
    SrhHandle selected = owner.empty() ? 0 : named(owner, true);
    unsigned captured = 0;
    for (const auto &s : surfaces) {
      if (selected && s.owner != selected)
        continue;
      uint64_t length = uint64_t(s.width) * s.height * 4;
      if (!length || length > 256 * 1024 * 1024 || s.width > INT32_MAX ||
          s.height > INT32_MAX)
        throw std::runtime_error("Invalid or oversized video surface");
      std::vector<uint8_t> bytes(static_cast<size_t>(length));
      uint32_t size = static_cast<uint32_t>(length), total = 0;
      check(srz80_engine_video_read(e, s.id, 0, bytes.data(), &size, &total),
            "Read video");
      if (size != length || total != length)
        throw std::runtime_error("Incomplete video capture");
      auto path = directory / ("surface-" + std::to_string(s.id) + ".bmp");
      bitmap(path, s.width, s.height, bytes);
      report["artifacts"].push_back({{"kind", "video"},
                                     {"path", path.string()},
                                     {"surface", s.id},
                                     {"owner", s.owner},
                                     {"width", s.width},
                                     {"height", s.height},
                                     {"time_ns", srz80_engine_now(e)}});
      ++captured;
    }
    if (!captured)
      throw std::runtime_error("No matching video surfaces");
  }
  void step(const Json &s, const fs::path &base) {
    auto op = s.at("op").get<std::string>();
    if (op == "run")
      run(duration(s.at("duration").get<std::string>()));
    else if (op == "screenshot")
      screenshots(base / utf8_path(s.at("directory").get<std::string>()),
                  s.value("card", std::string()));
    else if (op == "input") {
      auto endpoint = s.at("endpoint").get<std::string>();
      if (s.contains("text") == s.contains("bytes"))
        throw std::runtime_error("Input requires exactly one of text or bytes");
      std::string data;
      if (s.contains("text"))
        data = s.at("text").get<std::string>();
      else {
        if (!s.at("bytes").is_array())
          throw std::runtime_error("Input bytes must be an array");
        for (const auto &v : s.at("bytes")) {
          auto b = number(v);
          if (b > 255)
            throw std::runtime_error("Input byte out of range");
          data.push_back(static_cast<char>(b));
        }
      }
      auto delay =
          s.contains("after") ? duration(s.at("after").get<std::string>()) : 0;
      auto now = srz80_engine_now(e);
      if (delay > UINT64_MAX - now)
        throw std::runtime_error("Input time overflow");
      check(srz80_engine_enqueue_input_batch(
                e, slice(endpoint), now + delay,
                reinterpret_cast<const uint8_t *>(data.data()), data.size(), 1,
                0),
            "Queue input");
    } else if (op == "write" || op == "expect_memory") {
      auto space = named(s.at("space"), false);
      auto address = number(s.at("address"));
      auto bytes = s.at("bytes");
      if (!bytes.is_array() || bytes.empty() || bytes.size() > 1048576 ||
          bytes.size() - 1 > UINT64_MAX - address)
        throw std::runtime_error("Invalid byte array/range");
      std::vector<uint8_t> values;
      for (const auto &v : bytes) {
        auto b = number(v);
        if (b > 255)
          throw std::runtime_error("Byte out of range");
        values.push_back(static_cast<uint8_t>(b));
      }
      for (size_t i = 0; i < values.size(); ++i) {
        if (op == "write")
          check(srz80_engine_write(e, 0, space, address + i, values[i]),
                "Write memory");
        else {
          uint8_t actual = 0;
          check(srz80_engine_read(e, 0, space, address + i, &actual, 1),
                "Peek memory");
          if (actual != values[i])
            throw std::runtime_error("Memory mismatch at " +
                                     std::to_string(address + i) +
                                     ": expected " + std::to_string(values[i]) +
                                     ", got " + std::to_string(actual));
        }
      }
    } else if (op == "expect_text") {
      auto actual = text(named(s.at("card"), true));
      auto expected = s.at("contains").get<std::string>();
      if (actual.find(expected) == std::string::npos)
        throw std::runtime_error("Text assertion failed for " +
                                 s.at("card").get<std::string>());
    } else if (op == "expect_property") {
      auto props = properties(named(s.at("card"), true));
      bool found = false;
      auto field = s.value("field", std::string("unsigned"));
      if (field != "unsigned" && field != "signed" && field != "text")
        throw std::runtime_error("Invalid property field");
      for (const auto &p : props)
        if (p.at("name") == s.at("property")) {
          found = true;
          auto expected = field == "unsigned" ? Json(number(s.at("equals")))
                                              : s.at("equals");
          if (p.at(field) != expected)
            throw std::runtime_error("Property mismatch: " + p.dump());
        }
      if (!found)
        throw std::runtime_error("Unknown property");
    } else
      throw std::runtime_error("Unknown script operation: " + op);
  }
  void snapshot() {
    if (!e || !r)
      return;
    report["engine_version"] = safe(srz80_engine_version());
    report["time_ns"] = srz80_engine_now(e);
    report["advanced_ns"] = srz80_engine_now(e) - start_ns;
    report["stop_reason"] = message(srz80_engine_stop_reason);
    check(srz80_engine_cards(e, r), "List cards");
    uint32_t n = 0;
    auto cards = srz80_engine_result_cards(r, &n);
    Json list = Json::array();
    for (uint32_t i = 0; i < n; ++i)
      list.push_back({{"id", cards[i].id},
                      {"name", safe(cards[i].name)},
                      {"type", safe(cards[i].type)},
                      {"active", cards[i].active != 0},
                      {"load_error", safe(cards[i].load_error)}});
    report["cards"] = list;
    for (auto &c : report["cards"])
      if (c["load_error"] == "")
        c["properties"] = properties(c["id"].get<SrhHandle>());
    check(srz80_engine_logs(e, r), "Read logs");
    auto logs = srz80_engine_result_logs(r, &n);
    report["logs"] = Json::array();
    for (uint32_t i = 0; i < n; ++i)
      report["logs"].push_back(safe(logs[i]));
    check(srz80_engine_text_endpoints(e, r), "List text");
    auto endpoints = srz80_engine_result_text_endpoints(r, &n);
    std::vector<SrhHandle> ids;
    for (uint32_t i = 0; i < n; ++i)
      ids.push_back(endpoints[i].card);
    report["text"] = Json::array();
    for (auto id : ids)
      report["text"].push_back({{"card", id}, {"text", text(id)}});
    check(srz80_engine_spaces(e, r), "List spaces");
    auto spaces = srz80_engine_result_spaces(r, &n);
    report["spaces"] = Json::array();
    for (uint32_t i = 0; i < n; ++i)
      report["spaces"].push_back({{"id", spaces[i].id},
                                  {"name", safe(spaces[i].name)},
                                  {"maximum", spaces[i].maximum}});
    check(srz80_engine_input_endpoints(e, r), "List inputs");
    auto inputs = srz80_engine_result_endpoints(r, &n);
    report["inputs"] = Json::array();
    for (uint32_t i = 0; i < n; ++i)
      report["inputs"].push_back(safe(inputs[i]));
    check(srz80_engine_video_surfaces(e, r), "List video");
    auto video = srz80_engine_result_video_surfaces(r, &n);
    report["video"] = Json::array();
    for (uint32_t i = 0; i < n; ++i)
      report["video"].push_back({{"id", video[i].id},
                                 {"owner", video[i].owner},
                                 {"width", video[i].width},
                                 {"height", video[i].height}});
    check(srz80_engine_audio_sources(e, r), "List audio");
    auto sources = srz80_engine_result_audio_sources(r, &n);
    report["audio_sources"] = Json::array();
    for (uint32_t i = 0; i < n; ++i)
      report["audio_sources"].push_back(
          {{"id", sources[i].id},
           {"owner", sources[i].owner},
           {"name", safe(sources[i].name)},
           {"active", sources[i].active != 0},
           {"muted", sources[i].muted != 0},
           {"volume_percent", sources[i].volume_percent}, {"pan", srz80_engine_audio_source_pan(e, sources[i].id)}});
    SrzClock clocks[3]{};
    for (auto &c : clocks) {
      c.abi_version = SRZ80_ENGINE_ABI;
      c.struct_size = sizeof(c);
    }
    check(srz80_engine_clocks(e, clocks, 3, &n), "Read clocks");
    report["clocks"] = Json::array();
    for (uint32_t i = 0; i < n; ++i)
      report["clocks"].push_back(
          {{"hz", clocks[i].hz}, {"ticks", clocks[i].ticks}});
    SrzAudioDiagnostics d{};
    d.abi_version = SRZ80_ENGINE_ABI;
    d.struct_size = sizeof(d);
    check(srz80_engine_audio_diagnostics(e, &d), "Audio diagnostics");
    report["audio"] = {{"sample_rate", options.rate},
                       {"dropped_frames", d.dropped_frames},
                       {"source_errors", d.source_errors}};
    if (d.source_errors || (wav && d.dropped_frames))
      throw std::runtime_error("Audio source failure or lost capture frames");
  }
  void finish_audio() {
    if (wav) {
      auto frames = wav->finish();
      wav.reset();
      report["artifacts"].push_back({{"kind", "audio"},
                                     {"path", options.wav},
                                     {"frames", frames},
                                     {"sample_rate", options.rate},
                                     {"channels", 2}});
    }
  }
};
} // namespace

int main(int argc, char **argv) {
  if (argc == 1) {
    help();
    return 0;
  }
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
      help();
      return 0;
    }
  Options o;
  try {
    o = parse(argc, argv);
  } catch (const std::exception &ex) {
    std::cerr << ex.what() << '\n';
    return 2;
  }
  Runner runner(o);
  std::string error;
  try {
    Json steps = Json::array();
    if (!o.script.empty()) {
      std::ifstream f(utf8_path(o.script));
      if (!f)
        throw std::runtime_error("Cannot open script: " + o.script);
      Json script;
      f >> script;
      if (script.at("version") != 1 || !script.at("steps").is_array())
        throw std::runtime_error("Expected script version 1 and steps array");
      steps = script.at("steps");
    }
    runner.load();
    if (o.run)
      runner.run(o.run);
    for (size_t i = 0; i < steps.size(); ++i) {
      Json result = {{"index", i}, {"ok", false}};
      try {
        result["op"] = steps[i].at("op");
        runner.step(steps[i], utf8_path(o.script).parent_path());
        result["ok"] = true;
      } catch (const std::exception &ex) {
        result["error"] = ex.what();
        runner.report["steps"].push_back(result);
        throw;
      }
      runner.report["steps"].push_back(result);
    }
    if (!o.screenshots.empty())
      runner.screenshots(utf8_path(o.screenshots));
  } catch (const std::exception &ex) {
    error = ex.what();
  }
  try {
    runner.snapshot();
  } catch (const std::exception &ex) {
    if (error.empty())
      error = ex.what();
    else
      runner.report["inspection_error"] = ex.what();
  }
  try {
    runner.finish_audio();
  } catch (const std::exception &ex) {
    if (error.empty())
      error = ex.what();
  }
  runner.report["ok"] = error.empty();
  if (!error.empty())
    runner.report["error"] = error;
  try {
    auto json =
        runner.report.dump(2, ' ', false, Json::error_handler_t::replace);
    if (o.report == "-")
      std::cout << json << '\n';
    else {
      if (!o.report.empty()) {
        auto f = output(utf8_path(o.report));
        f << json << '\n';
        f.close();
      }
      std::cout << (error.empty() ? "OK" : "FAILED") << ": advanced "
                << runner.report.value("advanced_ns", uint64_t(0)) << " ns\n";
    }
  } catch (const std::exception &ex) {
    std::cerr << "Cannot write report: " << ex.what() << '\n';
    return 1;
  }
  if (!error.empty())
    std::cerr << error << '\n';
  return error.empty() ? 0 : 1;
}
