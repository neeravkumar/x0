/* <src/x0d.cpp>
 *
 * This file is part of the x0 web server project and is released under GPL-3.
 * http://www.xzero.io/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 */

#include <x0/http/HttpServer.h>
#include <x0/http/HttpRequest.h>
#include <x0/http/HttpCore.h>
#include <x0/flow/FlowRunner.h>
#include <x0/Logger.h>
#include <x0/strutils.h>
#include <x0/Severity.h>

#include <boost/tokenizer.hpp>
#include <boost/lexical_cast.hpp>

#include <ev++.h>
#include <sd-daemon.h>

#include <functional>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <getopt.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h> // O_CLOEXEC

#if !defined(NDEBUG)
#	define X0D_DEBUG(msg...) XzeroHttpDaemon::log(x0::Severity::debug, msg)
#else
#	define X0D_DEBUG(msg...) /*!*/ ((void)0)
#endif

#define SIG_X0_SUSPEND (SIGRTMIN+4)
#define SIG_X0_RESUME (SIGRTMIN+5)

using x0::Severity;

// {{{ helper
static std::string getcwd()
{
	char buf[1024];
	return std::string(::getcwd(buf, sizeof(buf)));
}

static std::string& gsub(std::string& buf, const std::string& src, const std::string& dst)
{
	std::size_t i = buf.find(src);

	while (i != std::string::npos) {
		buf.replace(i, src.size(), dst);
		i = buf.find(src);
	}

	return buf;
}

static std::string& gsub(std::string& buf, const std::string& src, int dst)
{
	std::string tmp(boost::lexical_cast<std::string>(dst));
	return gsub(buf, src, tmp);
}

static std::string sig2str(int sig)
{
	static const char* sval[_NSIG + 1] = {
		nullptr,		// 0
		"SIGHUP",		// 1
		"SIGINT",		// 2
		"SIGQUIT",		// 3
		"SIGILL",		// 4
		"SIGTRAP",		// 5
		"SIGIOT",		// 6
		"SIGBUS",		// 7
		"SIGFPE",		// 8
		"SIGKILL",		// 9

		"SIGUSR1",		// 10
		"SIGSEGV",		// 11
		"SIGUSR2",		// 12
		"SIGPIPE",		// 13
		"SIGALRM",		// 14
		"SIGTERM",		// 15
		"SIGSTKFLT",	// 16
		"SIGCHLD",		// 17
		"SIGCONT",		// 18
		"SIGSTOP",		// 19

		"SIGTSTP",		// 20
		"SIGTTIN",		// 21
		"SIGTTOU",		// 22
		"SIGURG	",		// 23
		"SIGXCPU",		// 24
		"SIGXFSZ",		// 25
		"SIGVTALRM",	// 26
		"SIGPROF",		// 27
		"SIGWINCH",		// 28
		"SIGIO",		// 29

		"SIGPWR",		// 30
		"SIGSYS",		// 31
		nullptr
	};

	char buf[64];
	if (sig >= SIGRTMIN && sig <= SIGRTMAX) {
		snprintf(buf, sizeof(buf), "SIGRTMIN+%d (%d)", sig - SIGRTMIN, sig);
	} else {
		snprintf(buf, sizeof(buf), "%s (%d)", sval[sig], sig);
	}
	return buf;
}
// }}}

class XzeroHttpDaemon // {{{
{
private:
	/** concats a path with a filename and optionally inserts a path seperator if path 
	 *  doesn't contain a trailing path seperator. */
	static inline std::string pathcat(const std::string& path, const std::string& filename)
	{
		if (!path.empty() && path[path.size() - 1] != '/')
		{
			return path + "/" + filename;
		}
		else
		{
			return path + filename;
		}
	}

	enum class State {
		Inactive,
		Initializing,
		Running,
		Upgrading,
		GracefullyShuttingdown
	};

public:
	XzeroHttpDaemon(int argc, char *argv[]);
	~XzeroHttpDaemon();

	static XzeroHttpDaemon *instance() { return instance_; }

	int run();

private:
	bool createPidFile();
	bool parse();
	void setState(State newState);
	bool setupConfig();
	void daemonize();
	bool drop_privileges(const std::string& username, const std::string& groupname);

