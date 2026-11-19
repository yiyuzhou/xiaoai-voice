#include "realtime/client.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

#include "common/log.hpp"
#include "memory/core.hpp"

namespace xiaoai_plus::realtime {
namespace {
const auto kLog = xiaoai_plus::GetLogger("realtime");
using Clock = std::chrono::steady_clock;
int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now().time_since_epoch()).count();
}
thread_local Client* fresh_callback_client=nullptr;
struct FreshCallbackGuard {
  Client* previous;
  explicit FreshCallbackGuard(Client* c):previous(fresh_callback_client){if(c)fresh_callback_client=c;}
  ~FreshCallbackGuard(){fresh_callback_client=previous;}
};
bool SameCaptureOrigin(const audio::shadow::Context& a,const audio::shadow::Context& b) {
  return a.epoch==b.epoch && a.session==b.session && a.response==b.response &&
    a.playback==b.playback && a.ingress==b.ingress && a.capture==b.capture;
}
bool ValidCaptureOrigin(const audio::shadow::Context& a) {
  return a.epoch && a.session && a.response && a.playback && a.ingress && a.capture;
}
const char* PhaseName(duplex::State state) {
  switch (state) {
    case duplex::State::kIdle: return "idle";
    case duplex::State::kAwaitingCreated: return "awaiting_created";
    case duplex::State::kActive: return "active";
    case duplex::State::kClosing: return "closing";
    case duplex::State::kClosed: return "closed";
    case duplex::State::kFailed: return "failed";
  }
  return "unknown";
}
std::string SafeField(const std::string& value, bool message = false) {
  if (value.empty() || value.size() > (message ? 100 : 64)) return "unavailable";
  for (unsigned char c : value)
    if (message ? (c < 32 || c == 127) : !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
      return "unavailable";
  return value;
}
}  // namespace

Client::Client(config::Config cfg, Callbacks callbacks, bool trial_once,
               bool segmented_input, bool scoped_barge_control, bool fresh_transport_control)
    : cfg_(std::move(cfg)), callbacks_(std::move(callbacks)),
      trial_once_(trial_once), segmented_input_(segmented_input),
      scoped_barge_control_(scoped_barge_control), fresh_transport_control_(fresh_transport_control),
      fresh_owner_(fresh_transport_control), barge_(scoped_barge_control),
      usage_(cfg_.conversation.usage_dir) {}

Client::~Client() { Stop(); }

bool Client::Start() {
  if (running_.exchange(true)) return true;
  sender_thread_ = std::thread([this] {
    try { SendLoop(); }
    catch (const std::exception& e) { kLog->error("sender thread: {}", e.what()); if (callbacks_.on_fatal) callbacks_.on_fatal(); }
    catch (...) { kLog->error("sender thread unknown exception"); if (callbacks_.on_fatal) callbacks_.on_fatal(); }
  });
  return true;
}

void Client::Stop() {
  if (running_.load()) (void)FinishSession(std::chrono::seconds(2));
  running_.store(false);
  audio_cv_.notify_all();
  state_cv_.notify_all();
  CloseSocket();
  if (sender_thread_.joinable()) sender_thread_.join();
  JoinCleanup();
  if (raw_dump_fd_ >= 0) { close(raw_dump_fd_); raw_dump_fd_ = -1; }
}

bool Client::StartSession(std::chrono::milliseconds timeout, const std::string& instructions_override,
    const IdentitySessionOrigin& identity_origin) {
  if (!running_.load()) return false;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    if (scoped_barge_control_ && (segmented_input_ || barge_epoch_ == 0 || capture_generation_ == 0)) return false;
    if(fresh_transport_control_ && (scoped_barge_control_ || segmented_input_ || fresh_retired_ ||
                                  !fresh_owner_.Current(fresh_envelope_)))return false;
    if (session_.state() == duplex::State::kActive)
      return !identity_origin.valid() || identity_origin==identity_origin_;
    if(fresh_transport_control_){if(fresh_connection_attempted_)return false;fresh_connection_attempted_=true;}
  }
  if (usage_unknown_.load()) {
    kLog->warn("daily usage unavailable; refusing new session");
    return false;
  }
  try {
    const auto tokens = usage_.ReadToday();
    if (cfg_.conversation.daily_token_limit != 0 &&
        tokens >= cfg_.conversation.daily_token_limit) {
      kLog->warn("daily token limit reached: used={} limit={}", tokens,
                 cfg_.conversation.daily_token_limit);
      return false;
    }
  } catch (...) {
    kLog->error("daily usage read failed; refusing new session");
    return false;
  }
  if (trial_once_ && connection_attempted_.exchange(true)) {
    kLog->warn("trial connection budget exhausted; no new connection");
    return false;
  }
  // One connection attempt only. A rejected/chargeable request is never retried automatically.
  CloseSocket();
  JoinCleanup();
  {
    std::lock_guard<std::mutex> lock(cleanup_mu_);
    cleanup_requested_ = false;
  }
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    session_ = duplex::Session{};
    {std::lock_guard<std::mutex> wire_lock(o2_mu_);o2_session_.Reset();}
    o2_connection_ready_=false;
    identity_origin_=identity_origin;
    if (scoped_barge_control_) barge_.BeginSession(barge_epoch_, capture_generation_);
    ai_speaking_ = false;
    close_notified_ = false;
    user_turn_open_=false;user_turn_committed_=false;
    output_format_.Reset();
    format_probe_blocks_ = 0;
    first_output_delta_logged_ = false;
    if (raw_dump_fd_ >= 0) { close(raw_dump_fd_); raw_dump_fd_ = -1; }
    raw_dump_bytes_ = 0;
  }
  {
    std::lock_guard<std::mutex> lock(audio_mu_);
    audio_queue_.clear();
    audio_queue_bytes_ = 0;
  }
  playback_hold_until_ns_.store(0);
  const auto deadline = Clock::now() + timeout;
  if (!OpenConnection(timeout)) {
    kLog->error("duplex WebSocket connection failed");
    CloseSocket();
    return false;
  }

  if(cfg_.realtime.protocol=="o2") {
    if(!SendText(R"({"type":"connection.start"})"))return false;
    std::unique_lock<std::mutex> lock(state_mu_);
    if(!state_cv_.wait_until(lock,deadline,[this]{return o2_connection_ready_ || session_.state()==duplex::State::kFailed;}) ||
       !o2_connection_ready_) {lock.unlock();CloseSocket();return false;}
  }
  bool sent = false;
  try {
    std::lock_guard<std::mutex> write_lock(write_mu_);
    std::string create;
    {
      std::lock_guard<std::mutex> state_lock(state_mu_);
      std::string instructions = instructions_override;
      if (instructions.empty() && cfg_.realtime.general_assistant) {
        instructions = cfg_.realtime.preset.system_role + " " + cfg_.realtime.preset.speaking_style +
            " 每次新会话默认用普通话交流。用户要求换语言时，立即用指定语言回答，"
            "并在本次会话中继续使用该语言，直到用户再次要求切换。"
            "用户没有明确指定语言时，可以用与用户相同的语言回应。"
            "用户要求调音量时，用当前语言简短确认，音量由音箱处理。";
      } else if (instructions.empty() && !cfg_.memory.enabled && cfg_.realtime.protocol == "o2") {
        instructions = memory::PlaymateInstructions(cfg_.memory.child_names);
      } else if (instructions.empty()) {
        instructions = cfg_.realtime.preset.system_role + " " + cfg_.realtime.preset.speaking_style +
            " Volume requests: say only '好的'; handled locally, never say you cannot. "
            "Usually 2 short sentences, never more than 3; a fun sound like \"Roar!\" doesn't count. At most one question, only at the end. "
            "During children's English practice, be natural and encouraging. "
            "Preserve the required identity question and continuation opening.";
      }
      create = session_.Create(instructions, cfg_.realtime.preset.voice, cfg_.memory.enabled);
    }
    sent = SendTextLocked(create);
  } catch (...) {
    sent = false;
  }
  if (!sent) {
    kLog->error("session.create send failed");
    CloseSocket();
    return false;
  }
  {
    std::unique_lock<std::mutex> lock(state_mu_);
    const bool done = state_cv_.wait_until(lock, deadline, [this] {
      return session_.state() == duplex::State::kActive ||
             session_.state() == duplex::State::kFailed || !running_.load();
    });
    if (done && session_.state() == duplex::State::kActive) {
      kLog->info("duplex session.created received");
      return true;
    }
    session_.Fail();
  }
  kLog->error("session.create rejected or timed out");
  CloseSocket();
  return false;
}

