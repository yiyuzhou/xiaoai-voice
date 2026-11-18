#include "app/app.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <future>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/log.hpp"
#include "audio/kws_preprocess.hpp"
#include "app/conversation_policy.hpp"

#include "wakeup/kws_zipformer.hpp"

extern char** environ;

namespace xiaoai_plus::app {

namespace {

const auto kLog = xiaoai_plus::GetLogger("app");

void PauseMphelperBounded() {
  pid_t pid = -1;
  char arg0[] = "mphelper";
  char arg1[] = "pause";
  char* argv[] = {arg0, arg1, nullptr};
  const int rc = posix_spawn(&pid, "/usr/bin/mphelper", nullptr, nullptr, argv, environ);
  if (rc != 0 || pid <= 0) { kLog->warn("mphelper pause spawn failed"); return; }
  for (int i = 0; i < 20; ++i) {
    int status = 0;
    const pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid || result < 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  kill(pid, SIGKILL);
  bool reaped = false;
  for (int i = 0; i < 20; ++i) {
    const pid_t result = waitpid(pid, nullptr, WNOHANG);
    if (result == pid || result < 0) { reaped = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  kLog->warn("mphelper pause exceeded 1s; killed={}, reaped={}", true, reaped);
}

bool SayRepeatBounded(const std::string& message) {
  pid_t pid=-1;
  char executable[]="/usr/sbin/tts_play.sh";
  char* argv[]={executable,const_cast<char*>(message.c_str()),nullptr};
  posix_spawnattr_t attr;
  if(posix_spawnattr_init(&attr)!=0)return false;
  (void)posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP);
  (void)posix_spawnattr_setpgroup(&attr,0);
  const int rc=posix_spawn(&pid,executable,nullptr,&attr,argv,environ);
  posix_spawnattr_destroy(&attr);
  if(rc || pid<=0) {kLog->warn("repeat speech spawn failed");return true;}
  bool reaped=false;
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
  while(std::chrono::steady_clock::now()<deadline) {
    int status=0;const pid_t result=waitpid(pid,&status,WNOHANG);
    if(result==pid || (result<0 && errno==ECHILD))reaped=true;
    if(reaped && kill(-pid,0)!=0 && errno==ESRCH) {
      kLog->info("repeat speech group finished");return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  // Kill only the private process group created for this feedback. Do not
  // leave a background descendant audible after suppression is released.
  if(kill(-pid,0)==0)(void)kill(-pid,SIGKILL);
  kLog->warn("repeat speech killed at bound");
  for(int i=0;i<50;++i) {
    if(!reaped) {const auto result=waitpid(pid,nullptr,WNOHANG);if(result==pid || (result<0 && errno==ECHILD))reaped=true;}
    if(reaped && kill(-pid,0)!=0 && errno==ESRCH)return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  kLog->warn("repeat speech exceeded bounded wait");
  return false;
}

constexpr int kMinTriggerIntervalMs = 800;

struct EchoStats {
  double mic_rms{0.0};
  double ref_rms{0.0};
  double corr{0.0};
  double ratio{0.0};
  bool valid_ref{false};
};

EchoStats AnalyzeEcho(const std::vector<uint8_t>& mic_mono,
                      const std::vector<uint8_t>& reverse_ref) {
  EchoStats stats;
  if (mic_mono.empty() || reverse_ref.empty()) {
    return stats;
  }
  const size_t n = std::min(mic_mono.size(), reverse_ref.size()) / sizeof(int16_t);
  if (n == 0) {
    return stats;
  }

  const auto* mic = reinterpret_cast<const int16_t*>(mic_mono.data());
  const auto* ref = reinterpret_cast<const int16_t*>(reverse_ref.data());

  double mic_energy = 0.0;
  double ref_energy = 0.0;
  double cross = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double m = static_cast<double>(mic[i]);
    const double r = static_cast<double>(ref[i]);
    mic_energy += m * m;
    ref_energy += r * r;
    cross += m * r;
  }
  stats.mic_rms = std::sqrt(mic_energy / static_cast<double>(n));
  stats.ref_rms = std::sqrt(ref_energy / static_cast<double>(n));

  // Reference too weak, skip suppression.
  if (ref_energy < 1e7) {
    return stats;
  }
  stats.valid_ref = true;

  stats.corr = std::abs(cross) / std::sqrt((mic_energy + 1.0) * (ref_energy + 1.0));
  stats.ratio = std::sqrt((mic_energy + 1.0) / (ref_energy + 1.0));
  return stats;
}

void ScalePcmS16InPlace(std::vector<uint8_t>* pcm, float gain) {
  if (!pcm || pcm->empty() || pcm->size() % sizeof(int16_t) != 0) {
    return;
  }
  const float safe_gain = std::max(0.0f, gain);
  if (safe_gain >= 0.999f && safe_gain <= 1.001f) {
    return;
  }
  auto* samples = reinterpret_cast<int16_t*>(pcm->data());
  const size_t n = pcm->size() / sizeof(int16_t);
  for (size_t i = 0; i < n; ++i) {
    const float scaled = static_cast<float>(samples[i]) * safe_gain;
    const int32_t v = static_cast<int32_t>(std::round(scaled));
    samples[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, v)));
  }
}

float ComputeUplinkGainWhileAiSpeaking(const EchoStats& stats) {
  // Keep full-duplex: never hard-mute uplink while AI is speaking.
  if (!stats.valid_ref) {
    return 0.30f;
  }

  // Echo-dominant: reference is strong, mic/reference ratio is low, and correlation is non-trivial.
  const bool echo_dominant =
      (stats.ref_rms > 1200.0 && stats.ratio < 0.55 && stats.corr > 0.06);

  // Far-field speech can be low-RMS; avoid near-field-only thresholds.
  // Treat low correlation or higher mic/reference ratio as near-end double-talk evidence.
  const bool has_near_end_speech = (stats.corr < 0.04 || stats.ratio > 0.70);

  if (echo_dominant) {
    // Echo-only: strong suppression; double-talk: allow more uplink for user speech.
    return has_near_end_speech ? 0.34f : 0.006f;
  }

  return has_near_end_speech ? 0.55f : 0.18f;
}

std::string TrimSpace(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::vector<std::string> LoadWakeupKeywords(const std::string& keywords_file) {
  std::ifstream in(keywords_file);
  if (!in.is_open()) {
    throw std::runtime_error("failed to open wakeup.keywords_file: " + keywords_file);
  }

  std::vector<std::string> keywords;
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    line = TrimSpace(std::move(line));
    if (line.empty() || line.front() == '#' || line.front() == ';') {
      continue;
    }

    const auto at = line.rfind('@');
    if (at == std::string::npos) {
      throw std::runtime_error("invalid wakeup.keywords_file line " + std::to_string(lineno) +
                               ": missing '@' label");
    }
    auto keyword = TrimSpace(line.substr(at + 1));
    if (keyword.empty()) {
      throw std::runtime_error("invalid wakeup.keywords_file line " + std::to_string(lineno) +
                               ": empty keyword label");
    }
    if (keyword != "结束") keywords.push_back(std::move(keyword));
  }

  std::sort(keywords.begin(), keywords.end());
  keywords.erase(std::unique(keywords.begin(), keywords.end()), keywords.end());
  if (keywords.empty()) {
    throw std::runtime_error("wakeup.keywords_file has no valid keyword label: " +
                             keywords_file);
  }

  return keywords;
}

}  // namespace

App::App(config::Config cfg, bool kws_only, bool trial_once, bool kws_diagnostics,
         int kws_s32_shift, int kws_input_channel)
    : cfg_(std::move(cfg)), kws_only_(kws_only), trial_once_(trial_once),
      kws_diagnostics_(kws_diagnostics), kws_s32_shift_(kws_s32_shift),
      kws_input_channel_(kws_input_channel) {
  if (kws_diagnostics_ && !kws_only_) {
    throw std::invalid_argument("KWS diagnostics requires --kws-only");
  }
  if ((kws_s32_shift_ != 8 && kws_s32_shift_ != 16) ||
      kws_input_channel_ < -1 || kws_input_channel_ > 2 ||
      !(cfg_.wakeup.kws_threshold > 0.0f && cfg_.wakeup.kws_threshold <= 1.0f)) {
    throw std::invalid_argument("invalid KWS PCM configuration");
  }
  // LX06 uses the mono S16 noop PCM provided by its Open-XiaoAI ALSA patch.
  if (cfg_.audio.speaker_profile != "lx06") {
    cfg_.audio.output_device = "Playback";
    cfg_.audio.channels = 4;
    cfg_.audio.bits_per_sample = 32;
  }

  recorder_ = std::make_unique<audio::ArecordRecorder>(cfg_.audio);
  if (kws_diagnostics_) {
    kws_stats_ = std::make_unique<audio::KwsDiagnostics>(cfg_.audio.channels);
  }

  // Cloud TTS is 24 kHz mono S16LE. Capture stays at 16 kHz.
  config::Audio player_audio = config::MakeTtsPlaybackAudio(cfg_.audio);
  if (!kws_only_) {
    player_ = std::make_unique<audio::AplayPlayer>(player_audio, cfg_.budget.output_queue_frames);
  }

  // NS/AGC/AEC run on mono audio for uplink audio processing.
  aec_ = std::make_unique<dsp::AecWebrtc>(cfg_.audio.sample_rate, 1);

  wakeup::Hooks hooks;
  if(cfg_.conversation.ordinary_barge_in)
    hooks.after_disarm_scoped=[this](const std::string& reason,uint64_t revision){AfterDisarmScoped(reason,revision);};
  else hooks.after_disarm=[this](const std::string& reason){AfterDisarm(reason);};
  hooks.on_arm = [this](const std::string& reason) { OnArm(reason); };
  hooks.on_interrupt = [this](const std::string& reason) { OnWakeInterrupt(reason); };
  hooks.on_speaking_stall = [this]() { OnPlaybackStall(); };
  hooks.on_fatal = [this]() { FailWorker("gate"); };

  gate_ = std::make_unique<wakeup::Gate>(
      std::chrono::seconds(cfg_.conversation.idle_seconds), hooks,
      std::chrono::seconds(cfg_.conversation.absolute_timeout_seconds));

  auto wakeup_keywords = LoadWakeupKeywords(cfg_.wakeup.keywords_file);
  kLog->info("loaded {} wakeup keyword labels from '{}'", wakeup_keywords.size(),
             cfg_.wakeup.keywords_file);
  trigger_ = std::make_unique<wakeup::Trigger>(
      std::move(wakeup_keywords),
      [this](const std::string& keyword) {
        if(rephrase_speaking_.load())return false;
        if(barge_capture_ && !capture_healthy_.load())return false;
        if (native_wake_monitor_ && native_wake_monitor_->Blocked()) return false;
        return gate_->TryWakeup(keyword);
      });
  if (!kws_only_) {
    native_wake_monitor_ = std::make_unique<NativeWakeMonitor>(
        [this] { OnNativeWake(); }, cfg_.audio.native_wake_log,
        cfg_.audio.speaker_profile == "lx06");
  }

  auto kws_engine = std::make_shared<wakeup::ZipformerKwsEngine>(cfg_.wakeup);
  wakeup::LocalListener::Config kws_cfg;
  kws_cfg.gate = gate_.get();
  kws_cfg.trigger = trigger_.get();
  kws_cfg.kws_engine = std::move(kws_engine);
  kws_cfg.sample_rate = cfg_.audio.sample_rate;
  kws_cfg.channels = 1;
  kws_cfg.bounded_decisions = cfg_.conversation.ordinary_barge_in;
  kws_cfg.bit_depth = 16;
  kws_cfg.min_trigger_interval_ms = kMinTriggerIntervalMs;
  kws_cfg.exit_playback_threshold = cfg_.wakeup.exit_playback_threshold;
  kws_cfg.allow_playback_interrupt = cfg_.conversation.duplex_mode == "half";
  kws_cfg.ai_text_contains = [this](const std::string& label) { return exit_echo_guard_.Contains(label); };
  kws_cfg.on_end = [this]() {
    kLog->info("local KWS requested conversation exit");
    EndLocalConversation("local_kws_exit");
  };
  kws_listener_ = std::make_unique<wakeup::LocalListener>(kws_cfg);

  realtime::Client::Callbacks callbacks;
  callbacks.on_audio = [this](const std::vector<uint8_t>& chunk) { std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_); if(legacy_effects_live_.load())OnAudio(chunk); };
  callbacks.on_set_ai_speaking = [this](bool speaking) { std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_); if(legacy_effects_live_.load())OnSetAiSpeaking(speaking); };
  callbacks.on_asr_turn = [this](const realtime::duplex::Event& event) {
    std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_);
    if(legacy_effects_live_.load())OnSmartTurn(event);
  };
  callbacks.on_asr_final = [this](const std::string& text) { std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_); if(legacy_effects_live_.load())OnAsrFinal(text); };
  callbacks.on_asr_empty = [this]() { std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_); if(legacy_effects_live_.load()){UplinkLevelEvent(2);gate_->SetUserTurnOpen(false);} };
  callbacks.on_user_activity = [this]() { std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_); if(legacy_effects_live_.load())OnUserActivity(); };
  callbacks.on_session_closed = [this](const std::string& reason) { std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_); if(legacy_effects_live_.load())OnSessionClosed(reason); };
  callbacks.on_chat_ended = [this]() { std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_); if(legacy_effects_live_.load())OnChatEnded(); };
  callbacks.on_exit_intent = [this]() { std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_); if(legacy_effects_live_.load())OnServerExitIntent(); };
  callbacks.on_response_text = [this](const realtime::duplex::Event& event) {
    std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_);
    if(!legacy_effects_live_.load())return;
    exit_echo_guard_.Update(event.response_id,event.text,
      event.type=="response.output_text.done",event.type=="response.created");
    if(event.type=="response.done") {
      {std::lock_guard<std::mutex> lock(mu_);reply_pending_=false;}
      control_cv_.notify_one();
    }
  };
  callbacks.on_fatal = [this]() { std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_); if(legacy_effects_live_.load())FailWorker("realtime"); };
  if (!kws_only_ && cfg_.memory.enabled) {
    callbacks.on_text_event = [this](const realtime::duplex::Event& event) {
      std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_);
      if (legacy_effects_live_.load() && memory_ && gate_->step()==wakeup::Step::kActive && !memory_switching_.load())
        memory_->Event(memory_epoch_.load(), event);
    };
    if(cfg_.conversation.ordinary_barge_in) {
      callbacks.on_text_event={};
      callbacks.on_identity_text_event=[this](const realtime::IdentitySessionOrigin& origin,const realtime::duplex::Event& event) {
        std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_);
        if(legacy_effects_live_.load() && memory_ && gate_->step()==wakeup::Step::kActive && !memory_switching_.load())
          memory_->IdentitySessionEvent(origin,event);
      };
    }
    memory_ = std::make_unique<memory::Service>(cfg_.memory,
      memory::HttpsTransport(cfg_.memory, cfg_.realtime.preset.ca_file),
      [this](memory::Action action) { QueueMemoryAction(std::move(action)); });
  }
  if (!kws_only_) client_ = std::make_unique<realtime::Client>(cfg_, callbacks, trial_once_);
  if(!kws_only_ && cfg_.conversation.ordinary_barge_in) {
    barge_aec_=std::make_unique<audio::shadow::PairedWebrtcAec>();
    barge_vad_=std::make_unique<audio::shadow::WebrtcVoiceDetector>();
    barge_capture_=std::make_unique<audio::OrdinaryBargeCapture>(*barge_aec_,*barge_vad_,
      cfg_.conversation.barge_processing_io_delay_hint_ms);
    realtime::Client::Callbacks fresh;
    fresh.on_transport_event=[this](const auto& envelope,const auto& event) { OnFreshEvent(envelope,event); };
    fresh.on_transport_closed=[this](const auto& envelope,const auto& reason) {
      std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_);
      { std::lock_guard<std::mutex> lock(barge_mu_);if(envelope!=fresh_envelope_)return; }
      OnSessionClosed(reason);
    };
    fresh.on_fatal=[this] { FailWorker("fresh_realtime"); };
    fresh_client_=std::make_unique<realtime::Client>(cfg_,std::move(fresh),trial_once_,false,false,true);
    kLog->info("ordinary barge enabled; processing_io_delay_hint_ms={} provenance=nominal_90ms_capture_buffer_estimate_not_ADC_DAC_or_room_measurement",
      cfg_.conversation.barge_processing_io_delay_hint_ms);
  }


  if (player_) {
    if(cfg_.conversation.ordinary_barge_in) {
      player_->SetOnScopedChunkProcessed([this](uint64_t generation,audio::AplayPlayer::ChunkResult result) {
        std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_);
        {std::lock_guard<std::mutex> lock(barge_mu_);
          if(generation!=playback_owner_generation_ || result==audio::AplayPlayer::ChunkResult::kCanceled)return;}
        OnPlaybackChunkProcessed(result==audio::AplayPlayer::ChunkResult::kWritten);
        MaybeConfirmFreshPlaybackDrained();
      });
    }else player_->SetOnChunkProcessed([this](bool success) {OnPlaybackChunkProcessed(success);});
    player_->SetOnWriteProgress([this]() { if (gate_) gate_->NoteAiProgress(); });
    player_->SetOnFatal([this]() { FailWorker("player"); });
  }
}