	void log(Severity severity, const char *msg, ...);

	void reopenLogsHandler(ev::sig&, int);
	void reexecHandler(ev::sig& sig, int);
	void onChild(ev::child&, int);
	void suspendHandler(ev::sig& sig, int);
	void resumeHandler(ev::sig& sig, int);
	void gracefulShutdownHandler(ev::sig& sig, int);
	void quickShutdownHandler(ev::sig& sig, int);
	void quickShutdownTimeout(ev::timer&, int);

private:
	State state_;
	int argc_;
	char** argv_;
	std::string configfile_;
	std::string pidfile_;
	std::string user_;
	std::string group_;

	std::string logFile_;
	Severity logLevel_;

	std::string instant_;
	std::string documentRoot_;

	int nofork_;
	int systemd_;
	int doguard_;
	int dumpIR_;
	int optimizationLevel_;
	struct ev_loop* loop_;
	x0::HttpServer *server_;
	ev::sig terminateSignal_;
	ev::sig ctrlcSignal_;
	ev::sig quitSignal_;
	ev::sig user1Signal_;
	ev::sig hupSignal_;
	ev::sig suspendSignal_;
	ev::sig resumeSignal_;
	ev::timer terminationTimeout_;
	ev::child child_;

	static XzeroHttpDaemon* instance_;
}; // }}}

// {{{
XzeroHttpDaemon::XzeroHttpDaemon(int argc, char *argv[]) :
	state_(State::Inactive),
	argc_(argc),
	argv_(argv),
	configfile_(pathcat(SYSCONFDIR, "x0d.conf")),
	pidfile_(),
	user_(),
	group_(),
	logFile_(pathcat(LOGDIR, "x0d.log")),
	logLevel_(Severity::info),
	instant_(),
	documentRoot_(),
	nofork_(false),
	systemd_(getppid() == 1),
	doguard_(false),
	dumpIR_(false),
	optimizationLevel_(2),
	loop_(ev_default_loop()),
	server_(nullptr),
	terminateSignal_(loop_),
	ctrlcSignal_(loop_),
	quitSignal_(loop_),
	user1Signal_(loop_),
	hupSignal_(loop_),
	terminationTimeout_(loop_),
	child_(loop_)
{
	setState(State::Initializing);
	x0::FlowRunner::initialize();

	instance_ = this;

	terminateSignal_.set<XzeroHttpDaemon, &XzeroHttpDaemon::quickShutdownHandler>(this);
	terminateSignal_.start(SIGTERM);
	ev_unref(loop_);

	ctrlcSignal_.set<XzeroHttpDaemon, &XzeroHttpDaemon::quickShutdownHandler>(this);
	ctrlcSignal_.start(SIGINT);
	ev_unref(loop_);

	quitSignal_.set<XzeroHttpDaemon, &XzeroHttpDaemon::gracefulShutdownHandler>(this);
	quitSignal_.start(SIGQUIT);
	ev_unref(loop_);

	user1Signal_.set<XzeroHttpDaemon, &XzeroHttpDaemon::reopenLogsHandler>(this);
	user1Signal_.start(SIGUSR1);
	ev_unref(loop_);

	hupSignal_.set<XzeroHttpDaemon, &XzeroHttpDaemon::reexecHandler>(this);
	hupSignal_.start(SIGHUP);
	ev_unref(loop_);

	suspendSignal_.set<XzeroHttpDaemon, &XzeroHttpDaemon::suspendHandler>(this);
	suspendSignal_.start(SIG_X0_SUSPEND);
	ev_unref(loop_);

	resumeSignal_.set<XzeroHttpDaemon, &XzeroHttpDaemon::resumeHandler>(this);
	resumeSignal_.start(SIG_X0_RESUME);
	ev_unref(loop_);
}