bool Client::FinishSession(std::chrono::milliseconds timeout) {
  // Generic shutdown has no Player stop proof and cannot authorize a successor.
  if(fresh_transport_control_)return RetireFreshTransport(fresh_envelope_,[]{return false;});
  {
    std::lock_guard<std::mutex> lock(audio_mu_);
    audio_queue_.clear();
    audio_queue_bytes_ = 0;
  }
  if (scoped_barge_control_) {
    std::lock_guard<std::recursive_mutex> fence(callback_fence_);
    std::lock_guard<std::mutex> state_lock(state_mu_);
    barge_.Invalidate();
  }
  bool should_close = false;
  bool sent = false;
  {
    std::lock_guard<std::mutex> write_lock(write_mu_);
    std::string close;
    {
      std::lock_guard<std::mutex> state_lock(state_mu_);
      if (session_.state() == duplex::State::kActive) {
        close = session_.Close();
        should_close = true;
      }
    }
    if (should_close) sent = SendTextLocked(close);
  }
  if (should_close && sent && timeout.count()>0) {
    std::unique_lock<std::mutex> lock(state_mu_);
    state_cv_.wait_for(lock, timeout, [this] {
      return session_.state() == duplex::State::kClosed ||
             session_.state() == duplex::State::kFailed || !running_.load();
    });
  }
  bool graceful = true;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    graceful = timeout.count()==0 || (should_close ? session_.state() == duplex::State::kClosed
                            : session_.state() != duplex::State::kFailed);
    if (!graceful || timeout.count()==0) session_.Fail();
  }
  if (should_close && !graceful) kLog->warn("session.closed not received before timeout");
  if (should_close) NotifyClosed(timeout.count()==0 ? "session_replaced" :
                                graceful ? "session_finished" : "session_close_timeout");
  if(cfg_.realtime.protocol=="o2") {try{(void)SendText(R"({"type":"connection.finish"})");}catch(...){}}
  playback_hold_until_ns_.store(0);
  CloseSocket();
  {
    std::lock_guard<std::mutex> lock(audio_mu_);
    audio_queue_.clear();
    audio_queue_bytes_ = 0;
  }
  return graceful;
}

bool Client::EnqueueAudio(const std::vector<uint8_t>& chunk) {
  return EnqueueAudio(std::vector<uint8_t>(chunk));
}

bool Client::EnqueueAudio(std::vector<uint8_t>&& chunk) {
  if(fresh_transport_control_)return false;
  return EnqueueAudioBound(scoped_barge_control_ ? BargeToken() : BargeInController::Token{},
                           std::move(chunk));
}

BargeInController::Token Client::BargeToken() {
  std::lock_guard<std::mutex> lock(state_mu_);
  return barge_.token();
}

void Client::SetBargeContext(uint64_t epoch, uint64_t capture) {
  if (!scoped_barge_control_) return;
  std::lock_guard<std::recursive_mutex> fence(callback_fence_);
  std::lock_guard<std::mutex> lock(state_mu_);
  barge_epoch_ = epoch;
  capture_generation_ = capture;
  barge_.Invalidate();
}

bool Client::AuthorizeBargeResponse(const BargeInController::Token& token,
                                    const std::string& response_id) {
  if (!scoped_barge_control_) return false;
  std::lock_guard<std::recursive_mutex> fence(callback_fence_);
  std::lock_guard<std::mutex> lock(state_mu_);
  return session_.state() == duplex::State::kActive &&
         barge_.AuthorizeNextResponse(token, response_id);
}

bool Client::EnqueueAudioBound(const BargeInController::Token& token, std::vector<uint8_t> chunk) {
  if(fresh_transport_control_)return false;
  if (scoped_barge_control_) {
    std::lock_guard<std::mutex> lock(state_mu_);
    if (session_.state() != duplex::State::kActive || !barge_.AcceptInput(token)) return false;
  }
  if (chunk.empty()) return true;
  if (SuppressInput()) return true;
  if (chunk.size() % 2 != 0 || chunk.size() > 16000 * 2) return false;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    if (session_.state() != duplex::State::kActive) return false;
    if (scoped_barge_control_ && !barge_.AcceptInput(token)) return false;
  }
  std::lock_guard<std::mutex> lock(audio_mu_);
  const size_t max_items = segmented_input_ ? 512 :
      static_cast<size_t>(cfg_.budget.input_queue_frames);
  if (audio_queue_.size() >= max_items ||
      (segmented_input_ && audio_queue_bytes_ + chunk.size() > 512 * 1024)) {
    kLog->warn("uplink queue full; audio chunk rejected");
    return false;
  }
  audio_queue_bytes_ += chunk.size();
  audio_queue_.push_back(AudioItem{std::move(chunk), false, token});
  audio_cv_.notify_one();
  return true;
}

bool Client::CommitUserTurn() {
  std::lock_guard<std::mutex> state(state_mu_);
  if(cfg_.realtime.protocol!="o2" || session_.state()!=duplex::State::kActive ||
     !user_turn_open_ || user_turn_committed_)return false;
  std::lock_guard<std::mutex> queue(audio_mu_);
  AudioItem item;item.smart_commit=true;audio_queue_.push_back(std::move(item));
  user_turn_committed_=true;audio_cv_.notify_one();return true;
}

bool Client::EndInputTurn() {
  if (!segmented_input_) return false;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    if (session_.state() != duplex::State::kActive) return false;
  }
  std::lock_guard<std::mutex> lock(audio_mu_);
  if (audio_queue_.size() >= 512) return false;
  audio_queue_.push_back(AudioItem{{}, true, {}});
  audio_cv_.notify_one();
  return true;
}

bool Client::SendSayHello(const std::string& override_text) {
  if(fresh_transport_control_)return false; // No greeting or synthetic input in a resume lease.
  try {
    std::lock_guard<std::mutex> write_lock(write_mu_);
    std::string greeting;
    {
      std::lock_guard<std::mutex> state_lock(state_mu_);
      greeting = session_.Greeting(override_text.empty() ? cfg_.wakeup.say_hello : override_text);
    }
    return SendTextLocked(greeting);
  } catch (...) {
    return false;
  }
}

bool Client::SuppressInput() const {
  return cfg_.conversation.duplex_mode == "half" &&
         NowNs() < playback_hold_until_ns_.load();
}

bool Client::SetPlaybackActiveBound(const BargeInController::Token& token, bool active,
                                    bool interrupted) {
  if (!scoped_barge_control_) return false;
  std::lock_guard<std::recursive_mutex> fence(callback_fence_);
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    if (session_.state() != duplex::State::kActive || !barge_.AcceptInput(token)) return false;
    if (!active) barge_.PlaybackDrained(token);
  }
  if (cfg_.conversation.duplex_mode != "half") return true;
  playback_hold_until_ns_.store(active ? std::numeric_limits<int64_t>::max() :
      (interrupted ? 0 : NowNs() + 300000000));
  if (active) {
    std::lock_guard<std::mutex> lock(audio_mu_);
    audio_queue_.clear();
    audio_queue_bytes_ = 0;
  }
  audio_cv_.notify_all();
  return true;
}

void Client::SetPlaybackActive(bool active, bool interrupted) {
  if (scoped_barge_control_ || fresh_transport_control_) return;
  if (cfg_.conversation.duplex_mode != "half") return;
  playback_hold_until_ns_.store(active ? std::numeric_limits<int64_t>::max() :
      (interrupted ? 0 : NowNs() + 300000000));
  if (active) {
    std::lock_guard<std::mutex> lock(audio_mu_);
    audio_queue_.clear();
    audio_queue_bytes_ = 0;
  }
  audio_cv_.notify_all();
}

void Client::RecoverStalledPlayback() {
  if (scoped_barge_control_ || fresh_transport_control_) return;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    ai_speaking_ = false;
  }
  SetPlaybackActive(false, true);
}

bool Client::InterruptResponse() {
  if (scoped_barge_control_ || fresh_transport_control_) return false;
  SetPlaybackActive(false, true);
  try {
    std::lock_guard<std::mutex> write_lock(write_mu_);
    std::string cancel;
    {
      std::lock_guard<std::mutex> state_lock(state_mu_);
      if (session_.state() != duplex::State::kActive) return false;
      cancel = session_.CancelResponse();
      if(cfg_.realtime.protocol=="o2")ai_speaking_=false;
    }
    return SendTextLocked(cancel);
  } catch (...) { return false; }
}

