#include "core/rt/Logging.h"

#include <algorithm>
#include <cstdio>
#include <regex>
#include <system_error>

namespace bf::rt {

namespace {

thread_local const char* tlThreadName = nullptr;

// Days since 1970-01-01 -> civil date (H. Hinnant's algorithm).
void civilFromDays(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2 ? 1 : 0));
}

struct UtcParts {
    int year = 1970;
    unsigned month = 1, day = 1, hour = 0, minute = 0, second = 0, milli = 0;
};

UtcParts utcNow() {
    const auto now = std::chrono::system_clock::now();
    const std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::int64_t days = ms / 86400000;
    std::int64_t rem = ms % 86400000;
    if (rem < 0) {
        rem += 86400000;
        --days;
    }
    UtcParts p;
    civilFromDays(days, p.year, p.month, p.day);
    p.hour = static_cast<unsigned>(rem / 3600000);
    p.minute = static_cast<unsigned>((rem / 60000) % 60);
    p.second = static_cast<unsigned>((rem / 1000) % 60);
    p.milli = static_cast<unsigned>(rem % 1000);
    return p;
}

std::string dayString(const UtcParts& p) {
    char b[16];
    std::snprintf(b, sizeof b, "%04d%02u%02u", p.year, p.month, p.day);
    return b;
}

std::uint32_t fnv1a32(std::string_view s) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

LogLevel rtLevel(RtLogCode c) {
    return c == RtLogCode::TapOverflow ? LogLevel::Info : LogLevel::Warn;
}

const char* rtCode(RtLogCode c) {
    switch (c) {
    case RtLogCode::Xrun: return logcode::kAudioXrun;
    case RtLogCode::Overrun: return logcode::kAudioOverrun;
    case RtLogCode::CallbackGap: return logcode::kAudioCallbackGap;
    case RtLogCode::LateStart: return logcode::kPreloadLateStart;
    case RtLogCode::Starvation: return logcode::kPreloadStarvation;
    case RtLogCode::Underflow: return logcode::kPreloadUnderflow;
    case RtLogCode::TapOverflow: return logcode::kAnalysisTapOverflow;
    }
    return "rt.unknown";
}

const char* rtMessage(RtLogCode c) {
    switch (c) {
    case RtLogCode::Xrun: return "driver-reported buffer underrun";
    case RtLogCode::Overrun: return "callback took more than 90% of the buffer duration";
    case RtLogCode::CallbackGap: return "callback start gap above 1.5 buffer durations";
    case RtLogCode::LateStart: return "talker event postponed: source not preloaded";
    case RtLogCode::Starvation: return "talker event dropped: source not preloaded after 1 s";
    case RtLogCode::Underflow: return "active talker underflow: fast fade-out";
    case RtLogCode::TapOverflow: return "analysis tap ring overflow";
    }
    return "";
}

}  // namespace

std::string_view toString(LogLevel l) noexcept {
    switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    }
    return "INFO";
}

void setCurrentThreadName(const char* name) noexcept { tlThreadName = name; }
const char* currentThreadName() noexcept { return tlThreadName ? tlThreadName : "unnamed"; }

std::int64_t monotonicMicros() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string isoUtcNow() {
    const UtcParts p = utcNow();
    char b[40];
    std::snprintf(b, sizeof b, "%04d-%02u-%02uT%02u:%02u:%02u.%03uZ", p.year, p.month, p.day, p.hour, p.minute,
                  p.second, p.milli);
    return b;
}

std::string Logger::redactPaths(std::string_view s) {
    static const std::regex re(R"((?:[A-Za-z]:[\\/]|/)(?:[^\s"'<>|*?\\/]+[\\/])+[^\s"'<>|*?\\/]+)");
    std::string in(s), out;
    auto it = std::sregex_iterator(in.begin(), in.end(), re);
    std::size_t last = 0;
    for (; it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        out.append(in, last, static_cast<std::size_t>(m.position()) - last);
        const std::string path = m.str();
        const std::size_t slash = path.find_last_of("/\\");
        const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
        const std::size_t dot = leaf.find_last_of('.');
        char h[16];
        std::snprintf(h, sizeof h, "%08x", static_cast<unsigned>(fnv1a32(path)));
        out += "<path>/\xE2\x80\xA6/";
        out += h;
        if (dot != std::string::npos && dot > 0) out += leaf.substr(dot);
        last = static_cast<std::size_t>(m.position() + m.length());
    }
    out.append(in, last, std::string::npos);
    return out;
}

