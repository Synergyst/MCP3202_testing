#include "Ht9032Driver.hpp"
#include "HeaderPins.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

namespace {
constexpr size_t MAX_BITS = 2400;
constexpr size_t MAX_BYTES = 512;
constexpr auto CARRIER_LOSS_HOLDOFF = std::chrono::milliseconds(75);
int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
bool boolish(const json& j, const char* k, bool d) {
    if (!j.contains(k)) return d;
    if (j[k].is_boolean()) return j[k].get<bool>();
    if (j[k].is_number_integer()) return j[k].get<int>() != 0;
    if (j[k].is_string()) {
        std::string s = j[k].get<std::string>(); std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s == "1" || s == "true" || s == "yes" || s == "on" || s == "powered";
    }
    return d;
}
std::string ascii(const std::vector<uint8_t>& b, size_t p, size_t len) {
    std::string out;
    for (size_t i = 0; i < len && p + i < b.size(); ++i) out.push_back((b[p+i] >= 32 && b[p+i] <= 126) ? char(b[p+i]) : '.');
    return out;
}
std::string fmtDate(const std::string& raw) { return raw.size() == 8 ? raw.substr(0,2)+"/"+raw.substr(2,2)+" "+raw.substr(4,2)+":"+raw.substr(6,2) : raw; }
bool csum(const std::vector<uint8_t>& b, size_t start, size_t total) { uint32_t s=0; for(size_t i=0;i<total;i++) s += b[start+i]; return (s & 0xffu) == 0; }
gpiod::line_request inputPullupRequest(const std::string& consumer) {
    gpiod::line_request req;
    req.consumer = consumer;
    req.request_type = gpiod::line_request::DIRECTION_INPUT;
    req.flags = gpiod::line_request::FLAG_BIAS_PULL_UP;
    return req;
}
}

Ht9032Driver::Ht9032Driver(std::shared_ptr<SystemContext> context) : context_(std::move(context)) { load(); }
Ht9032Driver::~Ht9032Driver() { stop(); }

void Ht9032Driver::validate(const Ht9032Settings& s) {
    if (!s.enabled) return;
    std::map<std::string,int> pins;
    if (s.pdwn_control) pins["PDWN"] = s.pdwn_phys;
    pins["CDET"] = s.cdet_phys;
    pins["DOUT"] = s.dout_phys;
    pins["DOUTC"] = s.doutc_phys;
    if (s.rdet_phys > 0) pins["RDET"] = s.rdet_phys;
    requireDistinctEnabledPins("HT9032C", pins);
    if (s.monitor_mode != "both" && s.monitor_mode != "dout" && s.monitor_mode != "doutc") {
        throw std::runtime_error("HT9032C illegal monitor_mode: use 'both', 'dout', or 'doutc'. " + gpioHelpText());
    }
    if (s.baud < 300 || s.baud > 2400) throw std::runtime_error("HT9032C illegal baud: use 300..2400, normally 1200 for Bell 202 Caller ID.");
}

Ht9032Settings Ht9032Driver::settingsFromJson(const json& j, const Ht9032Settings& defaults) {
    Ht9032Settings s = defaults;
    if (!j.is_object()) return s;
    s.enabled = boolish(j, "enabled", s.enabled);
    s.pdwn_phys = clampInt(j.value("pdwn_phys", s.pdwn_phys), 1, 40);
    s.cdet_phys = clampInt(j.value("cdet_phys", s.cdet_phys), 1, 40);
    s.dout_phys = clampInt(j.value("dout_phys", s.dout_phys), 1, 40);
    s.doutc_phys = clampInt(j.value("doutc_phys", s.doutc_phys), 1, 40);
    s.rdet_phys = clampInt(j.value("rdet_phys", s.rdet_phys), 0, 40);
    s.pdwn_control = boolish(j, "pdwn_control", s.pdwn_control);
    s.powered = boolish(j, "powered", s.powered);
    s.active_low_cdet = boolish(j, "active_low_cdet", s.active_low_cdet);
    s.active_low_rdet = boolish(j, "active_low_rdet", s.active_low_rdet);
    s.monitor_mode = j.value("monitor_mode", s.monitor_mode);
    s.baud = clampInt(j.value("baud", s.baud), 300, 2400);
    validate(s);
    return s;
}