XzeroHttpDaemon::~XzeroHttpDaemon()
{
	if (terminationTimeout_.is_active()) {
		ev_ref(loop_);
		terminationTimeout_.stop();
	}

	if (terminateSignal_.is_active()) {
		ev_ref(loop_);
		terminateSignal_.stop();
	}

	if (ctrlcSignal_.is_active()) {
		ev_ref(loop_);
		ctrlcSignal_.stop();
	}

	if (quitSignal_.is_active()) {
		ev_ref(loop_);
		quitSignal_.stop();
	}

	if (user1Signal_.is_active()) {
		ev_ref(loop_);
		user1Signal_.stop();
	}

	if (hupSignal_.is_active()) {
		ev_ref(loop_);
		hupSignal_.stop();
	}

	if (suspendSignal_.is_active()) {
		ev_ref(loop_);
		suspendSignal_.stop();
	}

	if (resumeSignal_.is_active()) {
		ev_ref(loop_);
		resumeSignal_.stop();
	}

	delete server_;
	server_ = nullptr;

	instance_ = nullptr;

	x0::FlowRunner::shutdown();
}

namespace x0 {
	std::string global_now(); // defined in HttpServer.cpp
}

int XzeroHttpDaemon::run()
{
	::signal(SIGPIPE, SIG_IGN);

	unsigned generation = 1;
	if (const char* v = getenv("XZERO_UPGRADE")) {
		generation = atoi(v) + 1;
		unsetenv("XZERO_UPGRADE");
	}

	server_ = new x0::HttpServer(loop_, generation);
#ifndef NDEBUG
	server_->logLevel(x0::Severity::debug5);
#endif

	if (!parse())
		return 1;

	if (systemd_) {
		nofork_ = true;
		server_->setLogger(std::make_shared<x0::SystemdLogger>());
	} else {
		if (!logFile_.empty()) {
			auto nowfn = std::bind(&x0::global_now);
			server_->setLogger(std::make_shared<x0::FileLogger<decltype(nowfn)>>(logFile_, nowfn));
		} else
			server_->setLogger(std::make_shared<x0::SystemLogger>());
	}

	server_->logger()->setLevel(logLevel_);

	if (!setupConfig()) {
		log(x0::Severity::error, "Could not start x0d.");
		return -1;
	}

	unsetenv("XZERO_LISTEN_FDS");

	if (dumpIR_)
		server_->dumpIR();

	if (!nofork_)
		daemonize();

	if (!createPidFile())
		return -1;

	if (group_.empty())
		group_ = user_;

	if (!drop_privileges(user_, group_))
		return -1;

	setState(State::Running);

	int rv = server_->run();

	// remove PID-file, if exists and not in systemd-mode
	if (!systemd_ && !pidfile_.empty())
		unlink(pidfile_.c_str());

	return rv;
}