namespace {
uint64_t KwsNowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
void App::OnStopKey() {
  // Idle presses remain entirely native; no media command is issued here.
  if (stopping_.load() || !gate_ || gate_->step() != wakeup::Step::kActive) return;
  kLog->info("physical play/pause key requested conversation exit");
  EndLocalConversation("user_exit");
}
void App::EndLocalConversation(const char* reason) {
  if (!gate_ || gate_->step() != wakeup::Step::kActive) return;
  session_ready_.store(false);
  InterruptPlayback();
  if (memory_) memory_->Interrupt(memory_epoch_.load());
  if (client_) (void)client_->InterruptResponse();
  gate_->Disarm(reason);
}
void App::AfterDisarm(const std::string& reason) {
    if(reason=="local_kws_exit" || reason=="user_exit")InterruptPlayback(audio::SessionCue::kExit);
    {std::lock_guard<std::mutex> lock(mu_);reply_pending_=false;}
    control_cv_.notify_one();
    memory_switching_.store(false);
    RetireBargeOwner();
    if(memory_) {
      const bool complete=reason=="user_exit" || reason=="local_kws_exit" || reason=="server_exit_intent" ||
        reason=="bye" || reason=="timeout" || reason=="welcome_timeout";
      realtime::TransportEnvelope envelope;
      {std::lock_guard<std::mutex> lock(barge_mu_);envelope=fresh_mode_ ? fresh_envelope_ : realtime::TransportEnvelope{};}
      if(envelope.valid())(void)memory_->End(envelope,complete);
      else (void)memory_->End(memory_epoch_.load(),complete);
    }
    session_ready_.store(false);
    QueueSessionClose();
}
void App::AfterDisarmScoped(const std::string& reason,uint64_t revision) {
  {
    std::lock_guard<std::mutex> fence(kws_dispatch_mu_);
    if(!gate_ || !gate_->ClaimDisarmCleanup({revision,reason}))return;
    // New KWS commits are temporarily refused, never blocked on cleanup.
    // If a newer commit won before this claim, its owner is untouched.
    kws_disarm_pending_=true;++kws_epoch_;kws_live_ticket_={};kws_suspended_=false;
  }
  {std::lock_guard<std::mutex> lock(kws_queue_mu_);kws_queue_.clear();}
  AfterDisarm(reason);
  {std::lock_guard<std::mutex> fence(kws_dispatch_mu_);kws_disarm_pending_=false;gate_->ReleaseDisarmCleanup({revision,reason});}
}
void App::InvalidateKws(uint64_t expected_epoch) {
  if(!cfg_.conversation.ordinary_barge_in)return;
  {
    std::lock_guard<std::mutex> fence(kws_dispatch_mu_);
    if(expected_epoch && expected_epoch!=kws_epoch_)return;
    ++kws_epoch_;
    // Invalidation rejects effects; native/fault/Stop owner retains its normal
    // Disarm/Close cleanup. Do not silently turn Gate idle before that hook.
    kws_live_ticket_={};kws_suspended_=false;
  }
  {std::lock_guard<std::mutex> lock(kws_queue_mu_);kws_queue_.clear();}
  kws_queue_cv_.notify_one();
}
void App::StartKwsConsumer() {
  if(!barge_capture_ || kws_consumer_thread_.joinable())return;
  kws_accepting_.store(true);
  kws_consumer_thread_=std::thread([this] {
    try {KwsConsumerLoop();}
    catch(const std::exception& e) {kLog->error("KWS consumer: {}",e.what());kws_accepting_.store(false);InvalidateKws();FailWorker("kws_consumer");}
    catch(...) {kws_accepting_.store(false);InvalidateKws();FailWorker("kws_consumer");}
  });
}
bool App::KwsBlockCurrentLocked(const KwsBlock& block) const {
  const auto now=KwsNowNs();
  return running_.load() && kws_accepting_.load() && !stopping_.load() && !kws_suspended_ && !kws_disarm_pending_ && !memory_switching_.load() &&
    block.epoch==kws_epoch_ && block.stamp.generation==kws_capture_generation_ &&
    block.identity==memory_epoch_.load() && gate_ && block.gate_revision==gate_->Revision() &&
    recorder_ && recorder_->Generation()==block.stamp.generation && capture_healthy_.load() &&
    block.stamp.continuous && !rephrase_speaking_.load() &&
    (!native_wake_monitor_ || !native_wake_monitor_->Blocked()) &&
    kws_delivery_ns_ && now>=kws_delivery_ns_ &&
    now-kws_delivery_ns_<=200000000ULL;
}
bool App::KwsTicketCurrentLocked(const KwsTicket& ticket) const {
  const auto now=KwsNowNs();
  return running_.load() && kws_accepting_.load() && ticket && ticket.serial==kws_live_ticket_.serial && ticket.epoch==kws_epoch_ &&
    ticket.capture==kws_capture_generation_ && ticket.identity==memory_epoch_.load() &&
    !stopping_.load() && capture_healthy_.load() && recorder_ && recorder_->Generation()==ticket.capture &&
    !rephrase_speaking_.load() && (!native_wake_monitor_ || !native_wake_monitor_->Blocked()) &&
    kws_delivery_ns_ && now>=kws_delivery_ns_ &&
    now-kws_delivery_ns_<=200000000ULL && gate_ &&
    (ticket.exit ? gate_->IsDisarmCurrent(ticket.exit) : gate_->IsWakeCurrent(ticket.wake));
}
bool App::KwsTicketCurrent(const KwsTicket& ticket) const {
  std::lock_guard<std::mutex> fence(kws_dispatch_mu_);return KwsTicketCurrentLocked(ticket);
}
void App::EnqueueKws(const std::vector<uint8_t>& pcm,const audio::CaptureStamp& stamp) {
  if(!kws_accepting_.load())return;
  KwsBlock block;
  {
    std::lock_guard<std::mutex> fence(kws_dispatch_mu_);
    if(kws_suspended_ || memory_switching_.load())return; // Legitimate wake preparation creates no backlog.
    block.stamp=stamp;block.epoch=kws_epoch_;block.identity=memory_epoch_.load();
    block.gate_revision=gate_ ? gate_->Revision() : 0;
    if(!KwsBlockCurrentLocked(block))return;
  }
  const size_t max_bytes=static_cast<size_t>(cfg_.audio.sample_rate)*90/1000*2;
  if(pcm.empty() || pcm.size()>max_bytes) {InvalidateKws(block.epoch);return;}
  block.pcm=pcm;
  bool overflow=false;
  {
    std::lock_guard<std::mutex> lock(kws_queue_mu_);
    if(kws_consumer_stop_ || !kws_accepting_.load())return;
    if(kws_queue_.size()>=3)overflow=true;
    else kws_queue_.push_back(std::move(block));
  }
  if(overflow)InvalidateKws(block.epoch);else kws_queue_cv_.notify_one();
}
void App::KwsConsumerLoop() {
  uint64_t stream_epoch=0,stream_capture=0;
  while(true) {
    KwsBlock block;
    {
      std::unique_lock<std::mutex> lock(kws_queue_mu_);
      kws_queue_cv_.wait(lock,[this] {return kws_consumer_stop_ || !kws_queue_.empty();});
      if(kws_consumer_stop_)return;
      block=std::move(kws_queue_.front());kws_queue_.pop_front();
    }
    bool valid;
    {std::lock_guard<std::mutex> fence(kws_dispatch_mu_);valid=KwsBlockCurrentLocked(block);}
    const auto now=KwsNowNs();
    if(!valid)continue;
    if(!block.stamp.first_read_ns || now<block.stamp.first_read_ns ||
        now-block.stamp.first_read_ns>300000000ULL) {InvalidateKws(block.epoch);continue;}
    if(stream_epoch!=block.epoch || stream_capture!=block.stamp.generation) {
      kws_listener_->Reset();stream_epoch=block.epoch;stream_capture=block.stamp.generation;
    }
    {std::lock_guard<std::mutex> fence(kws_dispatch_mu_);valid=KwsBlockCurrentLocked(block);}
    const auto entry=KwsNowNs();
    if(!valid)continue;
    if(entry<block.stamp.first_read_ns || entry-block.stamp.first_read_ns>300000000ULL) {InvalidateKws(block.epoch);continue;}
    const auto began=std::chrono::steady_clock::now();
    const auto birth=std::chrono::steady_clock::time_point(std::chrono::nanoseconds(block.stamp.first_read_ns));
    const auto result=kws_listener_->AnalyzePcm(block.pcm,birth);
    if(kws_stats_)kws_stats_->RecordDuration(audio::KwsDiagnostics::Stage::kKws,
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count());
    {std::lock_guard<std::mutex> fence(kws_dispatch_mu_);valid=KwsBlockCurrentLocked(block);}
    if(!valid)continue;
    const auto completed=KwsNowNs();
    if(completed<block.stamp.first_read_ns || completed-block.stamp.first_read_ns>500000000ULL) {InvalidateKws(block.epoch);continue;}
    if(result)DispatchKwsDecision(block,result);
  }
}
void App::DispatchKwsDecision(const KwsBlock& block,const wakeup::LocalListener::Decision& result) {
  KwsTicket ticket;
  {
    std::lock_guard<std::mutex> fence(kws_dispatch_mu_);
    const auto now=std::chrono::steady_clock::now();
    if(!KwsBlockCurrentLocked(block))return;
    const auto current_birth=std::chrono::steady_clock::time_point(std::chrono::nanoseconds(block.stamp.first_read_ns));
    if(!result.CanCommitAt(now,current_birth))return;
    if(result.end_session)ticket.exit=gate_->CommitDisarm("local_kws_exit");
    else ticket.wake=gate_->CommitWakeup(result.keyword);
    if(!ticket.wake && !ticket.exit)return;
    ticket.serial=++kws_serial_;ticket.epoch=++kws_epoch_;
    ticket.capture=block.stamp.generation;ticket.identity=block.identity;
    kws_live_ticket_=ticket;kws_suspended_=true;
  }
  // No queue/Listener/dispatch fence survives into the App hook effects.
  {std::lock_guard<std::mutex> lock(kws_queue_mu_);kws_queue_.clear();}
  kws_listener_->NoteAccepted();
  if(ticket.exit)OnKwsEnd(ticket);
  else if(ticket.wake.interrupt)OnWakeInterrupt(ticket.wake.reason,ticket);
  else OnArm(ticket.wake.reason,ticket);
}
void App::OnKwsEnd(const KwsTicket& ticket) {
  if(!KwsTicketCurrent(ticket))return;
  session_ready_.store(false);InterruptPlayback();
  if(memory_)memory_->Interrupt(ticket.identity);
  if(client_)(void)client_->InterruptResponse();
  if(KwsTicketCurrent(ticket))gate_->DispatchDisarm(ticket.exit);
}

bool App::StartRecorder() {
  auto failed=[this] {FailWorker("recorder");};
  auto stderr_observer=[this](const char* bytes,size_t count) {
    if(kws_stats_)kws_stats_->FeedRecorderStderr(std::string_view(bytes,count));
  };
  if(barge_capture_) {InvalidateKws();capture_healthy_.store(false);healthy_capture_generation_=0;healthy_capture_deliveries_=0;}
  if(barge_capture_)return recorder_->StartStamped(
    [this](const auto& chunk,const auto& stamp) {OnInputAudioStamped(chunk,stamp);},failed,stderr_observer);
  return recorder_->Start([this](const auto& chunk) {OnInputAudio(chunk);},failed,
    kws_stats_ ? std::function<void(const char*,size_t)>(stderr_observer) : std::function<void(const char*,size_t)>{});
}
void App::OnInputAudioStamped(const std::vector<uint8_t>& chunk,const audio::CaptureStamp& stamp) {
  {
    std::lock_guard<std::mutex> lock(kws_dispatch_mu_);
    kws_delivery_ns_=stamp.delivered_ns;
    current_capture_stamp_=stamp; // Protected publication; recorder remains sole producer.
    if(kws_capture_generation_!=stamp.generation) {
      ++kws_epoch_;kws_capture_generation_=stamp.generation;kws_live_ticket_={};kws_suspended_=false;
    }
  }
  if(!stamp.continuous)InvalidateKws();
  OnInputAudio(chunk);
}
void App::QueueMemoryAction(memory::Action action) {
  if(action.restart && !action.after_reply && action.epoch==memory_epoch_.load()) {
    std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_);
    legacy_effects_live_.store(false);
    session_ready_.store(false);memory_switching_.store(true);
    if(player_)player_->Interrupt(player_->Generation());
    InterruptPlayback();
  }
  std::lock_guard<std::mutex> lock(control_mu_);
  if(control_stop_ || action.epoch!=memory_epoch_.load())return;
  if(action.restart && !action.after_reply){session_ready_.store(false);memory_switching_.store(true);}
  if(memory_actions_.size()<4)memory_actions_.push_back(std::move(action));
  control_cv_.notify_one();
}
void App::RetireBargeOwner() {
  if(!barge_capture_)return;
  std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_);
  legacy_effects_live_.store(false);
  realtime::TransportEnvelope envelope;
  {std::lock_guard<std::mutex> lock(barge_mu_);
    envelope=fresh_envelope_;
    fresh_granted_=false;barge_collecting_=false;barge_pcm_.clear();
    fresh_input_ended_=true;
    fresh_permit_={};capture_context_={};barge_cv_.notify_all();}
  // Shut enqueue and presend immediately, before the asynchronous Service
  // end or owner socket close. Pause takes no network/write wait.
  if(fresh_client_ && envelope.valid())(void)fresh_client_->PauseFreshInput(envelope,"app_owner_retired");
}
void App::ProcessOrdinaryBargeCapture(const audio::CaptureStamp& stamp) {
  const bool active=gate_ && gate_->step()==wakeup::Step::kActive &&
    (!native_wake_monitor_ || !native_wake_monitor_->Blocked());
  bool speaking=false;
  {std::lock_guard<std::mutex> lock(mu_);speaking=is_ai_speaking_ || pending_playback_chunks_>0;}
  audio::shadow::Context context;
  bool confirm=false, actual_input_needed=false;
  {
    std::lock_guard<std::mutex> lock(barge_mu_);
    capture_last_ns_=stamp.delivered_ns;capture_last_generation_=stamp.generation;
    if(!capture_context_.valid() || capture_context_.epoch!=std::max<uint64_t>(1,memory_epoch_.load()) ||
        capture_context_.capture!=stamp.generation) {
      capture_context_={std::max<uint64_t>(1,memory_epoch_.load()),transport_sequence_,
        input_sequence_,player_ ? player_->Generation() : 1,input_sequence_,stamp.generation};
    }
    context=capture_context_;
    actual_input_needed=barge_collecting_ || (fresh_mode_ && fresh_granted_);
    confirm=active && !rephrase_speaking_.load() && !memory_switching_.load() &&
      !barge_collecting_ && (speaking || (fresh_mode_ && fresh_done_));
  }
  const auto result=barge_capture_->Push(context,stamp,s16_buf_,confirm,speaking,
                                       actual_input_needed);
  if(speaking && stamp.delivered_ns-echo_path_log_ns_>=5000000000ULL) {
    echo_path_log_ns_=stamp.delivered_ns;
    kLog->info("ordinary echo path capture={} reference_active={} confident={} raw_channel_lag_samples={} support=-32..1440; unconfident playback cannot confirm near_end",
      context.capture,result.reference_active,result.echo_path_confident,result.echo_delay_samples);
  }
  barge_cleaned_buf_.clear();
  for(const auto& frame:result.frames) {
    const auto* bytes=reinterpret_cast<const uint8_t*>(frame.pcm.data());
    barge_cleaned_buf_.insert(barge_cleaned_buf_.end(),bytes,bytes+320);
  }
  if(result.fault) {
    InvalidateKws();
    const bool first_fault=capture_healthy_.exchange(false) || capture_fault_generation_!=stamp.generation;
    healthy_capture_deliveries_=0;
    if(first_fault)kLog->warn("capture interrupted generation={} continuous={} reason=capture_health; keeping conversation",stamp.generation,stamp.continuous);
    {std::lock_guard<std::mutex> lock(barge_mu_);barge_buffer_failed_=true;fresh_granted_=false;barge_pcm_.clear();capture_fault_generation_=stamp.generation;}
    {std::lock_guard<std::mutex> lock(control_mu_);if(!control_stop_)capture_restart_requested_=true;}
    control_cv_.notify_one();return;
  }
  if(healthy_capture_generation_!=stamp.generation) {healthy_capture_generation_=stamp.generation;healthy_capture_deliveries_=0;}
  if(++healthy_capture_deliveries_>=2 && !capture_healthy_.exchange(true)) {
    kLog->info("ordinary capture ready generation={} healthy_deliveries={}; wake gate open",stamp.generation,healthy_capture_deliveries_);
    barge_cv_.notify_all();
  }
  if(!active) {
    realtime::TransportEnvelope paused;
    {std::lock_guard<std::mutex> lock(barge_mu_);
      if(barge_collecting_ || fresh_granted_) {
        fresh_granted_=false;barge_collecting_=false;barge_pcm_.clear();paused=fresh_envelope_;
      }}
    if(paused.valid())(void)fresh_client_->PauseFreshInput(paused,"inactive_or_native_interlock");
    return;
  }
  if(result.confirmed) {
    auto preroll=barge_capture_->TakePreroll();
    {legacy_effects_live_.store(false);
      std::lock_guard<std::mutex> lock(barge_mu_);
      barge_origin_=context;barge_playback_=player_ ? player_->Generation() : 0;
      barge_pcm_.assign(preroll.begin(),preroll.end());barge_collecting_=true;
      barge_buffer_failed_=false;fresh_granted_=false;
      fresh_seen_voice_=false;fresh_silence_frames_=0;fresh_input_ended_=false;
      for(const auto& frame:preroll) {
        if(frame.voiced) {fresh_seen_voice_=true;fresh_silence_frames_=0;}
        else if(fresh_seen_voice_)++fresh_silence_frames_;
      }
    }
    session_ready_.store(false);
    {std::lock_guard<std::mutex> lock(control_mu_);if(!control_stop_)barge_requested_=true;}
    control_cv_.notify_one();
    kLog->info("ordinary barge confirmed local_VAD_60ms capture={} first_sample={} delivery_ns={} origin_transport={} playback={}",
      context.capture,preroll.empty()?0:preroll.front().first_sample,stamp.delivered_ns,context.session,context.playback);
    return;
  }
  bool failed=false;bool end_input=false;realtime::TransportEnvelope end_envelope;
  {
    std::lock_guard<std::mutex> lock(barge_mu_);
    if(barge_collecting_ || (fresh_mode_ && fresh_granted_)) {
      for(const auto& frame:result.frames) {
        if(frame.origin!=barge_origin_ || barge_pcm_.size()>=300) {failed=true;break;}
        barge_pcm_.push_back(frame);
        if(frame.voiced) {fresh_seen_voice_=true;fresh_silence_frames_=0;}
        else if(fresh_seen_voice_)++fresh_silence_frames_;
      }
      const uint64_t now=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
      if(!barge_pcm_.empty() && now-barge_pcm_.front().first_read_ns>3000000000ULL)failed=true;
      if(!failed && fresh_granted_ && fresh_permit_.valid()) {
        // Transfer once, oldest first. A full sender queue leaves the original
        // frames in this bounded owner buffer; it never rewrites their birth.
        while(barge_pcm_.size()>=2) {
          const auto& a=barge_pcm_[0];const auto& b=barge_pcm_[1];
          if(a.origin!=b.origin || b.first_sample!=a.first_sample+160) {failed=true;break;}
          std::vector<uint8_t> bytes(640);
          std::memcpy(bytes.data(),a.pcm.data(),320);std::memcpy(bytes.data()+320,b.pcm.data(),320);
          realtime::Client::CapturedInput origin{a.origin,a.first_sample,a.first_read_ns,
            b.delivered_ns,fresh_envelope_.input()};
          if(!fresh_client_->EnqueueFreshCapturedAudio(fresh_envelope_,std::move(bytes),origin))break;
          barge_pcm_.pop_front();barge_pcm_.pop_front();
        }
        // The endpoint comes from real residual VAD silence. End only after
        // every queued 20ms frame has transferred; Client serializes the tail,
        // commit and mute on its sender, never on the capture worker.
        if(barge_pcm_.empty() && fresh_seen_voice_ && fresh_silence_frames_>=80 &&
            !fresh_input_ended_ && !fresh_output_started_) {
          fresh_input_ended_=true;fresh_granted_=false;
          end_envelope=fresh_envelope_;end_input=true;
        }
      }
      if(failed) {barge_buffer_failed_=true;fresh_granted_=false;barge_pcm_.clear();}
    }
  }
  if(end_input && !fresh_client_->EndFreshInput(end_envelope)) {
    // The sender may already have atomically paused input for a legitimate
    // output whose App callback is next. Do not cancel that answer merely
    // because the local endpoint lost this race; capture remains closed.
    kLog->warn("fresh input endpoint not queued; input stays closed awaiting legal output or idle timeout");
  }
  if(failed) {
    kLog->warn("ordinary input buffer lost continuity or exceeded 3 seconds; asking for a new wake/utterance");
    {std::lock_guard<std::mutex> lock(control_mu_);if(!control_stop_)rephrase_requested_=true;rephrase_text_="刚才那句话没有接全，请再叫豆包说一次。";}
    gate_->Disarm("ordinary_input_buffer_failed");control_cv_.notify_one();
  }
}
void App::RestartBargeCapture() {
  if(!barge_capture_ || stopping_.load())return;
  {std::lock_guard<std::mutex> lock(barge_mu_);if(capture_fault_generation_!=recorder_->Generation())return;}
  const auto now=std::chrono::steady_clock::now();
  if(capture_restart_window_.time_since_epoch().count()==0 || now-capture_restart_window_>std::chrono::seconds(60)) {
    capture_restart_window_=now;capture_restart_attempts_=0;
  }
  if(++capture_restart_attempts_>3) {kLog->error("ordinary capture repeated fault: three real restarts within 60 seconds failed");FailWorker("ordinary_capture_repeated_fault");return;}
  // Fault recovery is a physical producer stop/join then fork. Do not simply
  // mint a higher capture number for already queued bytes.
  realtime::TransportEnvelope old;
  {std::lock_guard<std::mutex> lock(barge_mu_);old=fresh_envelope_;}
  bool fresh=false;
  {std::lock_guard<std::mutex> lock(barge_mu_);fresh=fresh_mode_;barge_collecting_=false;barge_pcm_.clear();}
  if(fresh && old.valid()) { (void)fresh_client_->RetireFreshTransport(old,[this] {
      return player_ && player_->Interrupt(player_->Generation());});
    if(memory_)(void)memory_->RetireTransportResume(old);
    // A capture-bound input cannot reuse the old lease. The next voice opens
    // a fresh transport for the same identity through the existing path.
    {std::lock_guard<std::mutex> lock(barge_mu_);fresh_done_=true;fresh_granted_=false;
      fresh_envelope_={};fresh_permit_={};fresh_session_id_.clear();}
    {std::lock_guard<std::mutex> lock(mu_);is_ai_speaking_=false;stream_audio_done_=true;pending_playback_chunks_=0;turn_complete_pending_=false;ResetFarewellStateLocked();}
    if(gate_)gate_->SetAiSpeaking(false);
  }
  const uint64_t old_generation=recorder_->Generation();
  recorder_->Stop();
  if(!StartRecorder() || recorder_->Generation()<=old_generation) {FailWorker("capture_restart");return;}
  kLog->info("capture reopened old_generation={} new_generation={}; conversation retained",
    old_generation,recorder_->Generation());
}
void App::HandleOrdinaryBarge() {
  if(!fresh_client_ || !memory_ || !player_ || stopping_.load())return;
  audio::shadow::Context origin;realtime::TransportEnvelope old;uint64_t expected_playback;
  {
    std::lock_guard<std::mutex> lock(barge_mu_);
    if(!barge_collecting_ || barge_buffer_failed_)return;
    origin=barge_origin_;old=fresh_envelope_;expected_playback=barge_playback_;
  }
  const auto began=std::chrono::steady_clock::now();
  const auto deadline=began+std::chrono::milliseconds(2400);
  auto eligible=[&] {
    std::lock_guard<std::mutex> lock(barge_mu_);
    const auto now=std::chrono::steady_clock::now();
    const auto now_ns=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    return running_.load() && !stopping_.load() && gate_->step()==wakeup::Step::kActive &&
      (!native_wake_monitor_ || !native_wake_monitor_->Blocked()) &&
      origin.epoch==memory_epoch_.load() && capture_last_generation_==origin.capture &&
      recorder_ && recorder_->Generation()==origin.capture &&
      capture_last_ns_ && now_ns>=capture_last_ns_ && now_ns-capture_last_ns_<=3000000000ULL &&
      !barge_buffer_failed_ && barge_collecting_ && now<deadline;
  };
  auto fail=[&](const char* reason) {
    RetireBargeOwner();
    kLog->warn("ordinary input recovery reason={}; resuming same conversation",reason);
    {std::lock_guard<std::mutex> lock(mu_);is_ai_speaking_=false;stream_audio_done_=true;pending_playback_chunks_=0;turn_complete_pending_=false;ResetFarewellStateLocked();}
    gate_->SetAiSpeaking(false);
    if(gate_->step()==wakeup::Step::kActive && origin.epoch==memory_epoch_.load())
      (void)memory_->Resume(origin.epoch);
  };
  if(!eligible()) {fail("stale_owner");return;}
  bool stopped=false;
  if(old.valid()) {
    stopped=fresh_client_->RetireFreshTransport(old,[&] {return player_->Interrupt(expected_playback);});
    (void)memory_->RetireTransportResume(old);
  }else {
    (void)client_->InterruptResponse();
    stopped=player_->Interrupt(expected_playback);
    (void)client_->FinishSession(std::chrono::milliseconds(0));
    client_->Stop(); // Fully retires legacy callbacks/socket before new lease.
    (void)memory_->Interrupt(origin.epoch);
  }
  if(!stopped || !eligible()) {fail("stop_or_retire");return;}
  // A real interruption ends the old reply itself. Apply an already resolved
  // identity before granting its captured PCM; do not discard that utterance.
  {
    std::lock_guard<std::mutex> lock(control_mu_);
    if(!memory_actions_.empty() && memory_actions_.front().after_reply &&
        memory_actions_.front().epoch==origin.epoch) {
      identity_pending_.store(memory_actions_.front().identity_pending);
      memory_actions_.pop_front();
      (void)memory_->Activate(origin.epoch);
    }
  }
  {std::lock_guard<std::mutex> lock(mu_);is_ai_speaking_=false;stream_audio_done_=true;
    reply_pending_=false;pending_playback_chunks_=0;turn_complete_pending_=false;ResetFarewellStateLocked();}
  gate_->SetAiSpeaking(false);gate_->RefreshTimeout();
  auto await=[&](auto submit) -> std::optional<memory::ResumeResult> {
    auto promise=std::make_shared<std::promise<memory::ResumeResult>>();auto future=promise->get_future();
    if(!submit([promise](memory::ResumeResult r) {try {promise->set_value(std::move(r));}catch(...) {}}))return {};
    if(future.wait_until(deadline)!=std::future_status::ready)return {};
    return future.get();
  };
  const auto identity=await([&](auto reply) {return memory_->InspectTransportResume(origin.epoch,reply);});
  if(!identity || !identity->approved || !eligible()) {fail("identity_resume");return;}
  uint64_t transport,input;
  {std::lock_guard<std::mutex> lock(barge_mu_);transport=++transport_sequence_;input=++input_sequence_;}
  const auto prepared=await([&](auto reply) {return memory_->PrepareTransportResume(identity->identity,
    transport,origin.capture,input,reply);});
  if(!prepared || !prepared->approved || !eligible()) {fail("lease_prepare");return;}
  {std::lock_guard<std::mutex> lock(barge_mu_);fresh_envelope_=prepared->envelope;
    fresh_session_id_.clear();fresh_mode_=true;fresh_done_=false;
    fresh_audio_done_=false;fresh_output_started_=false;fresh_input_ended_=false;
    playback_owner_generation_=player_->Generation();}
  if(!usage_inherited_) {
    if(!fresh_client_->InheritRetiredUsage(*client_)) {fail("usage_inheritance");return;}
    usage_inherited_=true;
  }
  if(!fresh_client_->Start() || !fresh_client_->PrepareFreshTransport(prepared->envelope)) {
    fail("transport_prepare");return;
  }
  auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now());
  if(remaining.count()<1 || !fresh_client_->StartSession(remaining,prepared->instructions)) {fail("session_create");return;}
  std::string session;
  {std::unique_lock<std::mutex> lock(barge_mu_);barge_cv_.wait_until(lock,deadline,[&] {
    return !fresh_session_id_.empty() || barge_buffer_failed_ || !barge_collecting_;});session=fresh_session_id_;}
  const auto permit=await([&](auto reply) {return memory_->ActivateTransportResume(prepared->envelope,reply);});
  if(!permit || !permit->approved || !eligible() || session.empty()) {fail("owner_receipt");return;}
  realtime::FreshTransportOwner::Proof proof{true,true,true,true,true,false};
  if(!fresh_client_->GrantFreshInput(permit->permit,session,origin.capture,proof)) {fail("input_grant");return;}
  {std::lock_guard<std::mutex> lock(barge_mu_);fresh_permit_=permit->permit;
    fresh_granted_=true;barge_collecting_=false;}
  session_ready_.store(true);gate_->RefreshTimeout();
  kLog->info("ordinary barge fresh input granted transport={} input={} capture={} resume_ms={} source_transport={} original_first_read_preserved=true",
    transport,input,origin.capture,std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now()-began).count(),origin.session);
}
void App::OnFreshEvent(const realtime::TransportEnvelope& envelope,const realtime::duplex::Event& event) {
  std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_);
  {std::lock_guard<std::mutex> lock(barge_mu_);
    if(envelope!=fresh_envelope_ || !fresh_mode_)return;
    if(event.type=="session.created") {fresh_session_id_=event.session_id;barge_cv_.notify_all();return;}
    if(!fresh_permit_.valid())return;
  }
  if(gate_->step()!=wakeup::Step::kActive || memory_switching_.load() ||
     (native_wake_monitor_ && native_wake_monitor_->Blocked()))return;
  if(event.type=="conversation.item.input_audio_transcription.started")UplinkLevelEvent(1);
  if(event.type=="conversation.item.input_audio_transcription.ended_empty" ||
     event.type=="conversation.item.input_audio_transcription.failed")UplinkLevelEvent(2);
  if(event.type=="response.output_audio.started" || event.type=="response.output_audio.delta") {
    {std::lock_guard<std::mutex> lock(barge_mu_);fresh_granted_=false;fresh_output_started_=true;barge_pcm_.clear();}
    (void)fresh_client_->PauseFreshInput(envelope,"output_started");
    OnSetAiSpeaking(true);
    if(event.type=="response.output_audio.delta")OnAudio(event.audio);
  }else if(event.type=="response.output_audio.done") {
    {std::lock_guard<std::mutex> lock(barge_mu_);fresh_granted_=false;fresh_audio_done_=true;
      barge_pcm_.clear();}
    {std::lock_guard<std::mutex> lock(mu_);reply_pending_=false;}
    OnSetAiSpeaking(false);OnChatEnded();
    MaybeConfirmFreshPlaybackDrained();
  }else if(event.type=="conversation.item.input_audio_transcription.completed")OnAsrFinal(event.text);
  else if(event.type.rfind("conversation.item.input_audio_transcription.",0)==0) {
    gate_->RefreshTimeout();
  }else if(event.type.rfind("response.output_text.",0)==0) {
    exit_echo_guard_.Update(event.response_id,event.text,event.type=="response.output_text.done",false);
  }else if(event.type=="session.closed" || event.type=="error") {OnSessionClosed("fresh_session_closed");return;}
  if(memory_)(void)memory_->Event(envelope,event);
}
void App::MaybeConfirmFreshPlaybackDrained() {
  // Server audio.done is separate from physical Player completion. Only the
  // current envelope/generation can release the normal next-turn window.
  realtime::TransportEnvelope envelope;
  {std::lock_guard<std::mutex> lock(mu_);
    if(is_ai_speaking_ || pending_playback_chunks_!=0)return;}
  {std::lock_guard<std::mutex> lock(barge_mu_);
    if(!fresh_mode_ || !fresh_audio_done_ || fresh_done_ || !fresh_permit_.valid() ||
        !player_ || player_->Generation()!=playback_owner_generation_)return;
    envelope=fresh_envelope_;
  }
  if(!fresh_client_->ConfirmFreshPlaybackDrained(envelope))return;
  {std::lock_guard<std::mutex> lock(barge_mu_);
    if(envelope!=fresh_envelope_ || !fresh_audio_done_)return;
    fresh_done_=true;capture_context_={};}
  if(memory_)(void)memory_->PlaybackDrained(envelope);
  if(gate_->step()==wakeup::Step::kActive)gate_->RefreshTimeout();
  kLog->info("fresh audible turn drained transport={} input={}; next near-end uses new transport with same identity",envelope.transport(),envelope.input());
}