json Ht9032Driver::settingsToJson(const Ht9032Settings& s) {
    auto bcm = [](int p){ return p > 0 ? bcmForPhysicalPin(p) : -1; };
    return {{"enabled",s.enabled},{"pdwn_phys",s.pdwn_phys},{"pdwn_bcm",s.pdwn_control?bcm(s.pdwn_phys):-1},{"cdet_phys",s.cdet_phys},{"cdet_bcm",bcm(s.cdet_phys)},
            {"dout_phys",s.dout_phys},{"dout_bcm",bcm(s.dout_phys)},
            {"doutc_phys",s.doutc_phys},{"doutc_bcm",bcm(s.doutc_phys)},
            {"rdet_phys",s.rdet_phys},{"rdet_bcm",s.rdet_phys>0?bcm(s.rdet_phys):-1},{"pdwn_control",s.pdwn_control},{"powered",s.powered},
            {"active_low_cdet",s.active_low_cdet},{"active_low_rdet",s.active_low_rdet},{"monitor_mode",s.monitor_mode},{"baud",s.baud}};
}

void Ht9032Driver::load() {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    std::ifstream f(context_->config_path);
    if (!f.is_open()) return;
    try { json j; f >> j; if (j.contains("ht9032")) { std::lock_guard<std::mutex> lock(mtx_); settings_ = settingsFromJson(j["ht9032"], settings_); } }
    catch (const std::exception& e) { std::cerr << "[HT9032] Config load warning: " << e.what() << std::endl; }
}

void Ht9032Driver::saveLocked() {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    json j; { std::ifstream f(context_->config_path); if (f.is_open()) { try { f >> j; } catch (...) { j=json::object(); } } }
    if (!j.is_object()) j = json::object();
    j["ht9032"] = settingsToJson(settings_);
    std::ofstream out(context_->config_path); if (out.is_open()) out << j.dump(2);
}

void Ht9032Driver::start() { if (running_) return; validate(settings_); running_ = true; worker_ = std::thread(&Ht9032Driver::worker, this); }
void Ht9032Driver::stop() { running_ = false; if (worker_.joinable()) worker_.join(); }
Ht9032Settings Ht9032Driver::settings() const { std::lock_guard<std::mutex> lock(mtx_); return settings_; }
json Ht9032Driver::settingsJson() const { return settingsToJson(settings()); }
std::string Ht9032Driver::validationHelp() const { return gpioHelpText(); }

std::set<int> Ht9032Driver::reservedBcms() const {
    auto s = settings(); std::set<int> out; if (!s.enabled) return out;
    if (s.pdwn_control) out.insert(bcmForPhysicalPin(s.pdwn_phys));
    out.insert(bcmForPhysicalPin(s.cdet_phys));
    out.insert(bcmForPhysicalPin(s.dout_phys));
    out.insert(bcmForPhysicalPin(s.doutc_phys));
    if (s.rdet_phys > 0) out.insert(bcmForPhysicalPin(s.rdet_phys));
    return out;
}

void Ht9032Driver::updateFromJson(const json& j) {
    std::lock_guard<std::mutex> lock(mtx_);
    Ht9032Settings next = settingsFromJson(j, settings_);
    bool pin_change = next.enabled != settings_.enabled || next.pdwn_phys != settings_.pdwn_phys || next.cdet_phys != settings_.cdet_phys || next.dout_phys != settings_.dout_phys || next.doutc_phys != settings_.doutc_phys || next.rdet_phys != settings_.rdet_phys || next.pdwn_control != settings_.pdwn_control;
    if (pin_change && running_) throw std::runtime_error("HT9032C pin mapping changes require a server restart so GPIO lines can be safely re-requested. Runtime changes allowed: monitor_mode, powered, active-low flags, baud. " + gpioHelpText());
    settings_ = next; status_ = "settings updated"; saveLocked();
}

std::string Ht9032Driver::bytesHex(const std::vector<uint8_t>& bytes) { std::ostringstream oss; oss<<std::hex<<std::uppercase<<std::setfill('0'); for(size_t i=0;i<bytes.size();++i){ if(i)oss<<' '; oss<<std::setw(2)<<int(bytes[i]); } return oss.str(); }