bool Client::ConfirmBargeIn(const BargeInController::Token& token,
                            const BargeInController::Eligibility& eligibility,
                            const std::function<bool()>& stop_playback) {
  if (!scoped_barge_control_ || segmented_input_ || !stop_playback) return false;
  BargeInController::Token claimed;
  {
    std::lock_guard<std::recursive_mutex> fence(callback_fence_);
    std::lock_guard<std::mutex> write_lock(write_mu_);
    {
      std::lock_guard<std::mutex> state_lock(state_mu_);
      if (session_.state() != duplex::State::kActive ||
          !barge_.ConfirmSpeech(token, eligibility)) return false;
      claimed = barge_.token();
      ai_speaking_ = false;
      output_audio_chunks_ = output_audio_bytes_ = 0;
    }
    std::lock_guard<std::mutex> audio_lock(audio_mu_);
    audio_queue_.clear();
    audio_queue_bytes_ = 0;
  }
  // Pending state blocks old output, new ingress and any next-response grant.
  // Do not hold Client locks while waiting for Player: a completed Player
  // callback may itself query the Client. The callback must stop a scoped
  // playback generation, not whichever generation happens to be current.
  bool stopped = false;
  try { stopped = stop_playback(); } catch (...) { stopped = false; }
  bool sent = false;
  {
    std::lock_guard<std::recursive_mutex> fence(callback_fence_);
    std::lock_guard<std::mutex> write_lock(write_mu_);
    std::string cancel, unmute;
    {
      std::lock_guard<std::mutex> state_lock(state_mu_);
      // An identity/session/capture replacement while stopping is stale. It
      // must not send a global cancel or release the newer session's hold.
      if (session_.state() != duplex::State::kActive ||
          !barge_.CancellationPending(claimed)) return false;
      cancel = session_.CancelResponse();
      unmute = session_.UnmuteInput();
    }
    if (stopped) sent = SendTextLocked(cancel) && SendTextLocked(unmute);
    {
      std::lock_guard<std::mutex> state_lock(state_mu_);
      if (sent) (void)barge_.CompleteCancellation(claimed);
      else barge_.Invalidate();
    }
    if (sent) playback_hold_until_ns_.store(0);
    else HandleConnectionLost("barge_control_failed", true);
  }
  audio_cv_.notify_all();
  return sent;
}

bool Client::OpenConnection(std::chrono::milliseconds timeout) {
  const uint64_t socket_generation = ++socket_generation_;
  const TransportEnvelope envelope=fresh_envelope_; // Immutable registration-time source.
  const IdentitySessionOrigin identity_origin=identity_origin_; // Immutable WS-registration binding.
  auto ws = std::make_unique<ix::WebSocket>();
  ws->setUrl(cfg_.realtime.ws_url());
  ws->disableAutomaticReconnection();
  ix::WebSocketHttpHeaders headers;
  for (const auto& [name, value] : cfg_.realtime.request_headers()) headers[name] = value;
  ws->setExtraHeaders(headers);
  ix::SocketTLSOptions tls;
  tls.caFile = cfg_.realtime.preset.ca_file;
  ws->setTLSOptions(tls);
  ws->setOnMessageCallback([this, socket_generation, envelope, identity_origin](const ix::WebSocketMessagePtr& msg) {
    FreshCallbackGuard callback_guard(fresh_transport_control_?this:nullptr);
    std::unique_lock<std::recursive_mutex> fence(callback_fence_, std::defer_lock);
    if (scoped_barge_control_ || fresh_transport_control_) {
      fence.lock();
      if (socket_generation != socket_generation_.load()) {
        if(fresh_transport_control_ && msg->type==ix::WebSocketMessageType::Message && !msg->binary)AccountRetiredUsage(envelope,msg->str);
        return;
      }
      if(fresh_transport_control_){
        bool current;
        {std::lock_guard<std::mutex> lock(state_mu_);current=fresh_owner_.Current(envelope);}
        if(!current){if(msg->type==ix::WebSocketMessageType::Message && !msg->binary)AccountRetiredUsage(envelope,msg->str);return;}
      }
    }
    if (msg->type == ix::WebSocketMessageType::Open) {
      const auto logid = msg->openInfo.headers.find("X-Tt-Logid");
      kLog->info("handshake event=open phase=connecting http_status=101 X-Tt-Logid={}",
                 logid == msg->openInfo.headers.end() ? "unavailable" : SafeField(logid->second));
      {
        std::lock_guard<std::mutex> lock(conn_mu_);
        connected_ = true;
      }
      conn_cv_.notify_all();
      return;
    }
    if (msg->type == ix::WebSocketMessageType::Close ||
        msg->type == ix::WebSocketMessageType::Error) {
      // Never log server reason text: it may echo request headers.
      const char* phase;
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        phase = PhaseName(session_.state());
      }
      if (msg->type == ix::WebSocketMessageType::Close && std::string_view(phase) == "closed")
        kLog->info("handshake/socket event=close phase=closed http_status=0 X-Tt-Logid=unavailable");
      else
        kLog->warn("handshake/socket event={} phase={} http_status={} X-Tt-Logid=unavailable",
                   msg->type == ix::WebSocketMessageType::Error ? "error" : "close",
                   phase, msg->type == ix::WebSocketMessageType::Error ? msg->errorInfo.http_status : 0);
      HandleConnectionLost(msg->type == ix::WebSocketMessageType::Error ?
                           "websocket_error" : "connection_lost");
      return;
    }
    if (msg->type == ix::WebSocketMessageType::Message) {
      if (msg->binary && cfg_.realtime.protocol=="o2") {
        try {
          std::vector<std::string> events;
          {
            std::lock_guard<std::mutex> lock(state_mu_);
            std::lock_guard<std::mutex> wire_lock(o2_mu_);
            events=o2_session_.Receive(msg->str);o2_connection_ready_=o2_session_.connected();
          }
          state_cv_.notify_all();
          for(const auto& event:events)HandleText(event,identity_origin);
        } catch(...) {HandleConnectionLost("protocol_error");}
      } else if (msg->binary || cfg_.realtime.protocol=="o2") {
        HandleConnectionLost("protocol_error");
      } else {
        if(fresh_transport_control_)HandleTextBound(envelope,msg->str);
        else HandleText(msg->str,identity_origin);
      }
    }
  });
  {
    std::lock_guard<std::mutex> lock(conn_mu_);
    connected_ = false;
    connection_failed_ = false;
    ws_ = std::move(ws);
    ws_->start();
  }
  std::unique_lock<std::mutex> lock(conn_mu_);
  const bool signaled = conn_cv_.wait_for(lock, std::min(timeout, std::chrono::milliseconds(8000)),
                                          [this] { return connected_ || connection_failed_; });
  return signaled && connected_;
}

void Client::CloseSocket() {
  if (scoped_barge_control_ || fresh_transport_control_) ++socket_generation_;
  std::unique_ptr<ix::WebSocket> ws;
  {
    std::lock_guard<std::mutex> lock(conn_mu_);
    connected_ = false;
    connection_failed_ = true;
    ws = std::move(ws_);
  }
  conn_cv_.notify_all();
  if (ws) {
    try { ws->stop(); }
    catch (const std::exception&) { kLog->warn("WebSocket stop failed"); }
  }
}

void Client::HandleConnectionLost(const char* reason, bool only_if_active) {
  bool had_session = false;
  {
    std::lock_guard<std::mutex> lock(conn_mu_);
    std::lock_guard<std::mutex> state_lock(state_mu_);
    // A sender can wake from its 20 ms pacing sleep after session.close was
    // sent.  That expected shutdown must not overwrite a valid close ACK.
    if (only_if_active && session_.state() != duplex::State::kActive) return;
    connected_ = false;
    connection_failed_ = true;
    had_session = session_.state() == duplex::State::kActive ||
                  session_.state() == duplex::State::kClosing;
    if (session_.state() != duplex::State::kClosed) session_.Fail();
  }
  conn_cv_.notify_all();
  state_cv_.notify_all();
  if (had_session) NotifyClosed(reason);
  if (had_session) RequestCleanup();
}

void Client::RequestCleanup() {
  std::lock_guard<std::mutex> lock(cleanup_mu_);
  if (cleanup_requested_) return;
  cleanup_requested_ = true;
  cleanup_thread_ = std::thread([this] {
    try { CloseSocket(); }
    catch (const std::exception& e) { kLog->error("cleanup thread: {}", e.what()); if (callbacks_.on_fatal) callbacks_.on_fatal(); }
    catch (...) { kLog->error("cleanup thread unknown exception"); if (callbacks_.on_fatal) callbacks_.on_fatal(); }
  });
}

void Client::JoinCleanup() {
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(cleanup_mu_);
    if (cleanup_thread_.joinable()) worker = std::move(cleanup_thread_);
  }
  if (worker.joinable()) worker.join();
}