App::~App() { Stop(); }

bool App::Run() {
  if (!kws_only_) {
    if (!volume_.Initialize()) kLog->warn("shared volume initialization failed");
    kLog->info("playback pcm={} gain={} shared_volume={}", cfg_.audio.output_device,
               cfg_.audio.playback_gain, audio::VolumeControl::Read());
  }
  kLog->info("starting app device={} rate={} channels={} bits={} buffer={} period={} kws_threshold={} kws_num_threads={}",
      cfg_.audio.input_device, cfg_.audio.sample_rate, cfg_.audio.channels,
      cfg_.audio.bits_per_sample, cfg_.audio.buffer_size, cfg_.audio.period_size,
      cfg_.wakeup.kws_threshold,cfg_.wakeup.kws_num_threads);
  kLog->info("kws AGC {}: frame=10ms smoothing=100ms target=-25dBFS gain=0..24dB NS=off",
             cfg_.wakeup.kws_agc ? "enabled" : "disabled");
  if (kws_stats_) {
    kLog->info("kws-diag input conversion shift={} mono_source={}", kws_s32_shift_,
               kws_input_channel_ < 0 ? "beamformed" : std::to_string(kws_input_channel_));
  }

  {
    std::lock_guard<std::mutex> lock(state_mu_);
    lifecycle_ = Lifecycle::kRunning;
  }

  running_.store(true);
  recorder_failed_.store(false);
  if (native_wake_monitor_) native_wake_monitor_->Start();
  if (kws_stats_) last_kws_diag_log_ = std::chrono::steady_clock::now();
  if (memory_) { memory_->Start(); kLog->info("memory module enabled"); }
  if (!kws_only_) control_thread_ = std::thread([this] {
    try { ControlLoop(); }
    catch (const std::exception& e) { kLog->error("control thread: {}", e.what()); FailWorker("control"); }
    catch (...) { kLog->error("control thread unknown exception"); FailWorker("control"); }
  });
  if (!kws_only_) playback_thread_ = std::thread([this] {
    try { PlaybackControlLoop(); }
    catch (const std::exception& e) { kLog->error("playback control thread: {}", e.what()); FailWorker("playback_control"); }
    catch (...) { kLog->error("playback control thread unknown exception"); FailWorker("playback_control"); }
  });

  if (player_ && !player_->Start()) {
    kLog->error("app start failed: player start");
    running_.store(false);
    return false;
  }
  if (client_ && !client_->Start()) {
    kLog->error("app start failed: realtime client start");
    running_.store(false);
    if (player_) player_->Close();
    return false;
  }
  StartKwsConsumer();
  if (!StartRecorder()) {
    kLog->error("app start failed: recorder start");
    running_.store(false);
    if (client_) client_->Stop();
    if (player_) player_->Close();
    return false;
  }
  if (!kws_only_) {
    stop_key_ = std::make_unique<input::StopKey>(cfg_.stop_key.device, cfg_.stop_key.key_code,
                                               [this] { OnStopKey(); });
    if (!stop_key_->Start()) kLog->warn("stop key unavailable; verify input mapping");
  }
  kLog->info("app components started");

  if (!kws_only_) {
    std::thread([]() {
      try {
        const int rc = std::system("/usr/sbin/tts_play.sh '程序已启动' >/dev/null 2>&1");
        if (rc != 0) kLog->warn("startup tts command failed: rc={}", rc);
      } catch (const std::exception& e) { kLog->error("startup tts thread: {}", e.what()); }
        catch (...) { kLog->error("startup tts thread unknown exception"); }
    }).detach();
  }

  {
    std::unique_lock<std::mutex> lock(run_mu_);
    run_cv_.wait(lock, [this]() { return !running_.load(); });
  }

  {
    std::lock_guard<std::mutex> lock(state_mu_);
    lifecycle_ = Lifecycle::kStopped;
  }
  return !recorder_failed_.load();
}

