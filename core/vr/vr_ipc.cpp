#include "vr_ipc.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace od
{
std::string ipc::socket_path()
{
	const char * runtime = getenv("XDG_RUNTIME_DIR");
	return std::string(runtime && *runtime ? runtime : "/tmp") + "/opendesk/vr.sock";
}

std::string ipc::abstract_name()
{
	return std::string(1, '\0') + "opendesk-" + std::to_string(getuid()) + "/vr";
}

bool ipc::same_user(int fd)
{
	ucred cred{};
	socklen_t len = sizeof(cred);
	return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.uid == getuid();
}

bool ipc::service_available()
{
	if (access(socket_path().c_str(), F_OK) == 0)
		return true;
	// abstract sockets of this network namespace show up as "@name" in the last column
	std::string want = "@" + abstract_name().substr(1);
	std::ifstream unix_sockets("/proc/net/unix");
	for (std::string line; std::getline(unix_sockets, line);)
		if (line.size() >= want.size() && line.compare(line.size() - want.size(), want.size(), want) == 0)
			return true;
	return false;
}
} // namespace od