void Client::NotifyClosed(const char* reason) {
  bool notify = false;
  bool was_speaking = false;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    if (!close_notified_) {
      close_notified_ = true;
      notify = true;
      was_speaking = ai_speaking_;
      ai_speaking_ = false;
    }
  }
  if (notify && was_speaking && callbacks_.on_set_ai_speaking)
    if(!fresh_transport_control_)callbacks_.on_set_ai_speaking(false);
  if(fresh_transport_control_) {
    TransportEnvelope e;
    {std::lock_guard<std::mutex> lock(state_mu_);if(fresh_owner_.Current(fresh_envelope_))e=fresh_envelope_;}
    if(notify && e.valid() && callbacks_.on_transport_closed)callbacks_.on_transport_closed(e,reason);
  }else if (notify && callbacks_.on_session_closed) callbacks_.on_session_closed(reason);
}

bool Client::PrepareFreshTransport(const TransportEnvelope& envelope) {
  if(!fresh_transport_control_ || scoped_barge_control_ || segmented_input_)return false;
  std::lock_guard<std::recursive_mutex> fence(callback_fence_);
  std::lock_guard<std::mutex> lock(state_mu_);
  std::lock_guard<std::mutex> book_lock(retired_usage_->mutex);
  if(!fresh_retired_ || usage_unknown_.load() || PendingUsageLocked()>=128 || !envelope.valid())return false;
  if(retired_usage_->transports.size()>=128 && !PruneSettledUsageLocked())return false;
  if(!fresh_owner_.Prepare(envelope))return false;
  UsageTransport observed;observed.envelope=envelope;observed.anonymous.origin=envelope;
  observed.anonymous.source_type="local.input_admission";retired_usage_->transports.push_back(std::move(observed));
  fresh_envelope_=envelope;fresh_retired_=false;fresh_connection_attempted_=false;
  captured_input_bound_=false;last_captured_input_={};fresh_mute_sent_=false;return true;
}
bool Client::InheritRetiredUsage(Client& retired) {
  if(this==&retired || !fresh_transport_control_ || running_.load() || retired.running_.load() ||
     cfg_.conversation.usage_dir!=retired.cfg_.conversation.usage_dir ||
     cfg_.conversation.daily_token_limit!=retired.cfg_.conversation.daily_token_limit || trial_once_!=retired.trial_once_)return false;
  std::scoped_lock state_lock(state_mu_,retired.state_mu_);
  if(!fresh_retired_ || fresh_envelope_.valid() ||
     retired.sender_thread_.joinable() || retired.cleanup_thread_.joinable())return false;
  std::scoped_lock connection_lock(conn_mu_,retired.conn_mu_);
  if(ws_ || retired.ws_)return false;
  if(retired_usage_!=retired.retired_usage_) {
    std::lock_guard<std::mutex> book_lock(retired_usage_->mutex);
    if(usage_unknown_.load() || !retired_usage_->transports.empty())return false;
  }
  usage_unknown_=retired.usage_unknown_;retired_usage_=retired.retired_usage_;
  connection_attempted_.store(retired.connection_attempted_.load());return true;
}
bool Client::GrantFreshInput(const ResumePermit& permit,const std::string& session,uint64_t capture,
                            const FreshTransportOwner::Proof& proof) {
  if(!fresh_transport_control_)return false;
  std::lock_guard<std::recursive_mutex> fence(callback_fence_);
  std::lock_guard<std::mutex> lock(state_mu_);
  if(session_.state()!=duplex::State::kActive || !fresh_owner_.Grant(permit,session,capture,proof))return false;
  std::lock_guard<std::mutex> queue_lock(audio_mu_);
  AudioItem item;item.envelope=permit.envelope();item.fresh_control=3;
  audio_queue_.push_back(std::move(item));audio_cv_.notify_one();return true;
}
bool Client::RetireFreshTransport(const TransportEnvelope& envelope,const std::function<bool()>& stop_playback) {
  if(!fresh_transport_control_ || !stop_playback || fresh_callback_client==this)return false;
  {
    std::lock_guard<std::recursive_mutex> fence(callback_fence_);
    std::lock_guard<std::mutex> write_lock(write_mu_);
    std::lock_guard<std::mutex> lock(state_mu_);
    if(envelope!=fresh_envelope_ || fresh_retired_)return false;
    (void)fresh_owner_.Retire(envelope);++socket_generation_;
    {std::lock_guard<std::mutex> book_lock(retired_usage_->mutex);
      for(auto& old:retired_usage_->transports)if(old.envelope==envelope)old.retired=true;
    }
    // Best effort once. Neither cancel nor close is an ownership/drain ACK.
    if(session_.state()==duplex::State::kActive) {
      try{(void)SendTextLocked(session_.CancelResponse());(void)SendTextLocked(session_.Close());}catch(...){}
    }
    session_.Fail();ai_speaking_=false;
  }
  {std::lock_guard<std::mutex> lock(audio_mu_);audio_queue_.clear();audio_queue_bytes_=0;}
  // Neither callback fence nor state/write lock is held while Player/network join.
  bool stopped=false;try{stopped=stop_playback();}catch(...){}
  CloseSocket();JoinCleanup();
  {std::lock_guard<std::mutex> lock(state_mu_);fresh_retired_=stopped;}
  audio_cv_.notify_all();state_cv_.notify_all();return stopped;
}
bool Client::EnqueueFreshAudio(const TransportEnvelope& envelope,std::vector<uint8_t> chunk) {
  if(!fresh_transport_control_ || chunk.empty() || chunk.size()%2 || chunk.size()>32000)return false;
  std::lock_guard<std::mutex> lock(state_mu_);
  if(session_.state()!=duplex::State::kActive || !fresh_owner_.Input(envelope))return false;
  std::lock_guard<std::mutex> queue_lock(audio_mu_);
  if(audio_queue_.size()>=static_cast<size_t>(cfg_.budget.input_queue_frames))return false;
  (void)fresh_owner_.NoteAudio(envelope);NoteFreshUsageInput(envelope);audio_queue_bytes_+=chunk.size();
  audio_queue_.push_back(AudioItem{std::move(chunk),false,{},envelope});audio_cv_.notify_one();return true;
}
bool Client::EnqueueFreshCapturedAudio(const TransportEnvelope& envelope,std::vector<uint8_t> chunk,
                                     const CapturedInput& provenance) {
  const uint64_t now=static_cast<uint64_t>(NowNs());
  if(!fresh_transport_control_ || chunk.size()!=640 || !ValidCaptureOrigin(provenance.origin) ||
     provenance.origin.epoch!=envelope.epoch() || provenance.origin.capture!=envelope.capture() ||
     provenance.transfer_input!=envelope.input() || !provenance.first_read_ns ||
     provenance.first_read_ns>provenance.delivered_ns || provenance.delivered_ns>now ||
     now-provenance.first_read_ns>3000000000ULL)return false;
  std::lock_guard<std::mutex> lock(state_mu_);
  if(session_.state()!=duplex::State::kActive || !fresh_owner_.Input(envelope))return false;
  if(captured_input_bound_ && (!SameCaptureOrigin(last_captured_input_.origin,provenance.origin) ||
     provenance.first_sample!=last_captured_input_.first_sample+320 ||
     provenance.first_read_ns<last_captured_input_.first_read_ns ||
     provenance.delivered_ns<last_captured_input_.delivered_ns))return false;
  std::lock_guard<std::mutex> queue_lock(audio_mu_);
  if(audio_queue_.size()>=static_cast<size_t>(cfg_.budget.input_queue_frames))return false;
  captured_input_bound_=true;last_captured_input_=provenance;
  (void)fresh_owner_.NoteAudio(envelope);NoteFreshUsageInput(envelope);audio_queue_bytes_+=chunk.size();
  audio_queue_.push_back(AudioItem{std::move(chunk),false,{},envelope,provenance});
  audio_cv_.notify_one();return true;
}
void Client::QueueFreshMuteLocked(const TransportEnvelope& origin) {
  std::lock_guard<std::mutex> queue_lock(audio_mu_);
  audio_queue_.clear();audio_queue_bytes_=0;
  AudioItem item;item.envelope=origin;item.fresh_control=1;
  audio_queue_.push_back(std::move(item));audio_cv_.notify_one();
}
bool Client::PauseFreshInput(const TransportEnvelope& origin,const std::string&) {
  if(!fresh_transport_control_)return false;
  std::lock_guard<std::mutex> lock(state_mu_);
  if(!fresh_owner_.Pause(origin))return false;
  QueueFreshMuteLocked(origin);return true;
}
bool Client::EndFreshInput(const TransportEnvelope& origin) {
  if(!fresh_transport_control_)return false;
  std::lock_guard<std::mutex> lock(state_mu_);
  if(session_.state()!=duplex::State::kActive || !fresh_owner_.EndInput(origin))return false;
  std::lock_guard<std::mutex> queue_lock(audio_mu_);
  AudioItem item;item.envelope=origin;item.fresh_control=2;
  audio_queue_.push_back(std::move(item));audio_cv_.notify_one();return true;
}
bool Client::ConfirmFreshPlaybackDrained(const TransportEnvelope& origin) {
  if(!fresh_transport_control_)return false;
  std::lock_guard<std::mutex> lock(state_mu_);return fresh_owner_.PlaybackDrained(origin);
}
void Client::HandleTextBound(const TransportEnvelope& envelope,const std::string& text) {
  if(!fresh_transport_control_)return;
  std::lock_guard<std::recursive_mutex> fence(callback_fence_);
  // Billing sees provider IDs before owner rejection/retirement; never grants output.
  AccountRetiredUsage(envelope,text);
  bool current;
  {std::lock_guard<std::mutex> lock(state_mu_);current=fresh_owner_.Current(envelope);}
  if(!current)return;
  FreshCallbackGuard guard(this);
  duplex::Event event;bool accepted=false;
  try {
    std::lock_guard<std::mutex> lock(state_mu_);
    event=session_.Receive(text);event.correlation_proven=false;
    if(event.type=="session.created") {
      accepted=fresh_owner_.SessionCreated(envelope,event.session_id);
      if(!accepted)session_.Fail();
      else {std::lock_guard<std::mutex> queue_lock(audio_mu_);AudioItem item;item.envelope=envelope;item.fresh_control=1;audio_queue_.push_back(std::move(item));audio_cv_.notify_one();}
    }
    else if(event.type=="session.closed" || event.type=="error")accepted=true;
    else accepted=fresh_owner_.Accept(envelope,event);
    if(accepted && event.type.rfind("response.output_",0)==0) {
      if(fresh_owner_.Pause(envelope))QueueFreshMuteLocked(envelope);
      if(event.type=="response.output_audio.delta") {output_format_.Inspect(event.audio);event.audio=output_format_.ToS16(event.audio);}
    }

  }catch(...){HandleConnectionLost("protocol_error");return;}
  state_cv_.notify_all();
  if(accepted && callbacks_.on_transport_event)callbacks_.on_transport_event(envelope,event);
  if(event.type=="session.closed" || event.type=="error") {
    NotifyClosed(event.type=="error"?"server_error":"session_finished");RequestCleanup();
  }
}
namespace {
bool BillingPending(const Client::FreshUsageReceipt& r) {
  return r.ledger_failed || r.missing_total || (r.expected && !r.settled && !r.covered_by_anonymous);
}
}
size_t Client::PendingUsageLocked() const {
  size_t total=0;
  for(const auto& t:retired_usage_->transports) {
    size_t n=BillingPending(t.anonymous)?1:0;
    bool any_settled=false;
    for(const auto& r:t.responses){if(BillingPending(r))++n;any_settled|=r.settled;}
    // A single unassigned observation and one provider key may be the same bill;
    // never invent two fees just because its identifier is missing.
    if(t.responses.size()<=1 && !any_settled && n>1)n=1;
    total+=n;
  }
  return total;
}
bool Client::PruneSettledUsageLocked() {
  auto& ts=retired_usage_->transports;
  auto it=std::find_if(ts.begin(),ts.end(),[](const auto& t){
    return t.retired && t.attribution_conflict.empty() && !BillingPending(t.anonymous) &&
      std::none_of(t.responses.begin(),t.responses.end(),[](const auto& r){return BillingPending(r);});
  });
  if(it==ts.end())return false;
  retired_usage_->evicted_transport_frontier=std::max(retired_usage_->evicted_transport_frontier,it->envelope.transport());
  retired_usage_->evicted_input_frontier=std::max(retired_usage_->evicted_input_frontier,it->envelope.input());
  ts.erase(it);return true;
}
void Client::NoteFreshUsageInput(const TransportEnvelope& e) {
  std::lock_guard<std::mutex> lock(retired_usage_->mutex);
  for(auto& t:retired_usage_->transports)if(t.envelope==e){if(t.responses.empty())t.anonymous.expected=true;return;}
}
void Client::AccountRetiredUsage(const TransportEnvelope& envelope,const std::string& text) {
  // Independent shared billing book. No Session state, Player, Service, output
  // lease or callback may be reached from this path, including rejected output.
  std::lock_guard<std::mutex> lock(retired_usage_->mutex);
  duplex::Event e;try{e=duplex::ReadBillingObservation(text);}catch(...){return;}
  if(e.type.empty())return;
  auto& ts=retired_usage_->transports;
  auto it=std::find_if(ts.begin(),ts.end(),[&](const auto& t){return t.envelope==envelope;});
  if(it==ts.end()) {
    if(e.type=="response.done" && envelope.valid() && envelope.transport()<=retired_usage_->evicted_transport_frontier &&
       envelope.input()<=retired_usage_->evicted_input_frontier)usage_unknown_.attribution->store(true);
    return; // Unregistered/evicted origin can never be billed a second time.
  }
  auto conflict=[&](const std::string& reason){
    it->attribution_conflict=reason;usage_unknown_.attribution->store(true);
    // Bounded last rejected observation, separate from the anonymous receipt's key.
    auto& r=it->anonymous;r.conflicting_response_id=e.response_id;r.conflicting_question_id=e.question_id;
    r.conflict_source_type=e.type;r.conflict_event_id=e.event_id;r.conflicting_total=e.total_tokens;
  };
  const bool done=e.type=="response.done";
  const bool malformed=e.response_field.state==duplex::IdField::State::kInvalid ||
    e.response_field.state==duplex::IdField::State::kConflict ||
    e.question_field.state==duplex::IdField::State::kInvalid || e.question_field.state==duplex::IdField::State::kConflict;
  if(malformed && done){conflict("invalid_or_conflicting_provider_id");return;}
  if(!done && !e.response_field.valid())return;
  FreshUsageReceipt* receipt=nullptr;
  if(e.response_field.valid()) {
    for(auto& r:it->responses)if(r.response_id==e.response_id){receipt=&r;break;}
    if(!receipt) {
      const bool replaces_unassigned=it->responses.empty() && it->anonymous.expected && !it->anonymous.seen_done;
      if(PendingUsageLocked()>=128 && !replaces_unassigned){conflict("unsettled_observation_cap");it->anonymous.conflicting_total=e.total_tokens;return;}
      size_t count=0;for(const auto& t:ts)count+=t.responses.size();
      while(count>=128) {
        const auto old_size=ts.size();const TransportEnvelope keep=it->envelope;
        // Do not erase this channel while adding a new response to it.
        auto old=std::find_if(ts.begin(),ts.end(),[&](const auto& t){return t.envelope!=keep && t.retired && t.attribution_conflict.empty() && !BillingPending(t.anonymous) &&
          std::none_of(t.responses.begin(),t.responses.end(),[](const auto& r){return BillingPending(r);});});
        if(old==ts.end()){conflict("provider_response_observation_cap");it->anonymous.conflicting_total=e.total_tokens;return;}
        retired_usage_->evicted_transport_frontier=std::max(retired_usage_->evicted_transport_frontier,old->envelope.transport());
        retired_usage_->evicted_input_frontier=std::max(retired_usage_->evicted_input_frontier,old->envelope.input());
        ts.erase(old);it=std::find_if(ts.begin(),ts.end(),[&](const auto& t){return t.envelope==keep;});
        count=0;for(const auto& t:ts)count+=t.responses.size();if(ts.size()==old_size)break;
      }
      FreshUsageReceipt r;r.origin=envelope;r.response_id=e.response_id;r.expected=true;
      r.source_type=e.type;r.event_id=e.event_id;r.response_path=e.response_field.path;r.question_path=e.question_field.path;
      it->responses.push_back(std::move(r));receipt=&it->responses.back();
      if(!it->anonymous.seen_done)it->anonymous.expected=false;
    }
    if(e.question_field.valid()) {
      if(!receipt->question_id.empty() && receipt->question_id!=e.question_id){receipt->conflict="response_id_question_conflict";conflict(receipt->conflict);receipt->conflicting_total=e.total_tokens;return;}
      receipt->question_id=e.question_id;
    }
    if(!done)return; // Only metadata; rejected/late output never becomes output authority.
    if(it->anonymous.settled || it->anonymous.ledger_failed) {
      receipt->conflict="identified_done_after_unidentified_debit";receipt->conflicting_total=e.total_tokens;conflict(receipt->conflict);return;
    }
    // An anonymous missing-total frame cannot be assigned to this ID merely
    // because it is the only ID observed so far. Its own missing mark remains.
  }else {
    receipt=&it->anonymous;
    if(!done)return;
    if(e.question_field.valid() && !receipt->question_id.empty() && receipt->question_id!=e.question_id){
      conflict("unidentified_done_question_conflict");return;
    }
    const bool known_settled=std::any_of(it->responses.begin(),it->responses.end(),[](const auto& r){return r.settled || r.ledger_failed;});
    if(it->responses.size()>1 || known_settled || (e.question_field.valid() && !it->responses.empty() &&
       !it->responses.front().question_id.empty() && e.question_id!=it->responses.front().question_id)) {
      receipt->conflicting_total=e.total_tokens;conflict("unidentified_done_with_provider_response_ambiguity");return;
    }
    if(e.question_field.valid())receipt->question_id=e.question_id;
    if(!receipt->seen_done){receipt->source_type=e.type;receipt->event_id=e.event_id;receipt->response_path=e.response_field.path;receipt->question_path=e.question_field.path;}
  }
  receipt->seen_done=true;receipt->expected=true;
  if(receipt->ledger_failed)return; // write/ftruncate may already have applied.
  if(receipt->settled) {
    if(e.total_tokens && receipt->total_tokens!=e.total_tokens){receipt->conflicting_total=e.total_tokens;receipt->conflict="same_billing_key_total_conflict";conflict(receipt->conflict);}
    return; // Missing/duplicate same-key frames do not undo a known settlement.
  }
  if(!e.total_tokens) {
    if(!receipt->missing_total){receipt->missing_total=true;usage_unknown_.missing->fetch_add(1);}return;
  }
  receipt->total_tokens=e.total_tokens;
  receipt->total_event_id=e.event_id;
  try{usage_.AddToday(*e.total_tokens);receipt->settled=true;}catch(...){receipt->ledger_failed=true;usage_unknown_.store(true);return;}
  if(receipt->missing_total){receipt->missing_total=false;usage_unknown_.missing->fetch_sub(1);}
  if(receipt==&it->anonymous)for(auto& r:it->responses)r.covered_by_anonymous=true;
}
size_t Client::UnsettledFreshUsage() {
  std::lock_guard<std::mutex> lock(retired_usage_->mutex);return PendingUsageLocked();
}
Client::FreshUsageSnapshot Client::InspectFreshUsage() {
  std::lock_guard<std::mutex> lock(retired_usage_->mutex);FreshUsageSnapshot result;
  result.unsettled=PendingUsageLocked();result.storage_unknown=usage_unknown_.value->load();
  result.attribution_unknown=usage_unknown_.attribution->load();result.missing_totals=usage_unknown_.missing->load();
  result.evicted_transport_frontier=retired_usage_->evicted_transport_frontier;
  for(const auto& t:retired_usage_->transports) {
    auto add=[&](FreshUsageReceipt r){r.retired=t.retired;if(r.conflict.empty())r.conflict=t.attribution_conflict;result.receipts.push_back(std::move(r));};
    if(t.anonymous.expected || t.anonymous.seen_done || !t.attribution_conflict.empty())add(t.anonymous);
    for(const auto& r:t.responses)add(r);
  }
  return result;
}
void Client::HandleFreshConnectionLost(const TransportEnvelope& envelope,const char* reason) {
  std::lock_guard<std::recursive_mutex> fence(callback_fence_);
  {std::lock_guard<std::mutex> lock(state_mu_);if(!fresh_owner_.Current(envelope))return;}
  HandleConnectionLost(reason,true);
}

