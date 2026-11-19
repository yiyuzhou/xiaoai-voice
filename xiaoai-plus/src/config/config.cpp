#include "config/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fcntl.h>
#include <limits.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>

namespace xiaoai_plus::config {

namespace {

std::string Trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string Unquote(std::string s) {
  s = Trim(std::move(s));
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

void CheckConfigParent(const std::string& path) {
  const auto slash = path.find_last_of('/');
  const std::string parent = slash == std::string::npos ? "."
      : slash == 0 ? "/" : path.substr(0, slash);
  char resolved[PATH_MAX];
  if (!realpath(parent.c_str(), resolved)) {
    throw std::runtime_error("failed to resolve config parent directory");
  }
  const std::string canonical(resolved);
  std::string current;
  for (size_t offset = 1; offset <= canonical.size();) {
    const auto next = canonical.find('/', offset);
    current = canonical.substr(0, next == std::string::npos ? canonical.size() : next);
    struct stat dir {};
    if (stat(current.c_str(), &dir) != 0 || !S_ISDIR(dir.st_mode) ||
        (dir.st_uid != 0 && dir.st_uid != geteuid()) ||
        ((dir.st_mode & (S_IWGRP | S_IWOTH)) != 0 &&
         !(dir.st_uid == 0 && (dir.st_mode & S_ISVTX) != 0))) {
      throw std::runtime_error("config parent directory is not private");
    }
    if (next == std::string::npos) break;
    offset = next + 1;
  }
}

// No exceptions containing paths or contents; a bad memory credential must not stop voice.
bool LoadMemorySecret(const std::string& path, std::string* secret) {
  try { CheckConfigParent(path); } catch (...) { return false; }
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return false;
  struct stat st{};
  bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
            (st.st_mode & 0777) == 0600 && (st.st_size == 64 || st.st_size == 65);
  char buf[66]; ssize_t n = ok ? read(fd, buf, sizeof(buf)) : -1;
  close(fd);
  ok = ok && n == st.st_size && (n == 64 || (n == 65 && buf[64] == '\n'));
  if (ok) for (int i = 0; i < 64; ++i)
    if (!((buf[i] >= '0' && buf[i] <= '9') || (buf[i] >= 'a' && buf[i] <= 'f'))) ok = false;
  if (ok) secret->assign(buf, 64);
  return ok;
}

std::unordered_map<std::string, std::unordered_map<std::string, std::string>> ParseIni(
    const std::string& path) {
  CheckConfigParent(path);
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    throw std::runtime_error("failed to open config file");
  }
  struct stat info {};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
      (info.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
      info.st_uid != geteuid()) {
    close(fd);
    throw std::runtime_error("config must be a regular file owned by the running user with mode 0600");
  }
  std::string contents;
  char buffer[4096];
  while (true) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0) {
      close(fd);
      throw std::runtime_error("failed to read config file");
    }
    if (count == 0) break;
    if (contents.size() + static_cast<size_t>(count) > 65536) {
      close(fd);
      throw std::runtime_error("config file is too large");
    }
    contents.append(buffer, static_cast<size_t>(count));
  }
  close(fd);
  std::istringstream in(contents);

  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sections;
  std::string section;
  std::string line;
  int lineno = 0;

  while (std::getline(in, line)) {
    ++lineno;
    auto hash_pos = line.find('#');
    auto semicolon_pos = line.find(';');
    size_t comment_pos = std::string::npos;
    if (hash_pos != std::string::npos && semicolon_pos != std::string::npos) {
      comment_pos = std::min(hash_pos, semicolon_pos);
    } else if (hash_pos != std::string::npos) {
      comment_pos = hash_pos;
    } else if (semicolon_pos != std::string::npos) {
      comment_pos = semicolon_pos;
    }
    if (comment_pos != std::string::npos) {
      line = line.substr(0, comment_pos);
    }

    line = Trim(std::move(line));
    if (line.empty()) {
      continue;
    }

    if (line.front() == '[') {
      auto right = line.find(']');
      if (right == std::string::npos) {
        throw std::runtime_error("invalid section header at line " + std::to_string(lineno));
      }
      section = Trim(line.substr(1, right - 1));
      if (section.empty()) {
        throw std::runtime_error("empty section name at line " + std::to_string(lineno));
      }
      continue;
    }

    auto eq = line.find('=');
    if (eq == std::string::npos) {
      throw std::runtime_error("invalid key-value at line " + std::to_string(lineno));
    }

    if (section.empty()) {
      throw std::runtime_error("key-value outside section at line " + std::to_string(lineno));
    }

    std::string key = Trim(line.substr(0, eq));
    std::string value = Unquote(line.substr(eq + 1));
    if (key.empty()) {
      throw std::runtime_error("empty key at line " + std::to_string(lineno));
    }
    sections[section][key] = value;
  }

