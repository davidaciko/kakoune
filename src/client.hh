#ifndef client_hh_INCLUDED
#define client_hh_INCLUDED

#include "display_buffer.hh"
#include "env_vars.hh"
#include "input_handler.hh"
#include "safe_ptr.hh"
#include "string.hh"
#include "utils.hh"

namespace Kakoune
{

class UserInterface;
class Window;

class Client : public SafeCountable
{
public:
    Client(std::unique_ptr<UserInterface>&& ui,
           std::unique_ptr<Window>&& window,
           SelectionList selections,
           EnvVarMap env_vars,
           String name);
    ~Client();

    // handle all the keys currently available in the user interface
    void handle_available_input();

    void print_status(DisplayLine status_line);

    void redraw_ifn();

    UserInterface& ui() const { return *m_ui; }
    Window& window() const { return *m_window; }

    void check_buffer_fs_timestamp();

    Context& context() { return m_input_handler.context(); }
    const Context& context() const { return m_input_handler.context(); }

    InputHandler& input_handler() { return m_input_handler; }
    const InputHandler& input_handler() const { return m_input_handler; }

    void change_buffer(Buffer& buffer);

    const String& get_env_var(const String& name) const;

private:
    DisplayLine generate_mode_line() const;

    std::unique_ptr<UserInterface> m_ui;
    std::unique_ptr<Window> m_window;

    EnvVarMap m_env_vars;

    InputHandler m_input_handler;

    DisplayLine m_status_line;
    DisplayLine m_pending_status_line;
    DisplayLine m_mode_line;
};

}

#endif // client_hh_INCLUDED