bool XzeroHttpDaemon::parse()
{
	struct option long_options[] =
	{
		{ "no-fork", no_argument, &nofork_, 1 },
		{ "fork", no_argument, &nofork_, 0 },
		{ "systemd", no_argument, &systemd_, 1 },
		{ "guard", no_argument, &doguard_, 'G' },
		{ "pid-file", required_argument, nullptr, 'p' },
		{ "user", required_argument, nullptr, 'u' },
		{ "group", required_argument, nullptr, 'g' },
		{ "log-file", required_argument, nullptr, 'l' },
		{ "log-level", required_argument, nullptr, 'L' },
		{ "instant", required_argument, nullptr, 'i' },
		{ "dump-ir", no_argument, &dumpIR_, 1 },
		//.
		{ "version", no_argument, nullptr, 'v' },
		{ "copyright", no_argument, nullptr, 'y' },
		{ "config", required_argument, nullptr, 'f' },
		{ "optimization-level", required_argument, &optimizationLevel_, 'O' },
		{ "help", no_argument, nullptr, 'h' },
		//.
		{ 0, 0, 0, 0 }
	};

	static const char *package_header = 
		"x0d: x0 web server, version " PACKAGE_VERSION " [" PACKAGE_HOMEPAGE_URL "]";
	static const char *package_copyright =
		"Copyright (c) 2009 by Christian Parpart <trapni@gentoo.org>";
	static const char *package_license =
		"Licensed under GPL-3 [http://gplv3.fsf.org/]";

	for (;;)
	{
		int long_index = 0;
		switch (getopt_long(argc_, argv_, "vyf:O:p:u:g:l:L:i:hXG", long_options, &long_index))
		{
			case 'p':
				pidfile_ = optarg;
				break;
			case 'f':
				configfile_ = optarg;
				break;
			case 'O':
				optimizationLevel_ = std::atoi(optarg);
				break;
			case 'g':
				group_ = optarg;
				break;
			case 'u':
				user_ = optarg;
				break;
			case 'l':
				logFile_ = optarg;
				break;
			case 'L':
				logLevel_ = static_cast<Severity>(std::max(std::min(9, atoi(optarg)), 0));
				break;
			case 'i':
				instant_ = optarg;
				break;
			case 'v':
				std::cout
					<< package_header << std::endl
					<< package_copyright << std::endl;
			case 'y':
				std::cout << package_license << std::endl;
				return false;
			case 'h':
				std::cout
					<< package_header << std::endl
					<< package_copyright << std::endl
					<< package_license << std::endl
					<< std::endl
					<< "usage:" << std::endl
					<< "  x0d [options ...]" << std::endl
					<< std::endl
					<< "options:" << std::endl
					<< "  -h,--help                 print this help" << std::endl
					<< "  -f,--config=PATH          specify a custom configuration file [" << configfile_ << "]" << std::endl
					<< "  -O,--optimization-level=N sets the configuration optimization level [" << optimizationLevel_ << "]" << std::endl
					<< "  -X,--no-fork              do not fork into background" << std::endl
					<< "     --systemd              force systemd-mode, which is auto-detected otherwise" << std::endl
					<< "  -G,--guard                do run service as child of a special guard process to watch for crashes" << std::endl
					<< "  -p,--pid-file=PATH        PID file to create" << std::endl
					<< "  -u,--user=NAME            user to drop privileges to" << std::endl
					<< "  -g,--group=NAME           group to drop privileges to" << std::endl
					<< "  -l,--log-file=PATH        path to log file (ignored when in systemd-mode)" << std::endl
					<< "  -L,--log-level=VALUE      log level, a value between 0 and 9 (default " << static_cast<int>(logLevel_) << ")" << std::endl
					<< "     --dump-ir              dumps LLVM IR of the configuration file (for debugging purposes)" << std::endl
					<< "  -i,--instant=PATH[,PORT]  run x0d in simple pre-configured instant-mode" << std::endl
					<< "  -v,--version              print software version" << std::endl
					<< "  -y,--copyright            print software copyright notice / license" << std::endl
					<< std::endl;
				return false;
			case 'X':
				nofork_ = true;
				break;
			case 'G':
				doguard_ = true;
				break;
			case 0: // long option with (val!=nullptr && flag=0)
				break;
			case -1: // EOF - everything parsed.
				return true;
			case '?': // ambiguous match / unknown arg
			default:
				return false;
		}
	}
}

void XzeroHttpDaemon::daemonize()
{
	if (::daemon(true /*no chdir*/, true /*no close*/) < 0)
	{
		throw std::runtime_error(x0::fstringbuilder::format("Could not daemonize process: %s", strerror(errno)));
	}
}

/** drops runtime privileges current process to given user's/group's name. */
bool XzeroHttpDaemon::drop_privileges(const std::string& username, const std::string& groupname)
{
	if (!groupname.empty() && !getgid()) {
		if (struct group *gr = getgrnam(groupname.c_str())) {
			if (setgid(gr->gr_gid) != 0) {
				log(Severity::error, "could not setgid to %s: %s", groupname.c_str(), strerror(errno));
				return false;
			}

			setgroups(0, nullptr);

			if (!username.empty()) {
				initgroups(username.c_str(), gr->gr_gid);
			}
		} else {
			log(Severity::error, "Could not find group: %s", groupname.c_str());
			return false;
		}
		X0D_DEBUG("Dropped group privileges to '%s'.", groupname.c_str());
	}

	if (!username.empty() && !getuid()) {
		if (struct passwd *pw = getpwnam(username.c_str())) {
			if (setuid(pw->pw_uid) != 0) {
				log(Severity::error, "could not setgid to %s: %s", username.c_str(), strerror(errno));
				return false;
			}
			log(Severity::info, "Dropped privileges to user %s", username.c_str());

			if (chdir(pw->pw_dir) < 0) {
				log(Severity::error, "could not chdir to %s: %s", pw->pw_dir, strerror(errno));
				return false;
			}
		} else {
			log(Severity::error, "Could not find group: %s", groupname.c_str());
			return false;
		}

		X0D_DEBUG("Dropped user privileges to '%s'.", username.c_str());
	}

	if (!::getuid() || !::geteuid() || !::getgid() || !::getegid())
	{
#if defined(X0_RELEASE)
		log(x0::Severity::error, "Service is not allowed to run with administrative permissionsService is still running with administrative permissions.");
		return false;
#else
		log(x0::Severity::warn, "Service is still running with administrative permissions.");
#endif
	}
	return true;
}

