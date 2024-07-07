#include "nabu_media_player.h"

#ifdef USE_ESP_IDF

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace nabu {

static const char *const TAG = "nabu_media_player";
static const size_t TRANSFER_BUFFER_SIZE = 8192;

void NabuMediaPlayer::setup() {
  state = media_player::MEDIA_PLAYER_STATE_IDLE;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this->transfer_buffer_ = allocator.allocate(TRANSFER_BUFFER_SIZE * sizeof(int16_t));
  if (this->transfer_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate transfer buffer");
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "Set up nabu media player");
}

void NabuMediaPlayer::watch_() {
  TaskEvent event;

  if (this->announcement_streamer_ != nullptr) {
    while (this->announcement_streamer_->read_event(&event)) {
      switch (event.type) {
        case EventType::STARTING:
          ESP_LOGD(TAG, "Starting Announcement Playback");
          break;
        case EventType::STARTED:
          ESP_LOGD(TAG, "Started Announcement Playback");
          break;
        case EventType::IDLE:
          break;
        case EventType::RUNNING:
          this->status_clear_warning();
          break;
        case EventType::STOPPING:
          ESP_LOGD(TAG, "Stopping Announcement Playback");
          break;
        case EventType::STOPPED: {
          this->is_announcing_ = false;
          this->announcement_streamer_->stop();
          CommandEvent command_event;
          command_event.command = CommandEventType::DUCK;
          command_event.duck_bits = 0;
          this->combine_streamer_->send_command(&command_event);
          if (this->is_paused_) {
            this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
            this->publish_state();
          } else if (this->has_http_media_data_()) {
            this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
            this->publish_state();
          } else {
            this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
            this->publish_state();
          }
          ESP_LOGD(TAG, "Stopped Announcement Playback");
          break;
        }
        case EventType::WARNING:
          ESP_LOGW(TAG, "Error reading announcement media: %s", esp_err_to_name(event.err));
          this->status_set_warning();
          break;
      }
    }
  }

  if (this->media_streamer_ != nullptr) {
    while (this->media_streamer_->read_event(&event)) {
      switch (event.type) {
        case EventType::STARTING:
          ESP_LOGD(TAG, "Starting HTTP Media Playback");
          break;
        case EventType::STARTED:
          ESP_LOGD(TAG, "Started HTTP Media Playback");
          break;
        case EventType::IDLE:
          this->is_connected_ = false;
          break;
        case EventType::RUNNING:
          this->is_connected_ = true;
          this->status_clear_warning();
          break;
        case EventType::STOPPING:
          ESP_LOGD(TAG, "Stopping HTTP Media Playback");
          break;
        case EventType::STOPPED:
          this->media_streamer_->stop();

          this->is_connected_ = false;
          this->is_paused_ = false;

          if (this->state != media_player::MEDIA_PLAYER_STATE_ANNOUNCING) {
            this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
            this->publish_state();
          }

          ESP_LOGD(TAG, "Stopped HTTP Media Playback");
          break;
        case EventType::WARNING:
          ESP_LOGW(TAG, "Error reading HTTP media: %s", esp_err_to_name(event.err));
          this->status_set_warning();
          break;
      }
    }
  }
}

void NabuMediaPlayer::loop() {
  this->watch_();

  if ((this->announcement_streamer_ != nullptr) && (this->announcement_streamer_->available() > 0)) {
    this->is_announcing_ = true;

    size_t bytes_to_read = this->combine_streamer_->announcement_free();
    size_t bytes_read = this->announcement_streamer_->read(this->transfer_buffer_, bytes_to_read);
    if (bytes_read > 0) {
      this->combine_streamer_->write_announcement(this->transfer_buffer_, bytes_read);
    }

    // size_t speaker_free_bytes = this->speaker_->free_bytes();
    // size_t bytes_read = this->announcement_streamer_->read(this->transfer_buffer_, speaker_free_bytes);

    // if (bytes_read > 0) {
    //   this->speaker_->write(this->transfer_buffer_, bytes_read);
    // }
    // return;
  }

  if (this->has_http_media_data_() && !this->is_paused_) {
    size_t bytes_to_read = this->combine_streamer_->media_free();
    size_t bytes_read = this->media_streamer_->read(this->transfer_buffer_, bytes_to_read);

    if (bytes_read > 0) {
      this->combine_streamer_->write_media(this->transfer_buffer_, bytes_read);
    }
  }

  if ((this->combine_streamer_ != nullptr) && (this->combine_streamer_->available() > 0)) {
    size_t speaker_free_bytes = this->speaker_->free_bytes();
    size_t bytes_read = this->combine_streamer_->read(this->transfer_buffer_, speaker_free_bytes);
    if (bytes_read > 0) {
      this->speaker_->write(this->transfer_buffer_, bytes_read);
    }
  }

  // if (this->has_http_media_data_() && !this->is_announcing_ && !this->is_paused_) {
  //   size_t bytes_to_read = std::min(this->speaker_->free_bytes(), TRANSFER_BUFFER_SIZE);
  //   size_t bytes_read = this->media_streamer_->read(this->transfer_buffer_, bytes_to_read);

  //   if (bytes_read > 0) {
  //     this->speaker_->write(this->transfer_buffer_, bytes_read);
  //   }
  // }
}

void NabuMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  CommandEvent command_event;
  if (call.get_media_url().has_value()) {
    std::string new_uri = call.get_media_url().value();

    if (call.get_announcement().has_value() && call.get_announcement().value()) {
      if (this->announcement_streamer_ == nullptr) {
        this->announcement_streamer_ = make_unique<HTTPStreamer>();
        if (this->combine_streamer_ == nullptr) {
          this->combine_streamer_ = make_unique<CombineStreamer>();
        }
      }

      this->announcement_streamer_->start();
      this->combine_streamer_->start();

      command_event.command = CommandEventType::DUCK;
      command_event.duck_bits = 3;
      this->combine_streamer_->send_command(&command_event);
      this->announcement_streamer_->set_current_uri(new_uri);

      command_event.command = CommandEventType::START;
      command_event.duck_bits = 0;
      this->announcement_streamer_->send_command(&command_event);

      this->is_announcing_ = true;
      this->state = media_player::MEDIA_PLAYER_STATE_ANNOUNCING;
      this->publish_state();
    } else {
      if (this->media_streamer_ == nullptr) {
        this->media_streamer_ = make_unique<HTTPStreamer>();
        if (this->combine_streamer_ == nullptr) {
          this->combine_streamer_ = make_unique<CombineStreamer>();
        }
      }

      this->media_streamer_->start();
      this->combine_streamer_->start();
      this->media_streamer_->set_current_uri(new_uri);

      command_event.command = CommandEventType::START;
      this->media_streamer_->send_command(&command_event);

      this->is_paused_ = false;
      if (this->state != media_player::MEDIA_PLAYER_STATE_ANNOUNCING) {
        this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
        this->publish_state();
      }
    }
    return;
  }

  // if (call.get_volume().has_value()) {
  //   set_volume_(call.get_volume().value());
  //   unmute_();
  // }

  if (call.get_command().has_value()) {
    switch (call.get_command().value()) {
      case media_player::MEDIA_PLAYER_COMMAND_PLAY:
        if (this->is_paused_) {
          this->is_paused_ = false;

          if (this->state != media_player::MEDIA_PLAYER_STATE_ANNOUNCING) {
            this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
            this->publish_state();
          }
        }
        break;
      case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
        this->is_paused_ = true;

        if (this->state != media_player::MEDIA_PLAYER_STATE_ANNOUNCING) {
          this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
          this->publish_state();
        }
        break;
      case media_player::MEDIA_PLAYER_COMMAND_STOP:
        command_event.command = CommandEventType::STOP;
        this->media_streamer_->send_command(&command_event);

        this->is_paused_ = false;

        if (this->state != media_player::MEDIA_PLAYER_STATE_ANNOUNCING) {
          this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
          this->publish_state();
        }
        break;
      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        break;
      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        break;
      case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
        if (this->is_paused_) {
          this->is_paused_ = false;

          if (this->state != media_player::MEDIA_PLAYER_STATE_ANNOUNCING) {
            this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
            this->publish_state();
          }
        } else {
          this->is_paused_ = true;

          if (this->state != media_player::MEDIA_PLAYER_STATE_ANNOUNCING) {
            this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
            this->publish_state();
          }
        }
        break;
        //   case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP: {
        //     //       float new_volume = this->volume + 0.1f;
        //     //       if (new_volume > 1.0f)
        //     //         new_volume = 1.0f;
        //     //       set_volume_(new_volume);
        //     //       unmute_();
        //     break;
        //   }
        //   case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN: {
        //     // float new_volume = this->volume - 0.1f;
        //     // if (new_volume < 0.0f)
        //     //   new_volume = 0.0f;
        //     // set_volume_(new_volume);
        //     // unmute_();
        //     break;
        //   }
      default:
        break;
    }
  }
}

