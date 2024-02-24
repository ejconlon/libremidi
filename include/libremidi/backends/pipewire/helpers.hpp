#pragma once

#include <libremidi/backends/linux/helpers.hpp>
#include <libremidi/backends/linux/pipewire.hpp>
#include <libremidi/backends/pipewire/context.hpp>
#include <libremidi/detail/memory.hpp>
#include <libremidi/detail/midi_in.hpp>

#include <atomic>
#include <semaphore>
#include <stop_token>
#include <thread>

namespace libremidi
{
struct pipewire_helpers
{
  struct port
  {
    void* data{};
  };

  // All pipewire operations have to happen in the same thread
  // - and pipewire checks that internally.
  std::jthread main_loop_thread;
  const libpipewire& pw = libpipewire::instance();
  std::shared_ptr<pipewire_instance> global_instance;
  std::shared_ptr<pipewire_context> global_context;
  std::unique_ptr<pipewire_filter> filter;

  int64_t this_instance{};

  eventfd_notifier termination_event{};
  pollfd fds[2]{};

  pipewire_helpers()
  {
    static std::atomic_int64_t instance{};
    this_instance = ++instance;

    fds[1] = termination_event;
  }

  template <typename Self>
  int connect(Self& self)
  {
    if (this->filter)
      return 0;

    // Initialize PipeWire client
#if 0
    auto& configuration = self.configuration;
    if (configuration.context)
    {
      // FIXME case where user provides an existing filter

      if (!configuration.set_process_func)
        return -1;
      configuration.set_process_func(
          {.token = this_instance,
           .callback = [&self, p = std::weak_ptr{this->port.impl}](int nf) -> int {
             auto pt = p.lock();
             if (!pt)
               return 0;
             auto ppt = pt->load();
             if (!ppt)
               return 0;

             self.process(nf);

             self.check_client_released();
             return 0;
           }});

      this->client = configuration.context;
      return 0;
    }
    else
#endif
    {
      this->global_instance = std::make_shared<pipewire_instance>();
      this->global_context = std::make_shared<pipewire_context>(this->global_instance);
      this->filter = std::make_unique<pipewire_filter>(this->global_context);

      if constexpr (requires { self.process({}); })
      {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
      static constexpr struct pw_filter_events filter_events
          = {.version = PW_VERSION_FILTER_EVENTS,
             .process = +[](void* _data, struct spa_io_position* position) -> void {
               Self& self = *static_cast<Self*>(_data);
               self.process(position);
             }};
#pragma GCC diagnostic pop

      this->filter->create_filter(self.configuration.client_name, filter_events, &self);
      this->filter->start_filter();
      }
      return 0;
    }
    return 0;
  }

  template <typename Self>
  void disconnect(Self&)
  {
#if 0
    if (self.configuration.context)
    {
      if (self.configuration.clear_process_func)
      {
        self.configuration.clear_process_func(this_instance);
      }
    }
    else
#endif
    {
      termination_event.notify();
    }
  }

  void run_poll_loop()
  {
    // Note: called from a std::jthread.
    if (int fd = this->global_context->get_fd(); fd != -1)
    {
      fds[0] = {.fd = fd, .events = POLLIN, .revents = 0};

      for (;;)
      {
        if (int err = poll(fds, 2, -1); err < 0)
        {
          if (err == -EAGAIN)
            continue;
          else
            return;
        }

        // Check pipewire fd:
        if (fds[0].revents & POLLIN)
        {
          if (auto lp = this->global_context->lp)
          {
            int result = pw_loop_iterate(lp, 0);
            if (result < 0)
              std::cerr << "pw_loop_iterate: " << spa_strerror(result) << "\n";
          }
          fds[0].revents = 0;
        }

        // Check exit fd:
        if (fds[1].revents & POLLIN)
        {
          break;
        }
      }
    }
  }

  template <typename Self>
  bool create_local_port(Self& self, std::string_view portName, spa_direction direction)
  {
    assert(this->filter);

    if (portName.empty())
      portName = direction == SPA_DIRECTION_INPUT ? "i" : "o";

    if (!this->filter->port)
    {
      this->filter->create_local_port(portName.data(), direction);
    }

    if (!this->filter->port)
    {
      self.template error<driver_error>(self.configuration, "PipeWire: error creating port");
      return false;
    }
    return true;
  }

  void add_callbacks(const observer_configuration& conf)
  {
    assert(global_context);
    global_context->on_port_added = [&conf](const pipewire_context::port_info& port) {
      if (port.format.find("midi") == std::string::npos)
        return;

      bool unfiltered = conf.track_any;
      unfiltered |= (port.physical && conf.track_hardware);
      unfiltered |= (!port.physical && conf.track_virtual);
      if (unfiltered)
      {
        if (port.direction == SPA_DIRECTION_INPUT)
        {
          if (conf.output_added)
            conf.output_added(to_port_info<SPA_DIRECTION_INPUT>(port));
        }
        else
        {
          if (conf.input_added)
            conf.input_added(to_port_info<SPA_DIRECTION_OUTPUT>(port));
        }
      }
    };

    global_context->on_port_removed = [&conf](const pipewire_context::port_info& port) {
      if (port.format.find("midi") == std::string::npos)
        return;

      bool unfiltered = conf.track_any;
      unfiltered |= (port.physical && conf.track_hardware);
      unfiltered |= (!port.physical && conf.track_virtual);
      if (unfiltered)
      {
        if (port.direction == SPA_DIRECTION_INPUT)
        {
          if (conf.output_removed)
            conf.output_removed(to_port_info<SPA_DIRECTION_INPUT>(port));
        }
        else
        {
          if (conf.input_removed)
            conf.input_removed(to_port_info<SPA_DIRECTION_OUTPUT>(port));
        }
      }
    };
  }