void App::Stop() {
  if(stopping_.exchange(true))return;
  if (!kws_only_ && running_.load()) (void)volume_.Snapshot();
  kLog->info("app stopping");
  running_.store(false);
  kws_accepting_.store(false);
  InvalidateKws();
  {std::lock_guard<std::mutex> lock(kws_queue_mu_);kws_consumer_stop_=true;}
  kws_queue_cv_.notify_all();
  run_cv_.notify_all();

  {
    std::lock_guard<std::mutex> lock(state_mu_);
    lifecycle_ = Lifecycle::kStopping;
  }

  if (stop_key_) stop_key_->Stop();
  if (native_wake_monitor_) native_wake_monitor_->Stop();
  {
    std::lock_guard<std::mutex> lock(control_mu_);
    control_stop_ = true;
    close_requested_ = false;
    start_requested_ = false;
  }
  control_cv_.notify_all();
  if (control_thread_.joinable()) control_thread_.join();
  // End paid uplink before joining any playback thread or external audio process.
  if (client_) client_->Stop();
  if (fresh_client_) fresh_client_->Stop();
  if (memory_) memory_->Stop();
  {
    std::lock_guard<std::mutex> lock(playback_mu_);
    playback_stop_ = true;
    interrupt_requested_ = false;
  }
  playback_cv_.notify_all();
  if (playback_thread_.joinable()) playback_thread_.join();
  if (recorder_) recorder_->Stop();
  if(kws_consumer_thread_.joinable())kws_consumer_thread_.join();
  // Recorder and the ordinary KWS consumer are joined before closing the
  // listener and gate so a late KWS callback cannot cross their Close().
  if (kws_listener_) kws_listener_->Close();
  if (gate_) gate_->Close();
  if (kws_stats_) LogKwsDiagnostics();
  if (player_) player_->Close();

  {
    std::lock_guard<std::mutex> lock(state_mu_);
    lifecycle_ = Lifecycle::kStopped;
  }
  kLog->info("app stopped");
}