bool NabuMediaPlayer::has_http_media_data_() {
  if (this->media_streamer_ == nullptr)
    return false;

  if ((this->media_streamer_->available() > 0) || this->is_connected_)
    return true;

  return false;
}

// pausing is only supported if destroy_pipeline_on_stop is disabled
media_player::MediaPlayerTraits NabuMediaPlayer::get_traits() {
  auto traits = media_player::MediaPlayerTraits();
  traits.set_supports_pause(true);
  return traits;
};

bool NabuMediaPlayer::read_wav_header_(esp_http_client_handle_t *client) {
  uint8_t header_control_counter = 0;
  char data[4];
  uint8_t subchunk_extra_data = 0;

  while (header_control_counter <= 10) {
    size_t bytes_read = esp_http_client_read(*client, data, 4);
    if (bytes_read != 4) {
      ESP_LOGE(TAG, "Failed to read from header file");
    }

    if (header_control_counter == 0) {
      ++header_control_counter;
      if ((data[0] != 'R') || (data[1] != 'I') || (data[2] != 'F') || (data[3] != 'F')) {
        ESP_LOGW(TAG, "file has no RIFF tag");
        return false;
      }
    } else if (header_control_counter == 1) {
      ++header_control_counter;
      uint32_t chunk_size = (uint32_t) (data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24));
    } else if (header_control_counter == 2) {
      ++header_control_counter;
      if ((data[0] != 'W') || (data[1] != 'A') || (data[2] != 'V') || (data[3] != 'E')) {
        ESP_LOGW(TAG, "format tag is not WAVE");
        return false;
      }
    } else if (header_control_counter == 3) {
      ++header_control_counter;
      if ((data[0] != 'f') || (data[1] != 'm') || (data[2] != 't')) {
        ESP_LOGW(TAG, "Improper wave file header");
        return false;
      }
    } else if (header_control_counter == 4) {
      ++header_control_counter;

      uint32_t chunk_size = (uint32_t) (data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24));

      if (chunk_size != 16) {
        ESP_LOGW(TAG, "Audio is not PCM data, can't play");
        return false;
      }
    } else if (header_control_counter == 5) {
      ++header_control_counter;

      uint32_t chunk_size = (uint32_t) (data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24));

      // if (chunk_size != 16) {
      //   ESP_LOGW(TAG, "Audio is not PCM data, can't play");
      //   return false;
      // }
    } else if (header_control_counter == 6) {
      ++header_control_counter;

      uint16_t fc = (uint16_t) (data[0] + (data[1] << 8));         // Format code
      uint16_t nic = (uint16_t) ((data[2] + 2) + (data[3] << 8));  // Number of interleaved channels

      // if (fc != 1) {
      //   ESP_LOGW(TAG, "Audio is not PCM data, can't play");
      //   return false;
      // }

      if (nic > 2) {
        ESP_LOGW(TAG, "Can only play mono or stereo channel audio");
        return false;
      }
      // this->stream_info_.channels = (speaker::AudioChannels) nic;
    } else if (header_control_counter == 7) {
      ++header_control_counter;

      uint32_t sample_rate = (uint32_t) (data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24));

      // this->stream_info_.sample_rate = sample_rate;
    } else if (header_control_counter == 8) {
      ++header_control_counter;

      uint32_t byte_rate = (uint32_t) (data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24));
    } else if (header_control_counter == 8) {
      ++header_control_counter;

      uint16_t block_align = (uint16_t) (data[0] + (data[1] << 8));
      uint16_t bits_per_sample = (uint16_t) ((data[2] + 2) + (data[3] << 8));

      if ((bits_per_sample != 16)) {
        ESP_LOGW(TAG, "Can only play wave flies with 16 bits per sample");
        return false;
      }
      // this->stream_info_.bits_per_sample = (speaker::AudioSamplesPerBit) bits_per_sample;
    } else if (header_control_counter == 9) {
      ++header_control_counter;
      if ((data[0] != 'd') || (data[1] != 'a') || (data[2] != 't') || (data[3] != 'a')) {
        ESP_LOGW(TAG, "Improper Wave Header");
        // The wave header for the TTS responses has some extra stuff before this; just ignore for now, but it causes
        // clicking at the start of playback return false;
      }
    } else if (header_control_counter == 10) {
      ++header_control_counter;
      uint32_t chunk_size = (uint32_t) (data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24));
    } else {
      ESP_LOGW(TAG, "Unknown state when reading wave file header");
      // The wave header for the TTS responses has some extra stuff before this; just ignore for now, but it causes
      // clicking at the start of playback
      // return false;
    }
  }

  return true;
}

}  // namespace nabu
}  // namespace esphome
#endif