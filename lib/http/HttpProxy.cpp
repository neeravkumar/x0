#include <x0/http/HttpBackend.h>
#include <x0/http/HttpDirector.h>
#include <x0/http/HttpServer.h>
#include <x0/http/HttpRequest.h>
#include <x0/io/BufferSource.h>
#include <x0/io/BufferRefSource.h>
#include <x0/SocketSpec.h>
#include <x0/strutils.h>
#include <x0/Url.h>
#include <x0/Types.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>


#if 1
#	define TRACE(msg...) (this->Logging::debug(msg))
#else
#	define TRACE(msg...) do {} while (0)
#endif

namespace x0 {

// {{{ HttpProxy::ProxyConnection API
class HttpProxy::ProxyConnection :
#ifndef NDEBUG
	public Logging,
#endif
	public HttpMessageProcessor
{
private:
	HttpProxy* proxy_;			//!< owning proxy

	int refCount_;

	HttpRequest* request_;		//!< client's request
	Socket* backend_;			//!< connection to backend app
	bool cloak_;				//!< to cloak or not to cloak the "Server" response header

	int connectTimeout_;
	int readTimeout_;
	int writeTimeout_;

	Buffer writeBuffer_;
	size_t writeOffset_;
	size_t writeProgress_;

	Buffer readBuffer_;
	bool processingDone_;

private:
	HttpProxy* proxy() const { return proxy_; }

	void ref();
	void unref();

	inline void close();
	inline void readSome();
	inline void writeSome();

	void onConnected(Socket* s, int revents);
	void io(Socket* s, int revents);
	void onRequestChunk(const BufferRef& chunk);

	static void onAbort(void *p);
	void onWriteComplete();

	// response (HttpMessageProcessor)
	virtual bool onMessageBegin(int versionMajor, int versionMinor, int code, const BufferRef& text);
	virtual bool onMessageHeader(const BufferRef& name, const BufferRef& value);
	virtual bool onMessageContent(const BufferRef& chunk);
	virtual bool onMessageEnd();

public:
	inline explicit ProxyConnection(HttpProxy* proxy);
	~ProxyConnection();

	void start(HttpRequest* in, Socket* backend, bool cloak);
};
// }}}

// {{{ HttpProxy::ProxyConnection impl
HttpProxy::ProxyConnection::ProxyConnection(HttpProxy* proxy) :
	HttpMessageProcessor(HttpMessageProcessor::RESPONSE),
	proxy_(proxy),
	refCount_(1),
	request_(nullptr),
	backend_(nullptr),
	cloak_(true),

