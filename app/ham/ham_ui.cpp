// The Ham view (third choice of the RX / TX / Ham switch): FT8 / FT4 / FT2 operation like WSJT-X. Station settings, the decoded list with
// double click to call, CQ, TX slot, TX frequency + Hold, Halt TX, the auto-sequence engine, the watchdog, the ADIF log. The logic is in
// core/include/dect2/ham/ham.h (tested); this file draws it and feeds it the decodes. Transmission goes only through hamTransmit(), which
// is behind the gate hamTxAllowed (licence confirmed, explicit Arm, inside an amateur band): see docs/HAM_PLAN.md for what phase 2 wires.
#include "../app.h"
#include "ham_kbd_ui.h"
#include "../platform/platform.h"
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_upload.h"
#include "dect2/audio/audioout.h"
#include "dect2/source/hamlib_link.h"
#include "dect2/source/hamlib_rig.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

using namespace dect2;

namespace {
// The real rig: Hamlib PTT (when a rig is chosen; none = VOX / the radio keys itself from audio) and the chosen sound card output.
struct RealIo : ham::HamRigIo {
    HamlibLink link;
    int rigIdx = -1;            // index into hamlibRigs(), -1: no CAT PTT
    std::string device;         // audio output ("" = system default)
    AudioOut out;
    bool drained = false;
    bool ptt(bool on, std::string& err) override {
        if (rigIdx < 0) return true;
        if (!link.isOpen()) {
            const auto rigs = hamlibRigs();
            if (rigIdx >= (int)rigs.size()) { err = "the chosen rig is not in the rig list"; return false; }
            HamlibConn c; c.model = rigs[rigIdx].model; c.path = rigs[rigIdx].path; c.baud = rigs[rigIdx].baud;
            if (!link.open(c, err)) return false;
        }
        return link.setPtt(on, err);
    }
    bool beginAudio(int rate, std::string& err) override {
        out.setDevice(device);
        drained = false;
        if (!out.start(rate)) { err = "cannot open the sound card output"; return false; }
        out.setStartThreshold(rate / 20);
        return true;
    }
    int write(const float* m, int n) override {
        std::vector<float> st((size_t)n * 2);
        for (int i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = m[i];
        return out.write(st.data(), n);
    }
    int pending() override {
        const int b = out.bufferedFrames();
        if (b == 0 && !drained) { drained = true; std::this_thread::sleep_for(std::chrono::milliseconds(200)); }   // the device's own buffer
        return b;
    }
    void flushAudio() override { out.flush(); }
    void endAudio() override { out.stop(); }
};

struct HamUi {
    bool loaded = false, view = false, armed = false, licence = false, askLicence = false;
    char call[16] = {}, grid[8] = {};
    int mode = 0;                       // hfdig mode number: 0 FT8, 1 FT4, 2 FT2, 3 WSPR (beacon), 4 JT65, 5 JT9, 10 JT4
    int sub = 0;                        // JT65 A B C, JT4 A .. G
    int kbdIdx = -1;                    // a keyboard mode (ham::kbdPresets index) instead of the slot modes
    ham::WsprBeacon beacon;
    bool txBeacon = false;              // the running burst is a beacon one
    float txHz = 1500;
    ham::QsoEngine eng;
    ham::WorkedLog worked;
    std::string logPath, status, lastTx, lastLog;
    double lastFed = 0;
    long lastTxSlot = -1;
    int logged = 0;
    RealIo io;
    ham::HamSlotTx tx{io};
    std::thread worker;
    std::atomic<int> txState{0};      // 0 none, 1 running, 2 sent, 3 failed
    std::mutex mu;
    std::string txErr, txMsg;
    double txSlot = 0;
    int rigSel = -1;
    float level = 0.4f;
    ham::HamUploader up;                // logbook uploads: off by default, see uploadSection()
    ham::UpConfig upCfg;
    char qrzKey[64] = {}, clubEmail[64] = {}, clubPw[64] = {}, clubApi[64] = {}, eqslUser[32] = {}, eqslPw[64] = {};
    bool upFieldsInit = false;
    ~HamUi() { tx.abort(); if (worker.joinable()) worker.join(); }
};
HamUi& H() { static HamUi h; return h; }

void load(HamUi& h) {
    if (h.loaded) return;
    h.loaded = true;
    plat::Prefs& p = plat::basePrefs();
    snprintf(h.call, sizeof h.call, "%s", p.getS("ham.call", "").c_str());
    snprintf(h.grid, sizeof h.grid, "%s", p.getS("ham.grid", "").c_str());
    h.licence = p.getB("ham.licence", false);
    h.rigSel = (int)p.getI("ham.rig", -1);
    h.io.device = p.getS("ham.audioOut", "");
    h.txHz = (float)p.getD("ham.txHz", 1500);
    h.logPath = plat::dataDir() + "/onair_ham.adi";
    h.worked.load(h.logPath);
    h.eng.onLog = [&h](const ham::QsoRecord& q) {
        ham::QsoRecord r = q;
        std::string err;
        if (ham::adifAppend(h.logPath, r, err)) { h.worked.add(r); h.logged++; h.lastLog = r.call; } else h.status = err;
        h.up.enqueue(r, (long long)time(nullptr));   // queued only; nothing leaves unless a service is on and auto upload is ticked
    };
    h.upCfg.autoUpload = false;
    h.upCfg.on[0] = p.getB("ham.up.qrz", false); h.upCfg.on[1] = p.getB("ham.up.club", false); h.upCfg.on[2] = p.getB("ham.up.eqsl", false);
    h.upCfg.qrzKey = p.getS("ham.up.qrzKey", ""); h.upCfg.clubEmail = p.getS("ham.up.clubEmail", ""); h.upCfg.clubPassword = p.getS("ham.up.clubPw", "");
    h.upCfg.clubApiKey = p.getS("ham.up.clubApi", ""); h.upCfg.eqslUser = p.getS("ham.up.eqslUser", ""); h.upCfg.eqslPassword = p.getS("ham.up.eqslPw", "");
    h.up.load(plat::dataDir() + "/onair_ham_upload.tsv");
    h.up.setConfig(h.upCfg);
    h.up.startWorker();
    h.eng.onEvent = [&h](const std::string& e) { h.status = e == "watchdog" ? "Watchdog: no operator action for too long, TX halted" : "No answer: gave up the call"; h.armed = false; };
}
void save(HamUi& h) {
    plat::Prefs& p = plat::basePrefs();
    p.setS("ham.call", h.call); p.setS("ham.grid", h.grid); p.setB("ham.licence", h.licence); p.setD("ham.txHz", h.txHz);
    p.flush();
}
void uploadSection(HamUi& h) {
    if (!ImGui::CollapsingHeader(tr("Logbook uploads (off by default)"))) return;
    plat::Prefs& p = plat::basePrefs();
    auto fld = [](char* b, size_t n, const std::string& v) { snprintf(b, n, "%s", v.c_str()); };
    if (!h.upFieldsInit) {
        h.upFieldsInit = true;
        fld(h.qrzKey, sizeof h.qrzKey, h.upCfg.qrzKey); fld(h.clubEmail, sizeof h.clubEmail, h.upCfg.clubEmail); fld(h.clubPw, sizeof h.clubPw, h.upCfg.clubPassword);
        fld(h.clubApi, sizeof h.clubApi, h.upCfg.clubApiKey); fld(h.eqslUser, sizeof h.eqslUser, h.upCfg.eqslUser); fld(h.eqslPw, sizeof h.eqslPw, h.upCfg.eqslPassword);
    }
    ImGui::TextWrapped("%s", tr("Privacy: when a service is switched on, each logged QSO is sent to it over HTTPS: the other station's callsign and grid, mode, band and frequency, the reports, the UTC time, and your own callsign and grid. Nothing else, and nothing at all while a service is off. Your keys and passwords stay on this computer and go only to that service. LoTW is not supported (it needs TQSL certificates): export the ADIF log and sign it with TQSL."));
    bool ch = false;
    h.upCfg.autoUpload = false;
    ImGui::BeginDisabled(true);
    ImGui::Checkbox(tr("Upload automatically after logging"), &h.upCfg.autoUpload);
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", tr("Auto upload is disabled in this fork."));
    ImGui::Separator();
    ch |= ImGui::Checkbox("QRZ.com Logbook", &h.upCfg.on[0]);
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::InputText(tr("QRZ API key"), h.qrzKey, sizeof h.qrzKey, ImGuiInputTextFlags_Password)) { h.upCfg.qrzKey = h.qrzKey; ch = true; }
    ch |= ImGui::Checkbox("Club Log", &h.upCfg.on[1]);
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::InputText(tr("Club Log email"), h.clubEmail, sizeof h.clubEmail)) { h.upCfg.clubEmail = h.clubEmail; ch = true; }
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::InputText(tr("Club Log app password"), h.clubPw, sizeof h.clubPw, ImGuiInputTextFlags_Password)) { h.upCfg.clubPassword = h.clubPw; ch = true; }
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::InputText(tr("Club Log API key"), h.clubApi, sizeof h.clubApi, ImGuiInputTextFlags_Password)) { h.upCfg.clubApiKey = h.clubApi; ch = true; }
    ch |= ImGui::Checkbox("eQSL", &h.upCfg.on[2]);
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText(tr("eQSL user"), h.eqslUser, sizeof h.eqslUser)) { h.upCfg.eqslUser = h.eqslUser; ch = true; }
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText(tr("eQSL password"), h.eqslPw, sizeof h.eqslPw, ImGuiInputTextFlags_Password)) { h.upCfg.eqslPassword = h.eqslPw; ch = true; }
    if (ch) {
        p.setB("ham.up.auto", false); p.setB("ham.up.qrz", h.upCfg.on[0]); p.setB("ham.up.club", h.upCfg.on[1]); p.setB("ham.up.eqsl", h.upCfg.on[2]);
        p.setS("ham.up.qrzKey", h.upCfg.qrzKey); p.setS("ham.up.clubEmail", h.upCfg.clubEmail); p.setS("ham.up.clubPw", h.upCfg.clubPassword);
        p.setS("ham.up.clubApi", h.upCfg.clubApiKey); p.setS("ham.up.eqslUser", h.upCfg.eqslUser); p.setS("ham.up.eqslPw", h.upCfg.eqslPassword);
        p.flush();
        h.up.setConfig(h.upCfg);
    }
    if (ImGui::Button(tr("Upload now"))) h.up.uploadNow((long long)time(nullptr));
    ImGui::SameLine(); ImGui::TextDisabled("%s: %d", tr("Waiting"), (int)h.up.pendingCount());
    // result per QSO (newest first, last 12): U uploaded, F failed with the reason, . waiting, - skipped (service was off)
    const auto its = h.up.items();
    int shown = 0;
    for (int i = (int)its.size() - 1; i >= 0 && shown < 12; i--, shown++) {
        std::string line = its[i].q.call;
        for (int k = 0; k < ham::kUpServices; k++) {
            const ham::UpState& s = its[i].s[k];
            line += std::string("  ") + ham::upServiceName((ham::UpService)k) + ": " + (s.st == 'u' ? "uploaded" : s.st == 'f' ? "failed: " + s.reason : s.st == 'n' ? "not sent" : s.reason.empty() ? "waiting" : "retrying: " + s.reason);
        }
        ImGui::TextUnformatted(line.c_str());
    }
}
}  // namespace

