#pragma once
#include <libremidi/backends/winmm/config.hpp>
#include <libremidi/backends/winmm/helpers.hpp>
#include <libremidi/detail/midi_out.hpp>

namespace libremidi
{

class midi_out_winmm final
    : public midi1::out_api
    , public error_handler
{
public:
  struct
      : output_configuration
      , winmm_output_configuration
  {
  } configuration;

  midi_out_winmm(output_configuration&& conf, winmm_output_configuration&& apiconf)
      : configuration{std::move(conf), std::move(apiconf)}
  {
    // We'll issue a warning here if no devices are available but not
    // throw an error since the user can plug something in later.
    if (midiOutGetNumDevs() == 0)
    {
      warning(
          configuration,
          "midi_out_winmm::initialize: no MIDI output devices currently "
          "available.");
    }
  }

  ~midi_out_winmm() override
  {
    // Close a connection if it exists.
    midi_out_winmm::close_port();
  }

  libremidi::API get_current_api() const noexcept override { return libremidi::API::WINDOWS_MM; }

  [[nodiscard]] std::error_code do_open(unsigned int portNumber)
  {
    MMRESULT result = midiOutOpen(&this->outHandle, portNumber, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR)
    {
      error<driver_error>(
          configuration,
          "midi_out_winmm::open_port: error creating Windows MM MIDI output "
          "port.");
      return from_mmerr(result);
    }

    return std::error_code{};
  }

  std::error_code open_port(const output_port& p, std::string_view) override
  {
    observer_winmm obs{{}, winmm_observer_configuration{}};
    auto ports = obs.get_output_ports();

    // First check with the display name, e.g. MIDI KEYBOARD 2 will match MIDI KEYBOARD 2
    for (auto& port : ports)
    {
      if (p.display_name == port.display_name)
        return do_open(port.port);
    }
    // If nothing is found, try to check with the raw name
    for (auto& port : ports)
    {
      if (p.port_name == port.port_name)
        return do_open(port.port);
    }
    error<invalid_parameter_error>(
        configuration, "midi_out_winmm::open_port: port not found: " + p.port_name);
    return std::make_error_code(std::errc::invalid_argument);
  }

  std::error_code close_port() override
  {
    if (this->outHandle)
      midiOutClose(this->outHandle);

    this->outHandle = nullptr;
    connected_ = false;
    return std::error_code{};
  }

  std::error_code send_message(const unsigned char* message, size_t size) override
  {
    if (!connected_)
      return std::make_error_code(std::errc::not_connected);

    if (size == 0)
    {
      warning(configuration, "midi_out_winmm::send_message: message argument is empty!");
      return std::make_error_code(std::errc::invalid_argument);
    }

    if (message[0] == 0xF0)
    { // Sysex message

      buffer.assign(message, message + size);

      // FIXME this can be made asynchronous... see Chrome source.
      // But need to know whe buffers are freed.

      // Create and prepare MIDIHDR structure.
      MIDIHDR sysex{};
      sysex.lpData = (LPSTR)buffer.data();
      sysex.dwBufferLength = size;
      sysex.dwFlags = 0;
      auto result = midiOutPrepareHeader(this->outHandle, &sysex, sizeof(MIDIHDR));
      if (result != MMSYSERR_NOERROR)
      {
        error<driver_error>(
            configuration, "midi_out_winmm::send_message: error preparing sysex header.");
        return from_mmerr(result);
      }

      // Send the message.
      result = midiOutLongMsg(this->outHandle, &sysex, sizeof(MIDIHDR));
      if (result != MMSYSERR_NOERROR)
      {
        error<driver_error>(
            configuration, "midi_out_winmm::send_message: error sending sysex message.");
        return from_mmerr(result);
      }

      // Unprepare the buffer and MIDIHDR.
      // FIXME yuck
      while (MIDIERR_STILLPLAYING
             == midiOutUnprepareHeader(this->outHandle, &sysex, sizeof(MIDIHDR)))
        Sleep(1);
    }
    else
    { // Channel or system message.

      // Make sure the message size isn't too big.
      if (size > 3)
      {
        warning(
            configuration,
            "midi_out_winmm::send_message: message size is greater than 3 bytes "
            "(and not sysex)!");
        return std::make_error_code(std::errc::message_size);
      }

      // Pack MIDI bytes into double word.
      DWORD packet;
      std::copy_n(message, size, (unsigned char*)&packet);

      // Send the message immediately.
      auto result = midiOutShortMsg(this->outHandle, packet);
      if (result != MMSYSERR_NOERROR)
      {
        error<driver_error>(
            configuration, "midi_out_winmm::send_message: error sending MIDI message.");
        return from_mmerr(result);
      }
    }
    return std::error_code{};
  }

private:
  HMIDIOUT outHandle; // Handle to Midi Output Device
  std::vector<char> buffer;
};

}
