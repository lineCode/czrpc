#include "ServerConsolePCH.h"
#include "ServerConsoleCommand.h"

using namespace cz;


/* Parses a string as a command, where the first word is a command, followed by comma
separated parameters:
command param1,param2,param2
parameters can be numbers, strings, or file contents (contents of the file will be loaded):
command 100,100.5,"Hello", !"filename.txt"

returns the command and the parameters
*/
GenericCommand parseCommandParameters(const std::string& str)
{
	GenericCommand empty;
	GenericCommand cmd;

	std::string trimmed = cz::trim(str);
	std::stringstream ss(trimmed);
	std::string token;
	// read command
	if (!std::getline(ss, token, ' '))
		return std::move(empty);

	// Get connection name (if any) and command 
	{
		std::stringstream sstoken(token);
		if (token.find('.')!=std::string::npos)
		{
			if (!std::getline(sstoken, cmd.conName, '.'))
				return std::move(empty);
		}
		sstoken >> cmd.cmd;
		if (sstoken.peek()!= EOF)
			return std::move(empty);
	}

	// read all parameters
	while(std::getline(ss, token, ','))
	{
		token = cz::trim(token);
		std::stringstream sstoken(token);
		int i;
		std::string s;
		// first try to read integer (doesn't have a '.')
		// This is needed to differentiate between integer and float.
		int ch = sstoken.peek();
		if (ch>='0' && ch<='9')
		{
			float f;
			if (token.find('.')==std::string::npos && (sstoken >> i))
				cmd.params.emplace_back(i);
			else if (sstoken >> f)
				cmd.params.emplace_back(f);
			else
				return std::move(empty);
		}
		else if (ch=='"') // is a string
		{
			char tmpch;
			sstoken >> tmpch;
			if (!std::getline(sstoken, s, '"'))
				return std::move(empty);
			cmd.params.emplace_back(s.c_str());
			if (sstoken.peek()!= EOF)
				return std::move(empty);
		}
		else if (token=="false" || token=="FALSE")
		{
			cmd.params.emplace_back(false);
		}
		else if (token=="true" || token=="TRUE")
		{
			cmd.params.emplace_back(true);
		}
		else if (ch=='!') // it's a file's content
		{
			char tmpch;
			sstoken >> tmpch;
			if (sstoken.peek()=='"')
			{
				sstoken >> tmpch;
				if (!std::getline(sstoken, s, '"'))
					return std::move(empty);
			}
			else
			{
				sstoken >> s;
			}

			if (sstoken.peek()!=EOF)
				return std::move(empty);

			std::ifstream f(s.c_str(), std::ios::binary);
			if (!f.is_open())
				return std::move(empty);
			std::vector<uint8_t> contents((std::istreambuf_iterator<char>(f)),
				std::istreambuf_iterator<char>());
			cmd.params.emplace_back(std::move(contents));
		}

		else
			return std::move(empty);
	}

	return std::move(cmd);
}

typedef bool (*InternalCommandFunc)(const std::vector<cz::rpc::Any>&);
struct InternalCommand
{
	const char* shortName;
	const char* longName;
	InternalCommandFunc handler;
	const char* help;
};

bool cmd_Help(const std::vector<cz::rpc::Any>&);

bool cmd_Connect(const std::vector<cz::rpc::Any>& params)
{
	std::tuple<std::string> p;
	if (!toTuple(params, p))
	{
		std::cout << "Invalid number/type of parameters\n";
		return true;
	}

	auto tmp = splitAddress(std::get<0>(p));
	ConInfo::Addr addr;
	addr.ip = tmp.first;
	addr.port = tmp.second;

	for(auto&& it : gCons)
	{
		if (it.second->addr==addr)
		{
			std::cout << "There is already an active connection with address " << addr.to_string() << "\n";
			return true;
		}
	}

	auto con = TCPTransport<void, GenericServer>::create(
		*gIOService, addr.ip.c_str(), addr.port).get();

	if (!con)
	{
		std::cout << "Could not connect to specified address\n";
		return true;
	}

	// Try to get the name from the server (if any)
	auto res = CZRPC_CALLGENERIC(*con, "__getProperty", std::vector<Any>{Any("name")}).ft().get();
	if (!res.isValid())
	{
		std::cout << "Call to __getProperty failed\n";
		return true;
	}

	// If name property is not set, we give an autogenerated name to the connection
	std::string name(res.get().toString());
	static int counter = 0;

	while (name == "" || gCons.find(name) != gCons.end())
	{
		if (name == "")
		{
			name = "con" + std::to_string(counter++);
			std::cout << "Server did not provide a name. Trying auto generated name '" << name << "'\n";
		}
		else
		{
			std::cout << "An active connection with name '" << name << "' already exists.\n";
			name = "con" + std::to_string(counter++);
			std::cout << "Trying auto generated name '" << name << "'\n";
		}
	}

	std::cout << "Adding connection to " << addr.to_string() << " as '" << name << "'\n";
	auto conInfo = std::make_shared<ConInfo>();
	conInfo->name = name;
	conInfo->addr = addr;
	conInfo->con = con;
	gCons[name] = conInfo;
	con->setDisconnectSignal([info=conInfo.get()]
	{
		info->closed = true;
	});

	return true;
}