void Logger::redactJson(nlohmann::json& j) {
    if (j.is_string()) {
        j = redactPaths(j.get_ref<const std::string&>());
    } else if (j.is_array() || j.is_object()) {
        for (auto& v : j) redactJson(v);
    }
}

Logger::Logger(LoggerConfig cfg) : cfg_(std::move(cfg)), minLevel_(cfg_.minLevel) {
    rtFifo_ = std::make_unique<SpscFifo<RtLogEvent, 1024>>();
}

Logger::~Logger() { stop(); }

void Logger::start() {
    if (running_.exchange(true)) return;
    quit_.store(false);
    thread_ = std::thread([this] { run(); });
}

void Logger::stop() {
    if (!running_.load()) {
        drain();  // synchronous logger (never started): write what is queued
        if (file_.is_open()) file_.close();
        return;
    }
    {
        std::lock_guard<CheckedMutex> lk(qMutex_);
        quit_.store(true);
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    if (file_.is_open()) file_.close();
}

void Logger::setEngineSampleSource(std::function<std::int64_t()> src) {
    std::lock_guard<CheckedMutex> lk(srcMutex_);
    sampleSrc_ = std::move(src);
}

void Logger::log(LogLevel level, std::string code, std::string msg, nlohmann::json data, std::int64_t engineSample) {
    if (level < minLevel_.load(std::memory_order_relaxed)) return;
    Entry e;
    e.level = level;
    e.code = std::move(code);
    e.msg = std::move(msg);
    e.data = data.is_null() ? nlohmann::json::object() : std::move(data);
    e.thread = currentThreadName();
    e.ts = isoUtcNow();
    e.mono = monotonicMicros();
    if (engineSample < 0) {
        std::lock_guard<CheckedMutex> lk(srcMutex_);
        if (sampleSrc_) engineSample = sampleSrc_();
    }
    e.engineSample = engineSample;
    {
        std::lock_guard<CheckedMutex> lk(qMutex_);
        if (queue_.size() >= cfg_.queueCapacity) {
            queue_.pop_front();
            ++processed_;
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        queue_.push_back(std::move(e));
        ++enqueued_;
    }
    if (!running_.load(std::memory_order_acquire)) drain();  // not started: write synchronously
}

bool Logger::logRt(RtLogCode code, std::int64_t engineSample, double a0, double a1, double a2, double a3) noexcept {
    RtLogEvent ev;
    ev.code = code;
    ev.engineSample = engineSample;
    ev.monoUs = monotonicMicros();
    ev.args[0] = a0;
    ev.args[1] = a1;
    ev.args[2] = a2;
    ev.args[3] = a3;
    if (rtFifo_->push(ev)) return true;
    rtDropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Logger::flush() {
    if (!running_.load(std::memory_order_acquire)) {
        drain();
        return;
    }
    std::unique_lock<CheckedMutex> lk(qMutex_);
    const std::uint64_t target = enqueued_;
    flushReq_.store(true);
    cv_.notify_all();
    doneCv_.wait_for(lk, std::chrono::seconds(5), [&] { return processed_ >= target && rtFifo_->empty(); });
}

double Logger::queueFill() const {
    std::lock_guard<CheckedMutex> lk(qMutex_);
    return static_cast<double>(queue_.size()) / static_cast<double>(std::max<std::size_t>(cfg_.queueCapacity, 1));
}

std::uint64_t Logger::countOf(const std::string& code) const {
    std::lock_guard<CheckedMutex> lk(recentMutex_);
    const auto it = perCode_.find(code);
    return it == perCode_.end() ? 0 : it->second;
}

std::vector<nlohmann::json> Logger::recent(LogLevel minLevel) const {
    std::lock_guard<CheckedMutex> lk(recentMutex_);
    std::vector<nlohmann::json> r;
    for (const auto& j : recent_) {
        const std::string lv = j.value("level", "INFO");
        LogLevel l = LogLevel::Info;
        for (LogLevel c : {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal})
            if (toString(c) == lv) l = c;
        if (l >= minLevel) r.push_back(j);
    }
    return r;
}

std::vector<std::filesystem::path> Logger::files() const {
    std::vector<std::filesystem::path> r;
    std::error_code ec;
    if (cfg_.dir.empty() || !std::filesystem::is_directory(cfg_.dir, ec)) return r;
    for (const auto& de : std::filesystem::directory_iterator(cfg_.dir, ec)) {
        const std::string n = de.path().filename().string();
        if (n.rfind(cfg_.filePrefix + "-", 0) == 0 && de.path().extension() == ".jsonl") r.push_back(de.path());
    }
    std::sort(r.begin(), r.end());
    return r;
}

void Logger::run() {
    setCurrentThreadName("bf.logger");
    while (true) {
        {
            std::unique_lock<CheckedMutex> lk(qMutex_);
            cv_.wait_for(lk, std::chrono::milliseconds(cfg_.pollMs),
                         [&] { return quit_.load() || flushReq_.load(); });
            flushReq_.store(false);
        }
        drain();
        if (quit_.load()) {
            drain();
            break;
        }
    }
}

Logger::Entry Logger::fromRt(const RtLogEvent& ev) const {
    Entry e;
    e.level = rtLevel(ev.code);
    e.code = rtCode(ev.code);
    e.msg = rtMessage(ev.code);
    e.thread = "bf.audio";
    e.ts = isoUtcNow();
    e.mono = ev.monoUs;
    e.engineSample = ev.engineSample;
    e.data = {{"args", {ev.args[0], ev.args[1], ev.args[2], ev.args[3]}}};
    return e;
}

bool Logger::admit(const Entry& e, std::int64_t mono) {
    if (!cfg_.rateLimitedCodes.count(e.code)) return true;
    RateState& r = rate_[e.code];
    const auto window = static_cast<std::int64_t>(cfg_.rateLimitWindowS * 1e6);
    if (r.count == 0 || mono - r.windowStart >= window) {
        if (r.suppressed > 0) flushSuppressed(mono, true);
        r.windowStart = mono;
        r.count = 0;
    }
    if (r.count < cfg_.rateLimitBurst) {
        ++r.count;
        return true;
    }
    ++r.suppressed;
    suppressed_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Logger::flushSuppressed(std::int64_t mono, bool force) {
    const auto window = static_cast<std::int64_t>(cfg_.rateLimitWindowS * 1e6);
    for (auto& [code, r] : rate_) {
        if (r.suppressed == 0) continue;
        if (!force && mono - r.windowStart < window) continue;
        Entry s;
        s.level = LogLevel::Warn;
        s.code = code;
        s.msg = std::to_string(r.suppressed) + " similar entries suppressed";
        s.data = {{"suppressed", r.suppressed}, {"summary", true}};
        s.thread = "bf.logger";
        s.ts = isoUtcNow();
        s.mono = mono;
        r.suppressed = 0;
        r.windowStart = mono;
        r.count = 0;
        write(s);
    }
}

void Logger::drain() {
    std::lock_guard<CheckedMutex> dl(drainMutex_);
    std::deque<Entry> batch;
    std::uint64_t n = 0;
    {
        std::lock_guard<CheckedMutex> lk(qMutex_);
        batch.swap(queue_);
        n = batch.size();
    }
    // RT events go first by mono time order with the batch (both are roughly time ordered).
    std::vector<Entry> rtEntries;
    RtLogEvent ev;
    while (rtFifo_->pop(ev)) rtEntries.push_back(fromRt(ev));
    const std::int64_t mono = monotonicMicros();
    for (auto& e : rtEntries)
        if (admit(e, e.mono)) write(e);
    for (auto& e : batch)
        if (admit(e, e.mono)) write(e);
    flushSuppressed(mono, false);
    if (file_.is_open()) file_.flush();
    lastDrain_.store(mono, std::memory_order_relaxed);
    {
        std::lock_guard<CheckedMutex> lk(qMutex_);
        processed_ += n;
    }
    doneCv_.notify_all();
}

void Logger::openFile() {
    if (cfg_.dir.empty() || diskError_.load()) return;
    std::error_code ec;
    std::filesystem::create_directories(cfg_.dir, ec);
    fileDay_ = dayString(utcNow());
    filePath_ = cfg_.dir / (cfg_.filePrefix + "-" + fileDay_ + ".jsonl");
    file_.open(filePath_, std::ios::binary | std::ios::app);
    if (!file_) {
        diskError_.store(true);
        return;
    }
    fileBytes_ = std::filesystem::exists(filePath_, ec) ? static_cast<std::uint64_t>(std::filesystem::file_size(filePath_, ec)) : 0;
}

void Logger::rotate() {
    file_.close();
    const UtcParts p = utcNow();
    char b[64];
    std::snprintf(b, sizeof b, "-%04d%02u%02u-%02u%02u%02u-%06llu.jsonl", p.year, p.month, p.day, p.hour, p.minute,
                  p.second, static_cast<unsigned long long>(++rotSeq_));
    std::error_code ec;
    std::filesystem::rename(filePath_, cfg_.dir / (cfg_.filePrefix + b), ec);
    if (ec) {
        diskError_.store(true);
        return;
    }
    rotations_.fetch_add(1, std::memory_order_relaxed);
    prune();
    openFile();
}

void Logger::prune() {
    // Rotated files sort chronologically by name; the active file is never deleted.
    std::vector<std::filesystem::path> rotated;
    for (const auto& f : files())
        if (f.filename() != filePath_.filename()) rotated.push_back(f);
    std::sort(rotated.begin(), rotated.end(), [](const auto& a, const auto& b) {
        return a.filename().string() < b.filename().string();
    });
    const std::size_t keep = static_cast<std::size_t>(std::max(cfg_.maxFiles - 1, 0));
    std::error_code ec;
    for (std::size_t i = 0; i + keep < rotated.size(); ++i) std::filesystem::remove(rotated[i], ec);
}

void Logger::write(Entry& e) {
    if (cfg_.redactPathsInLogs) {
        e.msg = redactPaths(e.msg);
        redactJson(e.data);
    }
    nlohmann::json j;
    j["ts"] = e.ts;
    j["mono"] = e.mono;
    j["engineSample"] = e.engineSample;
    j["level"] = std::string(toString(e.level));
    j["code"] = e.code;
    j["thread"] = e.thread;
    j["msg"] = e.msg;
    j["data"] = e.data;
    {
        std::lock_guard<CheckedMutex> lk(recentMutex_);
        ++perCode_[e.code];
        if (e.level >= LogLevel::Warn) {
            recent_.push_back(j);
            while (recent_.size() > cfg_.recentCapacity) recent_.pop_front();
        }
    }
    if (cfg_.dir.empty() || diskError_.load(std::memory_order_relaxed)) return;
    const std::string line = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    if (!file_.is_open() || dayString(utcNow()) != fileDay_) {
        if (file_.is_open()) file_.close();
        openFile();
        if (!file_.is_open()) return;
    }
    if (fileBytes_ > 0 && fileBytes_ + line.size() > cfg_.maxFileBytes) {
        rotate();
        if (!file_.is_open()) return;
    }
    file_.write(line.data(), static_cast<std::streamsize>(line.size()));
    if (!file_) {
        diskError_.store(true);  // disk full / write error: stop writing, keep the ring
        file_.close();
        return;
    }
    fileBytes_ += line.size();
    written_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace bf::rt