void Client::HandleText(const std::string& text,const IdentitySessionOrigin& origin) {
  if(fresh_transport_control_)return; // An unbound async callback cannot claim the current lease.
  duplex::Event event;
  const char* event_phase = "unknown";
  bool was_speaking = false;
  bool now_speaking = false;
  bool audio_completed = false;
  bool effects_allowed = true;
  std::vector<uint8_t> playback_audio;
  size_t completed_chunks = 0;
  size_t completed_bytes = 0;
  try {
    std::lock_guard<std::mutex> lock(state_mu_);
    if(callbacks_.on_identity_text_event && identity_origin_.valid() && !(origin==identity_origin_))return;
    event_phase = PhaseName(session_.state());
    event = session_.Receive(text);
    if(event.type=="response.done" && event.warning) {
      event=duplex::ReadUsageOnly(text);event.warning=true;effects_allowed=false;
    }
    if (scoped_barge_control_) {
      if (event.type == "response.created") {
        effects_allowed = !event.warning &&
            barge_.ResponseCreated(event.response_id) == BargeInController::Created::kAccepted;
        if (effects_allowed) output_format_.Reset();
      } else if (event.type.rfind("response.", 0) == 0) {
        effects_allowed = !event.warning && barge_.AcceptOutput(event.response_id);
        if (effects_allowed && event.type == "response.output_audio.delta")
          effects_allowed = barge_.Audio(event.response_id);
        if (effects_allowed && event.type == "response.output_audio.done")
          effects_allowed = barge_.AudioDone(event.response_id);
      } else if (event.type.rfind("conversation.item.input_audio_transcription.", 0) == 0) {
        // The protocol has no proven local-ingress-to-ASR correlation contract.
        // Do not forward anonymous/possibly stale ASR into memory in this mode.
        effects_allowed = false;
      }
    }
    if (!effects_allowed) event.exit_intent = false;
    if (effects_allowed && event.type == "conversation.item.input_audio_transcription.started") {
      user_turn_open_=true;user_turn_committed_=false;
      was_speaking = ai_speaking_;
    }
    if (effects_allowed && event.type == "response.output_audio.delta" && !event.audio.empty()) {
      if (!first_output_delta_logged_) {
        kLog->info("first_output_audio_delta_at_ns={}", NowNs());
        first_output_delta_logged_ = true;
      }
      const auto previous_kind = output_format_.kind();
      const auto probe = output_format_.Inspect(event.audio);
      if (format_probe_blocks_++ < 8 || previous_kind != output_format_.kind()) {
        const char* selected = probe.selected == audio::OutputFormat::Kind::kF32 ? "f32le" :
                               probe.selected == audio::OutputFormat::Kind::kS16 ? "s16le" : "unknown";
        kLog->info("output_audio_format_probe=bytes:{} mod4:{} s16_rms:{:.4f} s16_clip:{:.4f} f32_in_range:{:.4f} f32_rms:{:.4f} selected:{}",
                   probe.bytes, probe.divisible_by_four, probe.s16_rms,
                   probe.s16_clipped_ratio, probe.f32_in_range_ratio, probe.f32_rms, selected);
      }
      if (cfg_.audio.dump_output_audio_raw && raw_dump_bytes_ < 24000U * 4U * 5U) {
        if (raw_dump_fd_ < 0 && raw_dump_bytes_ == 0) {
          const std::string path = "/tmp/xiaoai-out-" + std::to_string(std::time(nullptr)) +
                                   "-" + std::to_string(getpid()) + ".raw";
          raw_dump_fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
          if (raw_dump_fd_ >= 0) kLog->info("output_audio_raw_dump={}", path);
          else { kLog->warn("output audio raw dump could not be opened"); raw_dump_bytes_ = 24000U * 4U * 5U; }
        }
        if (raw_dump_fd_ >= 0) {
          const size_t limit = 24000U * (output_format_.kind() == audio::OutputFormat::Kind::kF32 ? 4U : 2U) * 5U;
          const size_t count = std::min(event.audio.size(), limit > raw_dump_bytes_ ? limit - raw_dump_bytes_ : 0U);
          if (count) {
            const ssize_t wrote = write(raw_dump_fd_, event.audio.data(), count);
            if (wrote == static_cast<ssize_t>(count)) raw_dump_bytes_ += count;
            else { kLog->warn("output audio raw dump write failed"); close(raw_dump_fd_); raw_dump_fd_ = -1; raw_dump_bytes_ = 24000U * 4U * 5U; }
          }
          if (raw_dump_bytes_ >= limit) { close(raw_dump_fd_); raw_dump_fd_ = -1; }
        }
      }
      playback_audio = output_format_.ToS16(event.audio);
      ++output_audio_chunks_;
      output_audio_bytes_ += event.audio.size();
      was_speaking = ai_speaking_;
      ai_speaking_ = true;
      now_speaking = true;
    } else if (effects_allowed && event.type == "response.output_audio.done") {
      first_output_delta_logged_ = false;
      audio_completed = true;
      completed_chunks = output_audio_chunks_;
      completed_bytes = output_audio_bytes_;
      output_audio_chunks_ = 0;
      output_audio_bytes_ = 0;
      was_speaking = ai_speaking_;
      ai_speaking_ = false;
    }
  } catch (...) {
    // Parse errors can contain fragments of remote data; never print exception text.
    kLog->error("invalid duplex server event");
    HandleConnectionLost("protocol_error");
    return;
  }
  state_cv_.notify_all();
  if (!effects_allowed && event.type != "response.done") return;
  if (event.warning)
    kLog->warn("duplex event={} phase={} warning=ignored_or_adjusted",
               SafeField(event.type), event_phase);
  if (audio_completed)
    kLog->info("response.output_audio.done chunks={} decoded_bytes={}", completed_chunks, completed_bytes);
  if (audio_completed && callbacks_.on_audio_done) callbacks_.on_audio_done();
  if (now_speaking && !was_speaking && callbacks_.on_set_ai_speaking)
    callbacks_.on_set_ai_speaking(true);
  if (!playback_audio.empty() && callbacks_.on_audio) callbacks_.on_audio(playback_audio);
  if (event.type == "response.output_audio.done" && was_speaking && callbacks_.on_set_ai_speaking)
    callbacks_.on_set_ai_speaking(false);
  if (event.type == "conversation.item.input_audio_transcription.started") {
    kLog->info("transcription.started at_ns={}", NowNs());
    if (was_speaking && cfg_.conversation.duplex_mode == "full") {
      try {
        std::lock_guard<std::mutex> write_lock(write_mu_);
        std::string cancel;
        {
          std::lock_guard<std::mutex> state_lock(state_mu_);
          cancel = session_.CancelResponse();
        }
        (void)SendTextLocked(cancel);
      } catch (...) { kLog->warn("response.cancel skipped"); }
    }
    if (callbacks_.on_user_activity &&
        (cfg_.conversation.duplex_mode == "full" || !was_speaking))
      callbacks_.on_user_activity();
  }
  if(event.type.rfind("conversation.item.input_audio_transcription.",0)==0) {
    if(event.type=="conversation.item.input_audio_transcription.completed" ||
       event.type=="conversation.item.input_audio_transcription.ended_empty" ||
       event.type=="conversation.item.input_audio_transcription.failed") {
      std::lock_guard<std::mutex> lock(state_mu_);user_turn_open_=false;
    }
    if(callbacks_.on_asr_turn)callbacks_.on_asr_turn(event);
  }
  if (event.type == "conversation.item.input_audio_transcription.completed" &&
      callbacks_.on_asr_final) callbacks_.on_asr_final(event.text);
  if (event.type == "conversation.item.input_audio_transcription.ended_empty" ||
      event.type == "conversation.item.input_audio_transcription.failed") {
    if (event.type == "conversation.item.input_audio_transcription.ended_empty")
      kLog->info("transcription.ended_empty at_ns={}", NowNs());
    if (callbacks_.on_asr_empty) callbacks_.on_asr_empty();
  }
  if (callbacks_.on_response_text && !event.warning &&
      (event.type=="response.output_text.delta" || event.type=="response.output_text.done" ||
       event.type=="response.created" || event.type=="response.done")) callbacks_.on_response_text(event);
  if (cfg_.memory.enabled && (callbacks_.on_text_event || callbacks_.on_identity_text_event) && !event.warning &&
      (event.type == "conversation.item.input_audio_transcription.completed" ||
       event.type == "response.output_text.done" ||
       event.type == "conversation.item.input_audio_transcription.failed" ||
       event.type == "response.canceled")) {
    if(callbacks_.on_identity_text_event)callbacks_.on_identity_text_event(origin,event);
    else callbacks_.on_text_event(event);
  }
  if (event.type == "conversation.item.input_audio_transcription.completed")
    kLog->info("transcription.completed at_ns={}", NowNs());
  if (event.type == "response.done") {
    if (event.total_tokens) kLog->info("response.done usage total_tokens={}", *event.total_tokens);
    else kLog->info("response.done usage_present={} recognized_total=false", event.usage_present);
    for (const auto& [field, count] : event.usage_numbers)
      kLog->info("response.done usage_number {}={}", field, count);
    if (event.estimated_yuan)
      kLog->info("response.done estimated_yuan={:.6f} price_basis=per_million_tokens", *event.estimated_yuan);
    else if (event.estimated_min_yuan && event.estimated_max_yuan)
      kLog->info("response.done estimated_yuan_range={:.6f}..{:.6f} breakdown_incomplete=true",
                 *event.estimated_min_yuan, *event.estimated_max_yuan);
    if (event.total_tokens) {
      try {
        const auto total = usage_.AddToday(*event.total_tokens);
        kLog->info("daily usage total_tokens={}", total);
      } catch (...) {
        usage_unknown_.store(true);
        kLog->error("daily usage write failed; future sessions blocked");
      }
    } else {
      usage_unknown_.store(true);
      kLog->error("response.done missing total_tokens; future sessions blocked");
    }
    if (effects_allowed && callbacks_.on_chat_ended) callbacks_.on_chat_ended();
  }
  if (event.exit_intent && callbacks_.on_exit_intent) callbacks_.on_exit_intent();
  if (event.type == "session.closed") NotifyClosed("session_finished");
  if (event.type == "error") {
    const bool idle_release = event.error_code == 45000003;
    const std::string message = std::string(event_phase)=="awaiting_created" ?
      " message="+SafeField(event.text.substr(0,100),true) : "";
    if (idle_release)
      kLog->info("duplex idle release phase={} error_code=45000003 X-Tt-Logid={}{}",
                 event_phase, SafeField(event.log_id), message);
    else
      kLog->error("duplex server event=error phase={} error_code={} X-Tt-Logid={}{}",
                  event_phase, event.error_code ? std::to_string(*event.error_code) : "unavailable",
                  SafeField(event.log_id), message);
    NotifyClosed(idle_release ? "server_idle_release" : "server_error");
    RequestCleanup();
  }
}

