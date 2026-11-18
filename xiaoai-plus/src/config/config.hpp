#pragma once

#include <cstdint>
#include <map>
#include <vector>
#include <string>

namespace xiaoai_plus::config {

// Aplay consumes 24 kHz mono S16LE. The output adapter converts F32 PCM if detected.
inline constexpr int kTtsPcmSampleRate = 24000;
inline constexpr int kTtsPcmChannels = 1;
inline constexpr int kTtsPcmBitsPerSample = 16;

struct RealtimePreset {
  std::string ws_url{"wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"};
  std::string ca_file{"assets/ca-certificates.crt"};
  std::string model{"1.2.6.1"};
  std::string voice{"zh_female_vv_jupiter_bigtts"};
  std::string bot_name{"豆包"};
  std::string system_role{"你是豆包同学，是小爱音箱上的语音助手。"};
  std::string speaking_style{"语气自然、友好、简洁。"};
  bool enable_music{true};
};

struct Realtime {
  std::string protocol{"v3"}; // Programmatic callers; device load() defaults to O2.
  int o2_end_smooth_window_ms{2500};
  std::string o2_voice{"zh_female_vv_jupiter_bigtts"};
  std::string ws_url() const;
  std::string api_key;
  std::string app_id;
  std::string access_token;
  std::string secret_key;
  RealtimePreset preset;

  std::map<std::string, std::string> request_headers() const;
};

struct AudioPreset {
  std::string input_device{"noop"};
  std::string output_device{"default"};
  int sample_rate{16000};
  int channels{1};
  int bits_per_sample{16};
  int buffer_size{1440};
  int period_size{360};
  float playback_gain{0.90f};
  bool dump_output_audio_raw{false};
  std::string speaker_profile{"oh2p"};
  std::string native_wake_log{"/tmp/log/messages"};
};
using Audio = AudioPreset;

inline Audio MakeTtsPlaybackAudio(Audio audio) {
  audio.sample_rate = kTtsPcmSampleRate;
  audio.channels = kTtsPcmChannels;
  audio.bits_per_sample = kTtsPcmBitsPerSample;
  return audio;
}

struct Wakeup {
  bool kws_agc{true};
  float kws_threshold{0.10f};
  int kws_num_threads{2};
  float exit_playback_threshold{0.45f};
  int s32_shift{16};
  int input_channel{-1};  // -1: beamformed; 0..2: one microphone
  std::string say_hello{"在"};
  std::string keywords_file{"assets/keywords.txt"};
  std::string tokens_path{"assets/tokens.txt"};
  std::string encoder_path{"assets/encoder.onnx"};
  std::string decoder_path{"assets/decoder.onnx"};
  std::string joiner_path{"assets/joiner.onnx"};
};

struct BudgetPreset {
  int audio_chunk_ms{20};
  int input_queue_frames{64};
  int output_queue_frames{128};
  int reconnect_backoff_min_ms{300};
  int reconnect_backoff_max_ms{4000};
};

struct ConversationPreset {
  // Time to wait for the next user turn after the assistant finishes speaking.
  int idle_seconds{30};
  bool smart_turn{false};
  int smart_turn_complete_ms{1500}, smart_turn_open_ms{4500};
  int absolute_timeout_seconds{0};  // 0: no absolute session limit
  std::string duplex_mode{"half"};
  uint64_t daily_token_limit{0};  // 0: no limit; usage is still recorded
  std::string usage_dir{"/data/xiaoai-plus/usage"};
  bool ordinary_barge_in{false};
  // Nominal paired capture buffer estimate (1440 / 16k = 90ms), not an ADC,
  // DAC or room measurement. Configurable while remaining in public 0..500.
  int barge_processing_io_delay_hint_ms{90};
};

struct StopKey {
  std::string device; // Empty until the physical play/pause mapping is verified.
  int key_code{-1};   // -1: disabled; no guessed KEY_PLAYPAUSE default.
};

struct Memory {
  bool enabled{false};
  std::string server_url;
  std::string device_secret;
  std::string device_secret_file;
  std::string device_id;
  bool secret_file_failed{false};
  std::string cache_dir{"/data/xiaoai-plus/memory"};
  bool identity_text_log{true};
  std::vector<std::string> child_names;
  std::string identity_question;
};

struct Config {
  Realtime realtime;
  AudioPreset audio;
  Wakeup wakeup;
  BudgetPreset budget;
  ConversationPreset conversation;
  Memory memory;
  StopKey stop_key;

  void normalize();
  void validate(bool memory_only = false) const;
};

Config load(const std::string& path, bool memory_only = false);

}  // namespace xiaoai_plus::config
