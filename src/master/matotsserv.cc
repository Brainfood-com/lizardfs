#include "common/platform.h"
#include "master/matotsserv.h"

#include <syslog.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <list>
#include <memory>

#include "common/cfg.h"
#include "common/cwrap.h"
#include "common/exceptions.h"
#include "common/input_packet.h"
#include "common/main.h"
#include "common/matots_communication.h"
#include "common/media_label.h"
#include "common/network_address.h"
#include "common/output_packet.h"
#include "common/packet.h"
#include "common/slogger.h"
#include "common/sockets.h"
#include "common/time_utils.h"
#include "common/tstoma_communication.h"
#include "master/filesystem.h"
#include "master/personality.h"

/// Maximum allowed length of a network packet
static constexpr uint32_t kMaxPacketSize = 500000000;

/// Communication timeout
static constexpr uint32_t kTapeserverTimeout_ms = 30000;

namespace {

struct matotsserventry {
	enum class Mode { kKill, kConnected};

	matotsserventry(int sock, TapeserverId id, NetworkAddress address)
			: mode(Mode::kConnected),
			  sock(sock),
			  pdescpos(-1),
			  inputPacket(kMaxPacketSize),
			  name("(unregistered)"),
			  label(kMediaLabelWildcard),
			  id(id),
			  address(address),
			  version(0),
			  filesRegistered(false) {
	}

	~matotsserventry() {
		if (sock >= 0) {
			tcpclose(sock);
		}
	}

	Mode mode;
	int sock;
	int32_t pdescpos;
	Timer lastRead, lastWrite;
	InputPacket inputPacket;
	std::list<OutputPacket> outputPackets;

	std::string name;
	MediaLabel label;
	TapeserverId id; // ID of the tapeserver
	NetworkAddress address;  // IP of the tapeserver (port not present there)
	uint32_t version;  // version of the tapeserver
	bool filesRegistered;  // set to true during a registration after registering all the files
};

} // anonymous namespace

// from config
static std::string gListenHost;
static std::string gListenPort;

// socket which we listen on and its position in the global table of descriptors
static int gListenSocket;
static int32_t gListenSocketPosition;

/// A pool od IDs for tapeservers
static TapeserverIdPool gIdPool(200);

/// All connected tapeservers.
static std::list<std::unique_ptr<matotsserventry>> gTapeservers;

/// Queue of files intended to be sent to tapeserver.
static std::vector<TapeKey> gFilesToBeSentToTapeserver;

/// Add a packet to tapeserver's message queue.
static void matotsserv_createpacket(matotsserventry* eptr, MessageBuffer message) {
	eptr->outputPackets.emplace_back(std::move(message));
}

/// Disconnects a tapeserver.
static void matotsserv_kill(matotsserventry* eptr) {
	lzfs_pretty_syslog(LOG_WARNING,
			"disconnecting tapeserver %s due to an error",
			eptr->address.toString().c_str());
	eptr->mode = matotsserventry::Mode::kKill;
}

/// Handler for tstoma::registerServer.
static void matotsserv_register_tapeserver(matotsserventry* eptr, const MessageBuffer& message) {
	if (eptr->version > 0) {
		lzfs_pretty_syslog(LOG_WARNING,
				"got register message from a registered tapeserver");
		matotsserv_kill(eptr);
		return;
	}

	uint32_t version;
	tstoma::registerTapeserver::deserialize(message, version);
	if (version == 0) {
		lzfs_pretty_syslog(LOG_INFO,
				"tapeserver %s failed to register",
				eptr->address.toString().c_str());
		uint8_t status = ERROR_EPERM;
		matotsserv_createpacket(eptr, matots::registerTapeserver::build(status));
	} else {
		lzfs_pretty_syslog(LOG_INFO,
				"tapeserver %s registered",
				eptr->address.toString().c_str());
		eptr->version = version;
		// TODO(msulikowski) registering by name
		// TODO(msulikowski) registration of labels
		eptr->name = "tapeserver" + std::to_string(eptr->id.value());
		eptr->label = kMediaLabelWildcard;
		uint32_t myVersion = LIZARDFS_VERSHEX;
		matotsserv_createpacket(eptr, matots::registerTapeserver::build(myVersion));
	}
}