Lifecycle App::State() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  return lifecycle_;
}

void App::ConvertS32ToS16(const std::vector<uint8_t>& chunk, std::vector<uint8_t>* out,
                          int shift) {
  if (!out) {
    return;
  }
  if (chunk.size() % sizeof(int32_t) != 0) {
    out->assign(chunk.begin(), chunk.end());
    return;
  }
  const size_t n = chunk.size() / sizeof(int32_t);
  const auto* in = reinterpret_cast<const int32_t*>(chunk.data());
  out->resize(n * sizeof(int16_t));
  audio::ConvertKwsS32ToS16(in, n, shift, reinterpret_cast<int16_t*>(out->data()));
}


void App::DownmixToMono(const std::vector<uint8_t>& chunk, int channels,
                        std::vector<uint8_t>* out) {
  if (!out) {
    return;
  }
  if (channels <= 1 || chunk.empty()) {
    out->assign(chunk.begin(), chunk.end());
    return;
  }
  const size_t stride = static_cast<size_t>(channels);
  if (chunk.size() % (sizeof(int16_t) * stride) != 0) {
    out->assign(chunk.begin(), chunk.end());
    return;
  }
  const auto* in = reinterpret_cast<const int16_t*>(chunk.data());
  const size_t n_frames = chunk.size() / (sizeof(int16_t) * stride);

  // Ch0-Ch2 are mics; Ch3 is playback reference on 4-ch capture.
  const int mic_channels = (channels == 4) ? 3 : std::max(1, channels);

  out->resize(n_frames * sizeof(int16_t));
  auto* dst = reinterpret_cast<int16_t*>(out->data());

  if (mic_channels < 2) {
    for (size_t i = 0; i < n_frames; ++i) {
      dst[i] = in[i * stride];
    }
    return;
  }

  // Delay-and-sum beamforming for 小爱音箱Pro 6-mic circular array.
  // Diameter 7.4cm → max inter-mic distance 7.4cm → max delay ≈ 3 samples at 16kHz.
  // Estimate per-channel delay relative to ch0 via cross-correlation, then align before summing.
  static constexpr int kMaxLag = 3;

  auto xcorr_lag = [&](int ch_a, int ch_b) -> int {
    int best_lag = 0;
    int64_t best = INT64_MIN;
    for (int lag = -kMaxLag; lag <= kMaxLag; ++lag) {
      const size_t t0 = static_cast<size_t>(std::max(0, -lag));
      const size_t t1 = n_frames - static_cast<size_t>(std::max(0, lag));
      int64_t s = 0;
      for (size_t t = t0; t < t1; ++t) {
        s += static_cast<int64_t>(in[t * stride + ch_a]) *
             static_cast<int64_t>(in[(static_cast<int>(t) + lag) * stride + ch_b]);
      }
      if (s > best) {
        best = s;
        best_lag = lag;
      }
    }
    return best_lag;
  };

  // lag_k: ch_k[t + lag_k] aligns with ch0[t].
  const int lag1 = xcorr_lag(0, 1);
  const int lag2 = (mic_channels >= 3) ? xcorr_lag(0, 2) : 0;

  const int n = static_cast<int>(n_frames);
  for (int t = 0; t < n; ++t) {
    int32_t sum = static_cast<int32_t>(in[t * stride]);
    const int t1 = std::max(0, std::min(n - 1, t + lag1));
    sum += static_cast<int32_t>(in[static_cast<size_t>(t1) * stride + 1]);
    if (mic_channels >= 3) {
      const int t2 = std::max(0, std::min(n - 1, t + lag2));
      sum += static_cast<int32_t>(in[static_cast<size_t>(t2) * stride + 2]);
    }
    dst[t] = static_cast<int16_t>(sum / mic_channels);
  }

}

void App::ExtractChannelS16(const std::vector<uint8_t>& chunk, int channels, int channel,
                            std::vector<uint8_t>* out) {
  if (!out) {
    return;
  }
  if (channels <= 1 || channel < 0 || channel >= channels) {
    out->clear();
    return;
  }
  const size_t stride = static_cast<size_t>(channels);
  if (chunk.size() % (sizeof(int16_t) * stride) != 0) {
    out->clear();
    return;
  }
  const auto* in = reinterpret_cast<const int16_t*>(chunk.data());
  const size_t n_frames = chunk.size() / (sizeof(int16_t) * stride);

  out->resize(n_frames * sizeof(int16_t));
  auto* dst = reinterpret_cast<int16_t*>(out->data());
  for (size_t i = 0; i < n_frames; ++i) {
    dst[i] = in[i * stride + static_cast<size_t>(channel)];
  }
}

