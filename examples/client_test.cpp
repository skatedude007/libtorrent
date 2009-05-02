/*

Copyright (c) 2003, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include <iostream>
#include <fstream>
#include <iterator>
#include <exception>

#include "libtorrent/config.hpp"

#ifdef _MSC_VER
#pragma warning(push, 1)
#endif

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/bind.hpp>
#include <boost/regex.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "libtorrent/extensions/metadata_transfer.hpp"
#include "libtorrent/extensions/ut_metadata.hpp"
#include "libtorrent/extensions/ut_pex.hpp"
#include "libtorrent/extensions/smart_ban.hpp"

#include "libtorrent/entry.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/identify_client.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/ip_filter.hpp"
#include "libtorrent/magnet_uri.hpp"
#include "libtorrent/bitfield.hpp"
#include "libtorrent/file.hpp"

using boost::bind;

#ifdef _WIN32

#if defined(_MSC_VER)
#	define for if (false) {} else for
#endif

#include <windows.h>
#include <conio.h>

bool sleep_and_input(char* c)
{
	Sleep(500);
	if (_kbhit())
	{
		*c = _getch();
		return true;
	}
	return false;
};

void clear_home()
{
	CONSOLE_SCREEN_BUFFER_INFO si;
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
	GetConsoleScreenBufferInfo(h, &si);
	COORD c = {0, 0};
	DWORD n;
	FillConsoleOutputCharacter(h, ' ', si.dwSize.X * si.dwSize.Y, c, &n);
	SetConsoleCursorPosition(h, c);
}

#else

#include <stdlib.h>
#include <stdio.h>

#include <termios.h>
#include <string.h>
#include <sys/ioctl.h>

#define ANSI_TERMINAL_COLORS

struct set_keypress
{
	set_keypress()
	{
		termios new_settings;
		tcgetattr(0,&stored_settings);
		new_settings = stored_settings;
		// Disable canonical mode, and set buffer size to 1 byte
		new_settings.c_lflag &= (~ICANON);
		new_settings.c_cc[VTIME] = 0;
		new_settings.c_cc[VMIN] = 1;
		tcsetattr(0,TCSANOW,&new_settings);
	}
	~set_keypress() { tcsetattr(0,TCSANOW,&stored_settings); }
	termios stored_settings;
};

bool sleep_and_input(char* c)
{
	// sets the terminal to single-character mode
	// and resets when destructed
	set_keypress s;

	fd_set set;
	FD_ZERO(&set);
	FD_SET(0, &set);
	timeval tv = {0, 500000};
	if (select(1, &set, 0, 0, &tv) > 0)
	{
		*c = getc(stdin);
		return true;
	}
	return false;
}

void clear_home()
{
	std::cout << "\033[2J\033[0;0H";
}

#endif

bool print_trackers = false;
bool print_peers = false;
bool print_log = false;
bool print_downloads = false;
bool print_piece_bar = false;
bool print_file_progress = false;
bool show_pad_files = false;
bool show_dht_status = false;
bool sequential_download = false;

bool print_ip = true;
bool print_as = false;
bool print_timers = false;
bool print_block = false;
bool print_peer_rate = false;
bool print_fails = false;
bool print_send_bufs = true;
std::ofstream g_log_file;

int active_torrent = 0;

char const* esc(char const* code)
{
#ifdef ANSI_TERMINAL_COLORS
	// this is a silly optimization
	// to avoid copying of strings
	enum { num_strings = 200 };
	static char buf[num_strings][20];
	static int round_robin = 0;
	char* ret = buf[round_robin];
	++round_robin;
	if (round_robin >= num_strings) round_robin = 0;
	ret[0] = '\033';
	ret[1] = '[';
	int i = 2;
	int j = 0;
	while (code[j]) ret[i++] = code[j++];
	ret[i++] = 'm';
	ret[i++] = 0;
	return ret;
#else
	return "";
#endif
}

std::string to_string(int v, int width)
{
	std::stringstream s;
	s.flags(std::ios_base::right);
	s.width(width);
	s.fill(' ');
	s << v;
	return s.str();
}

std::string& to_string(float v, int width, int precision = 3)
{
	// this is a silly optimization
	// to avoid copying of strings
	enum { num_strings = 20 };
	static std::string buf[num_strings];
	static int round_robin = 0;
	std::string& ret = buf[round_robin];
	++round_robin;
	if (round_robin >= num_strings) round_robin = 0;
	ret.resize(20);
	int size = std::sprintf(&ret[0], "%*.*f", width, precision, v);
	ret.resize((std::min)(size, width));
	return ret;
}

std::string const& add_suffix(float val)
{
	const char* prefix[] = {"kB", "MB", "GB", "TB"};
	const int num_prefix = sizeof(prefix) / sizeof(const char*);
	for (int i = 0; i < num_prefix; ++i)
	{
		val /= 1000.f;
		if (std::fabs(val) < 1000.f)
		{
			std::string& ret = to_string(val, 4);
			ret += prefix[i];
			return ret;
		}
	}
	std::string& ret = to_string(val, 4);
	ret += "PB";
	return ret;
}

std::string const& piece_bar(libtorrent::bitfield const& p, int width)
{
#ifdef ANSI_TERMINAL_COLORS
	static const char* lookup[] =
	{
		// black, blue, cyan, white
		"40", "44", "46", "47"
	};

	const int table_size = sizeof(lookup) / sizeof(lookup[0]);
#else
	static const char char_lookup[] =
	{ ' ', '.', ':', '-', '+', '*', '#'};

	const int table_size = sizeof(char_lookup) / sizeof(char_lookup[0]);
#endif
	
	double piece_per_char = p.size() / double(width);
	static std::string bar;
	bar.clear();
	bar.reserve(width * 6);
	bar += "[";
	if (p.size() == 0)
	{
		for (int i = 0; i < width; ++i) bar += ' ';
		bar += "]";
		return bar;
	}

	// the [piece, piece + pieces_per_char) range is the pieces that are represented by each character
	double piece = 0;
	for (int i = 0; i < width; ++i, piece += piece_per_char)
	{
		int num_pieces = 0;
		int num_have = 0;
		int end = (std::max)(int(piece + piece_per_char), int(piece) + 1);
		for (int k = int(piece); k < end; ++k, ++num_pieces)
			if (p[k]) ++num_have;
		int color = int(std::ceil(num_have / float(num_pieces) * (table_size - 1)));
#ifdef ANSI_TERMINAL_COLORS
		bar += esc(lookup[color]);
		bar += " ";
#else
		bar += char_lookup[color];
#endif
	}
#ifdef ANSI_TERMINAL_COLORS
	bar += esc("0");
#endif
	bar += "]";
	return bar;
}

std::string const& progress_bar(float progress, int width, char const* code = "33")
{
	static std::string bar;
	bar.clear();
	bar.reserve(width + 10);

	int progress_chars = static_cast<int>(progress * width + .5f);
	bar = esc(code);
	std::fill_n(std::back_inserter(bar), progress_chars, '#');
	bar += esc("0");
	std::fill_n(std::back_inserter(bar), width - progress_chars, '-');
	return bar;
}

int peer_index(libtorrent::tcp::endpoint addr, std::vector<libtorrent::peer_info> const& peers)
{
	using namespace libtorrent;
	std::vector<peer_info>::const_iterator i = std::find_if(peers.begin()
		, peers.end(), bind(&peer_info::ip, _1) == addr);
	if (i == peers.end()) return -1;

	return i - peers.begin();
}

void print_peer_info(std::ostream& out, std::vector<libtorrent::peer_info> const& peers)
{
	using namespace libtorrent;
	if (print_ip) out << "IP                     ";
#ifndef TORRENT_DISABLE_GEO_IP
	if (print_as) out << "AS                                         ";
#endif
	out << "down     (total | peak   )  up      (total | peak   ) sent-req recv flags         source ";
	if (print_fails) out << "fail hshf ";
	if (print_send_bufs) out << "rq sndb            quota rcvb            q-bytes ";
	if (print_timers) out << "inactive wait timeout q-time ";
	out << "disk   rtt ";
	if (print_block) out << "block-progress ";
#ifndef TORRENT_DISABLE_RESOLVE_COUNTRIES
	out << "country ";
#endif
	if (print_peer_rate) out << "peer-rate ";
	out << "client \n";

	for (std::vector<peer_info>::const_iterator i = peers.begin();
		i != peers.end(); ++i)
	{
		if (i->flags & (peer_info::handshake | peer_info::connecting | peer_info::queued))
			continue;

		out.fill(' ');
		if (print_ip)
		{
			error_code ec;
			std::stringstream ip;
			ip << i->ip.address().to_string(ec) << ":" << i->ip.port();
			out.width(22);
			out << ip.str() << " ";
		}

#ifndef TORRENT_DISABLE_GEO_IP
		if (print_as)
		{
			std::string as_name = i->inet_as_name;
			if (as_name.size() > 42) as_name.resize(42);
			out.width(42);
			out << as_name << " ";
		}
#endif
		out.width(2);
		out << esc("32") << (i->down_speed > 0 ? add_suffix(i->down_speed) + "/s " : "         ")
			<< "(" << (i->total_download > 0 ? add_suffix(i->total_download) : "      ") << "|"
			<< (i->download_rate_peak > 0 ? add_suffix(i->download_rate_peak) + "/s" : "        ") << ") " << esc("0")
			<< esc("31") << (i->up_speed > 0 ? add_suffix(i->up_speed) + "/s ": "         ")
			<< "(" << (i->total_upload > 0 ? add_suffix(i->total_upload) : "      ") << "|"
			<< (i->upload_rate_peak > 0 ? add_suffix(i->upload_rate_peak) + "/s" : "        ") << ") " << esc("0")
			<< to_string(i->download_queue_length, 3) << " ("
			<< to_string(i->target_dl_queue_length, 3) << ") "
			<< to_string(i->upload_queue_length, 3) << " "
			<< ((i->flags & peer_info::interesting)?'I':'.')
			<< ((i->flags & peer_info::choked)?'C':'.')
			<< ((i->flags & peer_info::remote_interested)?'i':'.')
			<< ((i->flags & peer_info::remote_choked)?'c':'.')
			<< ((i->flags & peer_info::supports_extensions)?'e':'.')
			<< ((i->flags & peer_info::local_connection)?'l':'r')
			<< ((i->flags & peer_info::seed)?'s':'.')
			<< ((i->flags & peer_info::on_parole)?'p':'.')
			<< ((i->flags & peer_info::optimistic_unchoke)?'O':'.')
			<< ((i->read_state == peer_info::bw_limit)?'r':
				(i->read_state == peer_info::bw_network)?'R':'.')
			<< ((i->write_state == peer_info::bw_limit)?'w':
				(i->write_state == peer_info::bw_network)?'W':'.')
			<< ((i->flags & peer_info::snubbed)?'S':'.')
			<< ((i->flags & peer_info::upload_only)?'U':'D')
#ifndef TORRENT_DISABLE_ENCRYPTION
			<< ((i->flags & peer_info::rc4_encrypted)?'E':
				(i->flags & peer_info::plaintext_encrypted)?'e':'.')
#else
			<< ".."
#endif
			<< " "
			<< ((i->source & peer_info::tracker)?"T":"_")
			<< ((i->source & peer_info::pex)?"P":"_")
			<< ((i->source & peer_info::dht)?"D":"_")
			<< ((i->source & peer_info::lsd)?"L":"_")
			<< ((i->source & peer_info::resume_data)?"R":"_") << "  ";
		if (print_fails)
		{
			out << to_string(i->failcount, 3) << " "
				<< to_string(i->num_hashfails, 3) << " ";
		}
		if (print_send_bufs)
		{
			out << to_string(i->requests_in_buffer, 2) << " "
				<< to_string(i->used_send_buffer, 6) << " ("<< add_suffix(i->send_buffer_size) << ") "
				<< to_string(i->send_quota, 5) << " "
				<< to_string(i->used_receive_buffer, 6) << " ("<< add_suffix(i->receive_buffer_size) << ") "
				<< to_string(i->queue_bytes, 6) << " ";
		}
		if (print_timers)
		{
			out << to_string(total_seconds(i->last_active), 8) << " "
				<< to_string(total_seconds(i->last_request), 4) << " "
				<< to_string(i->request_timeout, 7) << " "
				<< to_string(total_seconds(i->download_queue_time), 6) << " ";
		}
		out << add_suffix(i->pending_disk_bytes) << " "
			<< to_string(i->rtt, 4) << " ";

		if (print_block)
		{
			if (i->downloading_piece_index >= 0)
			{
				out << progress_bar(
					i->downloading_progress / float(i->downloading_total), 14);
			}
			else
			{
				out << progress_bar(0.f, 14);
			}
		}

#ifndef TORRENT_DISABLE_RESOLVE_COUNTRIES
		if (i->country[0] == 0)
		{
			out << " ..";
		}
		else
		{
			out << " " << i->country[0] << i->country[1];
		}
#endif
		if (print_peer_rate) out << " " << (i->remote_dl_rate > 0 ? add_suffix(i->remote_dl_rate) + "/s ": "         ");
		out << " ";

		if (i->flags & peer_info::handshake)
		{
			out << esc("31") << " waiting for handshake" << esc("0") << "\n";
		}
		else if (i->flags & peer_info::connecting)
		{
			out << esc("31") << " connecting to peer" << esc("0") << "\n";
		}
		else if (i->flags & peer_info::queued)
		{
			out << esc("33") << " queued" << esc("0") << "\n";
		}
		else
		{
			out << " " << i->client << "\n";
		}
	}
}

typedef std::multimap<std::string, libtorrent::torrent_handle> handles_t;

using boost::bind;
using boost::filesystem::path;
using boost::filesystem::exists;
using boost::filesystem::directory_iterator;
using boost::filesystem::extension;


// monitored_dir is true if this torrent is added because
// it was found in the directory that is monitored. If it
// is, it should be remembered so that it can be removed
// if it's no longer in that directory.
void add_torrent(libtorrent::session& ses
	, handles_t& handles
	, std::string const& torrent
	, float preferred_ratio
	, bool compact_mode
	, path const& save_path
	, bool monitored_dir
	, int torrent_upload_limit
	, int torrent_download_limit)
{
	using namespace libtorrent;

	boost::intrusive_ptr<torrent_info> t;
	error_code ec;
	t = new torrent_info(torrent.c_str(), ec);
	if (ec)
	{
		std::cout << torrent << ": " << ec.message() << std::endl;
		return;
	}

	std::cout << t->name() << std::endl;

	add_torrent_params p;
	lazy_entry resume_data;

	std::string filename = (save_path / (t->name() + ".fastresume")).string();

	std::vector<char> buf;
	if (load_file(filename.c_str(), buf) == 0)
		p.resume_data = &buf;

	p.ti = t;
	p.save_path = save_path;
	p.storage_mode = compact_mode ? storage_mode_compact : storage_mode_sparse;
	p.paused = true;
	p.duplicate_is_error = false;
	p.auto_managed = true;
	torrent_handle h = ses.add_torrent(p, ec);

	handles.insert(std::make_pair(
		monitored_dir?std::string(torrent):std::string(), h));

	h.set_max_connections(50);
	h.set_max_uploads(-1);
	h.set_ratio(preferred_ratio);
	h.set_upload_limit(torrent_upload_limit);
	h.set_download_limit(torrent_download_limit);
#ifndef TORRENT_DISABLE_RESOLVE_COUNTRIES
	h.resolve_countries(true);
#endif
}

void scan_dir(path const& dir_path
	, libtorrent::session& ses
	, handles_t& handles
	, float preferred_ratio
	, bool compact_mode
	, path const& save_path
	, int torrent_upload_limit
	, int torrent_download_limit)
{
	std::set<std::string> valid;

	using namespace libtorrent;

	for (directory_iterator i(dir_path), end; i != end; ++i)
	{
		if (extension(*i) != ".torrent") continue;
		std::string file = i->path().string();

		handles_t::iterator k = handles.find(file);
		if (k != handles.end())
		{
			valid.insert(file);
			continue;
		}

		// the file has been added to the dir, start
		// downloading it.
		add_torrent(ses, handles, file, preferred_ratio, compact_mode
			, save_path, true, torrent_upload_limit, torrent_download_limit);
		valid.insert(file);
	}

	// remove the torrents that are no longer in the directory

	for (handles_t::iterator i = handles.begin(); !handles.empty() && i != handles.end();)
	{
		if (i->first.empty() || valid.find(i->first) != valid.end())
		{
			++i;
			continue;
		}

		torrent_handle& h = i->second;
		if (!h.is_valid())
		{
			handles.erase(i++);
			continue;
		}
		
		h.auto_managed(false);
		h.pause();
		// the alert handler for save_resume_data_alert
		// will save it to disk
		h.save_resume_data();

		handles.erase(i++);
	}
}

libtorrent::torrent_handle get_active_torrent(handles_t const& handles)
{
	if (active_torrent >= handles.size()
		|| active_torrent < 0) return libtorrent::torrent_handle();
	handles_t::const_iterator i = handles.begin();
	std::advance(i, active_torrent);
	return i->second;
}

void print_alert(libtorrent::alert const* a, std::ostream& os)
{
	using namespace libtorrent;

#ifdef ANSI_TERMINAL_COLORS
	if (a->category() & alert::error_notification)
	{
		os << esc("31");
	}
	else if (a->category() & (alert::peer_notification | alert::storage_notification))
	{
		os << esc("33");
	}
#endif
	os << "[" << time_now_string() << "] " << a->message();
#ifdef ANSI_TERMINAL_COLORS
	os << esc("0");
#endif

	if (g_log_file.good())
		g_log_file << "[" << time_now_string() << "] " << a->message() << std::endl;
}

void handle_alert(libtorrent::session& ses, libtorrent::alert* a
	, handles_t const& handles)
{
	using namespace libtorrent;

	if (torrent_finished_alert* p = dynamic_cast<torrent_finished_alert*>(a))
	{
		p->handle.set_max_connections(30);

		// write resume data for the finished torrent
		// the alert handler for save_resume_data_alert
		// will save it to disk
		torrent_handle h = p->handle;
		h.save_resume_data();
	}
	else if (save_resume_data_alert* p = dynamic_cast<save_resume_data_alert*>(a))
	{
		torrent_handle h = p->handle;
		TORRENT_ASSERT(p->resume_data);
		if (p->resume_data)
		{
			boost::filesystem::ofstream out(h.save_path() / (h.name() + ".fastresume")
				, std::ios_base::binary);
			out.unsetf(std::ios_base::skipws);
			bencode(std::ostream_iterator<char>(out), *p->resume_data);
			if (std::find_if(handles.begin(), handles.end()
				, bind(&handles_t::value_type::second, _1) == h) == handles.end())
				ses.remove_torrent(h);
		}
	}
	else if (save_resume_data_failed_alert* p = dynamic_cast<save_resume_data_failed_alert*>(a))
	{
		torrent_handle h = p->handle;
		if (std::find_if(handles.begin(), handles.end()
			, bind(&handles_t::value_type::second, _1) == h) == handles.end())
			ses.remove_torrent(h);
	}
}

static char const* state_str[] =
	{"checking (q)", "checking", "dl metadata"
	, "downloading", "finished", "seeding", "allocating", "checking (r)"};

int main(int argc, char* argv[])
{
#if BOOST_VERSION < 103400
	using boost::filesystem::no_check;
	path::default_name_check(no_check);
#endif

	if (argc == 1)
	{
		std::cerr << "usage: client_test [OPTIONS] [TORRENT|MAGNETURL]\n\n"
			"OPTIONS:\n"
			"  -f <log file>         logs all events to the given file\n"
			"  -o <limit>            limits the number of simultaneous\n"
			"                        half-open TCP connections to the\n"
			"                        given number.\n"
			"  -p <port>             sets the listen port\n"
			"  -r <ratio>            sets the preferred share ratio\n"
			"  -d <rate>             limits the download rate\n"
			"  -u <rate>             limits the upload rate\n"
			"  -S <limit>            limits the upload slots\n"
			"  -a <mode>             sets the allocation mode. [compact|full]\n"
			"  -s <path>             sets the save path for downloads\n"
			"  -U <rate>             sets per-torrent upload rate\n"
			"  -D <rate>             sets per-torrent download rate\n"
			"  -m <path>             sets the .torrent monitor directory\n"
			"  -b <IP>               sets IP of the interface to bind the\n"
			"                        listen socket to\n"
			"  -w <seconds>          sets the retry time for failed web seeds\n"
			"  -t <seconds>          sets the scan interval of the monitor dir\n"
			"  -x <file>             loads an emule IP-filter file\n"
			"  -c <limit>            sets the max number of connections\n"
			"  -C <limit>            sets the max cache size. Specified in 16kB blocks\n"
			"\n\n"
			"TORRENT is a path to a .torrent file\n"
			"MAGNETURL is a magnet: url\n"
			;
		return 0;
	}

	using namespace libtorrent;
	session_settings settings;
	proxy_settings ps;

	settings.user_agent = "client_test/" LIBTORRENT_VERSION;
	settings.auto_upload_slots_rate_based = true;
	settings.announce_to_all_trackers = true;

	std::deque<std::string> events;

	ptime next_dir_scan = time_now();

	// the string is the filename of the .torrent file, but only if
	// it was added through the directory monitor. It is used to
	// be able to remove torrents that were added via the directory
	// monitor when they're not in the directory anymore.
	handles_t handles;
	session ses(fingerprint("LT", LIBTORRENT_VERSION_MAJOR, LIBTORRENT_VERSION_MINOR, 0, 0)
		, session::start_default_features | session::add_default_plugins, alert::all_categories);

	boost::filesystem::ifstream ses_state_file(".ses_state"
		, std::ios_base::binary);
	ses_state_file.unsetf(std::ios_base::skipws);
	ses.load_state(bdecode(
		std::istream_iterator<char>(ses_state_file)
		, std::istream_iterator<char>()));

#ifndef TORRENT_DISABLE_DHT
	settings.use_dht_as_fallback = false;

	ses.add_dht_router(std::make_pair(
			std::string("router.bittorrent.com"), 6881));
	ses.add_dht_router(std::make_pair(
			std::string("router.utorrent.com"), 6881));
	ses.add_dht_router(std::make_pair(
			std::string("router.bitcomet.com"), 6881));

	boost::filesystem::ifstream dht_state_file(".dht_state"
		, std::ios_base::binary);
	dht_state_file.unsetf(std::ios_base::skipws);
	entry dht_state;
	dht_state = bdecode(
		std::istream_iterator<char>(dht_state_file)
		, std::istream_iterator<char>());
	ses.start_dht(dht_state);
#endif

#ifndef TORRENT_DISABLE_GEO_IP
	ses.load_asnum_db("GeoIPASNum.dat");
	ses.load_country_db("GeoIP.dat");
#endif

	int listen_port = 6881;
	float preferred_ratio = 0.f;
	std::string allocation_mode = "sparse";
	boost::filesystem::path save_path(".");
	int torrent_upload_limit = 0;
	int torrent_download_limit = 0;
	boost::filesystem::path monitor_dir;
	std::string bind_to_interface = "";
	int poll_interval = 5;

	// load the torrents given on the commandline
	boost::regex ex("([0-9A-Fa-f]{40})@(.+)");

	for (int i = 1; i < argc; ++i)
	{
		if (argv[i][0] != '-')
		{
			// interpret this as a torrent

			// first see if this is a torrentless download
			if (std::strstr("magnet:", argv[i]) == argv[i])
			{
				add_torrent_params p;
				p.save_path = save_path;
				p.storage_mode = allocation_mode == "compact" ? storage_mode_compact
					: storage_mode_sparse;
				std::cout << "adding MANGET link: " << argv[i] << std::endl;
				error_code ec;
				torrent_handle h = add_magnet_uri(ses, argv[i], p, ec);
				if (ec)
				{
					std::cerr << ec.message() << std::endl;
					continue;
				}

				handles.insert(std::make_pair(std::string(), h));

				h.set_max_connections(50);
				h.set_max_uploads(-1);
				h.set_ratio(preferred_ratio);
				h.set_upload_limit(torrent_upload_limit);
				h.set_download_limit(torrent_download_limit);
				continue;
			}

			// match it against the <hash>@<tracker> format
			boost::cmatch what;
			if (boost::regex_match(argv[i], what, ex))
			{
				sha1_hash info_hash = boost::lexical_cast<sha1_hash>(what[1]);

				add_torrent_params p;
				p.name = std::string(what[2]).c_str();
				p.info_hash = info_hash;
				p.save_path = save_path;
				p.storage_mode = allocation_mode == "compact" ? storage_mode_compact
					: storage_mode_sparse;
				p.paused = true;
				p.duplicate_is_error = false;
				p.auto_managed = true;
				error_code ec;
				torrent_handle h = ses.add_torrent(p, ec);
				if (ec)
				{
					std::cerr << ec.message() << std::endl;
					continue;
				}

				handles.insert(std::make_pair(std::string(), h));

				h.set_max_connections(50);
				h.set_max_uploads(-1);
				h.set_ratio(preferred_ratio);
				h.set_upload_limit(torrent_upload_limit);
				h.set_download_limit(torrent_download_limit);
				continue;
			}

			// if it's a torrent file, open it as usual
			add_torrent(ses, handles, argv[i], preferred_ratio
				, allocation_mode == "compact", save_path, false
				, torrent_upload_limit, torrent_download_limit);
			continue;
		}

		// if there's a flag but no argument following, ignore it
		if (argc == i) continue;
		char const* arg = argv[i+1];
		switch (argv[i][1])
		{
			case 'f': g_log_file.open(arg); break;
			case 'o': ses.set_max_half_open_connections(atoi(arg)); break;
			case 'p': listen_port = atoi(arg); break;
			case 'r':
				preferred_ratio = atoi(arg);
				if (preferred_ratio != 0 && preferred_ratio < 1.f) preferred_ratio = 1.f;
				break;
			case 'd': ses.set_download_rate_limit(atoi(arg) * 1000); break;
			case 'u': ses.set_upload_rate_limit(atoi(arg) * 1000); break;
			case 'S': ses.set_max_uploads(atoi(arg)); break;
			case 'a': allocation_mode = arg; break;
			case 's': save_path = arg; break;
			case 'U': torrent_upload_limit = atoi(arg) * 1000; break;
			case 'D': torrent_download_limit = atoi(arg) * 1000; break;
			case 'm': monitor_dir = arg; break;
			case 'b': bind_to_interface = arg; break;
			case 'w': settings.urlseed_wait_retry = atoi(arg); break;
			case 't': poll_interval = atoi(arg); break;
			case 'x':
				{
					std::ifstream in(arg);
					ip_filter filter;
					while (in.good())
					{
						char line[300];
						in.getline(line, 300);
						int len = in.gcount();
						if (len <= 0) continue;
						if (line[0] == '#') continue;
						int a, b, c, d;
						char dummy;
						std::stringstream ln(line);
						ln >> a >> dummy >> b >> dummy >> c >> dummy >> d >> dummy;
						address_v4 start((a << 24) + (b << 16) + (c << 8) + d);
						ln >> a >> dummy >> b >> dummy >> c >> dummy >> d;
						address_v4 last((a << 24) + (b << 16) + (c << 8) + d);
						int flags;
						ln >> flags;
						if (flags <= 127) flags = ip_filter::blocked;
						else flags = 0;
						if (ln.fail()) break;
						filter.add_rule(start, last, flags);
					}
					ses.set_ip_filter(filter);
				}
				break;
			case 'c': ses.set_max_connections(atoi(arg)); break;
			case 'C': settings.cache_size = atoi(arg); break;
		}
		++i; // skip the argument
	}

	ses.listen_on(std::make_pair(listen_port, listen_port + 10)
		, bind_to_interface.c_str());

	ses.set_settings(settings);

	// main loop
	std::vector<peer_info> peers;
	std::vector<partial_piece_info> queue;

	for (;;)
	{
		char c;
		while (sleep_and_input(&c))
		{
			if (c == 27)
			{
				// escape code, read another character
#ifdef _WIN32
				c = _getch();
#else
				c = getc(stdin);
#endif
				if (c != '[') break;
#ifdef _WIN32
				c = _getch();
#else
				c = getc(stdin);
#endif
				if (c == 65)
				{
					// arrow up
					--active_torrent;
					if (active_torrent < 0) active_torrent = 0;
				}
				else if (c == 66)
				{
					// arrow down
					++active_torrent;
					if (active_torrent >= handles.size()) active_torrent = handles.size() - 1;
				}
			}

			if (c == ' ')
			{
				if (ses.is_paused()) ses.resume();
				else ses.pause();
			}

			if (c == 'm')
			{
				std::cout << "saving peers for torrents" << std::endl;

				std::vector<peer_list_entry> peers;
				for (handles_t::iterator i = handles.begin();
					i != handles.end(); ++i)
				{
					i->second.get_full_peer_list(peers);
					std::ofstream f(("peers_" + i->second.name()).c_str());
					for (std::vector<peer_list_entry>::iterator k = peers.begin()
						, end(peers.end()); k != end; ++k)
					{
						f << k->ip.address()
#ifndef TORRENT_DISABLE_GEO_IP
							<< "\t" << ses.as_for_ip(k->ip.address())
#endif
							<< std::endl;
					}
				}
			}

			if (c == 'q')
			{
				// keep track of the number of resume data
				// alerts to wait for
				int num_resume_data = 0;
				ses.pause();
				for (handles_t::iterator i = handles.begin();
					i != handles.end(); ++i)
				{
					torrent_handle& h = i->second;
					if (!h.is_valid()) continue;
					if (h.is_paused()) continue;
					if (!h.has_metadata()) continue;

					std::cout << "saving resume data for " << h.name() << std::endl;
					// save_resume_data will generate an alert when it's done
					h.save_resume_data();
					++num_resume_data;
				}
				std::cout << "waiting for resume data" << std::endl;

				while (num_resume_data > 0)
				{
					alert const* a = ses.wait_for_alert(seconds(30));
					if (a == 0)
					{
						std::cout << " aborting with " << num_resume_data << " outstanding "
							"torrents to save resume data for" << std::endl;
						break;
					}

					std::auto_ptr<alert> holder = ses.pop_alert();

					::print_alert(holder.get(), std::cout);
					std::cout << std::endl;

					if (dynamic_cast<save_resume_data_failed_alert const*>(a))
					{
						--num_resume_data;
						continue;
					}

					save_resume_data_alert const* rd = dynamic_cast<save_resume_data_alert const*>(a);
					if (!rd) continue;
					--num_resume_data;

					if (!rd->resume_data) continue;

					torrent_handle h = rd->handle;
					boost::filesystem::ofstream out(h.save_path()
						/ (h.get_torrent_info().name() + ".fastresume"), std::ios_base::binary);
					out.unsetf(std::ios_base::skipws);
					bencode(std::ostream_iterator<char>(out), *rd->resume_data);
				}
				break;
			}

			if (c == 'j')
			{
				torrent_handle h = get_active_torrent(handles);
				if (h.is_valid()) h.force_recheck();
			}

			if (c == 'r')
			{
				torrent_handle h = get_active_torrent(handles);
				if (h.is_valid()) h.force_reannounce();
			}

			if (c == 's')
			{
				torrent_handle h = get_active_torrent(handles);
				if (h.is_valid()) h.set_sequential_download(!h.is_sequential_download());
			}

			if (c == 'v')
			{
				torrent_handle h = get_active_torrent(handles);
				if (h.is_valid()) h.scrape_tracker();
			}

			if (c == 'p')
			{
				torrent_handle h = get_active_torrent(handles);
				if (h.is_valid())
				{
					if (!h.is_auto_managed() && h.is_paused())
					{
						h.auto_managed(true);
					}
					else
					{
						h.auto_managed(false);
						h.pause();
					}
					// the alert handler for save_resume_data_alert
					// will save it to disk
					h.save_resume_data();
				}
			}

			if (c == 'c')
			{
				torrent_handle h = get_active_torrent(handles);
				if (h.is_valid()) h.clear_error();
			}

			// toggle displays
			if (c == 't') print_trackers = !print_trackers;
			if (c == 'i') print_peers = !print_peers;
			if (c == 'l') print_log = !print_log;
			if (c == 'd') print_downloads = !print_downloads;
			if (c == 'f') print_file_progress = !print_file_progress;
			if (c == 'h') show_pad_files = !show_pad_files;
			if (c == 'a') print_piece_bar = !print_piece_bar;
			if (c == 'g') show_dht_status = !show_dht_status;
			// toggle columns
			if (c == '1') print_ip = !print_ip;
			if (c == '2') print_as = !print_as;
			if (c == '3') print_timers = !print_timers;
			if (c == '4') print_block = !print_block;
			if (c == '5') print_peer_rate = !print_peer_rate;
			if (c == '6') print_fails = !print_fails;
			if (c == '7') print_send_bufs = !print_send_bufs;
		}
		if (c == 'q') break;

		int terminal_width = 80;

#ifndef _WIN32
		{
			winsize size;
			ioctl(STDOUT_FILENO, TIOCGWINSZ, (char*)&size);
			terminal_width = size.ws_col;
		}
#endif

		// loop through the alert queue to see if anything has happened.
		std::auto_ptr<alert> a;
		a = ses.pop_alert();
		std::string now = time_now_string();
		while (a.get())
		{
			std::stringstream event_string;

			::print_alert(a.get(), event_string);
			::handle_alert(ses, a.get(), handles);

			events.push_back(event_string.str());
			if (events.size() >= 20) events.pop_front();

			a = ses.pop_alert();
		}

		session_status sess_stat = ses.status();

		std::stringstream out;
		out << "[q] quit [i] toggle peers [d] toggle downloading pieces [p] toggle paused "
			"[a] toggle piece bar [s] toggle download sequential [f] toggle files "
			"[j] force recheck [space] toggle session pause [c] clear error [v] scrape [g] show DHT\n"
			"[1] toggle IP [2] toggle AS [3] toggle timers [4] toggle block progress "
			"[5] toggle peer rate [6] toggle failures [7] toggle send buffers\n";

		int torrent_index = 0;
		torrent_handle active_handle;
		for (handles_t::iterator i = handles.begin();
			i != handles.end(); ++torrent_index)
		{
			torrent_handle& h = i->second;
			if (!h.is_valid())
			{
				handles.erase(i++);
				continue;
			}
			else
			{
				++i;
			}

#ifdef ANSI_TERMINAL_COLORS
			char const* term = "\x1b[0m";
#else
			char const* term = "";
#endif
			if (active_torrent == torrent_index)
			{
				term = "\x1b[0m\x1b[7m";
				out << esc("7") << "*";
			}
			else
			{
				out << " ";
			}

			int queue_pos = h.queue_position();
			if (queue_pos == -1) out << "-  ";
			else out << std::setw(3) << queue_pos;

			if (h.is_paused()) out << esc("34");
			else out << esc("37");
			out << std::setw(40) << std::setiosflags(std::ios::left);

			std::string name = h.name();
			if (name.size() > 40) name.resize(40);
			out << name;

			out << term << " ";

			torrent_status s = h.status();

			bool paused = h.is_paused();
			bool auto_managed = h.is_auto_managed();
			bool sequential_download = h.is_sequential_download();
			out << std::setw(13) << std::setiosflags(std::ios::left);
			if (!s.error.empty())
			{
				out << esc("31") << "error " << s.error;
				out << esc("0") << std::endl;
				continue;
			}

			if (paused && !auto_managed) out << "paused";
			else if (paused && auto_managed) out << "queued";
			else out << state_str[s.state];

			int seeds = 0;
			int downloaders = 0;

			if (s.num_complete >= 0) seeds = s.num_complete;
			else seeds = s.list_seeds;

			if (s.num_incomplete >= 0) downloaders = s.num_incomplete;
			else downloaders = s.list_peers - s.list_seeds;

			out << "download: " << "(" << esc("32") << add_suffix(s.total_download) << term << ") "
				"upload: " << esc("31") << (s.upload_rate > 0 ? add_suffix(s.upload_rate) + "/s ": "         ") << term
				<< "(" << esc("31") << add_suffix(s.total_upload) << term << ") "
				<< "swarm: " << to_string(downloaders, 4) << ":" << to_string(seeds, 4)
				<< "  bw queue: (" << s.up_bandwidth_queue << " | " << s.down_bandwidth_queue << ") "
				"all-time (Rx: " << esc("32") << add_suffix(s.all_time_download) << term
				<< " Tx: " << esc("31") << add_suffix(s.all_time_upload) << term << ") "
				<< std::hex << s.seed_rank << std::dec << " "
				<< s.last_scrape << "\n" << esc("0");

			if (torrent_index != active_torrent && s.state == torrent_status::seeding) continue;
			char const* progress_bar_color = "33"; // yellow
			if (s.state == torrent_status::checking_files
				|| s.state == torrent_status::downloading_metadata)
			{
				progress_bar_color = "35"; // magenta
			}
			else if (s.current_tracker.empty())
			{
				progress_bar_color = "31"; // red
			}
			else if (sess_stat.has_incoming_connections)
			{
				progress_bar_color = "32"; // green
			}
			if (sequential_download)
				out << "sequential: ";
			else
				out << "  progress: ";

			out << esc("32") << s.total_done << esc("0") << " Bytes ";
			out.precision(4);
			out.width(5);
			out.fill(' ');
			out << (s.progress*100) << "% ";
			out << progress_bar(s.progress, terminal_width - 37, progress_bar_color) << "\n";
			if (print_piece_bar && s.progress < 1.f)
				out << "  " << piece_bar(s.pieces, terminal_width - 5) << "\n";
			out << "  peers: " << esc("37") << s.num_peers << esc("0") << " (" << esc("37") << s.connect_candidates << esc("0") << ") "
				<< "seeds: " << esc("37") << s.num_seeds << esc("0") << " "
				<< "distributed copies: " << esc("37") << s.distributed_copies << esc("0")
				<< " sparse regions: " << s.sparse_regions
				//					<< " magnet-link: " << make_magnet_uri(h) << "\n"
				<< " download: " << esc("32") << (s.download_rate > 0 ? add_suffix(s.download_rate) + "/s ": "         ") << esc("0");
			boost::posix_time::time_duration t = s.next_announce;
			out << " next announce: " << esc("37")
				<< to_string(t.hours(), 2) << ":"
				<< to_string(t.minutes(), 2) << ":"
				<< to_string(t.seconds(), 2) << esc("0") << " ";
			out << "tracker: " << esc("36") << s.current_tracker << esc("0") << "\n";

			if (torrent_index != active_torrent) continue;
			active_handle = h;
		}

		cache_status cs = ses.get_cache_status();
		if (cs.blocks_read < 1) cs.blocks_read = 1;
		if (cs.blocks_written < 1) cs.blocks_written = 1;

		out << "==== conns: " << sess_stat.num_peers
			<< " down: " << esc("32") << add_suffix(sess_stat.download_rate) << "/s" << esc("0")
			<< " (" << esc("32") << add_suffix(sess_stat.total_download) << esc("0") << ")"
			" up: " << esc("31") << add_suffix(sess_stat.upload_rate) << "/s" << esc("0")
			<< " (" << esc("31") << add_suffix(sess_stat.total_upload) << esc("0") << ")"
			" tcp/ip: "
			<< esc("32") << add_suffix(sess_stat.ip_overhead_download_rate) << "/s" << esc("0") << " "
			<< esc("31") << add_suffix(sess_stat.ip_overhead_upload_rate) << "/s" << esc("0")
			<< " DHT: "
			<< esc("32") << add_suffix(sess_stat.dht_download_rate) << "/s" << esc("0") << " "
			<< esc("31") << add_suffix(sess_stat.dht_upload_rate) << "/s" << esc("0")
			<< " tracker: "
			<< esc("32") << add_suffix(sess_stat.tracker_download_rate) << "/s" << esc("0") << " "
			<< esc("31") << add_suffix(sess_stat.tracker_upload_rate) << "/s" << esc("0") << " ====\n"
			"==== waste: " << add_suffix(sess_stat.total_redundant_bytes)
			<< " fail: " << add_suffix(sess_stat.total_failed_bytes)
			<< " unchoked: " << sess_stat.num_unchoked << " / " << sess_stat.allowed_upload_slots
			<< " bw queues: " << sess_stat.up_bandwidth_bytes_queue
			<< " (" << sess_stat.up_bandwidth_queue<< ")"
			<< " | " << sess_stat.down_bandwidth_bytes_queue
			<< " (" << sess_stat.down_bandwidth_queue<< ") "
			" write cache hits: " << ((cs.blocks_written - cs.writes) * 100 / cs.blocks_written) << "% "
			" read cache hits: " << (cs.blocks_read_hit * 100 / cs.blocks_read) << "% "
			" cache size: " << add_suffix(cs.cache_size * 16 * 1024)
			<< " (" << add_suffix(cs.read_cache_size * 16 * 1024) << ") / "
			<< add_suffix(cs.total_used_buffers * 16 * 1024) <<
			" ====\n"
			"==== optimistic unchoke: " << sess_stat.optimistic_unchoke_counter
			<< " unchoke counter: " << sess_stat.unchoke_counter
			<< " ====" << std::endl;

		if (show_dht_status)
		{
			out << "DHT nodes: " << sess_stat.dht_nodes
				<< " DHT cached nodes: " << sess_stat.dht_node_cache
				<< " total DHT size: " << sess_stat.dht_global_nodes << std::endl;
			for (std::vector<dht_lookup>::iterator i = sess_stat.active_requests.begin()
				, end(sess_stat.active_requests.end()); i != end; ++i)
			{
				out << "  " << i->type << " " << i->outstanding_requests << " ("
					<< i->branch_factor << ") ( timeouts "
					<< i->timeouts << " responses " << i->responses << ")\n";
			}
		}

		if (active_handle.is_valid())
		{
			torrent_handle h = active_handle;
			torrent_status s = h.status();

			if ((print_downloads && s.state != torrent_status::seeding)
				|| print_peers)
				h.get_peer_info(peers);

			out << "====== " << h.name() << " ======" << std::endl;

			if (print_peers && !peers.empty())
				print_peer_info(out, peers);

			if (print_trackers)
			{
				std::vector<announce_entry> tr = h.trackers();
				ptime now = time_now();
				for (std::vector<announce_entry>::iterator i = tr.begin()
					, end(tr.end()); i != end; ++i)
				{
					std::string url = i->url;
					url.resize(55, ' ');
					out << to_string(i->tier, 2) << " " << url << " "
						<< to_string(i->fails, 3) << " " << (i->verified?"OK ":"-  ");
					if (i->updating) out << "updating";
					else out << to_string(total_seconds(i->next_announce - now), 8);
					out << "\n";
				}
			}

			if (print_downloads)
			{
				h.get_download_queue(queue);
				std::sort(queue.begin(), queue.end(), bind(&partial_piece_info::piece_index, _1)
					< bind(&partial_piece_info::piece_index, _2));

				std::vector<cached_piece_info> pieces;
				ses.get_cache_info(h.info_hash(), pieces);

				for (std::vector<partial_piece_info>::iterator i = queue.begin();
					i != queue.end(); ++i)
				{
					cached_piece_info* cp = 0;
					std::vector<cached_piece_info>::iterator cpi = std::find_if(pieces.begin(), pieces.end()
						, bind(&cached_piece_info::piece, _1) == i->piece_index);
					if (cpi != pieces.end()) cp = &*cpi;

					out << to_string(i->piece_index, 4) << ": [";
					for (int j = 0; j < i->blocks_in_piece; ++j)
					{
						int index = peer_index(i->blocks[j].peer, peers);
						char str[] = "+";
						if (index >= 0)
							str[0] = (index < 10)?'0' + index:'A' + index - 10;

#ifdef ANSI_TERMINAL_COLORS
						if (cp && cp->blocks[j]) out << esc("36;7") << str << esc("0");
						else if (i->blocks[j].bytes_progress > 0
							&& i->blocks[j].state == block_info::requested)
						{
							if (i->blocks[j].num_peers > 1)
								out << esc("1;7");
							else
								out << esc("33;7");
							out << to_string(i->blocks[j].bytes_progress / float(i->blocks[j].block_size) * 10, 1) << esc("0");
						}
						else if (i->blocks[j].state == block_info::finished) out << esc("32;7") << str << esc("0");
						else if (i->blocks[j].state == block_info::writing) out << esc("35;7") << str << esc("0");
						else if (i->blocks[j].state == block_info::requested) out << str;
						else out << " ";
#else
						if (cp && cp->blocks[j]) out << "c";
						else if (i->blocks[j].state == block_info::finished) out << "#";
						else if (i->blocks[j].state == block_info::writing) out << "+";
						else if (i->blocks[j].state == block_info::requested) out << str;
						else out << " ";
#endif
					}
					char const* piece_state[4] = {"", "slow", "medium", "fast"};
					out << "] " << piece_state[i->piece_state];
					if (cp) out << (i->piece_state > 0?" | ":"") << "cache age: " << (total_milliseconds(time_now() - cp->last_use) / 1000.f);
					out << "\n";
				}

				for (std::vector<cached_piece_info>::iterator i = pieces.begin()
					, end(pieces.end()); i != end; ++i)
				{
					if (i->kind != cached_piece_info::read_cache) continue;
					out << to_string(i->piece, 4) << ": [";
					for (std::vector<bool>::iterator k = i->blocks.begin()
						, end(i->blocks.end()); k != end; ++k)
					{
#ifdef ANSI_TERMINAL_COLORS
						if (*k) out << esc("33;7") << " " << esc("0");
						else out << " ";
#else
						if (*k) out << "#";
						else out << " ";
#endif
					}
					out << "] " << "cache age: "
						<< (total_milliseconds(time_now() - i->last_use) / 1000.f)
						<< "\n";
				}
				out << "___________________________________\n";
			}

			if (print_file_progress
				&& s.state != torrent_status::seeding
				&& h.has_metadata())
			{
				std::vector<size_type> file_progress;
				h.file_progress(file_progress);
				torrent_info const& info = h.get_torrent_info();
				for (int i = 0; i < info.num_files(); ++i)
				{
					bool pad_file = info.file_at(i).pad_file;
					if (!show_pad_files && pad_file) continue;
					float progress = info.file_at(i).size > 0
						?float(file_progress[i]) / info.file_at(i).size:1;
					if (file_progress[i] == info.file_at(i).size)
						out << progress_bar(1.f, 100, "32");
					else
						out << progress_bar(progress, 100, "33");
					if (pad_file) out << esc("34");
					out << " " << to_string(progress * 100.f, 5) << "% "
						<< add_suffix(file_progress[i]) << " "
						<< info.file_at(i).path.leaf() << "\n";
					if (pad_file) out << esc("0");
				}

				out << "___________________________________\n";
			}

		}

		if (print_log)
		{
			for (std::deque<std::string>::iterator i = events.begin();
				i != events.end(); ++i)
			{
				out << "\n" << *i;
			}
		}

		clear_home();
		puts(out.str().c_str());

		if (!monitor_dir.empty()
			&& next_dir_scan < time_now())
		{
			scan_dir(monitor_dir, ses, handles, preferred_ratio
				, allocation_mode == "compact", save_path, torrent_upload_limit
				, torrent_download_limit);
			next_dir_scan = time_now() + seconds(poll_interval);
		}
	}

	std::cout << "saving session state" << std::endl;
	{	
		entry session_state = ses.state();
		boost::filesystem::ofstream out(".ses_state"
			, std::ios_base::binary);
		out.unsetf(std::ios_base::skipws);
		bencode(std::ostream_iterator<char>(out), session_state);
	}

#ifndef TORRENT_DISABLE_DHT
	std::cout << "saving DHT state" << std::endl;
	dht_state = ses.dht_state();
	boost::filesystem::ofstream out(".dht_state"
		, std::ios_base::binary);
	out.unsetf(std::ios_base::skipws);
	bencode(std::ostream_iterator<char>(out), dht_state);
#endif
	std::cout << "closing session" << std::endl;

	return 0;
}