/// Handler for tstoma::hasFiles.
static void matotsserv_has_files(matotsserventry* eptr, const MessageBuffer& message) {
	std::vector<TapeKey> tapeContents;
	tstoma::hasFiles::deserialize(message, tapeContents);
	for (auto& key : tapeContents) {
		fs_add_tape_copy(key, eptr->id);
	}
}

/// Handler for tstoma::endOfFiles
static void matotsserv_end_of_files(matotsserventry* eptr, const MessageBuffer& message) {
	tstoma::endOfFiles::deserialize(message);
	lzfs_pretty_syslog(LOG_INFO,
			"tapeserver %s has finished registration",
			eptr->address.toString().c_str());
	eptr->filesRegistered = true;
}

/// Dispatches a received message
static void matotsserv_gotpacket(matotsserventry* eptr,
		PacketHeader header,
		const MessageBuffer& data) {
	try {
		switch (header.type) {
			case ANTOAN_NOP:
				break;
			case LIZ_TSTOMA_REGISTER_TAPESERVER:
				matotsserv_register_tapeserver(eptr, data);
				break;
			case LIZ_TSTOMA_HAS_FILES:
				matotsserv_has_files(eptr, data);
				break;
			case LIZ_TSTOMA_END_OF_FILES:
				matotsserv_end_of_files(eptr, data);
				break;
			default:
				syslog(LOG_NOTICE,
						"master <-> tapeservers module: got unknown message from %s, type:%" PRIu32,
						eptr->address.toString().c_str(),
						header.type);
				matotsserv_kill(eptr);
				break;
		}
	} catch (IncorrectDeserializationException& e) {
		syslog(LOG_WARNING,
				"master <-> tapeservers module: got inconsistent message from %s"
				"(type:%" PRIu32 ", length:%" PRIu32"), %s",
				eptr->address.toString().c_str(), header.type, header.length, e.what());
		matotsserv_kill(eptr);
	}
}

/// Read from the given tapeserver.
static void matotsserv_read(matotsserventry* eptr) {
	for (;;) {
		uint32_t bytesToRead = eptr->inputPacket.bytesToBeRead();
		ssize_t ret = read(eptr->sock, eptr->inputPacket.pointerToBeReadInto(), bytesToRead);
		if (ret == 0) {
			lzfs_silent_syslog(LOG_NOTICE, "connection with TS(%s) has been closed by peer",
					eptr->address.toString().c_str());
			eptr->mode = matotsserventry::Mode::kKill;
			return;
		} else if (ret < 0) {
			if (errno != EAGAIN) {
				lzfs_silent_errlog(LOG_NOTICE, "read from TS(%s) error",
						eptr->address.toString().c_str());
				matotsserv_kill(eptr);
			}
			return;
		}

		try {
			eptr->inputPacket.increaseBytesRead(ret);
		} catch (InputPacketTooLongException& ex) {
			lzfs_silent_syslog(LOG_WARNING, "reading from TS(%s): %s",
					eptr->address.toString().c_str(), ex.what());
			matotsserv_kill(eptr);
			return;
		}
		if (ret == bytesToRead && !eptr->inputPacket.hasData()) {
			// there might be more data to read in socket's buffer
			continue;
		} else if (!eptr->inputPacket.hasData()) {
			return;
		}

		matotsserv_gotpacket(eptr, eptr->inputPacket.getHeader(), eptr->inputPacket.getData());
		eptr->inputPacket.reset();
	}
}

/// Write to the given tapeserver.
static void matotsserv_write(matotsserventry* eptr) {
	while (!eptr->outputPackets.empty()) {
		OutputPacket& pack = eptr->outputPackets.front();
		ssize_t ret = write(eptr->sock,
				pack.packet.data() + pack.bytesSent,
				pack.packet.size() - pack.bytesSent);
		if (ret < 0) {
			if (errno != EAGAIN) {
				lzfs_silent_errlog(LOG_NOTICE,
						"write to TS(%s) error",
						eptr->address.toString().c_str());
				matotsserv_kill(eptr);
			}
			return;
		}
		pack.bytesSent += ret;
		if (pack.packet.size() != pack.bytesSent) {
			return;
		}
		eptr->outputPackets.pop_front();
	}
}

/// For \p main_pollregister.
static void matotsserv_desc(struct pollfd* pdesc, uint32_t* ndesc) {
	uint32_t pos = *ndesc;
	pdesc[pos].fd = gListenSocket;
	pdesc[pos].events = POLLIN;
	gListenSocketPosition = pos;
	pos++;

	for (auto& eptr : gTapeservers) {
		pdesc[pos].fd = eptr->sock;
		pdesc[pos].events = POLLIN;
		eptr->pdescpos = pos;
		if (!eptr->outputPackets.empty()) {
			pdesc[pos].events |= POLLOUT;
		}
		pos++;
	}
	*ndesc = pos;
}