  void start_thread()
  {
    main_loop_thread = std::jthread{[this]() { run_poll_loop(); }};
  }

  void stop_thread()
  {
    if (main_loop_thread.joinable())
    {
      termination_event.notify();
      main_loop_thread.request_stop();
      main_loop_thread.join();
    }
  }

  void do_close_port()
  {
    if (!this->filter)
      return;
    if (!this->filter->port)
      return;

    this->filter->remove_port();
  }

  void rename_port(std::string_view port_name)
  {
    if (this->filter)
      this->filter->rename_port(port_name);
  }

  bool link_ports(auto& self, const input_port& in_port)
  {
    // Wait for the pipewire server to send us back our node's info
    for (int i = 0; i < 1000; i++)
      this->filter->synchronize_node();

    auto this_node = this->filter->filter_node_id();
    auto& midi = this->global_context->current_graph.software_midi;
    auto node_it = midi.find(this_node);
    if (node_it == midi.end())
    {
      std::cerr << "Node " << this_node << " not found! \n";
      return false;
    }

    // Wait for the pipewire server to send us back our node's ports
    this->filter->synchronize_ports(node_it->second);

    if (node_it->second.inputs.empty())
    {
      std::cerr << "Node " << this_node << " has no ports! \n";
      return false;
    }

    // Link ports
    const auto& p = node_it->second.inputs.front();
    auto link = this->global_context->link_ports(in_port.port, p.id);
    pw_loop_iterate(this->global_context->lp, 1);
    if (!link)
    {
      self.template error<invalid_parameter_error>(
          self.configuration,
          "PipeWire: could not connect to port: " + in_port.port_name + " -> " + p.port_name);
      return false;
    }

    return true;
  }

  bool link_ports(auto& self, const output_port& out_port)
  {
    // Wait for the pipewire server to send us back our node's info
    for (int i = 0; i < 1000; i++)
      this->filter->synchronize_node();

    auto this_node = this->filter->filter_node_id();
    auto& midi = this->global_context->current_graph.software_midi;
    auto node_it = midi.find(this_node);
    if (node_it == midi.end())
    {
      std::cerr << "Node " << this_node << " not found! \n";
      return false;
    }

    // Wait for the pipewire server to send us back our node's ports
    this->filter->synchronize_ports(node_it->second);

    if (node_it->second.outputs.empty())
    {
      std::cerr << "Node " << this_node << " has no ports! \n";
      return false;
    }

    // Link ports
    const auto& p = node_it->second.outputs.front();
    auto link = this->global_context->link_ports(p.id, out_port.port);
    pw_loop_iterate(this->global_context->lp, 1);
    if (!link)
    {
      self.template error<invalid_parameter_error>(
          self.configuration,
          "PipeWire: could not connect to port: " + p.port_name + " -> " + out_port.port_name);
      return false;
    }

    return true;
  }

  template <spa_direction Direction>
  static auto to_port_info(const pipewire_context::port_info& port)
      -> std::conditional_t<Direction == SPA_DIRECTION_OUTPUT, input_port, output_port>
  {
    std::string device_name, port_name;
    auto name_colon = port.port_alias.find(':');
    if (name_colon != std::string::npos)
    {
      device_name = port.port_alias.substr(0, name_colon);
      port_name = port.port_alias.substr(name_colon + 1);
    }
    else
    {
      port_name = port.port_alias;
    }

    return {{
        .client = 0,
        .port = port.id,
        .manufacturer = "",
        .device_name = device_name,
        .port_name = port.port_name,
        .display_name = port_name,
    }};
  }

  // Note: keep in mind that an "input" port for us (e.g. a keyboard that goes to the computer)
  // is an "output" port from the point of view of pipewire as data will come out of it
  template <spa_direction Direction>
  static auto get_ports(const pipewire_context& ctx) noexcept -> std::vector<
      std::conditional_t<Direction == SPA_DIRECTION_OUTPUT, input_port, output_port>>
  {
    std::vector<std::conditional_t<Direction == SPA_DIRECTION_OUTPUT, input_port, output_port>>
        ret;

    {
      std::lock_guard _{ctx.current_graph.mtx};
      for (auto& node : ctx.current_graph.physical_midi)
      {
        for (auto& p :
             (Direction == SPA_DIRECTION_INPUT ? node.second.inputs : node.second.outputs))
        {
          ret.push_back(to_port_info<Direction>(p));
        }
      }
      for (auto& node : ctx.current_graph.software_midi)
      {
        for (auto& p :
             (Direction == SPA_DIRECTION_INPUT ? node.second.inputs : node.second.outputs))
        {
          ret.push_back(to_port_info<Direction>(p));
        }
      }
    }

    return ret;
  }
};
}