void Ht9032Driver::resetDecodeLocked(LineDecoder& d) { d.receiving=false; d.bit_index=0; d.byte=0; d.status="waiting"; d.confidence=0.0; }

void Ht9032Driver::feedSerialLocked(LineDecoder& d, bool level, std::chrono::steady_clock::time_point now, int baud) {
    if (!d.enabled) return;
    const auto bit_us = std::chrono::microseconds(static_cast<int>(1000000 / std::max(1, baud)));
    if (!d.receiving) {
        if (d.last_level && !level) { // falling edge start bit
            d.receiving = true; d.bit_index = -1; d.byte = 0; d.next_sample = now + bit_us + bit_us / 2; d.status = "receiving";
        }
    } else {
        int guard = 0;
        while (now >= d.next_sample && guard++ < 16) {
            if (d.bit_index == -1) {
                if (level != 0) { d.receiving = false; d.status = "false start"; break; }
            } else if (d.bit_index >= 0 && d.bit_index < 8) {
                if (level) d.byte |= uint8_t(1u << d.bit_index);
            } else if (d.bit_index == 8) {
                if (level) {
                    d.bytes.push_back(d.byte);
                    if (d.bytes.size() > MAX_BYTES) d.bytes.erase(d.bytes.begin(), d.bytes.begin() + (d.bytes.size() - MAX_BYTES));
                    d.status = "bytes seen";
                    d.confidence = std::min(0.80, 0.20 + static_cast<double>(d.bytes.size()) / 80.0);
                    parseCallerId(d);
                } else d.status = "bad stop bit";
                d.receiving = false; break;
            }
            d.bit_index++;
            d.next_sample += bit_us;
        }
    }
    d.last_level = level;
    d.bits.push_back(level ? '1' : '0');
    if (d.bits.size() > MAX_BITS) d.bits.erase(0, d.bits.size() - MAX_BITS);
}

void Ht9032Driver::parseCallerId(LineDecoder& d) {
    const auto& b = d.bytes;
    for (size_t i=0; i+2<b.size(); ++i) {
        uint8_t type=b[i]; if(type!=0x04 && type!=0x80) continue;
        size_t len=b[i+1], total=len+3; if(len>100 || i+total>b.size()) continue;
        d.decoded = true; d.checksum_ok = csum(b,i,total); d.message_type = type==0x80 ? "MDMF (0x80)" : "SDMF (0x04)";
        d.confidence = d.checksum_ok ? 1.0 : 0.70;
        if(type==0x04) { if(len>=8) d.date_time = fmtDate(ascii(b,i+2,8)); if(len>8) d.number = ascii(b,i+10,len-8); }
        else { size_t p=i+2, end=i+2+len; while(p+2<=end){ uint8_t pt=b[p++]; size_t pl=b[p++]; if(p+pl>end) break; std::string v=ascii(b,p,pl); if(pt==0x01)d.date_time=fmtDate(v); else if(pt==0x02||pt==0x04)d.number=v; else if(pt==0x07||pt==0x08)d.name=v; p+=pl; } }
        d.status = d.checksum_ok ? "decoded" : "decoded checksum mismatch"; return;
    }
}

json Ht9032Driver::snapshotJson() const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto dec = [](const LineDecoder& d){ return json{{"enabled",d.enabled},{"bits",d.bits},{"bytes_hex",Ht9032Driver::bytesHex(d.bytes)},{"byte_count",d.bytes.size()},{"status",d.status},{"confidence",d.confidence},{"decoded",d.decoded},{"checksum_ok",d.checksum_ok},{"message_type",d.message_type},{"date_time",d.date_time},{"number",d.number},{"name",d.name}}; };
    return {{"enabled",settings_.enabled},{"running",running_.load()},{"settings",settingsToJson(settings_)},{"pdwn_level",pdwn_level_},{"powered",settings_.powered},{"cdet_level",cdet_level_},{"carrier",carrier_},{"rdet_level",rdet_level_},{"ring_detect",ring_detect_},{"dout_level",dout_level_},{"doutc_level",doutc_level_},{"dout",dec(dout_)},{"doutc",dec(doutc_)},{"status",status_},{"last_error",last_error_},{"samples",samples_},{"help",gpioHelpText()}};
}

