/* <x0/plugins/filter_example.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.ws/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 */

#include <x0/http/HttpPlugin.h>
#include <x0/http/HttpServer.h>
#include <x0/http/HttpRequest.h>
#include <x0/http/HttpResponse.h>
#include <x0/http/HttpRangeDef.h>
#include <x0/io/Filter.h>
#include <x0/strutils.h>
#include <x0/Types.h>
#include <x0/sysconfig.h>

// {{{ ExampleFilter
class ExampleFilter :
	public x0::Filter
{
	bool mode_;
public:
	explicit ExampleFilter(int mode);
	virtual x0::Buffer process(const x0::BufferRef& input);
};

ExampleFilter::ExampleFilter(int mode) : mode_(mode)
{
}

x0::Buffer ExampleFilter::process(const x0::BufferRef& input)
{
	x0::Buffer result;

	switch (mode_)
	{
		case 2: // return lower-case
			for (auto i = input.begin(), e = input.end(); i != e; ++i)
				result.push_back(static_cast<char>(std::tolower(*i)));
			break;
		case 1: // return upper-case
			for (auto i = input.begin(), e = input.end(); i != e; ++i)
				result.push_back(static_cast<char>(std::toupper(*i)));
			break;
		case 0: // return identity
		default:
			result.push_back(input);
	}

	return result;
}
// }}}

/**
 * \ingroup plugins
 * \brief ...
 */
class filter_plugin :
	public x0::HttpPlugin
{
public:
	filter_plugin(x0::HttpServer& srv, const std::string& name) :
		x0::HttpPlugin(srv, name)
	{
		registerFunction<filter_plugin, &filter_plugin::install_filter>("example_filter", Flow::Value::VOID);
	}

	~filter_plugin() {
	}

	void install_filter(Flow::Value& /*result*/, x0::HttpRequest *in, x0::HttpResponse *out, const x0::Params& args)
	{
		if (args.count() != 1) {
			log(x0::Severity::error, "No argument passed.");
			return;
		}

		if (!args[0].isString()) {
			log(x0::Severity::error, "Invalid argument type passed.");
			return;
		}

		if (strcmp(args[0].toString(), "identity") == 0)
			out->filters.push_back(std::make_shared<ExampleFilter>(0));
		else if (strcmp(args[0].toString(), "upper") == 0)
			out->filters.push_back(std::make_shared<ExampleFilter>(1));
		else if (strcmp(args[0].toString(), "lower") == 0)
			out->filters.push_back(std::make_shared<ExampleFilter>(2));
		else {
			log(x0::Severity::error, "Invalid argument value passed.");
			return;
		}

		out->headers.push_back("Content-Encoding", "filter_example");

		// response might change according to Accept-Encoding
		if (!out->headers.contains("Vary"))
			out->headers.push_back("Vary", "Accept-Encoding");
		else
			out->headers["Vary"] += ",Accept-Encoding";

		// removing content-length implicitely enables chunked encoding
		out->headers.remove("Content-Length");
	}
};

X0_EXPORT_PLUGIN(filter)
