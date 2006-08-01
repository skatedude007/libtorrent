/*

Copyright (c) 2006, Arvid Norberg
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

#include <utility>
#include <boost/bind.hpp>
#include <boost/optional.hpp>
#include <boost/function.hpp>
#include <boost/iterator_adaptors.hpp>

#include "libtorrent/io.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/random_sample.hpp"
#include "libtorrent/kademlia/node_id.hpp"
#include "libtorrent/kademlia/rpc_manager.hpp"
#include "libtorrent/kademlia/packet_iterator.hpp"
#include "libtorrent/kademlia/routing_table.hpp"
#include "libtorrent/kademlia/node.hpp"

#include "libtorrent/kademlia/refresh.hpp"
#include "libtorrent/kademlia/closest_nodes.hpp"
#include "libtorrent/kademlia/find_data.hpp"

using boost::bind;
using boost::posix_time::second_clock;
using boost::posix_time::seconds;
using boost::posix_time::minutes;
using boost::posix_time::ptime;
using boost::posix_time::time_duration;

namespace libtorrent { namespace dht
{

#ifdef _MSC_VER
namespace
{
	char rand() { return (char)std::rand(); }
}
#endif

typedef boost::shared_ptr<observer> observer_ptr;

// TODO: configurable?
enum { announce_interval = 30 };

using asio::ip::udp;

#ifdef TORRENT_DHT_VERBOSE_LOGGING
TORRENT_DEFINE_LOG(node)
#endif

node_id generate_id()
{
	char random[20];
	std::srand(std::time(0));
#ifdef _MSC_VER
	std::generate(random, random + 20, &rand);
#else
	std::generate(random, random + 20, &std::rand);
#endif

	hasher h;
	h.update(random, 20);
	return h.final();
}

// remove peers that have timed out
void purge_peers(std::set<peer_entry>& peers)
{
	for (std::set<peer_entry>::iterator i = peers.begin()
		  , end(peers.end()); i != end;)
	{
		// the peer has timed out
		if (i->added + minutes(int(announce_interval * 1.5f)) < second_clock::universal_time())
		{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
			TORRENT_LOG(node) << "peer timed out at: " << i->addr.address();
#endif
			peers.erase(i++);
		}
		else
			++i;
	}
}

void nop() {}

node_impl::node_impl(boost::function<void(msg const&)> const& f
	, dht_settings const& settings, boost::optional<node_id> node_id)
	: m_settings(settings)
	, m_id(node_id ? *node_id : generate_id())
	, m_table(m_id, 8, settings)
	, m_rpc(bind(&node_impl::incoming_request, this, _1)
		, m_id, m_table, f)
	, m_last_tracker_tick(boost::posix_time::second_clock::universal_time())
{
	m_secret[0] = std::rand();
	m_secret[1] = std::rand();
}

bool node_impl::verify_token(msg const& m)
{
	if (m.write_token.type() != entry::string_t)
		return false;
	std::string const& token = m.write_token.string();
	if (token.length() != 4) return false;

	hasher h1;
	std::string address = m.addr.address().to_string();
	h1.update(&address[0], address.length());
	h1.update((char*)&m_secret[0], sizeof(m_secret[0]));
	h1.update((char*)&m.info_hash[0], sha1_hash::size);
	
	sha1_hash h = h1.final();
	if (std::equal(token.begin(), token.end(), (signed char*)&h[0]))
		return true;

	hasher h2;
	h2.update(&address[0], address.length());
	h2.update((char*)&m_secret[1], sizeof(m_secret[1]));
	h = h2.final();
	if (std::equal(token.begin(), token.end(), (signed char*)&h[0]))
		return true;
	return false;
}

entry node_impl::generate_token(msg const& m)
{
	std::string token;
	token.resize(4);
	hasher h;
	std::string address = m.addr.address().to_string();
	h.update(&address[0], address.length());
	h.update((char*)&m_secret[0], sizeof(m_secret[0]));
	h.update((char*)&m.info_hash[0], sha1_hash::size);

	sha1_hash hash = h.final();
	std::copy(hash.begin(), hash.begin() + 4, (signed char*)&token[0]);
	return entry(token);
}

void node_impl::check_refresh()
{
	for (int i = 0; i < 160; ++i)
	{
		if (m_table.should_refresh(i))
		{
			refresh_bucket(i);
		}
	}
}

void node_impl::refresh(node_id const& id
	, boost::function0<void> f)
{
	// use the 'bucket size' closest nodes
	// to start the refresh with
	std::vector<node_entry> start;
	start.reserve(m_table.bucket_size());
	m_table.find_node(id, start, false);
	refresh::initiate(id, m_settings.search_branching, 10, m_table.bucket_size()
		, m_table, start.begin(), start.end(), m_rpc, f);
}

void node_impl::bootstrap(std::vector<udp::endpoint> const& nodes
	, boost::function0<void> f)
{
	std::vector<node_entry> start;
	start.reserve(nodes.size());
	std::copy(nodes.begin(), nodes.end(), std::back_inserter(start));
	refresh::initiate(m_id, m_settings.search_branching, 10, m_table.bucket_size()
		, m_table, start.begin(), start.end(), m_rpc, f);
}

void node_impl::refresh()
{
	std::vector<node_entry> start;
	start.reserve(m_table.size().get<0>());
	std::copy(m_table.begin(), m_table.end(), std::back_inserter(start));

	refresh::initiate(m_id, m_settings.search_branching, 10, m_table.bucket_size()
		, m_table, start.begin(), start.end(), m_rpc, bind(&nop));
}

int node_impl::bucket_size(int bucket)
{
	return m_table.bucket_size(bucket);
}

void node_impl::new_write_key()
{
	m_secret[1] = m_secret[0];
	m_secret[0] = std::rand();
}

void node_impl::refresh_bucket(int bucket)
{
	assert(bucket >= 0 && bucket < 160);
	
	// generate a random node_id within the given bucket
	node_id target = generate_id();
	int num_bits = 160 - bucket;
	node_id mask(0);
	for (int i = 0; i < num_bits; ++i)
	{
		int byte = i / 8;
		mask[byte] |= 0x80 >> (i % 8);
	}

	node_id root = m_id;
	root &= mask;
	target &= ~mask;
	target |= root;

	// make sure this is in another subtree than m_id
	// clear the (num_bits - 1) bit and then set it to the
	// inverse of m_id's corresponding bit.
	target[(num_bits - 1) / 8] &= ~(0x80 >> ((num_bits - 1) % 8));
	target[(num_bits - 1) / 8] |=
		(~(m_id[(num_bits - 1) / 8])) & (0x80 >> ((num_bits - 1) % 8));

	int new_bucket = distance_exp(m_id, target);
	assert(new_bucket == bucket);

	std::vector<node_entry> start;
	start.reserve(m_table.bucket_size());
	m_table.find_node(target, start, false, m_table.bucket_size());

	refresh::initiate(target, m_settings.search_branching, 10, m_table.bucket_size()
		, m_table, start.begin(), start.end(), m_rpc, bind(&nop));
}


void node_impl::incoming(msg const& m)
{
	if (m_rpc.incoming(m))
	{
		refresh();
	}
}

namespace
{

	class announce_observer : public observer
	{
	public:
		announce_observer(sha1_hash const& info_hash, int listen_port
			, entry const& write_token)
			: m_info_hash(info_hash)
			, m_listen_port(listen_port)
			, m_token(write_token)
		{}

		void send(msg& m)
		{
			m.port = m_listen_port;
			m.info_hash = m_info_hash;
			m.write_token = m_token;
		}

		void timeout() {}
		void reply(msg const&) {}

	private:
		sha1_hash m_info_hash;
		int m_listen_port;
		entry m_token;
	};

	class get_peers_observer : public observer
	{
	public:
		get_peers_observer(sha1_hash const& info_hash, int listen_port
			, rpc_manager& rpc
			, boost::function<void(std::vector<tcp::endpoint> const&, sha1_hash const&)> f)
			: m_info_hash(info_hash)
			, m_listen_port(listen_port)
			, m_rpc(rpc)
			, m_fun(f)
		{}

		void send(msg& m)
		{
			m.port = m_listen_port;
			m.info_hash = m_info_hash;
		}

		void timeout() {}
		void reply(msg const& r)
		{
			m_rpc.invoke(messages::announce_peer, r.addr
				, boost::shared_ptr<observer>(
				new announce_observer(m_info_hash, m_listen_port, r.write_token)));
			m_fun(r.peers, m_info_hash);
		}

	private:
		sha1_hash m_info_hash;
		int m_listen_port;
		rpc_manager& m_rpc;
		boost::function<void(std::vector<tcp::endpoint> const&, sha1_hash const&)> m_fun;
	};


	void announce_fun(std::vector<node_entry> const& v, rpc_manager& rpc
		, int listen_port, sha1_hash const& ih
		, boost::function<void(std::vector<tcp::endpoint> const&, sha1_hash const&)> f)
	{
		bool nodes = false;
		// only store on the first k nodes
		for (std::vector<node_entry>::const_iterator i = v.begin()
			, end(v.end()); i != end; ++i)
		{
			rpc.invoke(messages::get_peers, i->addr, boost::shared_ptr<observer>(
				new get_peers_observer(ih, listen_port, rpc, f)));
			nodes = true;
		}
	}

}

namespace
{
	struct dummy_observer : observer
	{
		virtual void reply(msg const&) {}
		virtual void timeout() {}
		virtual void send(msg&) {}
	};
}

void node_impl::add_node(udp::endpoint node)
{
	// ping the node, and if we get a reply, it
	// will be added to the routing table
	observer_ptr p(new dummy_observer());
	m_rpc.invoke(messages::ping, node, p);
}

void node_impl::announce(sha1_hash const& info_hash, int listen_port
	, boost::function<void(std::vector<tcp::endpoint> const&, sha1_hash const&)> f)
{
	// search for nodes with ids close to id, and then invoke the
	// get_peers and then announce_peer rpc on them.
	closest_nodes::initiate(info_hash, m_settings.search_branching
		, m_table.bucket_size(), m_table, m_rpc
		, boost::bind(&announce_fun, _1, boost::ref(m_rpc), listen_port
		, info_hash, f));
}

time_duration node_impl::tick()
{
	time_duration d = m_rpc.tick();

	ptime now(second_clock::universal_time());
	if (now - m_last_tracker_tick < minutes(10)) return d;
	m_last_tracker_tick = now;
	
	// look through all peers and see if any have timed out
	for (data_iterator i = begin_data(), end(end_data()); i != end;)
	{
		torrent_entry& t = i->second;
		node_id const& key = i->first;
		++i;
		purge_peers(t.peers);

		// if there are no more peers, remove the entry altogether
		if (t.peers.empty())
		{
			table_t::iterator i = m_map.find(key);
			if (i != m_map.end()) m_map.erase(i);
		}
	}
	return d;
}

void node_impl::on_announce(msg const& m, msg& reply)
{
	if (!verify_token(m))
	{
		reply.message_id = messages::error;
		reply.error_code = 203;
		reply.error_msg = "Incorrect write token in announce_peer message";
		return;
	}

	torrent_entry& v = m_map[m.info_hash];
	peer_entry e;
	e.addr = tcp::endpoint(m.addr.address(), m.addr.port());
	e.added = second_clock::universal_time();
	std::set<peer_entry>::iterator i = v.peers.find(e);
	if (i != v.peers.end()) v.peers.erase(i++);
	v.peers.insert(i, e);
}

namespace
{
	tcp::endpoint get_endpoint(peer_entry const& p)
	{
		return p.addr;
	}
}

bool node_impl::on_find(msg const& m, std::vector<tcp::endpoint>& peers) const
{
	table_t::const_iterator i = m_map.find(m.info_hash);
	if (i == m_map.end()) return false;

	torrent_entry const& v = i->second;

	int num = (std::min)((int)v.peers.size(), m_settings.max_peers_reply);
	peers.clear();
	peers.reserve(num);
	random_sample_n(boost::make_transform_iterator(v.peers.begin(), &get_endpoint)
		, boost::make_transform_iterator(v.peers.end(), &get_endpoint)
		, std::back_inserter(peers), num);

#ifdef TORRENT_DHT_VERBOSE_LOGGING
	for (std::vector<tcp::endpoint>::iterator i = peers.begin()
		, end(peers.end()); i != end; ++i)
	{
		TORRENT_LOG(node) << "   " << *i;
	}
#endif
	return true;
}

void node_impl::incoming_request(msg const& m)
{
	msg reply;
	switch (m.message_id)
	{
	case messages::ping:
		break;
	case messages::get_peers:
		{
			reply.info_hash = m.info_hash;
			reply.write_token = generate_token(m);
			
			if (!on_find(m, reply.peers))
			{
				// we don't have any peers for this info_hash,
				// return nodes instead
				m_table.find_node(m.info_hash, reply.nodes, false);
#ifdef TORRENT_DHT_VERBOSE_LOGGING
				for (std::vector<node_entry>::iterator i = reply.nodes.begin()
					, end(reply.nodes.end()); i != end; ++i)
				{
					TORRENT_LOG(node) << "	" << i->id << " " << i->addr;
				}
#endif
			}
		}
		break;
	case messages::find_node:
		{
			reply.info_hash = m.info_hash;

			m_table.find_node(m.info_hash, reply.nodes, false);
#ifdef TORRENT_DHT_VERBOSE_LOGGING
			for (std::vector<node_entry>::iterator i = reply.nodes.begin()
				, end(reply.nodes.end()); i != end; ++i)
			{
				TORRENT_LOG(node) << "	" << i->id << " " << i->addr;
			}
#endif
		}
		break;
	case messages::announce_peer:
		{
			on_announce(m, reply);
		}
		break;
	};

	if (m_table.need_node(m.id))
		m_rpc.reply_with_ping(reply, m);
	else
		m_rpc.reply(reply, m);
}


} } // namespace libtorrent::dht