/// For \p main_pollregister.
static void matotsserv_serve(struct pollfd* pdesc) {
	if (gListenSocketPosition >= 0 && (pdesc[gListenSocketPosition].revents & POLLIN)) {
		int newSocket = tcpaccept(gListenSocket);
		if (newSocket < 0) {
			lzfs_silent_errlog(LOG_NOTICE, "master <-> tapeservers module: accept error");
		} else if (metadataserver::isMaster()) {
			uint32_t ip = 0;
			tcpnonblock(newSocket);
			tcpnodelay(newSocket);
			tcpgetpeer(newSocket, &ip, NULL);
			NetworkAddress address(ip, 0);
			try {
				TapeserverId id = gIdPool.get();
				gTapeservers.emplace_back(new matotsserventry(newSocket, id, address));
			} catch (IdPoolException&) {
				lzfs_pretty_syslog(LOG_WARNING,
						"master <-> tapeservers module: too many tapeservers registered, "
						"refusing a new connection from %s",
						address.toString().c_str());
				tcpclose(newSocket);
			}
		} else {
			tcpclose(newSocket);
		}
	}
	for (auto& eptr : gTapeservers) {
		if (eptr->pdescpos >= 0) {
			if (pdesc[eptr->pdescpos].revents & (POLLERR|POLLHUP)) {
				matotsserv_kill(eptr.get());
			}
			if ((pdesc[eptr->pdescpos].revents & POLLIN)
					&& eptr->mode != matotsserventry::Mode::kKill) {
				eptr->lastRead.reset();
				matotsserv_read(eptr.get());
			}
			if ((pdesc[eptr->pdescpos].revents & POLLOUT)
					&& eptr->mode != matotsserventry::Mode::kKill) {
				eptr->lastWrite.reset();
				matotsserv_write(eptr.get());
			}
		}
		if (eptr->lastRead.elapsed_ms() > kTapeserverTimeout_ms) {
			lzfs_pretty_syslog(LOG_INFO,
					"master <-> tapeservers module: TS(%s) timed out",
					eptr->address.toString().c_str());
			matotsserv_kill(eptr.get());
		}
		if (eptr->lastWrite.elapsed_ms() > (kTapeserverTimeout_ms / 4)
				&& eptr->outputPackets.empty()) {
			matotsserv_createpacket(eptr.get(), buildMooseFsPacket(ANTOAN_NOP));
		}
	}

	// Remove disconnected clients, close their sockets
	auto it = gTapeservers.begin();
	while (it != gTapeservers.end()) {
		auto& eptr = *it;
		if (eptr->mode == matotsserventry::Mode::kKill) {
			fs_tapeserver_disconnected(eptr->id);
			gIdPool.put(eptr->id);
			it = gTapeservers.erase(it);
		} else {
			++it;
		}
	}
}

/// Called periodically to flush queues to tapeservers.
static void matotsserv_periodic_put_files() {
	if (gTapeservers.empty() || gFilesToBeSentToTapeserver.empty()) {
		return;
	}
	matotsserv_createpacket(
			gTapeservers.front().get(),
			matots::putFiles::build(gFilesToBeSentToTapeserver));
	gFilesToBeSentToTapeserver.clear();
}

/// Terminates the module.
static void matotsserv_term() {
	syslog(LOG_INFO,
			"master <-> tapeservers module: closing socket %s:%s",
			gListenHost.c_str(),
			gListenPort.c_str());
	tcpclose(gListenSocket);
	gTapeservers.clear();
}

/// Reads those parts of configuration which are just stored in global variables.
static void matotsserv_read_config_file() {
	// nothing yet
}

