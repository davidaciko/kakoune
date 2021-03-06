#ifndef remote_hh_INCLUDED
#define remote_hh_INCLUDED

#include "display_buffer.hh"
#include "event_manager.hh"
#include "user_interface.hh"
#include "env_vars.hh"

namespace Kakoune
{

struct peer_disconnected {};

struct connection_failed : runtime_error
{
    connection_failed(const String& filename)
        : runtime_error{"connect to " + filename + " failed"}
    {}
};

// A remote client handle communication between a client running on the server
// and a user interface running on the local process.
class RemoteClient
{
public:
    RemoteClient(int socket, std::unique_ptr<UserInterface>&& ui,
                 const EnvVarMap& env_vars, const String& init_command);

private:
    void process_available_messages();
    void process_next_message();
    void write_next_key();

    std::unique_ptr<UserInterface> m_ui;
    CharCoord                      m_dimensions;
    FDWatcher                      m_socket_watcher;
};
std::unique_ptr<RemoteClient> connect_to(const String& session,
                                         std::unique_ptr<UserInterface>&& ui,
                                         const EnvVarMap& env_vars,
                                         const String& init_command);

void send_command(const String& session, const String& command);

struct Server : public Singleton<Server>
{
    Server(String session_name);
    ~Server();
    const String& session() const { return m_session; }

    void close_session();

private:
    class Accepter;
    void remove_accepter(Accepter* accepter);

    String m_session;
    std::unique_ptr<FDWatcher> m_listener;
    std::vector<std::unique_ptr<Accepter>> m_accepters;
};

}

#endif // remote_hh_INCLUDED