  return sections;
}

void SetIfPresent(const std::unordered_map<std::string, std::string>& kv, const std::string& key,
                  std::string* out) {
  auto it = kv.find(key);
  if (it != kv.end()) {
    *out = it->second;
  }
}

void SetIfPresent(const std::unordered_map<std::string, std::string>& kv, const std::string& key,
                  float* out) {
  auto it = kv.find(key);
  if (it != kv.end()) {
    *out = std::stof(it->second);
  }
}

}  // namespace

std::string Realtime::ws_url() const {
  return protocol=="o2" && preset.ws_url=="wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"
      ? "wss://openspeech.bytedance.com/api/v3/realtime/dialogue" : preset.ws_url;
}

std::map<std::string, std::string> Realtime::request_headers() const {
  if(protocol=="o2")return {{"X-Api-Key",api_key},{"X-Api-Resource-Id","volc.speech.dialog"},
                            {"X-Api-App-Key","PlgvMymc7f3tQnJ6"}};
  return {{"X-Api-Key", api_key}};
}

void Config::normalize() {
  realtime.general_assistant = audio.speaker_profile == "lx06" && !memory.enabled;
  if (realtime.general_assistant && wakeup.say_hello == "Hi! I'm Doubao. Story or riddle?") {
    wakeup.say_hello = "你好，我是豆包。你想聊什么？";
  }
  if (budget.reconnect_backoff_min_ms <= 0) {
    budget.reconnect_backoff_min_ms = 300;
  }
  if (budget.reconnect_backoff_max_ms < budget.reconnect_backoff_min_ms) {
    budget.reconnect_backoff_max_ms = budget.reconnect_backoff_min_ms * 4;
  }
}