bool Client::SendTextLocked(const std::string& text) {
  std::lock_guard<std::mutex> conn_lock(conn_mu_);
  if (!ws_ || !connected_) return false;
  if(cfg_.realtime.protocol!="o2")return ws_->sendText(text).success;
  std::lock_guard<std::mutex> wire_lock(o2_mu_);
  const auto binary=o2_session_.Encode(text,cfg_.realtime);
  return binary.empty() || ws_->sendBinary(binary).success;
}

bool Client::SendText(const std::string& text) {
  std::lock_guard<std::mutex> write_lock(write_mu_);
  return SendTextLocked(text);
}

bool Client::SendAudioFrame(const std::vector<uint8_t>& pcm, const BargeInController::Token& token) {
  try {
    std::lock_guard<std::mutex> write_lock(write_mu_);
    if (SuppressInput()) return true;
    std::string frame;
    {
      std::lock_guard<std::mutex> state_lock(state_mu_);
      if (session_.state() != duplex::State::kActive) return true;
      if (scoped_barge_control_ && !barge_.AcceptInput(token)) return true;
      frame = cfg_.realtime.protocol=="o2" ? o2_session_.Audio(pcm) : session_.AudioAppend(pcm);
    }
    if(cfg_.realtime.protocol=="o2") {
      std::lock_guard<std::mutex> lock(conn_mu_);return ws_ && connected_ && ws_->sendBinary(frame).success;
    }
    return SendTextLocked(frame);
  } catch (...) {
    return false;
  }
}
bool Client::SendFreshAudioFrame(const std::vector<uint8_t>& pcm,const TransportEnvelope& envelope) {
  std::lock_guard<std::mutex> write_lock(write_mu_);
  std::string frame;
  {std::lock_guard<std::mutex> lock(state_mu_);
    if(session_.state()!=duplex::State::kActive || !fresh_owner_.SendInput(envelope))return true;
    frame=session_.AudioAppend(pcm);}
  return SendTextLocked(frame);
}
bool Client::SendFreshCapturedFrame(const std::vector<uint8_t>& pcm,const TransportEnvelope& envelope,
                                   const CapturedInput& origin) {
  std::lock_guard<std::mutex> write_lock(write_mu_);
  std::string frame;
  {std::lock_guard<std::mutex> lock(state_mu_);
    if(session_.state()!=duplex::State::kActive || !fresh_owner_.SendInput(envelope))return true;
    const uint64_t now=static_cast<uint64_t>(NowNs());
    if(!origin.first_read_ns || now<origin.first_read_ns || now-origin.first_read_ns>3000000000ULL)return false;
    frame=session_.AudioAppend(pcm);}
  return SendTextLocked(frame);
}