void App::OnInputAudio(const std::vector<uint8_t>& chunk) {
  if (kws_stats_ && cfg_.audio.bits_per_sample == 32 &&
      chunk.size() % (sizeof(int32_t) * cfg_.audio.channels) == 0) {
    kws_stats_->ObserveCaptureS32(reinterpret_cast<const int32_t*>(chunk.data()),
        chunk.size() / (sizeof(int32_t) * cfg_.audio.channels));
  }
  const auto conversion_start = std::chrono::steady_clock::now();
  if (cfg_.audio.bits_per_sample == 32) {
    ConvertS32ToS16(chunk, &s16_buf_, kws_s32_shift_);
  } else {
    s16_buf_ = chunk;
  }
  if (kws_stats_) {
    kws_stats_->RecordDuration(audio::KwsDiagnostics::Stage::kS32ToS16,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - conversion_start).count());
    if (s16_buf_.size() % (sizeof(int16_t) * cfg_.audio.channels) == 0) {
      kws_stats_->ObserveCaptureS16(reinterpret_cast<const int16_t*>(s16_buf_.data()),
          s16_buf_.size() / (sizeof(int16_t) * cfg_.audio.channels));
    }
  }
  // AEC uses one stable mic channel for better linear echo model fitting.
  if (cfg_.audio.channels >= 2) {
    ExtractChannelS16(s16_buf_, cfg_.audio.channels, 0, &capture_mono_buf_);
  } else {
    capture_mono_buf_.clear();
  }
  if (capture_mono_buf_.empty()) {
    DownmixToMono(s16_buf_, cfg_.audio.channels, &capture_mono_buf_);
  }
  const auto downmix_start = std::chrono::steady_clock::now();
  if (kws_input_channel_ >= 0) {
    ExtractChannelS16(s16_buf_, cfg_.audio.channels, kws_input_channel_, &kws_mono_buf_);
  } else {
    DownmixToMono(s16_buf_, cfg_.audio.channels, &kws_mono_buf_);
  }
  if (cfg_.wakeup.kws_agc) {
    kws_agc_.Process(reinterpret_cast<int16_t*>(kws_mono_buf_.data()),
                     kws_mono_buf_.size() / sizeof(int16_t));
  }
  if (kws_stats_) {
    kws_stats_->RecordDuration(audio::KwsDiagnostics::Stage::kDownmix,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - downmix_start).count());
    kws_stats_->ObserveKwsMonoS16(reinterpret_cast<const int16_t*>(kws_mono_buf_.data()),
                                  kws_mono_buf_.size() / sizeof(int16_t));
  }
  if (cfg_.audio.channels >= 4) {
    ExtractChannelS16(s16_buf_, cfg_.audio.channels, 3, &reverse_ref_buf_);
  } else {
    reverse_ref_buf_.clear();
  }
  EchoStats echo_stats;
  if (!reverse_ref_buf_.empty()) {
    echo_stats = AnalyzeEcho(capture_mono_buf_, reverse_ref_buf_);
  }
  if (!barge_capture_ && aec_ && !reverse_ref_buf_.empty()) {
    aec_->AnalyzeReverseStream(reverse_ref_buf_.data(), reverse_ref_buf_.size());
  }
  if(barge_capture_) {
    // Health/APM sees the producer stamp before any KWS work. Only a mono
    // copy enters the bounded queue; reader never touches the model mutex.
    ProcessOrdinaryBargeCapture(current_capture_stamp_);
    EnqueueKws(kws_mono_buf_,current_capture_stamp_);
  } else if(kws_listener_) {
    const auto kws_start=std::chrono::steady_clock::now();
    if(!rephrase_speaking_.load() && (!native_wake_monitor_ || !native_wake_monitor_->Blocked()))
      kws_listener_->AcceptPcm(kws_mono_buf_);
    if(kws_stats_)kws_stats_->RecordDuration(audio::KwsDiagnostics::Stage::kKws,
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-kws_start).count());
  }
  if(kws_stats_ && std::chrono::steady_clock::now()-last_kws_diag_log_>=std::chrono::seconds(5)) {
    LogKwsDiagnostics();last_kws_diag_log_=std::chrono::steady_clock::now();
  }
  if (!gate_ || gate_->step() != wakeup::Step::kActive) return;
  if (native_wake_monitor_ && native_wake_monitor_->Blocked()) return;
  if(barge_capture_ && (!capture_healthy_.load() || !legacy_effects_live_.load()))return;
  if (client_ && session_ready_.load()) {
    bool ai_speaking = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ai_speaking = is_ai_speaking_;
    }
    if (cfg_.conversation.duplex_mode == "half" && ai_speaking) return;

    const std::vector<uint8_t>* api_audio = barge_capture_ ? &barge_cleaned_buf_ : &capture_mono_buf_;
    std::vector<uint8_t> owned_audio;
    if (!barge_capture_ && aec_) {
      auto processed = aec_->ProcessCaptureStream(capture_mono_buf_.data(), capture_mono_buf_.size());
      if (!processed.empty()) {
        owned_audio = std::move(processed);
        api_audio = &owned_audio;
      }
    }

    // Keep full-duplex uplink while suppressing self-playback leakage.
    if (ai_speaking) {
      const float gain = ComputeUplinkGainWhileAiSpeaking(echo_stats);
      if (gain < 0.999f) {
        if (api_audio == &capture_mono_buf_) {
          owned_audio = capture_mono_buf_;
          api_audio = &owned_audio;
        }
        ScalePcmS16InPlace(&owned_audio, gain);
      }
    }

    RecordUplinkLevel(*api_audio);
    if (api_audio == &owned_audio) {
      client_->EnqueueAudio(std::move(owned_audio));
    } else {
      client_->EnqueueAudio(*api_audio);
    }
    CheckSmartTurn();
  }
}

void App::OnSmartTurn(const realtime::duplex::Event& event) {
  if(cfg_.realtime.protocol!="o2" || !cfg_.conversation.smart_turn)return;
  const auto now=std::chrono::steady_clock::now();
  const auto suffix=event.type.substr(event.type.rfind('.')+1);
  if(suffix=="started") {
    smart_turn_active_=true;smart_turn_sent_=false;smart_turn_text_.clear();
    smart_turn_words_.clear();smart_turn_changed_=now;smart_turn_quiet_ms_=0;
  } else if(smart_turn_active_ && suffix=="delta") {
    const auto words=ConversationWords(event.text);
    if(words!=smart_turn_words_) {smart_turn_words_=words;smart_turn_changed_=now;}
    smart_turn_text_=event.text;
  } else if(smart_turn_active_ && (suffix=="completed" || suffix=="ended_empty" || suffix=="failed")) {
    const auto quiet=smart_turn_sent_?smart_turn_quiet_ms_:
      std::chrono::duration_cast<std::chrono::milliseconds>(now-smart_turn_changed_).count();
    kLog->info("user turn end by={} quiet_ms={} words={} open={}",
      smart_turn_sent_?"smart":"server",quiet,smart_turn_words_.size(),LooksUnfinished(smart_turn_text_)?1:0);
    smart_turn_active_=false;
  }
}

void App::CheckSmartTurn() {
  std::unique_lock<std::recursive_mutex> fence(transport_effect_mu_, std::try_to_lock);
  if(!fence.owns_lock())return;
  if(cfg_.realtime.protocol!="o2" || !cfg_.conversation.smart_turn ||
     !legacy_effects_live_.load() || !smart_turn_active_ || smart_turn_sent_)return;
  const auto quiet=std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now()-smart_turn_changed_).count();
  if(ShouldCommitTurn(smart_turn_text_,quiet,cfg_.conversation.smart_turn_complete_ms,
      cfg_.conversation.smart_turn_open_ms) && client_->CommitUserTurn()) {
    smart_turn_sent_=true;smart_turn_quiet_ms_=quiet;
  }
}

void App::LogKwsDiagnostics() {
  if (!kws_stats_) return;
  const auto snapshot = kws_stats_->TakeSnapshotAndReset();
  for (size_t i = 0; i < snapshot.raw_capture_channels.size(); ++i) {
    const auto& raw = snapshot.raw_capture_channels[i];
    kLog->info("kws-diag raw ch={} samples={} peak_abs={} outside_s24={} low_byte_zero={} full_scale_s32={}",
               i, raw.samples, raw.peak_abs, raw.outside_signed_24,
               raw.low_byte_zero, raw.full_scale_32);
  }
  for (size_t i = 0; i < snapshot.capture_channels.size(); ++i) {
    const auto& level = snapshot.capture_channels[i];
    kLog->info("kws-diag capture ch={} samples={} rms={:.1f} peak={} clipped={}",
               i, level.samples, level.rms, level.peak, level.clipped);
  }
  const auto& mono = snapshot.kws_mono;
  kLog->info("kws-diag mono samples={} rms={:.1f} peak={} clipped={} overrun_lines={}",
             mono.samples, mono.rms, mono.peak, mono.clipped, snapshot.overrun_lines);
  const char* names[] = {"s32_to_s16", "downmix", "resample", "kws"};
  for (size_t i = 0; i < snapshot.durations.size(); ++i) {
    const auto& duration = snapshot.durations[i];
    kLog->info("kws-diag stage={} calls={} mean_ms={:.3f} p95_bucket_upper_ms={:.1f} max_ms={:.3f}",
               names[i], duration.calls, duration.mean_ms, duration.p95_upper_ms,
               duration.max_ms);
  }
}

void App::OnAudio(const std::vector<uint8_t>& chunk) {
  if (memory_switching_.load() || cue_requested_.load()!=audio::SessionCue::kNone) return;
  if (!player_ || !gate_ || gate_->step() != wakeup::Step::kActive ||
      (native_wake_monitor_ && native_wake_monitor_->Blocked())) {
    return;
  }
  if (gate_) gate_->NoteAiProgress();
  if (client_ && legacy_effects_live_.load()) client_->SetPlaybackActive(true);
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++pending_playback_chunks_;
  }

  const float gain = cfg_.audio.playback_gain;
  const bool need_gain = !(gain >= 0.999f && gain <= 1.001f);

  const uint64_t generation=player_->Generation();
  if(barge_capture_) {std::lock_guard<std::mutex> lock(barge_mu_);playback_owner_generation_=generation;}
  bool played;
  if (need_gain) {
    std::vector<uint8_t> amplified = chunk;
    ScalePcmS16InPlace(&amplified, gain);
    played = player_->Play(amplified,generation);
  } else {
    played = player_->Play(chunk,generation);
  }

  if (gate_->step() != wakeup::Step::kActive ||
      (native_wake_monitor_ && native_wake_monitor_->Blocked())) {
        InterruptPlayback();
  }

  if (!played) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (pending_playback_chunks_ > 0) {
        --pending_playback_chunks_;
      }
    }
    kLog->warn("player queue full, drop tts chunk: bytes={}", chunk.size());
    MaybeStartNextTurnWindow();
    TryFinalizeFarewell();
  }
}

void App::OnPlaybackStall() {
  if (stopping_.load()) return;
  {
    std::lock_guard<std::mutex> lock(mu_);
    is_ai_speaking_ = false;
    stream_audio_done_ = true;
    reply_pending_ = false;
    turn_complete_pending_ = false;
    pending_playback_chunks_ = 0;
    ResetFarewellStateLocked();
  }
  InterruptPlayback();
  if (client_) client_->RecoverStalledPlayback();
}

void App::FailWorker(const char* name) {
  if(cfg_.conversation.ordinary_barge_in) {
    {
      // Failure shares the qualification fence with Gate commit, final wake
      // enqueue and Service::Begin. A late hit cannot claim a new input after
      // the producer/worker has failed. No model, hook, I/O or join is here.
      std::lock_guard<std::mutex> fence(kws_dispatch_mu_);
      recorder_failed_.store(true);running_.store(false);
      kws_accepting_.store(false);capture_healthy_.store(false);
      ++kws_epoch_;kws_live_ticket_={};kws_suspended_=false;
    }
    {std::lock_guard<std::mutex> lock(kws_queue_mu_);kws_queue_.clear();}
    kws_queue_cv_.notify_all();
  } else {
    recorder_failed_.store(true);running_.store(false);
  }
  kLog->error("worker {} failed; requesting guardian restart", name);
  run_cv_.notify_all();
}

void App::OnPlaybackChunkProcessed(bool success) {
  bool playback_drained = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (pending_playback_chunks_ > 0) {
      --pending_playback_chunks_;
    }
    if (pending_playback_chunks_ == 0 && stream_audio_done_ && is_ai_speaking_) {
      is_ai_speaking_ = false;
      playback_drained = true;
    }
  }
  if (!success) {
    kLog->error("audio playback failed; ending cloud session");
    InterruptPlayback();
    if (gate_) gate_->Disarm("playback_failed");
    return;
  }
  if (playback_drained && gate_) gate_->SetAiSpeaking(false);
  if (playback_drained && client_) client_->SetPlaybackActive(false);
  MaybeStartNextTurnWindow();
  TryFinalizeFarewell();
  MaybeFinishExitIntent();
  control_cv_.notify_one();
}