void XzeroHttpDaemon::log(Severity severity, const char *msg, ...)
{
	va_list va;
	char buf[4096];

	va_start(va, msg);
	vsnprintf(buf, sizeof(buf), msg, va);
	va_end(va);

	server_->log(severity, "%s", buf);
}

bool XzeroHttpDaemon::setupConfig()
{
	if (instant_.empty()) {
		// this is no instant-mode, so setup via configuration file.
		std::ifstream ifs(configfile_);
		return server_->setup(&ifs, configfile_, optimizationLevel_);
	}

	// --instant=docroot[,port[,bind]]

	typedef boost::tokenizer<boost::char_separator<char>> tokenizer;

	auto tokens = x0::split<std::string>(instant_, ",");
	documentRoot_ = tokens.size() > 0 ? tokens[0] : "";
	int port = tokens.size() > 1 ? boost::lexical_cast<int>(tokens[1]) : 0;
	std::string bind = tokens.size() > 2 ? tokens[2] : "";

	if (documentRoot_.empty())
		documentRoot_ = getcwd();

	if (!port)
		port = 8080;

	if (bind.empty())
		bind = "0.0.0.0"; //TODO: "0::0";

	std::string source(
		"import 'compress';\n"
		"import 'dirlisting';\n"
		"import 'cgi';\n"
		"\n"
		"handler setup\n"
		"{\n"
		"    listen '#{bind}:#{port}';\n"
		"    worker 1;\n"
		"}\n"
		"\n"
		"handler main\n"
		"{\n"
		"    docroot '#{docroot}';\n"
		"    autoindex ['index.cgi', 'index.html'];\n"
		"    cgi.exec if phys.path =$ '.cgi';\n"
		"    dirlisting;\n"
		"    staticfile;\n"
		"}\n"
	);
	gsub(source, "#{docroot}", documentRoot_);
	gsub(source, "#{bind}", bind);
	gsub(source, "#{port}", port);

	server_->tcpCork(true);

	std::istringstream s(source);
	return server_->setup(&s, "instant-mode.conf", optimizationLevel_);
}

/*! Updates state change, and tell supervisors about our state change.
 */
void XzeroHttpDaemon::setState(State newState)
{
	if (state_ == newState) {
		// most probabely a bug
	}

	switch (newState) {
		case State::Inactive:
			break;
		case State::Initializing:
			sd_notify(0, "STATUS=Initializing ...");
			break;
		case State::Running:
			if (server_->generation() == 1) {
				// we have been started up directoy (e.g. by systemd)
				sd_notifyf(0,
					"MAINPID=%d\n"
					"STATUS=Accepting requests ...\n"
					"READY=1\n",
					getpid()
				);
			} else {
				// we have been invoked by x0d itself, e.g. a executable upgrade and/or
				// configuration reload.
				// Tell the parent-x0d to shutdown gracefully.
				// On receive, the parent process will tell systemd, that we are the new master.
				::kill(getppid(), SIGQUIT);
			}
			break;
		case State::Upgrading:
			sd_notify(0, "STATUS=Upgrading");
			server_->log(x0::Severity::info, "Upgrading ...");
			break;
		case State::GracefullyShuttingdown:
			if (state_ == State::Running) {
				sd_notify(0, "STATUS=Shutting down gracefully ...");
			} else if (state_ == State::Upgrading) {
				// we're not the master anymore
				// tell systemd, that our freshly spawned child is taking over, and the new master
				// XXX as of systemd v28, RELOADED=1 is not yet implemented, but on their TODO list
				sd_notifyf(0,
					"MAINPID=%d\n"
					"STATUS=Accepting requests ...\n"
					"RELOADED=1\n",
					child_.pid
				);
			}
			break;
		default:
			// must be a bug
			break;
	}

	state_ = newState;
}