bool Client::SendFreshControl(const TransportEnvelope& envelope,int control) {
  std::lock_guard<std::mutex> write_lock(write_mu_);
  std::string frame,mute;
  {std::lock_guard<std::mutex> lock(state_mu_);
    if(session_.state()!=duplex::State::kActive || !fresh_owner_.Current(envelope))return true;
    if(control==1){if(fresh_mute_sent_)return true;frame=session_.MuteInput();}
    else if(control==3){if(!fresh_owner_.SendInput(envelope))return true;frame=session_.UnmuteInput();}
    else if(control==2){if(!fresh_owner_.Ending(envelope))return true;frame=session_.CommitInput();mute=session_.MuteInput();}
    else return false;
  }
  if(!SendTextLocked(frame))return false;
  if(control==2 && !SendTextLocked(mute))return false;
  {std::lock_guard<std::mutex> lock(state_mu_);
    if(!fresh_owner_.Current(envelope))return true;
    fresh_mute_sent_=control!=3;
    if(control==2)(void)fresh_owner_.InputCommitted(envelope);
  }
  return true;
}

void Client::SendLoop() {
  duplex::InputFramer framer;
  BargeInController::Token framer_token;
  TransportEnvelope fresh_framer;
  bool muted = false;
  bool segment_has_audio = false;
  auto next_send = Clock::now();
  while (running_.load()) {
    AudioItem item;
    bool have_item = false;
    {
      std::unique_lock<std::mutex> lock(audio_mu_);
      audio_cv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
        return !running_.load() || !audio_queue_.empty();
      });
      if (!running_.load()) break;
      if (!audio_queue_.empty()) {
        item = std::move(audio_queue_.front());
        audio_queue_bytes_ -= item.pcm.size();
        audio_queue_.pop_front();
        have_item = true;
      }
    }
    bool active;
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      active = session_.state() == duplex::State::kActive;
    }
    if(fresh_transport_control_) {
      if(!active){framer.Reset();continue;}
      if(!have_item)continue;
      if(item.envelope!=fresh_framer){framer.Reset();fresh_framer=item.envelope;next_send=Clock::now();}
      if(item.fresh_control) {
        if(item.fresh_control==1)framer.Reset();
        if(item.fresh_control==2) {
          bool ending;{std::lock_guard<std::mutex> lock(state_mu_);ending=fresh_owner_.Ending(item.envelope);}
          if(!ending){framer.Reset();continue;}
          auto tail=framer.FlushWithSilence();
          if(!tail.empty()) {
            if(next_send>Clock::now())std::this_thread::sleep_until(next_send);
            if(!SendFreshAudioFrame(tail,item.envelope)){HandleFreshConnectionLost(item.envelope,"audio_tail_failed");continue;}
          }
        }
        if(!SendFreshControl(item.envelope,item.fresh_control))HandleFreshConnectionLost(item.envelope,"input_control_failed");
        continue;
      }
      {std::lock_guard<std::mutex> lock(state_mu_);if(!fresh_owner_.SendInput(item.envelope)){framer.Reset();continue;}}
      if(item.captured.transfer_input) {
        if(next_send>Clock::now())std::this_thread::sleep_until(next_send);
        if(!SendFreshCapturedFrame(item.pcm,item.envelope,item.captured))HandleFreshConnectionLost(item.envelope,"captured_input_expired_or_send_failed");
        next_send=Clock::now()+std::chrono::milliseconds(20);continue;
      }
      for(const auto& frame:framer.Append(item.pcm)) {
        if(!running_.load())break;
        {std::lock_guard<std::mutex> lock(state_mu_);if(!fresh_owner_.SendInput(item.envelope))break;}
        if(next_send>Clock::now())std::this_thread::sleep_until(next_send);
        {std::lock_guard<std::mutex> lock(state_mu_);if(!fresh_owner_.SendInput(item.envelope))break;}
        if(!SendFreshAudioFrame(frame,item.envelope)){HandleFreshConnectionLost(item.envelope,"audio_send_failed");break;}
        next_send=Clock::now()+std::chrono::milliseconds(20);
      }
      continue;
    }
    if (scoped_barge_control_) {
      const auto current = BargeToken();
      if (current != framer_token) {
        framer.Reset();
        segment_has_audio = false;
        muted = false;
        framer_token = current;
      }
      if (have_item && item.token != current) continue;
    }
    if (!active) {
      framer.Reset();
      muted = false;
      segment_has_audio = false;
      continue;
    }
    if(have_item && item.smart_commit) {
      const auto tail=framer.FlushWithSilence();
      if(!tail.empty() && !SendAudioFrame(tail,framer_token)) {
        HandleConnectionLost("audio_send_failed",true);continue;
      }
      std::lock_guard<std::mutex> write(write_mu_);
      if(!SendTextLocked(R"({"type":"input_audio_buffer.commit"})"))
        HandleConnectionLost("input_commit_failed",true);
      continue;
    }
    if (SuppressInput()) {
      framer.Reset();
      if (!muted) {
        std::lock_guard<std::mutex> write_lock(write_mu_);
        std::string mute;
        {
          std::lock_guard<std::mutex> state_lock(state_mu_);
          if (session_.state() != duplex::State::kActive ||
              (scoped_barge_control_ && !barge_.AcceptInput(framer_token))) continue;
          mute = session_.MuteInput();
        }
        muted = SendTextLocked(mute);
        if (!muted) HandleConnectionLost("audio_mute_failed", true);
      }
      continue;
    }
    if (segmented_input_ && have_item && item.end_turn) {
      if (!segment_has_audio) {
        kLog->warn("native input turn ended without audio");
        HandleConnectionLost("empty_input_turn", true);
        continue;
      }
      const auto tail = framer.FlushWithSilence();
      if (!tail.empty() && !SendAudioFrame(tail, framer_token)) {
        HandleConnectionLost("audio_send_failed", true);
        continue;
      }
      bool sent = false;
      try {
        std::lock_guard<std::mutex> write_lock(write_mu_);
        std::string commit;
        std::string mute;
        {
          std::lock_guard<std::mutex> state_lock(state_mu_);
          if (session_.state() != duplex::State::kActive ||
              (scoped_barge_control_ && !barge_.AcceptInput(framer_token))) continue;
          commit = session_.CommitInput();
          mute = session_.MuteInput();
        }
        sent = SendTextLocked(commit) && SendTextLocked(mute);
      } catch (...) { sent = false; }
      if (!sent) HandleConnectionLost("input_commit_failed", true);
      muted = sent;
      segment_has_audio = false;
      next_send = Clock::now();
      continue;
    }
    if (!have_item) {
      if (segmented_input_) continue;
      if (cfg_.conversation.duplex_mode == "half") {
        if (muted) {
          try {
            std::lock_guard<std::mutex> write_lock(write_mu_);
            std::string event;
            {
              std::lock_guard<std::mutex> state_lock(state_mu_);
              if (session_.state() != duplex::State::kActive ||
                  (scoped_barge_control_ && !barge_.AcceptInput(framer_token))) continue;
              event = session_.UnmuteInput();
            }
            if (!SendTextLocked(event)) {
              HandleConnectionLost("audio_unmute_failed", true);
              continue;
            }
            muted = false;
          } catch (...) {
            HandleConnectionLost("audio_unmute_failed", true);
          }
        }
        continue;
      }
      if (!muted) {
        const auto tail = framer.FlushWithSilence();
        if (!tail.empty() && !SendAudioFrame(tail, framer_token)) {
          HandleConnectionLost("audio_send_failed", true);
          continue;
        }
        try {
          std::lock_guard<std::mutex> write_lock(write_mu_);
          std::string event;
          {
            std::lock_guard<std::mutex> state_lock(state_mu_);
            if (scoped_barge_control_ && !barge_.AcceptInput(framer_token)) continue;
            event = session_.MuteInput();
          }
          muted = SendTextLocked(event);
          if (!muted) HandleConnectionLost("audio_send_failed", true);
          next_send = Clock::now();
        } catch (...) { muted = false; }
      }
      continue;
    }
    const auto& chunk = item.pcm;
    if (muted) {
      try {
        std::lock_guard<std::mutex> write_lock(write_mu_);
        std::string event;
        {
          std::lock_guard<std::mutex> state_lock(state_mu_);
          if (scoped_barge_control_ && !barge_.AcceptInput(framer_token)) continue;
          event = session_.UnmuteInput();
        }
        if (!SendTextLocked(event)) {
          HandleConnectionLost("audio_send_failed", true);
          continue;
        }
        muted = false;
        next_send = Clock::now();
      } catch (...) { continue; }
    }
    std::vector<std::vector<uint8_t>> frames;
    try { frames = framer.Append(chunk); }
    catch (...) {
      kLog->error("recorder delivered invalid PCM chunk");
      HandleConnectionLost("invalid_input_audio", true);
      continue;
    }
    if (segmented_input_) segment_has_audio = true;
    for (const auto& frame : frames) {
      if (!running_.load()) break;
      {
        std::lock_guard<std::mutex> state_lock(state_mu_);
        if (session_.state() != duplex::State::kActive) break;
      }
      const auto now = Clock::now();
      if (next_send > now) std::this_thread::sleep_until(next_send);
      if (now > next_send + std::chrono::milliseconds(40)) next_send = now;
      if (!SendAudioFrame(frame, framer_token)) {
        kLog->warn("duplex audio frame send failed");
        framer.Reset();
        HandleConnectionLost("audio_send_failed", true);
        break;
      }
      next_send += std::chrono::milliseconds(20);
    }
  }
}

}  // namespace xiaoai_plus::realtime