void App::OnSetAiSpeaking(bool is_speaking) {
  bool gate_speaking = is_speaking;
  {
    std::lock_guard<std::mutex> lock(mu_);
    stream_audio_done_ = !is_speaking;
    if (is_speaking || pending_playback_chunks_ == 0) {
      is_ai_speaking_ = is_speaking;
    } else {
      // Audio frames are still queued locally. Keep the idle timeout paused
      // until the player has consumed them.
      gate_speaking = true;
    }
    if (is_speaking && farewell_pending_) {
      farewell_tts_started_ = true;
    }
  }
  if (gate_) {
    gate_->SetAiSpeaking(gate_speaking);
  }
  if (!gate_speaking && client_) client_->SetPlaybackActive(false);
  if (!is_speaking) {
    MaybeStartNextTurnWindow();
    TryFinalizeFarewell();
    MaybeFinishExitIntent();
    control_cv_.notify_one();
  }
}

void App::OnAsrFinal(const std::string& text) {
  UplinkLevelEvent(2);
  {std::lock_guard<std::mutex> lock(mu_);reply_pending_=true;}
  if(cfg_.memory.identity_text_log && identity_pending_.load() && !memory_switching_.load() &&
     gate_ && gate_->step()==wakeup::Step::kActive)
    kLog->info("identity input text={}",memory::IdentityLogText(text));
  if (gate_ && gate_->step() == wakeup::Step::kActive) {
    gate_->SetUserTurnOpen(false);
    gate_->RefreshTimeout();
    const int direction = audio::VolumeControl::Command(text);
    if (direction) {
      const bool applied = volume_.Adjust(direction);
      kLog->info("volume command direction={} applied={} shared_volume={}",
          direction, applied, audio::VolumeControl::Read());
    }
  }
  if (const char* command = gate_ && gate_->step()==wakeup::Step::kActive ? MatchedExitCommand(text) : nullptr) {
    kLog->info("asr_exit command={}",command);
    InterruptPlayback();
    if (memory_) memory_->Interrupt(memory_epoch_.load());
    if (client_) (void)client_->InterruptResponse();
    if (gate_) gate_->Disarm("user_exit");
    return;
  }
}

void App::OnUserActivity() {
  UplinkLevelEvent(1);
  if(barge_capture_) {if(gate_->step()==wakeup::Step::kActive)gate_->RefreshTimeout();return;}
  if (cfg_.conversation.duplex_mode == "half") {
    std::lock_guard<std::mutex> lock(mu_);
    if (is_ai_speaking_ || pending_playback_chunks_ > 0) return;
  }
  if (gate_ && gate_->step() == wakeup::Step::kActive) {
    gate_->RefreshTimeout();
    gate_->SetUserTurnOpen(true);
  }

  bool should_interrupt = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    turn_complete_pending_ = false;
    should_interrupt = is_ai_speaking_ || pending_playback_chunks_ > 0;
    if (should_interrupt) {
      is_ai_speaking_ = false;
      stream_audio_done_ = true;
      // User barge-in cancels pending auto-close on prior farewell turn.
      ResetFarewellStateLocked();
    }
  }
  if (!should_interrupt) {
    return;
  }

  if (memory_) memory_->Interrupt(memory_epoch_.load());
  kLog->info("barge-in detected: interrupt current playback");
  if (gate_) {
    gate_->SetAiSpeaking(false);
  }
  InterruptPlayback();
}

void App::OnSessionClosed(const std::string& reason) {
  if (memory_switching_.load()) return;
  if (stopping_.load()) {
    return;
  }
  kLog->info("session closed: {}", reason);
  {std::lock_guard<std::mutex> lock(mu_);reply_pending_=false;}
  control_cv_.notify_one();
  if(memory_ && gate_ && gate_->step()==wakeup::Step::kActive &&
      (!native_wake_monitor_ || !native_wake_monitor_->Blocked())) {
    InterruptPlayback();OnSetAiSpeaking(false);
    session_ready_.store(false);
    if(memory_->Resume(memory_epoch_.load()))return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    ResetFarewellStateLocked();
    turn_complete_pending_ = false;
  }
  if (gate_) {
    gate_->Disarm(reason);
  }
}

void App::OnChatEnded() {
  if (stopping_.load()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!farewell_pending_) {
      turn_complete_pending_ = true;
    } else {
      farewell_chat_ended_ = true;
    }
  }
  MaybeStartNextTurnWindow();
  TryFinalizeFarewell();
}

void App::OnServerExitIntent() {
  // The model's end intent is not the child's explicit exit command.
  kLog->info("server end intent ignored; waiting for user exit or idle timeout");
}

void App::OnNativeWake() {
  InvalidateKws();
  kLog->info("native XiaoAi wake detected; stopping local playback and session");
  session_ready_.store(false);
  InterruptPlayback();
  if (memory_) memory_->Interrupt(memory_epoch_.load());
  if (client_) (void)client_->InterruptResponse();
  if (gate_) gate_->Disarm("native_xiaoai_wake");
}

void App::MaybeFinishExitIntent() {
  bool close = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (exit_after_playback_ && !is_ai_speaking_ && pending_playback_chunks_ == 0) {
      exit_after_playback_ = false;
      close = true;
    }
  }
  if (close && gate_) gate_->Disarm("server_exit_intent");
}

void App::OnWakeInterrupt(const std::string& reason) {OnWakeInterrupt(reason,KwsTicket{});}
void App::OnWakeInterrupt(const std::string&,const KwsTicket& ticket) {
  if(ticket && !KwsTicketCurrent(ticket))return;
  if(!ticket)InvalidateKws();
  if (memory_) memory_->Interrupt(ticket ? ticket.identity : memory_epoch_.load());
  kLog->info("local wake interrupted playback");
  {
    std::lock_guard<std::mutex> lock(mu_);
    is_ai_speaking_ = false;
    stream_audio_done_ = true;
    reply_pending_ = false;
    turn_complete_pending_ = false;
    exit_after_playback_ = false;
    ResetFarewellStateLocked();
  }
  InterruptPlayback(audio::SessionCue::kWake);
  if(gate_)gate_->SetAiSpeaking(false);
  if (client_ && !client_->InterruptResponse()) kLog->warn("response.cancel failed after wake");
  if (memory_) {
    session_ready_.store(false);
    // A wake inside the conversation interrupts the answer, not the actor.
    (void)memory_->Resume(memory_epoch_.load());
  }
}

void App::MaybeStartNextTurnWindow() {
  bool ready = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (turn_complete_pending_ && !farewell_pending_ &&
        pending_playback_chunks_ == 0 && !is_ai_speaking_) {
      turn_complete_pending_ = false;
      ready = true;
    }
  }
  if (ready && gate_ && gate_->step() == wakeup::Step::kActive) {
    UplinkLevelEvent(0);
    gate_->RefreshTimeout();
    kLog->info("ready for next user turn; idle_window_seconds={}",
               cfg_.conversation.idle_seconds);
  }
}

void App::RecordUplinkLevel(const std::vector<uint8_t>& pcm) {
  std::lock_guard<std::mutex> lock(uplink_level_mu_);
  if(!uplink_measuring_)return;
  const size_t frame_samples=std::max(1,cfg_.audio.sample_rate/50);
  for(size_t i=0;i+1<pcm.size();i+=2) {
    const int16_t sample=static_cast<int16_t>(static_cast<uint16_t>(pcm[i]) |
                                           (static_cast<uint16_t>(pcm[i+1])<<8));
    uplink_square_sum_+=double(sample)*sample;
    uplink_peak_=std::max(uplink_peak_,std::abs(double(sample)));
    if(++uplink_samples_==frame_samples) {
      if(uplink_level_count_<uplink_levels_.size())
        uplink_levels_[uplink_level_count_++]=std::sqrt(uplink_square_sum_/frame_samples);
      uplink_samples_=0;uplink_square_sum_=0;
    }
  }
}
void App::UplinkLevelEvent(int event) {
  std::lock_guard<std::mutex> lock(uplink_level_mu_);
  std::sort(uplink_levels_.begin(),uplink_levels_.begin()+uplink_level_count_);
  const auto percentile=[&](size_t p) {
    return uplink_level_count_ ? uplink_levels_[(uplink_level_count_-1)*p/100] : 0.0;
  };
  if(event==1) {
    if(uplink_turn_)return;
    uplink_floor50_=percentile(50);uplink_floor90_=percentile(90);
  }else if(event==2 && uplink_turn_) {
    kLog->info("uplink level turn frames={} rms_p10={:.1f} rms_p50={:.1f} rms_p90={:.1f} peak={:.1f} floor_p50={:.1f} floor_p90={:.1f}",
      uplink_level_count_,percentile(10),percentile(50),percentile(90),uplink_peak_,uplink_floor50_,uplink_floor90_);
  }
  uplink_level_count_=0;uplink_samples_=0;uplink_square_sum_=0;uplink_peak_=0;
  uplink_turn_=event==1;uplink_measuring_=event!=2;
}

void App::OnArm(const std::string& reason) {OnArm(reason,KwsTicket{});}
void App::OnArm(const std::string& reason,const KwsTicket& ticket) {
  if(ticket && !KwsTicketCurrent(ticket))return;
  if(!ticket)InvalidateKws();
  exit_echo_guard_.Reset();
  kLog->info("gate armed: {}", reason);
  if (native_wake_monitor_ && native_wake_monitor_->Blocked()) {
    if (gate_) gate_->Disarm("native_xiaoai_active");
    return;
  }
  if (kws_only_) {
    if (gate_) gate_->Disarm("kws_only");
    return;
  }
  session_ready_.store(false);
  identity_pending_.store(cfg_.memory.enabled);
  InterruptPlayback(audio::SessionCue::kWake);

  {
    std::lock_guard<std::mutex> lock(mu_);
    ResetFarewellStateLocked();
    is_ai_speaking_ = false;
    stream_audio_done_ = false;
    turn_complete_pending_ = false;
    exit_after_playback_ = false;
  }

  if (!client_ || !gate_) {
    kLog->error("on_arm skipped: client/gate missing");
    return;
  }
  QueueSessionStart(ticket);
}

void App::QueueSessionClose() {
  if (kws_only_ || stopping_.load()) return;
  {
    std::lock_guard<std::mutex> lock(control_mu_);
    if (control_stop_) return;
    close_requested_ = true;
  }
  control_cv_.notify_one();
}

void App::QueueSessionStart() {QueueSessionStart(KwsTicket{});}
void App::QueueSessionStart(const KwsTicket& ticket) {
  if (kws_only_ || stopping_.load()) return;
  {
    std::lock_guard<std::mutex> lock(control_mu_);
    if (control_stop_) return;
    // Final enqueue is linearized with invalidation/commit. This fence does
    // not hold the KWS PCM queue or Listener mutex and performs no hook/I/O.
    std::lock_guard<std::mutex> fence(kws_dispatch_mu_);
    if(ticket && !KwsTicketCurrentLocked(ticket))return;
    start_ticket_=ticket;start_requested_ = true;
  }
  control_cv_.notify_one();
}

