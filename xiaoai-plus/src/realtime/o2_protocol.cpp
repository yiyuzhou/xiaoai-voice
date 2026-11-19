#include "realtime/o2_protocol.hpp"
#include <chrono>
#include <stdexcept>
#include <unistd.h>

namespace xiaoai_plus::realtime::o2 {
namespace {
using J = nlohmann::json;
using E = protocol::EventId;
std::string Bytes(const std::vector<uint8_t>& v) { return {v.begin(), v.end()}; }
std::string Base64(const std::vector<uint8_t>& v) {
  constexpr char a[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for(size_t i=0;i<v.size();i+=3) {
    const uint32_t n=(uint32_t(v[i])<<16)|(i+1<v.size()?uint32_t(v[i+1])<<8:0)|(i+2<v.size()?v[i+2]:0);
    out+=a[n>>18];out+=a[(n>>12)&63];out+=i+1<v.size()?a[(n>>6)&63]:'=';out+=i+2<v.size()?a[n&63]:'=';
  }
  return out;
}
}
void Session::Reset() {
  *this=Session{};
  session_id_="o2-"+std::to_string(getpid())+"-"+std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
}
std::string Session::Json(E event,const J& body) const {
  return Bytes(protocol::BuildJsonFrame(protocol::MessageType::kFullClientRequest,event,body,session_id_));
}
std::string Session::Audio(const std::vector<uint8_t>& pcm) const {
  if(pcm.size()!=640)throw std::runtime_error("O2 audio requires 20ms S16 PCM");
  return Bytes(protocol::BuildAudioFrame(E::kTaskRequest,session_id_,pcm));
}
std::string Session::Encode(const std::string& event,const config::Realtime& cfg) {
  const auto msg=J::parse(event);const auto type=msg.at("type").get<std::string>();
  if(type=="connection.start")return Json(E::kStartConnection,J::object());
  if(type=="connection.finish")return Json(E::kFinishConnection,J::object());
  if(type=="session.create") {
    const auto& s=msg.at("session");
    J extra={{"model","1.2.1.1"},{"input_mod","keep_alive"},{"enable_user_query_exit",true},
             {"enable_music",cfg.preset.enable_music},{"enable_volc_websearch",false}};
    if(s.contains("extension"))extra.update(s["extension"]["dialog"]["extra"]);
    const std::string speaking_style = cfg.general_assistant ? cfg.preset.speaking_style :
        "Cheerful, playful big buddy for young kids. English only. Slow, clear, simple words. Usually 2 short sentences, never more than 3; a fun sound like \"Roar!\" doesn't count. At most one question, only at the end.";
    return Json(E::kStartSession,{{"asr",{{"extra",{{"enable_custom_vad",true},
        {"end_smooth_window_ms",cfg.o2_end_smooth_window_ms}}}}},
      {"tts",{{"speaker",cfg.o2_voice},{"audio_config",{{"format","pcm_s16le"},{"sample_rate",24000},{"channel",1}}}}},
      {"dialog",{{"bot_name",cfg.preset.bot_name},{"system_role",s.at("instructions")},
        {"speaking_style",speaking_style},
        {"extra",extra}}}});
  }
  if(type=="input_audio_buffer.commit")return Json(E::kEndAsr,J::object());
  if(type=="session.close")return Json(E::kFinishSession,J::object());
  if(type=="speech_text_buffer.commit")return Json(E::kSayHello,{{"content",msg.at("text")}});
  if(type=="response.cancel") {
    // Streaming O2 has no documented cancel command. Fence the entire old turn;
    // the next recognized utterance interrupts it at the provider. Exit closes it.
    canceled_=true;output_allowed_=false;canceled_question_=question_;return {};
  }
  if(type=="input_audio_mute.commit" || type=="input_audio_unmute.commit")return {};
  throw std::runtime_error("unsupported O2 streaming control");
}
std::vector<std::string> Session::Receive(const std::string& binary) {
  if(binary.size()>1024*1024)throw std::runtime_error("O2 frame too large");
  const auto f=protocol::DecodeFrame({binary.begin(),binary.end()});
  const auto id=f.event_id?static_cast<uint32_t>(*f.event_id):0;
  if(f.session_id && *f.session_id!=session_id_)return {};
  if(id==50){connected_=true;return {};}
  if(f.error_code || id==51 || id==153 || id==599) {
    auto body=f.JsonPayload();
    J error={{"type","error"},{"code",f.error_code?J(*f.error_code):body.value("status_code",J(nullptr))}};
    if(body.contains("error") && body["error"].is_string())error["message"]=body["error"];
    return {error.dump()};
  }
  if(id==150)return {J{{"type","session.created"},{"session",{{"id",session_id_}}}}.dump()};
  if(id==152)return {J{{"type","session.closed"}}.dump()};
  J body=f.serialization==protocol::Serialization::kJson?f.JsonPayload():J::object();
  if(!body.is_object())throw std::runtime_error("O2 invalid JSON payload");
  std::vector<std::string> events;
  auto emit=[&](const char* type,J data=J::object()) {
    data["type"]=type;data["event_id"]=session_id_+":"+std::to_string(++serial_);
    data["question_id"]=question_;data["response_id"]=reply_;events.push_back(data.dump());
  };
  if(id==450) {
    question_=body.value("question_id","");asr_.clear();
    emit("conversation.item.input_audio_transcription.started");
  } else if(id==451) {
    if(body.contains("results"))for(const auto& result:body["results"])
      if(result.contains("text")) {
        asr_=result.at("text").get<std::string>();
        emit("conversation.item.input_audio_transcription.delta",{{"delta",asr_}});
      }
  } else if(id==459) {
    if(!asr_.empty())emit("conversation.item.input_audio_transcription.completed",{{"item_id",question_},{"transcript",asr_}});
    else emit("conversation.item.input_audio_transcription.ended_empty",{{"item_id",question_}});
    asr_.clear();
  } else if(id==350 || id==550 || id==559 || id==359) {
    const auto q=body.value("question_id","");const auto r=body.value("reply_id","");
    if(canceled_) {
      // An old header/done must not reopen anonymous following audio. Only a
      // new ASR question AND its explicitly matching reply may reopen output.
      output_allowed_=!q.empty() && q==question_ && q!=canceled_question_;
      if(!output_allowed_)return {};
    }
    if(!q.empty())question_=q;
    if(!r.empty() && r!=reply_){reply_=r;text_.clear();emit("response.created");}
    if(id==550){text_+=body.value("content","");emit("response.output_text.delta",{{"delta",body.value("content","")}});}
    if(id==559){emit("response.output_text.done",{{"text",text_}});text_.clear();}
    if(id==359)emit("response.output_audio.done",{{"status_code",body.value("status_code",J(nullptr))}});
  } else if(id==352) {
    if(output_allowed_)emit("response.output_audio.delta",{{"delta",Base64(f.payload)}});
  } else if(id==154) {
    // O2 reports six disjoint counters, including cached input separately.
    auto u=body.at("usage");uint64_t total=0;
    for(const char* k:{"input_text_tokens","input_audio_tokens","cached_text_tokens","cached_audio_tokens","output_text_tokens","output_audio_tokens"}) {
      if(!u.contains(k) || !u[k].is_number_unsigned()) {
        if(!u.contains(k) || !u[k].is_number_integer() || u[k].get<int64_t>()<0)
          throw std::runtime_error("invalid O2 usage counter");
      }
      const auto n=u[k].get<uint64_t>();if(n>1000000000)throw std::runtime_error("invalid O2 usage counter");total+=n;
    }
    u["input_text_tokens"]=u["input_text_tokens"].get<uint64_t>()+u["cached_text_tokens"].get<uint64_t>();
    u["input_audio_tokens"]=u["input_audio_tokens"].get<uint64_t>()+u["cached_audio_tokens"].get<uint64_t>();
    u["cached_input_tokens"]=u["cached_text_tokens"].get<uint64_t>()+u["cached_audio_tokens"].get<uint64_t>();
    u["total_tokens"]=total;emit("response.done",{{"usage",u}});
  }
  return events;
}
}  // namespace xiaoai_plus::realtime::o2