bool XzeroHttpDaemon::createPidFile()
{
	if (systemd_) {
		if (!pidfile_.empty())
			log(x0::Severity::info, "PID file requested but process is being supervised by systemd. Ignoring.");

		return true;
	}

	if (pidfile_.empty()) {
		log(x0::Severity::warn, "No PID file specified. Use %s --pid-file=PATH.", argv_[0]);
		return false;
	}

	FILE *pidfile = fopen(pidfile_.c_str(), "w");
	if (!pidfile) {
		log(x0::Severity::error, "Could not create PID file %s: %s.", pidfile_.c_str(), strerror(errno));
		return false;
	}

	fprintf(pidfile, "%d\n", getpid());
	fclose(pidfile);

	log(x0::Severity::info, "Created PID file with value %d [%s]", getpid(), pidfile_.c_str());

	return true;
}

void XzeroHttpDaemon::reopenLogsHandler(ev::sig&, int)
{
	server_->log(x0::Severity::info, "Reopening of all log files requested.");
	/// \! todo implement reopening of all log-files
}

/** starts new binary with (new) config - as child process, and gracefully shutdown self.
 */
void XzeroHttpDaemon::reexecHandler(ev::sig& sig, int)
{
	if (state_ != State::Running) {
		server_->log(x0::Severity::info, "Reexec requested again?. Ignoring.");
		return;
	}

	setState(State::Upgrading);

	// suspend worker threads while performing the reexec
	for (x0::HttpWorker* worker: server_->workers()) {
		worker->suspend();
	}

	x0::Buffer serializedListeners;

	for (x0::ServerSocket* listener: server_->listeners()) {
		// stop accepting new connections
		listener->stop();

		// and clear O_CLOEXEC on listener socket, as we want to probably resume these listeners in the child process
		listener->setCloseOnExec(false);

		serializedListeners.push_back(listener->serialize());
		serializedListeners.push_back(';');
	}

	server_->log(x0::Severity::debug, "Setting envvar XZERO_LISTEN_FDS to: '%s'", serializedListeners.c_str());
	setenv("XZERO_LISTEN_FDS", serializedListeners.c_str(), true);

	// prepare environment for new binary
	char sgen[20];
	snprintf(sgen, sizeof(sgen), "%u", server_->generation());
	setenv("XZERO_UPGRADE", sgen, true);

	std::vector<const char*> args;
	args.push_back(argv_[0]);

	if (systemd_)
		args.push_back("--systemd");

	if (!instant_.empty()) {
		args.push_back("--instant");
		args.push_back(instant_.c_str());
	} else {
		args.push_back("-f");
		args.push_back(configfile_.c_str());
	}

	if (!logFile_.empty()) {
		args.push_back("--log-file");
		args.push_back(logFile_.c_str());
	}

	args.push_back("--log-level");
	char logLevel[16];
	snprintf(logLevel, sizeof(logLevel), "%d", static_cast<int>(logLevel_));
	args.push_back(logLevel);

	args.push_back("--no-fork"); // we never fork (potentially again)
	args.push_back(nullptr);

	int childPid = vfork();
	switch (childPid) {
		case 0:
			// in child
			execve(argv_[0], (char**)args.data(), environ);
			server_->log(x0::Severity::error, "Executing new child process failed: %s", strerror(errno));
			abort();
			break;
		case -1:
			// error
			server_->log(x0::Severity::error, "Forking for new child process failed: %s", strerror(errno));
			break;
		default:
			// in parent
			// the child process must tell us whether to gracefully shutdown or to resume.
			child_.set<XzeroHttpDaemon, &XzeroHttpDaemon::onChild>(this);
			child_.set(childPid, 0);
			child_.start();

			// FIXME do we want a reexecTimeout, to handle possible cases where the child is not calling back? to kill them, if so!
			break;
	}

	// continue running the the process (with listeners disabled)
	server_->log(x0::Severity::debug, "Setting O_CLOEXEC on listener sockets");
	for (auto listener: server_->listeners()) {
		listener->setCloseOnExec(true);
	}
}