void App::ControlLoop() {
  while (true) {
    bool do_close = false;
    bool do_start = false;
    KwsTicket start_ticket;
    bool do_rephrase = false;
    std::string repeat_message;
    bool do_barge = false, do_capture_restart = false;
    std::optional<memory::Action> memory_action;
    {
      std::unique_lock<std::mutex> lock(control_mu_);
      control_cv_.wait(lock, [this] {
        bool memory_ready=!memory_actions_.empty();
        if(memory_ready && memory_actions_.front().after_reply) {
          std::lock_guard<std::mutex> playback(mu_);
          memory_ready=!reply_pending_ && !is_ai_speaking_ && pending_playback_chunks_==0;
        }
        return control_stop_ || close_requested_ || start_requested_ || rephrase_requested_ || barge_requested_ || capture_restart_requested_ || memory_ready || cue_requested_.load()!=audio::SessionCue::kNone;
      });
      if (control_stop_) return;
      do_close = close_requested_;
      do_start = start_requested_;start_ticket=start_ticket_;start_ticket_={};
      do_rephrase=rephrase_requested_;rephrase_requested_=false;
      repeat_message=rephrase_text_;rephrase_text_="请再说一次。";
      do_barge=barge_requested_;barge_requested_=false;
      do_capture_restart=capture_restart_requested_;capture_restart_requested_=false;
      close_requested_ = false;
      start_requested_ = false;
      if (!memory_actions_.empty() && !do_barge) {
        bool ready=true;
        if(memory_actions_.front().after_reply) {
          std::lock_guard<std::mutex> playback(mu_);
          ready=!reply_pending_ && !is_ai_speaking_ && pending_playback_chunks_==0;
        }
        if(ready){memory_action=std::move(memory_actions_.front());memory_actions_.pop_front();}
      }
    }
    PlaySessionCue();
    if(do_start && start_ticket && !KwsTicketCurrent(start_ticket))do_start=false;
    if(do_capture_restart && !stopping_.load())RestartBargeCapture();
    if(do_barge && !do_close && !do_start && !memory_action && !stopping_.load())HandleOrdinaryBarge();
    if (memory_ && do_start && !stopping_.load() &&
        (!start_ticket || KwsTicketCurrent(start_ticket))) {
      if(barge_capture_) {
        RetireBargeOwner();
        if(fresh_client_ && fresh_envelope_.valid()) {
          const auto old=fresh_envelope_;
          (void)fresh_client_->RetireFreshTransport(old,[this] {return player_ && player_->Interrupt(player_->Generation());});
          (void)memory_->RetireTransportResume(old);
        }
        client_->Stop();client_->Start();
      }
      bool begin_accepted=false;
      {
        std::lock_guard<std::mutex> fence(kws_dispatch_mu_);
        if(!start_ticket || KwsTicketCurrentLocked(start_ticket)) {
          memory_switching_.store(true);
          // Service::Begin is only a bounded command Push/notify: its worker
          // releases commands.mu before handling actions/network. This is the
          // identity-preparation commit, never a socket/TLS/model wait.
          begin_accepted=memory_->Begin(++memory_epoch_);
          kws_suspended_=false;kws_live_ticket_={};++kws_epoch_;
          if(!begin_accepted)memory_switching_.store(false);
        }
      }
      if(!begin_accepted) {do_start=false;continue;}
      do_start = false;
    }
    if(memory_action && barge_capture_ && !capture_healthy_.load() &&
        !stopping_.load() && gate_->step()==wakeup::Step::kActive) {
      std::unique_lock<std::mutex> lock(barge_mu_);
      barge_cv_.wait_for(lock,std::chrono::milliseconds(250),[this]{return capture_healthy_.load() || stopping_.load();});
      if(!capture_healthy_.load()) {
        std::lock_guard<std::mutex> control(control_mu_);
        memory_actions_.push_front(std::move(*memory_action));memory_action.reset();
      }
    }
    if (memory_action && !stopping_.load() && gate_->step()==wakeup::Step::kActive &&
        memory_action->epoch==memory_epoch_.load() && (!barge_capture_ || capture_healthy_.load()) &&
        (!native_wake_monitor_ || !native_wake_monitor_->Blocked())) {
      if(memory_action->exit) {
        InterruptPlayback();(void)client_->InterruptResponse();gate_->Disarm("user_exit");continue;
      }
      if (memory_action->restart) {
        if(memory_action->after_reply && player_) {
          const bool drained=player_->Drain(player_->Generation());
          std::lock_guard<std::mutex> lock(control_mu_);
          if(!drained || barge_requested_) {
            memory_actions_.push_front(std::move(*memory_action));
            control_cv_.notify_one();continue;
          }
        }
        session_ready_.store(false);memory_switching_.store(true);
        InvalidateKws();
        if(barge_capture_) {
          RetireBargeOwner();
          realtime::TransportEnvelope old;
          {std::lock_guard<std::mutex> lock(barge_mu_);old=fresh_envelope_;}
          if(old.valid()) { (void)fresh_client_->RetireFreshTransport(old,[this] {return player_ && player_->Interrupt(player_->Generation());});
            (void)memory_->RetireTransportResume(old); }
          client_->Stop();client_->Start();
        }
        InterruptPlayback();
        (void)client_->InterruptResponse();
        const auto switch_begin = std::chrono::steady_clock::now();
        identity_pending_.store(true);
        gate_->SetUserTurnOpen(false);
        legacy_effects_live_.store(false);
        (void)client_->FinishSession(std::chrono::milliseconds(0));
        exit_echo_guard_.Reset();
        const realtime::IdentitySessionOrigin identity_origin(memory_action->epoch,memory_action->identity_revision,++identity_session_sequence_);
        if (!client_->StartSession(std::chrono::seconds(12),memory_action->instructions,barge_capture_ ? identity_origin : realtime::IdentitySessionOrigin{})) {
          if(gate_->step()==wakeup::Step::kActive && !stopping_.load()) {
            if(++memory_action->start_failures>=2) {
              memory_switching_.store(false);
              {std::lock_guard<std::mutex> lock(control_mu_);rephrase_requested_=true;rephrase_text_="豆包连不上了，请告诉爸爸妈妈。";}
              kLog->warn("session unavailable after {} attempts",memory_action->start_failures);
              gate_->Disarm("session_unavailable");control_cv_.notify_one();continue;
            }
            std::unique_lock<std::mutex> lock(control_mu_);
            control_cv_.wait_for(lock,std::chrono::seconds(1),[&]{return control_stop_ || close_requested_ || start_requested_;});
            memory_actions_.push_back(std::move(*memory_action));control_cv_.notify_one();continue;
          }
          memory_switching_.store(false);
          continue; // The user/native/idle gate already ended this conversation.
        }
        if (gate_->step()!=wakeup::Step::kActive || stopping_.load() ||
            (native_wake_monitor_ && native_wake_monitor_->Blocked())) {
          memory_switching_.store(false);(void)client_->FinishSession();continue;
        }
        if(barge_capture_) {
          auto reply=std::make_shared<std::promise<memory::ResumeResult>>();auto future=reply->get_future();
          const bool queued=memory_->ActivateIdentitySession(identity_origin,[reply](auto r){reply->set_value(std::move(r));});
          if(!queued || future.wait_for(std::chrono::seconds(1))!=std::future_status::ready || !future.get().approved) {
            memory_switching_.store(false);(void)client_->FinishSession(std::chrono::milliseconds(0));
            {std::lock_guard<std::mutex> lock(control_mu_);rephrase_requested_=true;rephrase_text_="请再叫豆包，说一次你的名字。";}
            gate_->Disarm("identity_origin_activation_failed");control_cv_.notify_one();continue;
          }
        }else memory_->Activate(memory_action->epoch);
        identity_pending_.store(memory_action->identity_pending);
        kLog->info("identity session_switch_ms={}", std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now()-switch_begin).count());
        {std::lock_guard<std::recursive_mutex> fence(transport_effect_mu_);
          legacy_effects_live_.store(true);
          std::lock_guard<std::mutex> lock(barge_mu_);fresh_mode_=false;fresh_granted_=false;fresh_done_=false;
          playback_owner_generation_=player_ ? player_->Generation() : 1;
          fresh_envelope_={};fresh_session_id_.clear();capture_context_={};}
        memory_switching_.store(false);
        session_ready_.store(true);
        PauseMphelperBounded();
      }
      if(!memory_action->greeting.empty()) {
        if (!client_->SendSayHello(memory_action->greeting)) gate_->Disarm("memory_greeting_failed");
        gate_->RefreshTimeout();
      }else kLog->info("conversation resumed silently; identity and context retained");
    }
    if (do_close && !stopping_.load() && client_) {
      realtime::TransportEnvelope old;
      {std::lock_guard<std::mutex> lock(barge_mu_);old=fresh_envelope_;}
      if(fresh_client_ && old.valid()) {
        (void)fresh_client_->RetireFreshTransport(old,[this] {return player_ && player_->Interrupt(player_->Generation());});
        if(memory_)(void)memory_->RetireTransportResume(old);
      }
      (void)client_->FinishSession(std::chrono::seconds(4));
    }
    if(do_rephrase && !stopping_.load() && (!native_wake_monitor_ || !native_wake_monitor_->Blocked())) {
      InvalidateKws();
      rephrase_speaking_.store(true);
      if(SayRepeatBounded(repeat_message))rephrase_speaking_.store(false);
      else FailWorker("repeat_feedback_stop");
    }
    if (do_start && !stopping_.load() &&
        (!native_wake_monitor_ || !native_wake_monitor_->Blocked()) && client_ && gate_ &&
        gate_->step() == wakeup::Step::kActive) {
      if(start_ticket && !KwsTicketCurrent(start_ticket))continue;
      if (!client_->StartSession(std::chrono::seconds(12))) {
        kLog->error("start session failed");
        gate_->Disarm("session_start_failed");
        continue;
      }
      if (gate_->step() != wakeup::Step::kActive || stopping_.load()) {
        (void)client_->FinishSession(std::chrono::seconds(2));
        continue;
      }
      // Only a newly established Doubao session takes over native media.
      // Playback interruption and session teardown must leave it alone.
      if (!native_wake_monitor_ || !native_wake_monitor_->Blocked()) {
        PauseMphelperBounded();
      }
      if (!client_->SendSayHello()) {
        kLog->error("send say_hello failed");
        gate_->Disarm("say_hello_failed");
        continue;
      }
      session_ready_.store(true);
      {std::lock_guard<std::mutex> lock(kws_dispatch_mu_);kws_suspended_=false;kws_live_ticket_={};++kws_epoch_;}
      gate_->RefreshTimeout();

    }
  }
}

void App::PlaySessionCue() {
  auto cue=cue_requested_.load();
  if(cue==audio::SessionCue::kNone)return;
  if(player_ && !stopping_.load() && (!native_wake_monitor_ || !native_wake_monitor_->Blocked())) {
    player_->Interrupt();const auto generation=player_->Generation();
    const bool queued=player_->Play(audio::MakeSessionCue(cue,cfg_.audio.playback_gain),generation,false);
    const bool drained=queued && player_->Drain(generation,std::chrono::milliseconds(300));
    if(queued && !drained)(void)player_->Interrupt(generation);
    kLog->info("local session cue={} drained={}",cue==audio::SessionCue::kWake?"wake":"exit",drained);
  }
  cue_requested_.compare_exchange_strong(cue,audio::SessionCue::kNone);
}

void App::PlaybackControlLoop() {
  while (true) {
    uint64_t expected=0;
    {
      std::unique_lock<std::mutex> lock(playback_mu_);
      playback_cv_.wait(lock, [this] { return playback_stop_ || interrupt_requested_; });
      if (playback_stop_) return;
      expected=interrupt_generation_;interrupt_requested_ = false;
    }
    if (player_) (void)player_->Interrupt(expected);
  }
}

void App::TryFinalizeFarewell() {
  bool should_close = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (farewell_pending_ && farewell_chat_ended_ && farewell_tts_started_ &&
        !is_ai_speaking_ &&
        pending_playback_chunks_ == 0) {
      should_close = true;
      ResetFarewellStateLocked();
    }
  }

  if (!should_close) {
    return;
  }

  kLog->info("farewell close: playback drained, closing session");
  if (gate_) gate_->Disarm("bye");
}

void App::ResetFarewellStateLocked() {
  farewell_pending_ = false;
  farewell_chat_ended_ = false;
  farewell_tts_started_ = false;
}

void App::BeginFarewellStateLocked() {
  farewell_pending_ = true;
  farewell_chat_ended_ = false;
  farewell_tts_started_ = false;
}

void App::InterruptPlayback(audio::SessionCue cue) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    pending_playback_chunks_ = 0;
  }
  if (!kws_only_ && !stopping_.load()) {
    {
      std::lock_guard<std::mutex> lock(playback_mu_);
      if (!playback_stop_) {interrupt_generation_=player_ ? player_->Generation() : 0;interrupt_requested_=true;
        if(cue!=audio::SessionCue::kNone)cue_requested_.store(cue);}
    }
    playback_cv_.notify_one();
    if(cue!=audio::SessionCue::kNone)control_cv_.notify_one();
  }
}

}  // namespace xiaoai_plus::app
