/* <x0/mod_userdir.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 *
 * (c) 2009 Chrisitan Parpart <trapni@gentoo.org>
 */

#include <x0/server.hpp>
#include <x0/request.hpp>
#include <x0/response.hpp>
#include <x0/header.hpp>
#include <x0/strutils.hpp>
#include <x0/types.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>
#include <pwd.h>

/**
 * \ingroup plugins
 * \brief implements automatic index file resolving, if mapped request path is a path.
 */
class userdir_plugin :
	public x0::plugin
{
private:
	x0::signal<void(x0::request&)>::connection c;

	struct context
	{
		std::string prefix;			// userdir prefix (default: "~")
		std::string docroot;		// user's document root (default: "public_html")
	};

public:
	userdir_plugin(x0::server& srv, const std::string& name) :
		x0::plugin(srv, name)
	{
		c = server_.resolve_entity.connect(/*0, */ boost::bind(&userdir_plugin::resolve_entity, this, _1));
		server_.create_context<context>(this);
	}

	~userdir_plugin()
	{
		server_.resolve_entity.disconnect(c);
		server_.free_context<context>(this);
	}

	virtual void configure()
	{
		context& ctx = server_.context<context>(this);

		server_.config().load("UserDir.PathPrefix", ctx.prefix);
		server_.config().load("UserDir.DocumentRoot", ctx.docroot);

		if (ctx.prefix.empty())
		{
			ctx.prefix = "~";
		}

		if (ctx.docroot.empty())
		{
			ctx.docroot = "/public_html";
		}
		else
		{
			// force slash at begin
			if (ctx.docroot[0] != '/')
			{
				ctx.docroot.insert(0, "/");
			}

			// strip slash at end
			if (ctx.docroot[ctx.docroot.size() - 1] == '/')
			{
				ctx.docroot = ctx.docroot.substr(0, ctx.docroot.size() - 1);
			}
		}
	}

private:
	void resolve_entity(x0::request& in)
	{
		const context& ctx = server_.context<context>(this);

		if (in.path.size() > 2 && std::strncmp(&in.path[1], ctx.prefix.c_str(), ctx.prefix.length()) == 0)
		{
			const int prefix_len1 = ctx.prefix.length() + 1;
			const std::size_t i = in.path.find("/", prefix_len1);
			std::string userName, userPath;

			if (i != std::string::npos)
			{
				userName = in.path.substr(prefix_len1, i - prefix_len1);
				userPath = in.path.substr(i);
			}
			else
			{
				userName = in.path.substr(prefix_len1);
				userPath = "";
			}

			if (struct passwd *pw = getpwnam(userName.c_str()))
			{
				in.document_root = pw->pw_dir + ctx.docroot;
				in.fileinfo = server_.fileinfo(in.document_root + userPath);
			}
		}
	}
};

X0_EXPORT_PLUGIN(userdir);