void XzeroHttpDaemon::onChild(ev::child&, int)
{
	// the child exited before we receive a SUCCESS from it. so resume normal operation again.
	server_->log(x0::Severity::error, "New process exited with %d. Resuming normal operation.");

	child_.stop();

	// reenable HUP-signal
	if (!hupSignal_.is_active()) {
		server_->log(x0::Severity::error, "Reenable HUP-signal.");
		hupSignal_.start();
		ev_unref(loop_);
	}

	server_->log(x0::Severity::debug, "Reactivating listeners.");
	for (x0::ServerSocket* listener: server_->listeners()) {
		// reenable O_CLOEXEC on listener socket
		listener->setCloseOnExec(true);

		// start accepting new connections
		listener->start();
	}

	server_->log(x0::Severity::debug, "Resuming workers.");
	for (x0::HttpWorker* worker: server_->workers()) {
		worker->resume();
	}
}

/** temporarily suspends processing new and currently active connections.
 */
void XzeroHttpDaemon::suspendHandler(ev::sig& sig, int)
{
	// suspend worker threads while performing the reexec
	for (x0::HttpWorker* worker: server_->workers()) {
		worker->suspend();
	}

	for (x0::ServerSocket* listener: server_->listeners()) {
		// stop accepting new connections
		listener->stop();
	}
}

/** resumes previousely suspended execution.
 */
void XzeroHttpDaemon::resumeHandler(ev::sig& sig, int)
{
	server_->log(x0::Severity::debug, "Siganl %s received.", sig2str(sig.signum).c_str());

	server_->log(x0::Severity::debug, "Resuming worker threads.");
	for (x0::HttpWorker* worker: server_->workers()) {
		worker->resume();
	}
}

// stage-1 termination handler
void XzeroHttpDaemon::gracefulShutdownHandler(ev::sig& sig, int)
{
	log(x0::Severity::info, "%s received. Shutting down gracefully.", sig2str(sig.signum).c_str());

	for (x0::ServerSocket* listener: server_->listeners()) {
		listener->close();
	}

	if (state_ == State::Upgrading) {
		child_.stop();

		for (x0::HttpWorker* worker: server_->workers()) {
			worker->resume();
		}
	}
	setState(State::GracefullyShuttingdown);

	// initiate graceful server-stop
	server_->maxKeepAlive = x0::TimeSpan::Zero;
	server_->stop();
}

// stage-2 termination handler
void XzeroHttpDaemon::quickShutdownHandler(ev::sig& sig, int)
{
	log(x0::Severity::info, "%s received. shutting down NOW.", sig2str(sig.signum).c_str());

	if (!child_.pid) {
		// we are no garbage parent process
		sd_notify(0, "STATUS=Shutting down.");
	}

	// default to standard signal-handler
	ev_ref(loop_);
	sig.stop();

	// install shutdown timeout handler
	terminationTimeout_.set<XzeroHttpDaemon, &XzeroHttpDaemon::quickShutdownTimeout>(this);
	terminationTimeout_.start(10, 0);
	ev_unref(loop_);

	// kill active HTTP connections
	server_->kill();
}

void XzeroHttpDaemon::quickShutdownTimeout(ev::timer&, int)
{
	log(x0::Severity::warn, "Quick shutdown timed out. Terminating.");

	ev_ref(loop_);
	terminationTimeout_.stop();

	ev_break(loop_, ev::ALL);
}
// }}}

XzeroHttpDaemon* XzeroHttpDaemon::instance_ = 0;

int main(int argc, char *argv[])
{
#if !defined(NDEBUG)
	if (argc == 1) {
		const char* args[] = {
			argv[0],
			"--no-fork",
			"-f", "src/test.conf",
			"--pid-file", "test.pid",
			"--log-file", "/dev/stdout",
			"--log-level", "9",
			nullptr
		};
//		const char* args[] = { argv[0], "--systemd", "-c", "../../src/test.conf", nullptr };
		argv = (char **) args;
		argc = sizeof(args) / sizeof(*args) - 1;
	}
#endif
	XzeroHttpDaemon daemon(argc, argv);
	return daemon.run();
}