void Ht9032Driver::worker() {
    Ht9032Settings cfg; { std::lock_guard<std::mutex> lock(mtx_); cfg=settings_; status_=cfg.enabled?"running":"disabled"; dout_.enabled=(cfg.monitor_mode=="both"||cfg.monitor_mode=="dout"); doutc_.enabled=(cfg.monitor_mode=="both"||cfg.monitor_mode=="doutc"); last_carrier_seen_=std::chrono::steady_clock::now() - CARRIER_LOSS_HOLDOFF; }
    if (!cfg.enabled) return;
    gpiod::chip chip("0");
    std::unique_ptr<gpiod::line> pdwn, cdet, rdet, dout, doutc;
    bool rq_pdwn=false,rq_cdet=false,rq_rdet=false,rq_dout=false,rq_doutc=false;
    try {
        if(cfg.pdwn_control){ pdwn=std::make_unique<gpiod::line>(chip.get_line(bcmForPhysicalPin(cfg.pdwn_phys))); pdwn->request({"ht9032_pdwn",gpiod::line_request::DIRECTION_OUTPUT,0}, cfg.powered?0:1); rq_pdwn=true; }
        cdet=std::make_unique<gpiod::line>(chip.get_line(bcmForPhysicalPin(cfg.cdet_phys))); cdet->request(inputPullupRequest("ht9032_cdet")); rq_cdet=true;
        if(cfg.rdet_phys>0){ rdet=std::make_unique<gpiod::line>(chip.get_line(bcmForPhysicalPin(cfg.rdet_phys))); rdet->request(inputPullupRequest("ht9032_rdet")); rq_rdet=true; }
        dout=std::make_unique<gpiod::line>(chip.get_line(bcmForPhysicalPin(cfg.dout_phys))); dout->request({"ht9032_dout",gpiod::line_request::DIRECTION_INPUT,0}); rq_dout=true;
        doutc=std::make_unique<gpiod::line>(chip.get_line(bcmForPhysicalPin(cfg.doutc_phys))); doutc->request({"ht9032_doutc",gpiod::line_request::DIRECTION_INPUT,0}); rq_doutc=true;
        while(running_) {
            Ht9032Settings s; { std::lock_guard<std::mutex> lock(mtx_); s=settings_; }
            if(pdwn) pdwn->set_value(s.powered?0:1);
            bool cl=cdet->get_value(); bool car=s.active_low_cdet ? !cl : cl;
            bool rl=true, ring=false; if(rdet){ rl=rdet->get_value(); ring=s.active_low_rdet ? !rl : rl; }
            bool dl=true,dcl=true; if(dout) dl=dout->get_value(); if(doutc) dcl=doutc->get_value();
            auto now=std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (car) last_carrier_seen_ = now;
                const bool carrier_recent = car || ((now - last_carrier_seen_) <= CARRIER_LOSS_HOLDOFF);
                pdwn_level_=s.pdwn_control ? !s.powered : false; cdet_level_=cl; carrier_=car; rdet_level_=rl; ring_detect_=ring; dout_level_=dl; doutc_level_=dcl; dout_.enabled=(s.monitor_mode=="both"||s.monitor_mode=="dout"); doutc_.enabled=(s.monitor_mode=="both"||s.monitor_mode=="doutc"); samples_++;
                if(carrier_recent){
                    feedSerialLocked(dout_,dl,now,s.baud);
                    feedSerialLocked(doutc_,dcl,now,s.baud);
                    status_=car ? "carrier present" : "carrier holdoff";
                } else {
                    resetDecodeLocked(dout_); resetDecodeLocked(doutc_); status_="waiting for carrier";
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    } catch(const std::exception& e) { std::lock_guard<std::mutex> lock(mtx_); last_error_=e.what(); status_="error"; }
    if(rq_doutc) try{doutc->release();}catch(...){} if(rq_dout) try{dout->release();}catch(...){} if(rq_rdet) try{rdet->release();}catch(...){} if(rq_cdet) try{cdet->release();}catch(...){} if(rq_pdwn) try{pdwn->release();}catch(...){}
}
