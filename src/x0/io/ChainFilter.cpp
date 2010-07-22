/* <x0/ChainFilter.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.ws/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 */

#include <x0/io/ChainFilter.h>

namespace x0 {

Buffer ChainFilter::process(const BufferRef& input, bool eof)
{
	if (filters_.empty())
		return Buffer(input);

	auto i = filters_.begin();
	auto e = filters_.end();
	Buffer result((*i++)->process(input, eof));

	while (i != e)
		result = (*i++)->process(result.ref(), eof);

	return result;
}

} // namespace x0