	connectTimeout_(0),
	readTimeout_(0),
	writeTimeout_(0),
	writeBuffer_(),
	writeOffset_(0),
	writeProgress_(0),
	readBuffer_(),
	processingDone_(false)
{
#ifndef NDEBUG
	setLoggingPrefix("ProxyConnection/%p", this);
#endif
	TRACE("ProxyConnection()");
}

HttpProxy::ProxyConnection::~ProxyConnection()
{
	TRACE("~ProxyConnection()");

	if (backend_) {
		backend_->close();

		delete backend_;
	}

	if (request_) {
		if (request_->status == HttpError::Undefined) {
			// We failed processing this request, so reschedule
			// this request within the director and give it the chance
			// to be processed by another backend,
			// or give up when the director's request processing
			// timeout has been reached.

			proxy_->director_->reschedule(request_, proxy_);
		} else {
			// We actually served ths request, so finish() it.
			request_->finish();

			// Notify director that this backend has just completed a request,
			// and thus, is potentially available for surving the next.
			proxy_->release();
		}
	}
}

void HttpProxy::ProxyConnection::ref()
{
	++refCount_;
}

void HttpProxy::ProxyConnection::unref()
{
	assert(refCount_ > 0);

	--refCount_;

	if (refCount_ == 0) {
		delete this;
	}
}

void HttpProxy::ProxyConnection::close()
{
	if (backend_)
		// stop watching on any backend I/O events, if active
		backend_->close();

	unref(); // the one from the constructor
}

void HttpProxy::ProxyConnection::onAbort(void *p)
{
	ProxyConnection *self = reinterpret_cast<ProxyConnection *>(p);
	self->close();
}

void HttpProxy::ProxyConnection::start(HttpRequest* in, Socket* backend, bool cloak)
{
	TRACE("ProxyConnection.start(in, backend, cloak=%d)", cloak);

	request_ = in;
	request_->setAbortHandler(&ProxyConnection::onAbort, this);
	backend_ = backend;
	cloak_ = cloak;

	// request line
	writeBuffer_.push_back(request_->method);
	writeBuffer_.push_back(' ');
	writeBuffer_.push_back(request_->uri);
	writeBuffer_.push_back(" HTTP/1.1\r\n");

	BufferRef forwardedFor;

	// request headers
	for (auto& header: request_->requestHeaders) {
		if (iequals(header.name, "X-Forwarded-For")) {
			forwardedFor = header.value;
			continue;
		}
		else if (iequals(header.name, "Content-Transfer")
				|| iequals(header.name, "Expect")
				|| iequals(header.name, "Connection")) {
			TRACE("skip requestHeader(%s: %s)", header.name.str().c_str(), header.value.str().c_str());
			continue;
		}

		TRACE("pass requestHeader(%s: %s)", header.name.str().c_str(), header.value.str().c_str());
		writeBuffer_.push_back(header.name);
		writeBuffer_.push_back(": ");
		writeBuffer_.push_back(header.value);
		writeBuffer_.push_back("\r\n");
	}

	// additional headers to add
	writeBuffer_.push_back("Connection: closed\r\n");

	// X-Forwarded-For
	writeBuffer_.push_back("X-Forwarded-For: ");
	if (forwardedFor) {
		writeBuffer_.push_back(forwardedFor);
		writeBuffer_.push_back(", ");
	}
	writeBuffer_.push_back(request_->connection.remoteIP());
	writeBuffer_.push_back("\r\n");

#if defined(WITH_SSL)
	// X-Forwarded-Proto
	if (request_->requestHeader("X-Forwarded-Proto").empty()) {
		if (request_->connection.isSecure())
			writeBuffer_.push_back("X-Forwarded-Proto: https\r\n");
		else
			writeBuffer_.push_back("X-Forwarded-Proto: http\r\n");
	}
#endif

	// request headers terminator
	writeBuffer_.push_back("\r\n");

	if (request_->contentAvailable()) {
		TRACE("start: request content available: reading.");
		request_->setBodyCallback<ProxyConnection, &ProxyConnection::onRequestChunk>(this);
	}

	if (backend_->state() == Socket::Connecting) {
		TRACE("start: connect in progress");
		backend_->setReadyCallback<ProxyConnection, &ProxyConnection::onConnected>(this);
	} else { // connected
		TRACE("start: flushing");
		backend_->setReadyCallback<ProxyConnection, &ProxyConnection::io>(this);
		backend_->setMode(Socket::ReadWrite);
	}
}

void HttpProxy::ProxyConnection::onConnected(Socket* s, int revents)
{
	TRACE("onConnected: content? %d", request_->contentAvailable());
	//TRACE("onConnected.pending:\n%s\n", writeBuffer_.c_str());

	if (backend_->state() == Socket::Operational) {
		TRACE("onConnected: flushing");
		request_->responseHeaders.push_back("X-Director-Backend", proxy_->name());
		backend_->setReadyCallback<ProxyConnection, &ProxyConnection::io>(this);
		backend_->setMode(Socket::ReadWrite); // flush already serialized request
	} else {
		TRACE("onConnected: failed");
		close();
	}
}

/** transferres a request body chunk to the origin server.  */
void HttpProxy::ProxyConnection::onRequestChunk(const BufferRef& chunk)
{
	TRACE("onRequestChunk(nb:%ld)", chunk.size());
	writeBuffer_.push_back(chunk);

	if (backend_->state() == Socket::Operational) {
		backend_->setMode(Socket::ReadWrite);
	}
}

/** callback, invoked when the origin server has passed us the response status line.
 *
 * We will use the status code only.
 * However, we could pass the text field, too - once x0 core supports it.
 */
bool HttpProxy::ProxyConnection::onMessageBegin(int major, int minor, int code, const BufferRef& text)
{
	TRACE("ProxyConnection(%p).status(HTTP/%d.%d, %d, '%s')", (void*)this, major, minor, code, text.str().c_str());

	request_->status = static_cast<HttpError>(code);
	TRACE("status: %d", (int)request_->status);
	return true;
}

/** callback, invoked on every successfully parsed response header.
 *
 * We will pass this header directly to the client's response,
 * if that is NOT a connection-level header.
 */
bool HttpProxy::ProxyConnection::onMessageHeader(const BufferRef& name, const BufferRef& value)
{
	TRACE("ProxyConnection(%p).onHeader('%s', '%s')", (void*)this, name.str().c_str(), value.str().c_str());

	// XXX do not allow origin's connection-level response headers to be passed to the client.
	if (iequals(name, "Connection"))
		goto skip;

	if (iequals(name, "Transfer-Encoding"))
		goto skip;

	if (cloak_ && iequals(name, "Server")) {
		TRACE("skipping \"Server\"-header");
		goto skip;
	}

	request_->responseHeaders.push_back(name.str(), value.str());
	return true;

skip:
	TRACE("skip (connection-)level header");

	return true;
}

/** callback, invoked on a new response content chunk. */
bool HttpProxy::ProxyConnection::onMessageContent(const BufferRef& chunk)
{
	TRACE("messageContent(nb:%lu) state:%s", chunk.size(), backend_->state_str());

	// stop watching for more input
	backend_->setMode(Socket::None);

	// transfer response-body chunk to client
	request_->write<BufferRefSource>(chunk);

	// start listening on backend I/O when chunk has been fully transmitted
	ref();
	request_->writeCallback<ProxyConnection, &ProxyConnection::onWriteComplete>(this);

	return true;
}

void HttpProxy::ProxyConnection::onWriteComplete()
{
	TRACE("chunk write complete: %s", state_str());
	backend_->setMode(Socket::Read);
	unref();
}

bool HttpProxy::ProxyConnection::onMessageEnd()
{
	TRACE("messageEnd() backend-state:%s", backend_->state_str());
	processingDone_ = true;
	return false;
}

void HttpProxy::ProxyConnection::io(Socket* s, int revents)
{
	TRACE("io(0x%04x)", revents);

	if (revents & Socket::Read)
		readSome();

	if (revents & Socket::Write)
		writeSome();
}

void HttpProxy::ProxyConnection::writeSome()
{
	TRACE("writeSome() - %s (%d)", state_str(), request_->contentAvailable());

	ssize_t rv = backend_->write(writeBuffer_.data() + writeOffset_, writeBuffer_.size() - writeOffset_);

	if (rv > 0) {
		TRACE("write request: %ld (of %ld) bytes", rv, writeBuffer_.size() - writeOffset_);

		writeOffset_ += rv;
		writeProgress_ += rv;

		if (writeOffset_ == writeBuffer_.size()) {
			TRACE("writeOffset == writeBuffser.size (%ld) p:%ld, ca: %d, clr:%ld", writeOffset_,
				writeProgress_, request_->contentAvailable(), request_->connection.contentLength());

			writeOffset_ = 0;
			writeBuffer_.clear();
			backend_->setMode(Socket::Read);
		}
	} else {
		TRACE("write request failed(%ld): %s", rv, strerror(errno));
		close();
	}
}

void HttpProxy::ProxyConnection::readSome()
{
	TRACE("readSome() - %s", state_str());

	std::size_t lower_bound = readBuffer_.size();

	if (lower_bound == readBuffer_.capacity())
		readBuffer_.setCapacity(lower_bound + 4096);

	ssize_t rv = backend_->read(readBuffer_);

	if (rv > 0) {
		TRACE("read response: %ld bytes", rv);
		std::size_t np = process(readBuffer_.ref(lower_bound, rv));
		(void) np;
		TRACE("readSome(): process(): %ld / %ld", np, rv);

		if (processingDone_ || state() == SYNTAX_ERROR) {
			close();
		} else {
			TRACE("resume with io:%d, state:%s", backend_->mode(), backend_->state_str());
			backend_->setMode(Socket::Read);
		}
	} else if (rv == 0) {
		TRACE("http server connection closed");
		close();
	} else {
		TRACE("read response failed(%ld): %s", rv, strerror(errno));

		switch (errno) {
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
		case EWOULDBLOCK:
#endif
		case EAGAIN:
		case EINTR:
			backend_->setMode(Socket::Read);
			break;
		default:
			close();
			break;
		}
	}
}
// }}}

// {{{ HttpProxy impl
HttpProxy::HttpProxy(HttpDirector* director, const std::string& name,
		size_t capacity, const std::string& hostname, int port) :
	HttpBackend(director, name, capacity),
	hostname_(hostname),
	port_(port),
	connections_()
{
#ifndef NDEBUG
	setLoggingPrefix("HttpProxy/%s", name.c_str());
#endif
}

HttpProxy::~HttpProxy()
{
}

bool HttpProxy::process(HttpRequest* r)
{
	TRACE("process...");

	Socket* backend = new Socket(r->connection.worker().loop());
	backend->openTcp(hostname_, port_, O_NONBLOCK | O_CLOEXEC);

	if (backend->isOpen()) {
		TRACE("in.content? %d", r->contentAvailable());

		if (ProxyConnection* pc = new ProxyConnection(this)) {
			hit();
			pc->start(r, backend, director_->cloakOrigin());
			return true;
		}
	}

	r->log(Severity::error, "HTTP proxy: Could not connect to backend: %s", strerror(errno));
	return false;
}

size_t HttpProxy::writeJSON(Buffer& output) const
{
	size_t offset = output.size();

	HttpBackend::writeJSON(output);

	output << ", \"type\": \"http\""
		   << ", \"hostname\": \"" << hostname_ << "\""
		   << ", \"port\": " << port_;

	return output.size() - offset;
}
// }}}

} // namespace x0