void Config::validate(bool memory_only) const {
  if (memory.enabled) {
    if (memory.server_url.rfind("https://", 0) != 0 || memory.server_url.size() > 256 ||
        memory.server_url.find_first_of("\r\n?#@") != std::string::npos ||
        memory.server_url.back() == '/' || memory.cache_dir.empty() ||
        memory.device_secret.size() != 64 ||
        !std::all_of(memory.device_secret.begin(), memory.device_secret.end(),
                     [](unsigned char c) { return std::isxdigit(c); }) ||
        memory.identity_question.size() > 160)
      throw std::runtime_error("memory requires HTTPS origin, random 256-bit device secret and bounded identity question");
    if (audio.dump_output_audio_raw)
      throw std::runtime_error("memory mode forbids audio dumps");
  }
  if (memory_only) {
    if(!memory.enabled)throw std::runtime_error("memory selftest requires valid memory configuration");
    return;
  }
  const std::string input_prefix = "/dev/input/event";
  if (!stop_key.device.empty() &&
      (stop_key.device.rfind(input_prefix, 0) != 0 ||
       stop_key.device.size() == input_prefix.size() ||
       !std::all_of(stop_key.device.begin() + input_prefix.size(), stop_key.device.end(),
                    [](unsigned char c) { return std::isdigit(c); })))
    throw std::runtime_error("stop_key.device must be /dev/input/event followed by a node number");
  if (stop_key.key_code < -1 || stop_key.key_code == 0 || stop_key.key_code > 767)
    throw std::runtime_error("stop_key.key_code must be -1 (disabled) or 1..767");
  if(conversation.smart_turn_complete_ms<300 || conversation.smart_turn_complete_ms>10000)
    throw std::runtime_error("conversation.smart_turn_complete_ms must be 300..10000");
  if(conversation.smart_turn_open_ms<300 || conversation.smart_turn_open_ms>20000)
    throw std::runtime_error("conversation.smart_turn_open_ms must be 300..20000");
  if (conversation.idle_seconds < 1 || conversation.idle_seconds > 900) {
    throw std::runtime_error("conversation.idle_seconds must be 1..900");
  }
  if (conversation.absolute_timeout_seconds < 0 ||
      conversation.absolute_timeout_seconds > 86400) {
    throw std::runtime_error("conversation.absolute_timeout_seconds must be 0..86400");
  }
  if (conversation.duplex_mode != "half" && conversation.duplex_mode != "full") {
    throw std::runtime_error("conversation.duplex_mode must be half or full");
  }
  if(conversation.barge_processing_io_delay_hint_ms<0 ||
     conversation.barge_processing_io_delay_hint_ms>500)
    throw std::runtime_error("conversation.barge_processing_io_delay_hint_ms must be 0..500");
  if(conversation.ordinary_barge_in && !memory.enabled)
    throw std::runtime_error("ordinary barge-in requires the identity Service");
  if (conversation.daily_token_limit > 1000000000) {
    throw std::runtime_error("conversation.daily_token_limit must be 0..1000000000");
  }
  if (wakeup.s32_shift != 8 && wakeup.s32_shift != 16)
    throw std::runtime_error("wakeup.s32_shift must be 8 or 16");
  if (wakeup.input_channel < -1 || wakeup.input_channel > 2)
    throw std::runtime_error("wakeup.input_channel must be beamformed or 0..2");
  if(wakeup.kws_num_threads<1)throw std::runtime_error("wakeup.kws_num_threads must be positive");
  if (!(wakeup.kws_threshold > 0.0f && wakeup.kws_threshold <= 1.0f))
    throw std::runtime_error("wakeup.kws_threshold must be in (0,1]");
  if (!(wakeup.exit_playback_threshold >= 0.30f && wakeup.exit_playback_threshold <= 1.0f))
    throw std::runtime_error("wakeup.exit_playback_threshold must be in [0.30,1]");
  if(realtime.protocol!="v3" && realtime.protocol!="o2")
    throw std::runtime_error("realtime.protocol must be v3 or o2");
  if (!(audio.playback_gain > 0.0f && audio.playback_gain <= 1.0f))
    throw std::runtime_error("audio.playback_gain must be in (0,1]");
  if (audio.speaker_profile != "oh2p" && audio.speaker_profile != "lx06")
    throw std::runtime_error("audio.speaker_profile must be oh2p or lx06");
  if (audio.native_wake_log.empty() || audio.native_wake_log.front() != '/')
    throw std::runtime_error("audio.native_wake_log must be an absolute path");
  if (audio.speaker_profile == "lx06" &&
      (conversation.duplex_mode != "half" || conversation.ordinary_barge_in ||
       wakeup.input_channel > 0))
    throw std::runtime_error("LX06 requires half duplex and mono capture");
  if(realtime.o2_end_smooth_window_ms<500 || realtime.o2_end_smooth_window_ms>50000 || realtime.o2_voice.empty())
    throw std::runtime_error("invalid realtime.o2 window or voice");
  if(realtime.protocol=="o2" && conversation.ordinary_barge_in)
    throw std::runtime_error("O2 requires the existing half-duplex conversation path");
  if (realtime.preset.model != "1.2.6.1") {
    throw std::runtime_error("this build supports only Seeduplex 3.0 model 1.2.6.1");
  }
  if (realtime.preset.ws_url !=
      "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue" ||
      realtime.preset.voice.empty()) {
    throw std::runtime_error("unsupported Seeduplex 3.0 endpoint or voice");
  }
  if (realtime.preset.ca_file.empty() || realtime.preset.ca_file == "NONE") {
    throw std::runtime_error("TLS CA bundle path is required");
  }
  if (realtime.api_key == "your_api_key" || realtime.app_id == "your_app_id" ||
      realtime.access_token == "your_access_token") {
    throw std::runtime_error("replace example credentials before starting");
  }
  if (!realtime.app_id.empty() || !realtime.access_token.empty() ||
      !realtime.secret_key.empty()) {
    throw std::runtime_error("legacy credentials are not supported by the Seeduplex client");
  }
  if (realtime.api_key.empty()) {
    throw std::runtime_error("realtime.api_key is required");
  }
}

