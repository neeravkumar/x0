/* <x0/Severity.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 *
 * (c) 2009 Chrisitan Parpart <trapni@gentoo.org>
 */

#include <x0/Severity.h>
#include <x0/strutils.h>

namespace x0 {

Severity::Severity(const std::string& name)
{
	if (name == "emergency")
		value_ = emergency;
	else if (name == "alert")
		value_ = alert;
	else if (name == "critical")
		value_ = critical;
	else if (name == "error")
		value_ = error;
	else if (name == "warn" || name == "warning" || name.empty()) // <- default: warn
		value_ = warn;
	else if (name == "notice")
		value_ = notice;
	else if (name == "info")
		value_ = info;
	else if (name == "debug")
		value_ = debug;
	else
		throw std::runtime_error(fstringbuilder::format("Invalid Severity '%s'", name.c_str()));
}

const char *Severity::c_str() const
{
	static const char *tr[] =
	{
		"emergency",
		"alert",
		"critical",
		"error",
		"warn",
		"notice",
		"info",
		"debug"
	};

	return tr[value_];
}

} // namespace x0