/// Reloads configuration of this module.
static void matotsserv_reload() {
	matotsserv_read_config_file();

	// Reload socket's address
	std::string listenHost = cfg_getstring("MATOTS_LISTEN_HOST", "*");
	std::string listenPort = cfg_getstring("MATOTS_LISTEN_PORT", "9424");
	if (listenHost == gListenHost && listenPort == gListenPort) {
		lzfs_pretty_syslog(LOG_NOTICE,
				"master <-> tapeservers module: socket address hasn't changed (%s:%s)",
				listenHost.c_str(), listenPort.c_str());
		return;
	}

	int newSocket = tcpsocket();
	if (newSocket < 0) {
		lzfs_pretty_errlog(LOG_WARNING,
				"master <-> tapeservers module: socket address has changed to (%s:%s), "
				"but can't create new socket",
				listenHost.c_str(), listenPort.c_str());
		return;
	}
	tcpnonblock(newSocket);
	tcpnodelay(newSocket);
	tcpreuseaddr(newSocket);
	if (tcpsetacceptfilter(newSocket) < 0 && errno != ENOTSUP) {
		lzfs_silent_errlog(LOG_NOTICE,"master <-> tapeservers module: can't set accept filter");
	}
	if (tcpstrlisten(newSocket, listenHost.c_str(), listenPort.c_str(),100)<0) {
		lzfs_pretty_errlog(LOG_ERR,
				"master <-> tapeservers module: socket address has changed, "
				"but can't listen on the new socket (%s:%s)",
				listenHost.c_str(), listenPort.c_str());
		tcpclose(newSocket);
		return;
	}
	tcpclose(gListenSocket);
	gListenSocket = newSocket;
	gListenHost = listenHost;
	gListenPort = listenPort;
	lzfs_pretty_syslog(LOG_NOTICE,
			"master <-> tapeservers module: socket address has changed, now listen on %s:%s",
			listenHost.c_str(), listenPort.c_str());
}

/// Called when personality is changed from shadow to master.
static void matotsserv_become_master() {
	main_timeregister(TIMEMODE_RUN_LATE, 1, 0, matotsserv_periodic_put_files);
}

int matotsserv_init() {
	matotsserv_read_config_file();

	gListenHost = cfg_getstring("MATOTS_LISTEN_HOST", "*");
	gListenPort = cfg_getstring("MATOTS_LISTEN_PORT", "9424");

	gListenSocket = tcpsocket();
	if (gListenSocket < 0) {
		throw InitializeException(
				"master <-> tapeservers module: can't create socket: " + errorString(errno));
	}
	tcpnonblock(gListenSocket);
	tcpnodelay(gListenSocket);
	tcpreuseaddr(gListenSocket);
	if (tcpsetacceptfilter(gListenSocket) < 0 && errno != ENOTSUP) {
		lzfs_silent_errlog(LOG_NOTICE, "master <-> tapeservers module: can't set accept filter");
	}
	if (tcpstrlisten(gListenSocket, gListenHost.c_str(), gListenPort.c_str(), 100) < 0) {
		throw InitializeException(
				"master <-> tapeservers module: can't listen on " +
				gListenHost + ":" + gListenPort + ": " + errorString(errno));
	}
	lzfs_pretty_syslog(LOG_NOTICE,
			"master <-> tapeservers module: listen on (%s:%s)",
			gListenHost.c_str(),
			gListenPort.c_str());

	main_reloadregister(matotsserv_reload);
	main_destructregister(matotsserv_term);
	main_pollregister(matotsserv_desc, matotsserv_serve);
	metadataserver::registerFunctionCalledOnPromotion(matotsserv_become_master);
	if (metadataserver::isMaster()) {
		matotsserv_become_master();
	}
	return 0;
}

TapeserverId matotsserv_enqueue_node(const TapeKey& key) {
	if (gTapeservers.empty()) {
		return TapeserverIdPool::nullId();
	}
	gFilesToBeSentToTapeserver.push_back(key);
	return gTapeservers.front().get()->id;
}

uint8_t matotsserv_get_tapeserver_info(TapeserverId id, TapeserverListEntry& tapeserverInfo) {
	for (auto& tapeserver : gTapeservers) {
		if (tapeserver->id == id) {
			tapeserverInfo.version = tapeserver->version;
			tapeserverInfo.server = tapeserver->name;
			tapeserverInfo.label = tapeserver->label;
			tapeserverInfo.address = tapeserver->address;
			return STATUS_OK;
		}
	}
	return ERROR_ENOENT;
}

std::vector<TapeserverListEntry> matotsserv_get_tapeservers() {
	std::vector<TapeserverListEntry> tapeservers;
	for (auto& tapeserver : gTapeservers) {
		tapeservers.emplace_back(tapeserver->version, tapeserver->name, tapeserver->label,
		        tapeserver->address);
	}
	return tapeservers;
}