Config load(const std::string& path, bool memory_only) {
  Config cfg;
  cfg.realtime.protocol="o2";
  if (!path.empty()) {
    auto sections = ParseIni(path);

    if (auto it = sections.find("realtime"); it != sections.end()) {
      const auto& kv = it->second;
      SetIfPresent(kv, "protocol", &cfg.realtime.protocol);
      SetIfPresent(kv, "api_key", &cfg.realtime.api_key);
      SetIfPresent(kv, "app_id", &cfg.realtime.app_id);
      SetIfPresent(kv, "access_token", &cfg.realtime.access_token);
      SetIfPresent(kv, "secret_key", &cfg.realtime.secret_key);
      SetIfPresent(kv, "model", &cfg.realtime.preset.model);
      SetIfPresent(kv, "voice", &cfg.realtime.preset.voice);
      SetIfPresent(kv, "bot_name", &cfg.realtime.preset.bot_name);
      SetIfPresent(kv, "system_role", &cfg.realtime.preset.system_role);
      SetIfPresent(kv, "speaking_style", &cfg.realtime.preset.speaking_style);
    }

    if (auto it = sections.find("realtime.o2"); it != sections.end()) {
      if(auto value=it->second.find("end_smooth_window_ms");value!=it->second.end()) {
        size_t parsed=0;
        try{cfg.realtime.o2_end_smooth_window_ms=std::stoi(value->second,&parsed);}
        catch(...){throw std::runtime_error("invalid realtime.o2.end_smooth_window_ms");}
        if(parsed!=value->second.size())throw std::runtime_error("invalid realtime.o2.end_smooth_window_ms");
      }
      SetIfPresent(it->second,"voice",&cfg.realtime.o2_voice);
    }

    if (auto it = sections.find("wakeup"); it != sections.end()) {
      const auto& kv = it->second;
      if (auto agc = kv.find("kws_agc"); agc != kv.end()) {
        if (agc->second == "on") cfg.wakeup.kws_agc = true;
        else if (agc->second == "off") cfg.wakeup.kws_agc = false;
        else throw std::runtime_error("wakeup.kws_agc must be on or off");
      }
      SetIfPresent(kv, "say_hello", &cfg.wakeup.say_hello);
      SetIfPresent(kv, "keywords_file", &cfg.wakeup.keywords_file);
      SetIfPresent(kv, "kws_threshold", &cfg.wakeup.kws_threshold);
      if(auto threads=kv.find("kws_num_threads");threads!=kv.end()) {
        size_t parsed=0;
        try {cfg.wakeup.kws_num_threads=std::stoi(threads->second,&parsed);}
        catch(...) {throw std::runtime_error("wakeup.kws_num_threads must be a positive integer");}
        if(parsed!=threads->second.size())throw std::runtime_error("wakeup.kws_num_threads must be a positive integer");
      }
      SetIfPresent(kv, "exit_playback_threshold", &cfg.wakeup.exit_playback_threshold);
      if (auto shift = kv.find("s32_shift"); shift != kv.end()) {
        if (shift->second == "8") cfg.wakeup.s32_shift = 8;
        else if (shift->second == "16") cfg.wakeup.s32_shift = 16;
        else throw std::runtime_error("wakeup.s32_shift must be 8 or 16");
      }
      if (auto channel = kv.find("input_channel"); channel != kv.end()) {
        if (channel->second == "beamformed") cfg.wakeup.input_channel = -1;
        else if (channel->second == "0" || channel->second == "1" || channel->second == "2")
          cfg.wakeup.input_channel = channel->second[0] - '0';
        else throw std::runtime_error("wakeup.input_channel must be beamformed or 0..2");
      }
    }

    if (auto it = sections.find("conversation"); it != sections.end()) {
      const auto& kv = it->second;
      if(auto v=kv.find("smart_turn");v!=kv.end()) {
        if(v->second=="on")cfg.conversation.smart_turn=true;
        else if(v->second=="off")cfg.conversation.smart_turn=false;
        else throw std::runtime_error("conversation.smart_turn must be on or off");
      }
      if(auto v=kv.find("ordinary_barge_in");v!=kv.end()) {
        if(v->second=="on")cfg.conversation.ordinary_barge_in=true;
        else if(v->second=="off")cfg.conversation.ordinary_barge_in=false;
        else throw std::runtime_error("conversation.ordinary_barge_in must be on or off");
      }
      SetIfPresent(kv, "duplex_mode", &cfg.conversation.duplex_mode);
      if (auto limit = kv.find("daily_token_limit"); limit != kv.end()) {
        if (limit->second.empty() ||
            !std::all_of(limit->second.begin(), limit->second.end(),
                         [](unsigned char c) { return std::isdigit(c); })) {
          throw std::runtime_error("conversation.daily_token_limit must be decimal");
        }
        try { cfg.conversation.daily_token_limit = std::stoull(limit->second); }
        catch (...) { throw std::runtime_error("conversation.daily_token_limit is too large"); }
      }
      for (const auto& [key, target] :
           {std::pair<const char*, int*>{"idle_seconds", &cfg.conversation.idle_seconds},
            {"smart_turn_complete_ms", &cfg.conversation.smart_turn_complete_ms},
            {"smart_turn_open_ms", &cfg.conversation.smart_turn_open_ms},
            {"absolute_timeout_seconds", &cfg.conversation.absolute_timeout_seconds},
            {"barge_processing_io_delay_hint_ms", &cfg.conversation.barge_processing_io_delay_hint_ms}}) {
        if (auto value = kv.find(key); value != kv.end()) {
          const auto& raw = value->second;
          if (raw.empty() || !std::all_of(raw.begin(), raw.end(),
              [](unsigned char c) { return std::isdigit(c); })) {
            throw std::runtime_error(std::string("conversation.") + key + " must be decimal");
          }
          try { *target = std::stoi(raw); }
          catch (...) { throw std::runtime_error(std::string("conversation.") + key + " is too large"); }
        }
      }
    }

    if (auto it = sections.find("memory"); it != sections.end()) {
      const auto& kv = it->second;
      if (auto enabled = kv.find("enabled"); enabled != kv.end()) {
        if (enabled->second == "on") cfg.memory.enabled = true;
        else if (enabled->second == "off") cfg.memory.enabled = false;
        else throw std::runtime_error("memory.enabled must be on or off");
      }
      SetIfPresent(kv, "server_url", &cfg.memory.server_url);
      SetIfPresent(kv, "server", &cfg.memory.server_url);
      SetIfPresent(kv, "device_id", &cfg.memory.device_id);
      SetIfPresent(kv, "device_secret_file", &cfg.memory.device_secret_file);
      SetIfPresent(kv, "device_secret", &cfg.memory.device_secret);
      SetIfPresent(kv, "cache_dir", &cfg.memory.cache_dir);
      SetIfPresent(kv, "identity_question", &cfg.memory.identity_question);
      if(auto v=kv.find("child_names");v!=kv.end()) {
        std::istringstream names(v->second);std::string name;
        while(std::getline(names,name,',')) {
          const auto first=name.find_first_not_of(" \t\r\n");
          if(first==std::string::npos)continue;
          name=name.substr(first,name.find_last_not_of(" \t\r\n")-first+1);
          if(cfg.memory.child_names.size()<16)cfg.memory.child_names.push_back(name);
        }
      }
      if(auto v=kv.find("identity_text_log");v!=kv.end()) {
        if(v->second=="on")cfg.memory.identity_text_log=true;
        else if(v->second=="off")cfg.memory.identity_text_log=false;
        else throw std::runtime_error("memory.identity_text_log must be on or off");
      }
    }

    if (auto it = sections.find("stop_key"); it != sections.end()) {
      const auto& kv = it->second;
      SetIfPresent(kv, "device", &cfg.stop_key.device);
      if (auto code = kv.find("key_code"); code != kv.end()) {
        const auto& raw = code->second;
        if (raw == "-1") cfg.stop_key.key_code = -1;
        else {
          if (raw.empty() || !std::all_of(raw.begin(), raw.end(),
              [](unsigned char c) { return std::isdigit(c); }))
            throw std::runtime_error("stop_key.key_code must be decimal or -1");
          try { cfg.stop_key.key_code = std::stoi(raw); }
          catch (...) { throw std::runtime_error("stop_key.key_code is too large"); }
        }
      }
    }

    if (auto it = sections.find("audio"); it != sections.end()) {
      const auto& kv = it->second;
      SetIfPresent(kv, "speaker_profile", &cfg.audio.speaker_profile);
      SetIfPresent(kv, "native_wake_log", &cfg.audio.native_wake_log);
      SetIfPresent(kv, "playback_gain", &cfg.audio.playback_gain);
      if (auto dump = kv.find("dump_output_audio_raw"); dump != kv.end()) {
        if (dump->second != "0" && dump->second != "1")
          throw std::runtime_error("audio.dump_output_audio_raw must be 0 or 1");
        cfg.audio.dump_output_audio_raw = dump->second == "1";
      }
    }
  }

  if (cfg.memory.enabled && !cfg.memory.device_secret_file.empty()) {
    cfg.memory.device_secret.clear(); // Explicit file never falls back to an inline secret.
    if (!LoadMemorySecret(cfg.memory.device_secret_file, &cfg.memory.device_secret)) {
      cfg.memory.enabled = false;
      cfg.memory.secret_file_failed = true;
    }
  }
  cfg.normalize();
  cfg.validate(memory_only);
  return cfg;
}

}  // namespace xiaoai_plus::config
