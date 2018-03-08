/*
 * Copyright (C) 2013 Emeric Poupon
 *
 * This file is part of LMS.
 *
 * LMS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * LMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LMS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Types.hpp"

namespace Database {

// must be ordered
const std::vector<std::size_t>
User::audioBitrates =
{
	64000,
	128000,
	192000,
	320000,
};

const std::vector<AudioEncoding>
User::audioEncodings =
{
	AudioEncoding::AUTO,
	AudioEncoding::MP3,
	AudioEncoding::OGA,
	AudioEncoding::WEBMA,
};

User::User()
: _maxAudioBitrate(maxAudioBitrate),
_isAdmin(false),
_audioBitrate(defaultAudioBitrate),
_audioEncoding(AudioEncoding::AUTO),
_curPlayingTrackPos(0)
{

}

std::vector<User::pointer>
User::getAll(Wt::Dbo::Session& session)
{
	Wt::Dbo::collection<pointer> res = session.find<User>();
	return std::vector<pointer>(res.begin(), res.end());
}

User::pointer
User::create(Wt::Dbo::Session& session)
{
	return session.add(new User());
}

User::pointer
User::getById(Wt::Dbo::Session& session, id_type id)
{
	return session.find<User>().where("id = ?").bind( id );
}

std::string
User::getId( pointer user)
{
	std::ostringstream oss; oss << user.id();
	return oss.str();
}

void
User::setAudioBitrate(std::size_t bitrate)
{
	_audioBitrate = std::min(bitrate, std::min(static_cast<std::size_t>(_maxAudioBitrate), audioBitrates.back()));
}

void
User::setMaxAudioBitrate(std::size_t bitrate)
{
	_maxAudioBitrate = std::min(bitrate, static_cast<std::size_t>(_maxAudioBitrate));
}

std::size_t
User::getAudioBitrate(void) const
{
	if (!isAdmin())
		return std::min(static_cast<std::size_t>(_audioBitrate), std::min(static_cast<std::size_t>(_maxAudioBitrate), audioBitrates.back()));
	else
		return std::min(static_cast<std::size_t>(_audioBitrate), audioBitrates.back());
}

std::size_t
User::getMaxAudioBitrate(void) const
{
	if (!isAdmin())
		return std::min(static_cast<std::size_t>(_maxAudioBitrate), audioBitrates.back());
	else
		return audioBitrates.back();
}

} // namespace Database