bool& hamViewFlag() { static bool init = (H().view = getenv("DECT2_HAMVIEW") != nullptr, true); (void)init; return H().view; }

void hamPanel(App& a) {
    HamUi& h = H();
    load(h);
    const double now = (double)time(nullptr);
    // the Ham modes: hfdig mode number, name, mode-list subId (the receiver must be on that mode for the decodes to exist)
    struct HamMode { int num; const char* name; const char* subId; };
    static const HamMode kHam[] = {{0, "FT8", "ft8"}, {1, "FT4", "ft4"}, {2, "FT2", "ft2"}, {4, "JT65", "jt65"}, {5, "JT9", "jt9"}, {10, "JT4", "jt4"}, {3, "WSPR beacon", "wspr"}};
    ham::QsoConfig& c = h.eng.config();
    c.myCall = h.call; c.myGrid = h.grid; c.mode = h.mode;
    h.beacon.cfg.myCall = h.call; h.beacon.cfg.myGrid = h.grid;
    const bool isBeacon = h.mode == 3;

    for (const HamMode& hm : kHam) {
        if (&hm != kHam) ImGui::SameLine();
        if (ImGui::RadioButton(hm.name, h.kbdIdx < 0 && h.mode == hm.num) && (h.mode != hm.num || h.kbdIdx >= 0)) {
            h.kbdIdx = -1;
            h.mode = hm.num; h.sub = 0; h.eng.halt(); h.beacon.stop(); h.lastFed = 0;
            for (const auto& m : allModes()) if (m.family == 25 && m.subId && !strcmp(m.subId, hm.subId)) { selectMode(a, m.family, m.subId); break; }
            a.engine.hfdig().ftx().setModeOption(h.mode, 0, 0);
        }
    }
    {   // keyboard modes
        const auto& kp = ham::kbdPresets();
        const std::string cur = h.kbdIdx >= 0 ? kp[(size_t)h.kbdIdx].name : "Keyboard modes";
        ImGui::SameLine(); ImGui::SetNextItemWidth(150 * gUi);
        if (ImGui::BeginCombo("##kbdmode", cur.c_str())) {
            for (int i = 0; i < (int)kp.size(); i++)
                if (ImGui::Selectable(kp[(size_t)i].name, h.kbdIdx == i)) { h.kbdIdx = i; h.eng.halt(); h.beacon.stop(); hamKbdSelect(a, i); }
            ImGui::EndCombo();
        }
    }
    if (h.kbdIdx < 0 && (h.mode == 4 || h.mode == 10)) {   // sub-mode: JT65 A B C, JT4 A .. G
        int sb = h.sub;
        ImGui::SetNextItemWidth(70 * gUi);
        if (ImGui::Combo(tr("Sub-mode"), &sb, h.mode == 4 ? "A\0B\0C\0" : "A\0B\0C\0D\0E\0F\0G\0") && sb != h.sub) {
            h.sub = sb; h.eng.halt();
            a.engine.hfdig().ftx().setModeOption(h.mode, sb, 0);
        }
    }
    ImGui::SameLine(); ImGui::TextDisabled("%.4f MHz", a.freqMhz);

    ImGui::SetNextItemWidth(110 * gUi);
    bool ch = ImGui::InputText(tr("Callsign"), h.call, sizeof h.call, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
    ch |= ImGui::InputText(tr("Grid"), h.grid, sizeof h.grid, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);
    if (ch) save(h);
    const bool stationOk = ham::hamCallValid(h.call) && ham::hamGridValid(h.grid);
    if (!stationOk) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", tr("Enter your callsign and a 4 or 6 character grid locator."));

    // licence confirmation, stored; asked before the first TX
    if (ImGui::Checkbox(tr("I hold an amateur licence for this callsign and will follow its rules"), &h.licence)) { save(h); if (!h.licence) h.armed = false; }

    auto rigWidgets = [&h] {   // rig and audio output, level (shared with the keyboard modes)
        const auto rigs = hamlibRigs();
        const std::string curS = h.rigSel >= 0 && h.rigSel < (int)rigs.size() ? hamlibRigDeviceName(rigs[h.rigSel]) : "none (VOX / audio only)";
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo(tr("PTT rig"), curS.c_str())) {
            if (ImGui::Selectable("none (VOX / audio only)", h.rigSel < 0)) { h.rigSel = -1; plat::basePrefs().setI("ham.rig", -1); }
            for (int i = 0; i < (int)rigs.size(); i++) if (ImGui::Selectable(hamlibRigDeviceName(rigs[i]).c_str(), h.rigSel == i)) { h.rigSel = i; plat::basePrefs().setI("ham.rig", i); h.io.link.close(); }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(220 * gUi);
        const std::string dn = h.io.device.empty() ? "system default" : h.io.device;
        if (ImGui::BeginCombo(tr("Audio out"), dn.c_str())) {
            if (ImGui::Selectable("system default", h.io.device.empty())) { h.io.device.clear(); plat::basePrefs().setS("ham.audioOut", ""); }
            for (const auto& d : AudioOut::devices()) if (!d.empty() && ImGui::Selectable(d.c_str(), d == h.io.device)) { h.io.device = d; plat::basePrefs().setS("ham.audioOut", d); }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
        ImGui::SliderFloat(tr("Level"), &h.level, 0.05f, 0.9f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Audio level into the rig. Set it so the rig shows no ALC action and the power stays low."));
    };

    if (h.kbdIdx >= 0) {   // keyboard modes: their own panel, the same station, licence, Arm and transmitter
        HamKbdCtx kc{a, h.call, h.grid, stationOk, h.licence, h.armed, h.level, h.tx, h.worker, h.txState, h.mu, h.txErr, h.status, h.worked, rigWidgets,
                     [&h](const ham::QsoRecord& q) {
                         std::string err;
                         if (ham::adifAppend(h.logPath, q, err)) { h.worked.add(q); h.logged++; h.lastLog = q.call; } else h.status = err;
                     }};
        h.io.rigIdx = h.rigSel;
        hamKbdPanel(kc, h.kbdIdx);
        return;
    }

    // TX controls
    ImGui::Separator();
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::SliderFloat(tr("TX Hz"), &h.txHz, 200, 3000, "%.0f")) { c.txHz = h.txHz; h.eng.userAction(now); save(h); }
    ImGui::SameLine(); ImGui::Checkbox(tr("Hold"), &c.hold);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hold TX frequency: calling a station does not move your TX frequency to theirs."));
    ImGui::SameLine(); ImGui::Checkbox(tr("Auto-seq"), &c.autoSeq);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Answers the replies to your CQ or call by itself: report, R-report, RR73, 73."));
    int par = c.txEven ? 0 : 1;
    ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::Combo(tr("TX slot"), &par, "Even\0Odd\0")) { c.txEven = par == 0; h.eng.userAction(now); }
    if (!h.eng.active()) c.txHz = h.txHz; else h.txHz = (float)c.txHz;

    const bool canCq = stationOk && h.licence;
    if (isBeacon) {
        // WSPR beacon: transmit only. Power, how often (never more than every second slot), then Start; the gate and Arm apply as always.
        static const int kDbm[] = {0, 3, 7, 10, 13, 17, 20, 23, 27, 30, 33, 37, 40};
        int di = 3;
        for (int i = 0; i < 13; i++) if (kDbm[i] == h.beacon.cfg.dbm) di = i;
        char dl[16]; snprintf(dl, sizeof dl, "%d dBm", kDbm[di]);
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::BeginCombo(tr("Power"), dl)) {
            for (int i = 0; i < 13; i++) { snprintf(dl, sizeof dl, "%d dBm", kDbm[i]); if (ImGui::Selectable(dl, i == di)) h.beacon.cfg.dbm = kDbm[i]; }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
        int nth = std::max(2, h.beacon.cfg.everyNth);
        if (ImGui::SliderInt(tr("1 slot in"), &nth, 2, 20)) h.beacon.cfg.everyNth = nth;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Duty cycle: one 2 minute slot in N. Never more than every second slot."));
        h.beacon.cfg.audioHz = 1500;
        ImGui::BeginDisabled(!canCq);
        if (ImGui::Button(h.beacon.active() ? tr("Stop beacon") : tr("Start beacon"))) { if (h.beacon.active()) h.beacon.stop(); else { h.beacon.start(); h.status.clear(); } h.beacon.userAction(); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (!h.beacon.stopReason().empty()) h.status = "Beacon stopped after its limit of transmissions";
    } else {
    ImGui::BeginDisabled(!canCq);
    if (ImGui::Button(tr("CQ"))) { h.eng.startCq(now); c.txHz = h.txHz; h.status.clear(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    }
    if (ImGui::Button(tr("Halt TX"))) { h.eng.halt(); h.beacon.stop(); h.armed = false; h.tx.abort(); h.status = "Halted"; }
    ImGui::SameLine();
    ImGui::BeginDisabled(!h.licence);
    ImGui::PushStyleColor(ImGuiCol_Button, h.armed ? ImVec4(0.75f, 0.1f, 0.1f, 1) : ImVec4(0.3f, 0.3f, 0.3f, 1));
    if (ImGui::Button(h.armed ? tr("ARMED: click to disarm") : tr("Arm TX"))) { h.armed = !h.armed; h.eng.userAction(now); }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Nothing is sent unless this is armed, the licence box is ticked and the frequency is inside an amateur band. Disarmed again when you halt or the watchdog fires."));

    rigWidgets();
    // state line
    static const char* kState[] = {"Idle", "Calling CQ", "Calling: sent grid", "Sent report", "Sent R-report", "Sent RR73", "Sent 73"};
    if (isBeacon) ImGui::Text("%s: %s", tr("State"), h.beacon.active() ? "Beacon running" : "Beacon off");
    else ImGui::Text("%s: %s %s   %s: %d", tr("State"), kState[h.eng.state()], h.eng.dxCall().c_str(), tr("Logged"), h.logged);
    if (!h.status.empty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%s", h.status.c_str());
    uploadSection(h);
    if (!h.lastTx.empty()) ImGui::TextDisabled("%s", h.lastTx.c_str());

    // decodes in, the engine ticks
    HfdigFtxTelemetry f;
    a.engine.hfdig().ftx().telemetry(f);
    const double per = ham::slotPeriod(h.mode);
    double newest = h.lastFed;
    for (const auto& d : f.decodes) {
        if (d.mode != h.mode || d.slotUtc <= h.lastFed) continue;
        h.eng.onDecode(now, d.msg, (int)std::lround(d.snrDb), d.slotUtc, d.hz);
        newest = std::max(newest, d.slotUtc);
    }
    h.lastFed = newest;
    // a finished transmission: only now does the engine advance
    if (h.txState == 2 || h.txState == 3) {
        if (h.worker.joinable()) h.worker.join();
        std::lock_guard<std::mutex> l(h.mu);
        if (h.txState == 2) { if (h.txBeacon) h.beacon.txSent(); else h.eng.txSent(h.txSlot); h.lastTx = "Sent: " + h.txMsg; }
        else h.lastTx = "Not sent (" + h.txErr + "): " + h.txMsg;
        h.txState = 0;
    }
    if (!h.armed && h.tx.busy()) h.tx.abort();   // disarm, watchdog and Halt stop a running burst and release PTT at once
    h.io.rigIdx = h.rigSel;
    const long idx = ham::slotIndex(now, per);
    if ((isBeacon ? h.beacon.active() : h.eng.active()) && idx != h.lastTxSlot && now - ham::slotStart(idx, per) >= ham::txStartDelay(h.mode)) {
        h.lastTxSlot = idx;
        const std::string msg = isBeacon ? h.beacon.messageForSlot(ham::slotStart(idx, per)) : h.eng.messageForSlot(ham::slotStart(idx, per));
        if (!msg.empty() && h.txState == 0) {
            ham::HamTxCheck g;
            g.licenceConfirmed = h.licence; g.armed = h.armed; g.call = h.call; g.grid = h.grid;
            g.dialHz = a.freqMhz * 1e6; g.audioHz = isBeacon ? h.beacon.audioHz() : c.txHz; g.bwHz = ham::hamModeBwHz(h.mode, h.sub);
            std::string why;
            if (!ham::hamTxAllowed(g, why)) h.lastTx = "Not sent (" + why + "): " + msg;
            else {
                h.txMsg = msg; h.txSlot = ham::slotStart(idx, per); h.txState = 1; h.txBeacon = isBeacon;
                const double hz = g.audioHz, lv = h.level; const int md = h.mode, sb = h.sub;
                h.worker = std::thread([&h, g, md, msg, hz, lv, sb] {
                    std::string err;
                    const bool ok = h.tx.send(g, md, msg, hz, lv, err, sb);
                    std::lock_guard<std::mutex> l(h.mu);
                    h.txErr = err;
                    h.txState = ok ? 2 : 3;
                });
            }
        }
    }

    // the decoded list: double click calls the station
    ImGui::Separator();
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("##hamd", 5, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn("Hz"); ImGui::TableSetupColumn("W");
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        const std::string band = ham::bandNameFor(a.freqMhz * 1e6);
        for (size_t i = f.decodes.size(); i-- > 0;) {
            const FtxDecode& d = f.decodes[i];
            if (d.mode != h.mode) continue;
            ImGui::TableNextRow();
            ImGui::PushID((int)i);
            char b[16];
            const time_t t = (time_t)d.slotUtc;
            struct tm g;
#ifdef _WIN32
            gmtime_s(&g, &t);
#else
            gmtime_r(&t, &g);
#endif
            snprintf(b, sizeof b, "%02d%02d%02d", g.tm_hour, g.tm_min, g.tm_sec);
            ImGui::TableNextColumn();
            ImGui::Selectable(b, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !d.call.empty() && h.licence && stationOk && !isBeacon) {
                h.eng.callStation(now, d.call, d.grid, (int)std::lround(d.snrDb), d.slotUtc, d.hz);
                h.status.clear();
            }
            ImGui::TableNextColumn(); ImGui::Text("%+3.0f", d.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%5.0f", d.hz);
            ImGui::TableNextColumn();
            const bool w = !d.call.empty() && h.worked.workedOn(d.call, band, ham::hamModeName(h.mode));
            const bool wAny = !d.call.empty() && h.worked.worked(d.call);
            ImGui::TextUnformatted(w ? "B" : wAny ? "b" : "");
            ImGui::TableNextColumn();
            const bool toMe = !h.eng.config().myCall.empty() && d.msg.rfind(h.eng.config().myCall + " ", 0) == 0;
            if (toMe) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", d.msg.c_str());
            else if (d.cq) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "%s", d.msg.c_str());
            else ImGui::TextUnformatted(d.msg.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