bool cmd_ShutdownConnection(const std::vector<cz::rpc::Any>& params)
{
	std::tuple<std::string> p;
	if (!toTuple(params, p))
	{
		std::cout << "Invalid number/type of parameters\n";
		return true;
	}

	for(auto it=gCons.begin(); it!=gCons.end(); it++)
	{
		if (it->second->name==std::get<0>(p))
		{
			std::cout << "Closing connection " << it->second->name << "\n";
			it->second->con->close();
			return true;
		}
	}

	std::cout << "No connection with name " << std::get<0>(p) << " found.\n";
	return true;
}

bool cmd_Resume(const std::vector<cz::rpc::Any>&)
{
	std::cout << "Resuming..." << std::endl;
	return true;
}

bool cmd_Quit(const std::vector<cz::rpc::Any>&)
{
	std::cout << "Quitting..." << std::endl;
	return false;
}

bool cmd_List(const std::vector<cz::rpc::Any>&)
{
	std::cout << "Connections list:" << std::endl;
	if (gCons.size())
	{
		for (auto& c : gCons)
		{
			std::cout << "    " << c.first << " @ " << c.second->addr.to_string() << std::endl;
		}
	}
	else
	{
		std::cout << "    No active connections";
	}

	std::cout << std::endl;
	return true;
}

InternalCommand gCmds[] =
{
	{
		"h", "help", &cmd_Help,
		"Display this help"
	},
	{
		"c", "connect", &cmd_Connect,
		"\n" \
		"    Connects to a server.\n" \
		"    Format: connect \"ip:port\""
	},
	{
		"x", "shutdown", &cmd_ShutdownConnection,
		"\n" \
		"    Closes the specified connection.\n" \
		"    Format: shutdown \"name\""
	},

	{
		"r", "resume", &cmd_Resume,
		"Exit command mode and continue"
	},
	{
		"l", "list", &cmd_List,
		"List all active connections"
	} ,
	{
		"q", "quit", &cmd_Quit,
		"Quit the application"
	}
};

bool cmd_Help(const std::vector<cz::rpc::Any>&)
{
	std::cout <<
		"Help\n" \
		"To enter a command, just type it. It will go into command mode.\n" \
		"Internal commands start with ':' and have the following format:\n" \
		"    :cmd p1, p2, ...\n" \
		"    Where p1,p2,... are any parameters required\n" \
		"Anything that doesn't start with a ':' is interpreted as an RPC, and has the following format:\n" \
		"    con_name.rpcname p1, p2, ...\n" \
		"    Where conname is the connection name, rpcname the rpc to call, and p1,p2,... the parameters\n" \
		"";
	std::cout << "List of internal commands" << std::endl;
	for(auto&& p : gCmds)
	{
		std::cout << "(" << p.shortName << ")" << p.longName << ": " << p.help << std::endl;
	}
	return true;
}

bool processCommand(const std::string& str)
{
		GenericCommand cmd = parseCommandParameters(str);
		if (cmd.cmd.size()==0)
		{
			std::cout << "INVALID COMMAND" << std::endl;
			return true;
		}

		// Process local command
		if (cmd.cmd[0]==':')
		{
			cmd.cmd.erase(cmd.cmd.begin());

			InternalCommand* cmdPtr = nullptr;
			for(auto&& p : gCmds)
			{
				if (cmd.cmd == p.shortName || cmd.cmd == p.longName)
					return p.handler(cmd.params);
			}
			
			std::cout << "Unknown local command (" << cmd.cmd << ")" << std::endl;
			return true;
		}

		// Process an RPC command
		if (cmd.conName=="")
		{
			std::cout << "Connection not specified. " << std::endl;
			return true;
		}

		auto it = gCons.find(cmd.conName);
		if (it == gCons.end())
		{
			std::cout << "Connection " << cmd.conName << " not found" << std::endl;
			return true;
		}

		CZRPC_CALLGENERIC(*it->second->con, cmd.cmd.c_str(), cmd.params).async(
			[funcName=cmd.cmd](Result<Any> res)
		{
			if (res.isAborted())
				return;

			if (res.isException())
			{
				std::cout << "RESPONSE FOR " << funcName << " : " << res.getException() << std::endl;
			}
			else
			{
				std::cout << "RESPONSE FOR " << funcName << " : " << res.get().toString() <<  std::endl;
			}
		});

		return true;
}



