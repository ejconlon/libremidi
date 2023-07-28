#pragma once
#include <libremidi/detail/midi_api.hpp>
#include <libremidi/output_configuration.hpp>

#include <string_view>

namespace libremidi
{

class midi_out_api : public midi_api
{
public:
  midi_out_api() = default;
  ~midi_out_api() override = default;
  midi_out_api(const midi_out_api&) = delete;
  midi_out_api(midi_out_api&&) = delete;
  midi_out_api& operator=(const midi_out_api&) = delete;
  midi_out_api& operator=(midi_out_api&&) = delete;

  virtual void send_message(const unsigned char* message, size_t size) = 0;
  virtual void send_ump(const uint32_t* message, size_t size) { }

  virtual int64_t current_time() const noexcept { return 0; }
  virtual void schedule_message(int64_t ts, const unsigned char* message, size_t size)
  {
    return send_message(message, size);
  }
  virtual void schedule_ump(int64_t ts, const uint32_t* ump, size_t size)
  {
    return send_ump(ump, size);
  }
};

template <typename T, typename Arg>
std::unique_ptr<midi_out_api> make(libremidi::output_configuration&& conf, Arg&& arg)
{
  return std::make_unique<T>(std::move(conf), std::move(arg));
}
}
