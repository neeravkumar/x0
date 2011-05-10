/* <x0/HttpRequest.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.ws/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 */

#include <x0/http/HttpRequest.h>
#include <x0/http/HttpConnection.h>
#include <x0/http/HttpListener.h>
#include <x0/io/BufferSource.h>
#include <x0/io/FilterSource.h>
#include <x0/io/ChunkedEncoder.h>
#include <x0/strutils.h>
#include <strings.h>			// strcasecmp()

#if !defined(NDEBUG)
#	define TRACE(msg...) this->debug(msg)
#else
#	define TRACE(msg...) ((void *)0)
#endif

namespace x0 {

char HttpRequest::statusCodes_[512][4];

HttpRequest::HttpRequest(HttpConnection& conn) :
	outputState_(Unhandled),
	connection(conn),
	method(),
	uri(),
	path(),
	fileinfo(),
	pathinfo(),
	query(),
	httpVersionMajor(0),
	httpVersionMinor(0),
	requestHeaders(),
	bytesTransmitted_(0),
	username(),
	documentRoot(),
	expectingContinue(false),
	//customData(),

	status(),
	responseHeaders(),
	outputFilters(),

	hostid_(),
	bodyCallback_(nullptr),
	bodyCallbackData_(nullptr),
	errorHandler_(nullptr)
{
#ifndef NDEBUG
	static std::atomic<unsigned long long> rid(0);
	setLoggingPrefix("HttpRequest(%lld,%s:%d)", ++rid, connection.remoteIP().c_str(), connection.remotePort());
#endif

	responseHeaders.push_back("Date", connection.worker().now().http_str().str());

	++connection.worker().requestLoad_;

	if (connection.worker().server().advertise() && !connection.worker().server().tag().empty())
		if (!responseHeaders.contains("Server"))
			responseHeaders.push_back("Server", connection.worker().server().tag());
}

HttpRequest::~HttpRequest()
{
	TRACE("destructing");
	connection.worker().server().onRequestDone(this);
	--connection.worker().requestLoad_;
	clearCustomData();
}

void HttpRequest::updatePathInfo()
{
	if (!fileinfo)
		return;

	// split "/the/tail" from "/path/to/script.php/the/tail"

	std::string fullname(fileinfo->path());
	size_t origpos = fullname.size() - 1, pos = origpos;

	for (;;) {
		if (fileinfo->exists()) {
			if (pos != origpos)
				pathinfo = fullname.substr(pos);

			break;
		} if (fileinfo->error() == ENOTDIR) {
			pos = fileinfo->path().rfind('/', pos - 1);
			fileinfo = connection.worker().fileinfo(fileinfo->path().substr(0, pos));
		} else {
			break;
		}
	}
}

BufferRef HttpRequest::requestHeader(const std::string& name) const
{
	for (auto& i: requestHeaders)
		if (iequals(i.name, name))
			return i.value;

	return BufferRef();
}

std::string HttpRequest::hostid() const
{
	if (hostid_.empty())
		hostid_ = x0::make_hostid(requestHeader("Host"), connection.listener().port());

	return hostid_;
}

void HttpRequest::setHostid(const std::string& value)
{
	hostid_ = value;
}

/** report true whenever content is still in queue to be read (even if not yet received).
 *  @retval false no content expected anymore, thus, request fully parsed.
 */
bool HttpRequest::contentAvailable() const
{
	return connection.contentLength() > 0;
	//return connection.state() != HttpMessageProcessor::MESSAGE_BEGIN;
}

/*! setup request-body consumer callback.
 *
 * \param callback the callback to invoke on request-body chunks.
 * \param data a custom data pointer being also passed to the callback.
 */
void HttpRequest::setBodyCallback(void (*callback)(BufferRef&&, void*), void* data)
{
	bodyCallback_ = callback;
	bodyCallbackData_ = data;

	if (expectingContinue) {
		connection.write<BufferSource>("HTTP/1.1 100 Continue\r\n\r\n");
		expectingContinue = false;
	}
}

void HttpRequest::onRequestContent(BufferRef&& chunk)
{
	if (bodyCallback_) {
		TRACE("onRequestContent(chunkSize=%ld) pass to callback", chunk.size());
		bodyCallback_(std::move(chunk), bodyCallbackData_);
	} else {
		TRACE("onRequestContent(chunkSize=%ld) discard", chunk.size());
	}
}

/** serializes the HTTP response status line plus headers into a byte-stream.
 *
 * This method is invoked right before the response content is written or the
 * response is flushed at all.
 *
 * It first sets the status code (if not done yet), invoked post_process callback,
 * performs connection-level response header modifications and then
 * builds the response chunk for status line and headers.
 *
 * Post-modification done <b>after</b> the post_process hook has been invoked:
 * <ol>
 *   <li>set status code to 200 (Ok) if not set yet.</li>
 *   <li>set Content-Type header to a default if not set yet.</li>
 *   <li>set Connection header to keep-alive or close (computed value)</li>
 *   <li>append Transfer-Encoding chunked if no Content-Length header is set.</li>
 *   <li>optionally enable TCP_CORK if this is no keep-alive connection and the administrator allows it.</li>
 * </ol>
 *
 * \note this does not serialize the message body.
 */
Source* HttpRequest::serialize()
{
	Buffer buffers;
	bool keepalive = false;

	if (expectingContinue)
		status = HttpError::ExpectationFailed;
	else if (status == static_cast<HttpError>(0))
		status = HttpError::Ok;

	if (!responseHeaders.contains("Content-Type"))
	{
		responseHeaders.push_back("Content-Type", "text/plain"); //!< \todo pass "default" content-type instead!
	}

	// post-response hook
	connection.worker().server().onPostProcess(this);

	// setup (connection-level) response transfer
	if (!responseHeaders.contains("Content-Length") && !isResponseContentForbidden())
	{
		if (supportsProtocol(1, 1)
			&& equals(requestHeader("Connection"), "keep-alive")
			&& !responseHeaders.contains("Transfer-Encoding")
			&& !isResponseContentForbidden())
		{
			responseHeaders.overwrite("Connection", "keep-alive");
			responseHeaders.push_back("Transfer-Encoding", "chunked");
			outputFilters.push_back(std::make_shared<ChunkedEncoder>());
			keepalive = true;
		}
	}
	else if (!responseHeaders.contains("Connection"))
	{
		if (iequals(requestHeader("Connection"), "keep-alive"))
		{
			responseHeaders.push_back("Connection", "keep-alive");
			keepalive = true;
		}
	}
	else if (iequals(responseHeaders["Connection"], "keep-alive"))
	{
		keepalive = true;
	}

	if (!connection.worker().server().maxKeepAlive())
		keepalive = false;

	//keepalive = false; // FIXME workaround

	if (!keepalive)
		responseHeaders.overwrite("Connection", "close");

	if (!connection.worker().server().tcpCork())
		connection.socket()->setTcpCork(true);

	if (supportsProtocol(1, 1))
		buffers.push_back("HTTP/1.1 ");
	else if (supportsProtocol(1, 0))
		buffers.push_back("HTTP/1.0 ");
	else
		buffers.push_back("HTTP/0.9 ");

	buffers.push_back(statusCodes_[static_cast<int>(status)]);
	buffers.push_back(' ');
	buffers.push_back(statusStr(status));
	buffers.push_back("\r\n");

	for (auto& i: responseHeaders) {
		buffers.push_back(i.name.data(), i.name.size());
		buffers.push_back(": ");
		buffers.push_back(i.value.data(), i.value.size());
		buffers.push_back("\r\n");
	};

	buffers.push_back("\r\n");

	return new BufferSource(std::move(buffers));
}

/** populates a default-response content, possibly modifying a few response headers, too.
 *
 * \return the newly created response content or a null ptr if the statuc code forbids content.
 *
 * \note Modified headers are "Content-Type" and "Content-Length".
 */
void HttpRequest::writeDefaultResponseContent()
{
	if (isResponseContentForbidden())
		return;

	// XXX here we might try to customize the standard error output
	//connection.worker().server().onErrorDocument(this);
	//FIXME should the above have done a finish() on its own or not?
	//if (outputState_ != Unhandled)
	//	return;

	// TODO custom error documents
#if 0
	std::string filename(connection.server().config()["ErrorDocuments"][make_str(status)].as<std::string>());
	FileInfoPtr fi(connection.server().fileinfo(filename));
	int fd = ::open(fi->filename().c_str(), O_RDONLY);
	if (fi->exists() && fd >= 0)
	{
		responseHeaders.overwrite("Content-Type", fi->mimetype());
		responseHeaders.overwrite("Content-Length", boost::lexical_cast<std::string>(fi->size()));

		write<FileSource>(fd, 0, fi->size(), true);
		return;
	}
#endif
	std::string codeStr = http_category().message(static_cast<int>(status));
	char buf[1024];

	int nwritten = snprintf(buf, sizeof(buf),
		"<html>"
		"<head><title>%s</title></head>"
		"<body><h1>%d %s</h1></body>"
		"</html>\r\n",
		codeStr.c_str(), status, codeStr.c_str()
	);

	responseHeaders.overwrite("Content-Type", "text/html");

	char slen[64];
	snprintf(slen, sizeof(slen), "%d", nwritten);
	responseHeaders.overwrite("Content-Length", slen);

	write<BufferSource>(Buffer::fromCopy(buf, nwritten));
}

std::string HttpRequest::statusStr(HttpError value)
{
	return http_category().message(static_cast<int>(value));
}

/** finishes this response by flushing the content into the stream.
 *
 * \note this also queues the underlying connection for processing the next request (on keep-alive).
 * \note this also clears out the client abort callback set with \p setAbortHandler().
 */
void HttpRequest::finish()
{
	setAbortHandler(nullptr);

	if (isAborted()) {
		outputState_ = Finished;
		finalize();
		return;
	}

	switch (outputState_) {
		case Unhandled:
			if (static_cast<int>(status) == 0)
				status = HttpError::NotFound;

			if (errorHandler_) {
				TRACE("running custom error handler");
				// reset the handler right away to avoid endless nesting
				auto handler = errorHandler_;
				errorHandler_ = nullptr;

				if (handler(this))
					return;

				// the handler did not produce any response, so default to the
				// buildin-output
			}
			TRACE("streaming default error content");

			if (!isResponseContentForbidden() && status != HttpError::Ok)
				writeDefaultResponseContent();
			else
				connection.write(serialize());
			/* fall through */
		case Populating:
			// FIXME: can it become an issue when the response body may not be non-empty
			// but we have outputFilters defined, thus, writing a single (possibly empty?)
			// response body chunk?
			if (!outputFilters.empty()) {
				// mark the end of stream (EOS) by passing an empty chunk to the outputFilters.
				connection.write<FilterSource>(outputFilters, true);
			}

			outputState_ = Finished;

			if (!connection.isOutputPending()) {
				// the response body is already fully transmitted, so finalize this request object directly.
				finalize();
			}
			break;
		case Finished:
			assert(false && "You almost definitely invoked finish() twice.");
	}
}

void HttpRequest::finalize()
{
	if (isAborted() || !iequals(responseHeaders["Connection"], "keep-alive")) {
		TRACE("finalize: closing");
		connection.close();
	} else {
		TRACE("finalize: resuming");
		connection.resume();
	}
}

/** to be called <b>once</b> in order to initialize this class for instanciation.
 *
 * \note to be invoked by HttpServer constructor.
 * \see server
 */
void HttpRequest::initialize()
{
	// pre-compute string representations of status codes for use in response serialization
	for (std::size_t i = 0; i < sizeof(statusCodes_) / sizeof(*statusCodes_); ++i)
		snprintf(statusCodes_[i], sizeof(*statusCodes_), "%03ld", i);
}

/** sets a callback to invoke on early connection aborts (by the remote end).
 *
 * This callback is only invoked when the client closed the connection before
 * \p HttpRequest::finish() has been invoked and completed already.
 *
 * Do <b>NOT</b> try to access the request object within the callback as it
 * might be destroyed already.
 *
 * \see HttpRequest::finish()
 */
void HttpRequest::setAbortHandler(void (*cb)(void *), void *data)
{
	connection.abortHandler_ = cb;
	connection.abortData_ = data;

	if (cb) {
		connection.watchInput();
	}
}

} // namespace x0
