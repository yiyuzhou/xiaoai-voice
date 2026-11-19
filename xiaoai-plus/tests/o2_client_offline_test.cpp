#ifdef NDEBUG
#error requires active assertions
#endif
#include <atomic>
#include <cassert>
#include <filesystem>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXGetFreePort.h>
#include <spdlog/sinks/ostream_sink.h>
#include "common/log.hpp"
#include "realtime/client.hpp"
#include "memory/core.hpp"
using namespace xiaoai_plus;
using namespace std::chrono_literals;
using namespace realtime::protocol;
using J=nlohmann::json;
int main() {
  int port=ix::getFreePort();assert(port>0);ix::WebSocketServer server(port,"127.0.0.1");
  std::atomic<int> opens{0},final_asr{0},answers{0},chat_ended{0}, commits{0}, audio_frames{0}, deltas{0};
  std::string sid;
  std::string expected_instructions;
  std::atomic<bool> standalone_greeting{false};
  std::atomic<bool> general_assistant{false};
  std::atomic<bool> reject_create{false},reject_active{false};std::string error_message;
  server.setOnClientMessageCallback([&](auto,ix::WebSocket& ws,const ix::WebSocketMessagePtr& msg){
    if(msg->type==ix::WebSocketMessageType::Open){++opens;return;}
    if(msg->type!=ix::WebSocketMessageType::Message)return;
    assert(msg->binary);auto f=DecodeFrame({msg->str.begin(),msg->str.end()});
    if(f.session_id)sid=*f.session_id;
    auto send=[&](int id,J body=J::object()) {
      auto b=BuildJsonFrame(MessageType::kFullServerResponse,static_cast<EventId>(id),body,sid);
      assert(ws.sendBinary(std::string(b.begin(),b.end())).success);
    };
    if(f.event_id==EventId::kStartConnection)send(50);
    if(f.event_id==EventId::kStartSession) {
      if(reject_create){send(153,{{"status_code",45000292},{"error",error_message}});return;}
      const auto dialog=f.JsonPayload()["dialog"];
      if (general_assistant) {
        const auto role=dialog["system_role"].get<std::string>();
        assert(role.find("默认用普通话")!=std::string::npos);
        assert(role.find("继续使用该语言")!=std::string::npos);
        assert(role.find("Speak only English")==std::string::npos);
        assert(dialog["speaking_style"]=="语气自然、友好、简洁。");
      } else {
        assert(dialog["system_role"]==expected_instructions);
      }
      assert(f.JsonPayload()["asr"]["extra"]["end_smooth_window_ms"]==2500);send(150);
    }
    if(f.event_id==EventId::kTaskRequest)++audio_frames;
    if(f.event_id==EventId::kEndAsr) {assert(audio_frames==3);assert(++commits==1);send(459);}
    if(f.event_id==EventId::kSayHello) {
      if(standalone_greeting){
        assert(f.JsonPayload()["content"]==(general_assistant ? "你好，我是豆包。你想聊什么？"
                                                             : "Hi! I'm Doubao. Story or riddle?"));
        send(152);return;
      }
      if(reject_active){send(599,{{"status_code",45000292},{"error","PRIVATE_ACTIVE_ERROR"}});return;}
      send(450,{{"question_id","q1"}});
      send(451,{{"results",J::array({{{"text","我是 Amy"},{"is_interim",false}}})}});
      send(550,{{"question_id","q1"},{"reply_id","r1"},{"content","Hi Amy"}});
      send(559,{{"question_id","q1"},{"reply_id","r1"}});
    }
    if(f.event_id==EventId::kFinishSession) {
      // FinishSession(0) has already retired dialogue authority. This usage
      // still belongs in the ledger and must not trigger live turn callbacks.
      send(154,{{"usage",{{"input_text_tokens",1},{"input_audio_tokens",2},{"cached_text_tokens",0},
          {"cached_audio_tokens",0},{"output_text_tokens",3},{"output_audio_tokens",4}}}});
      std::this_thread::sleep_for(80ms);send(152);
    }
  });assert(server.listenAndStart());
  char dir[]="/tmp/o2-client-XXXXXX";assert(mkdtemp(dir));
  config::Config cfg;cfg.realtime.protocol="o2";cfg.realtime.api_key="loopback-only";cfg.memory.enabled=true;
  cfg.realtime.preset.ws_url="ws://127.0.0.1:"+std::to_string(port);cfg.conversation.usage_dir=dir;
  realtime::Client::Callbacks cb;
  cb.on_asr_turn=[&](const auto& e){if(e.type=="conversation.item.input_audio_transcription.delta"){assert(e.text=="我是 Amy");++deltas;}};
  cb.on_asr_final=[&](const auto& s){assert(s=="我是 Amy");++final_asr;};
  cb.on_text_event=[&](const auto& e){if(e.type=="response.output_text.done"){assert(e.text=="Hi Amy");++answers;}};
  cb.on_chat_ended=[&]{++chat_ended;};
  expected_instructions=cfg.realtime.preset.system_role+" "+cfg.realtime.preset.speaking_style+
    " Volume requests: say only '好的'; handled locally, never say you cannot. "
    "Usually 2 short sentences, never more than 3; a fun sound like \"Roar!\" doesn't count. At most one question, only at the end. "
    "During children's English practice, be natural and encouraging. "
    "Preserve the required identity question and continuation opening.";
  realtime::Client client(cfg,cb);assert(client.Start());assert(client.StartSession(2s));
  assert(client.SendSayHello("identity"));
  for(int i=0;i<100 && answers<1;++i)std::this_thread::sleep_for(10ms);
  for(int i=0;i<100 && deltas<1;++i)std::this_thread::sleep_for(10ms);
  assert(deltas==1);assert(client.EnqueueAudio(std::vector<uint8_t>(640*3,0)));
  assert(client.CommitUserTurn());assert(!client.CommitUserTurn());
  for(int i=0;i<100 && final_asr<1;++i)std::this_thread::sleep_for(10ms);
  assert(commits==1 && final_asr==1 && answers==1);assert(client.FinishSession(0ms));
  // No artificial sleep: StartSession joins the retired socket before reading its ledger.
  assert(client.StartSession(2s));assert(opens==2 && chat_ended==0);
  realtime::DailyUsage usage(dir);assert(usage.ReadToday()==10);
  client.Stop();
  auto standalone=cfg;standalone.memory.enabled=false;standalone.memory.child_names={"Amy","Ben"};
  standalone.wakeup.say_hello="Hi! I'm Doubao. Story or riddle?";
  expected_instructions=memory::PlaymateInstructions(standalone.memory.child_names);
  assert(expected_instructions.find("You are Doubao, an English playmate for Amy and Ben")==0);
  realtime::Client no_memory(standalone,{});assert(no_memory.Start() && no_memory.StartSession(2s));
  standalone_greeting=true;assert(no_memory.SendSayHello(standalone.wakeup.say_hello));
  std::this_thread::sleep_for(100ms);no_memory.Stop();standalone_greeting=false;
  auto lx06=standalone;
  lx06.audio.speaker_profile="lx06";
  lx06.normalize();
  general_assistant=true;standalone_greeting=true;
  realtime::Client assistant(lx06,{});
  assert(assistant.Start() && assistant.StartSession(2s));
  assert(assistant.SendSayHello());
  std::this_thread::sleep_for(100ms);assistant.Stop();
  general_assistant=false;standalone_greeting=false;
  expected_instructions="explicit override";
  realtime::Client overridden(cfg,{});assert(overridden.Start() && overridden.StartSession(2s,expected_instructions));overridden.Stop();
  std::ostringstream log;
  auto logger=GetLogger("realtime");auto old_sinks=logger->sinks();
  logger->sinks()={std::make_shared<spdlog::sinks::ostream_sink_mt>(log)};
  reject_create=true;
  for(const auto& text:{std::string("quota exceeded for types: tokens_lifetime"),std::string(120,'x'),std::string("bad\ninjected")}) {
    error_message=text;log.str("");realtime::Client rejected(cfg,{});assert(rejected.Start());
    assert(!rejected.StartSession(2s));rejected.Stop();
    const auto expected=text.find('\n')==std::string::npos?text.substr(0,100):"unavailable";
    assert(log.str().find("phase=awaiting_created error_code=45000292 X-Tt-Logid=unavailable message="+expected)!=std::string::npos);
    assert(log.str().find(std::string(101,'x'))==std::string::npos && log.str().find("injected")==std::string::npos);
  }
  expected_instructions=cfg.realtime.preset.system_role+" "+cfg.realtime.preset.speaking_style+
    " Volume requests: say only '好的'; handled locally, never say you cannot. "
    "Usually 2 short sentences, never more than 3; a fun sound like \"Roar!\" doesn't count. At most one question, only at the end. "
    "During children's English practice, be natural and encouraging. "
    "Preserve the required identity question and continuation opening.";
  reject_create=false;reject_active=true;log.str("");
  std::atomic<bool> active_closed{false};realtime::Client::Callbacks active_cb;
  active_cb.on_session_closed=[&](const auto&){active_closed=true;};
  realtime::Client active(cfg,active_cb);assert(active.Start() && active.StartSession(2s));assert(active.SendSayHello("error test"));
  for(int i=0;i<100 && !active_closed;++i)std::this_thread::sleep_for(10ms);
  assert(active_closed);
  active.Stop();assert(log.str().find("phase=active error_code=45000292")!=std::string::npos);
  assert(log.str().find("message=")==std::string::npos && log.str().find("PRIVATE_ACTIVE_ERROR")==std::string::npos);
  logger->sinks()=std::move(old_sinks);server.stop();std::filesystem::remove_all(dir);
}
