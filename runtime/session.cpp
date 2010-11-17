//-----------------------------------------------------------------------------------------
#if 0

Fix8 is released under the New BSD License.

Copyright (c) 2010, David L. Dight <fix@fix8.org>
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are
permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of
	 	conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list
	 	of conditions and the following disclaimer in the documentation and/or other
		materials provided with the distribution.
    * Neither the name of the author nor the names of its contributors may be used to
	 	endorse or promote products derived from this software without specific prior
		written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
OR  IMPLIED  WARRANTIES,  INCLUDING,  BUT  NOT  LIMITED  TO ,  THE  IMPLIED  WARRANTIES  OF
MERCHANTABILITY AND  FITNESS FOR A PARTICULAR  PURPOSE ARE  DISCLAIMED. IN  NO EVENT  SHALL
THE  COPYRIGHT  OWNER OR  CONTRIBUTORS BE  LIABLE  FOR  ANY DIRECT,  INDIRECT,  INCIDENTAL,
SPECIAL,  EXEMPLARY, OR CONSEQUENTIAL  DAMAGES (INCLUDING,  BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE  GOODS OR SERVICES; LOSS OF USE, DATA,  OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED  AND ON ANY THEORY OF LIABILITY, WHETHER  IN CONTRACT, STRICT  LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------------------
$Id$
$Date$
$URL$

#endif
//-----------------------------------------------------------------------------------------
#include <config.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <iterator>
#include <memory>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <bitset>

#ifdef HAS_TR1_UNORDERED_MAP
#include <tr1/unordered_map>
#endif

#include <strings.h>
#include <regex.h>

#include <f8includes.hpp>

//-------------------------------------------------------------------------------------------------
using namespace FIX8;
using namespace std;

//-------------------------------------------------------------------------------------------------
namespace {
	const string spacer(3, ' ');
}

//-------------------------------------------------------------------------------------------------
RegExp SessionID::_sid("([^:]+):([^-]+)->(.+)");

//-------------------------------------------------------------------------------------------------
template<>
const Session::Handlers::TypePair Session::Handlers::_valueTable[] =
{
	Session::Handlers::TypePair(Common_MsgType_HEARTBEAT, &Session::handle_heartbeat),
	Session::Handlers::TypePair(Common_MsgType_TEST_REQUEST, &Session::handle_test_request),
	Session::Handlers::TypePair(Common_MsgType_RESEND_REQUEST, &Session::handle_resend_request),
	Session::Handlers::TypePair(Common_MsgType_REJECT, &Session::handle_reject),
	Session::Handlers::TypePair(Common_MsgType_SEQUENCE_RESET, &Session::handle_sequence_reset),
	Session::Handlers::TypePair(Common_MsgType_LOGOUT, &Session::handle_logout),
	Session::Handlers::TypePair(Common_MsgType_LOGON, &Session::handle_logon),
};
template<>
const Session::Handlers::NotFoundType Session::Handlers::_noval(&Session::handle_application);

template<>
const Session::Handlers::TypeMap Session::Handlers::_valuemap(Session::Handlers::_valueTable,
	Session::Handlers::get_table_end());

//-------------------------------------------------------------------------------------------------
const f8String& SessionID::make_id()
{
	ostringstream ostr;
	ostr << _beginString << ':' << _senderCompID << "->" << _targetCompID;
	return _id = ostr.str();
}

//-------------------------------------------------------------------------------------------------
void SessionID::from_string(const f8String& from)
{
	RegMatch match;
	if (_sid.SearchString(match, from, 4) == 4)
	{
		f8String bstr, scid, tcid;
		_sid.SubExpr(match, from, bstr, 0, 1);
		_beginString.set(bstr);
		_sid.SubExpr(match, from, scid, 0, 2);
		_senderCompID.set(scid);
		_sid.SubExpr(match, from, tcid, 0, 3);
		_targetCompID.set(tcid);
		make_id();
	}
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
Session::Session(const F8MetaCntx& ctx, const SessionID& sid, Persister *persist, Logger *logger, Logger *plogger) :
	_ctx(ctx), _connection(), _sid(sid), _persist(persist), _logger(logger), _plogger(plogger)	// initiator
{
	_state = States::st_not_logged_in;
	_next_sender_seq = _next_target_seq = 1;
}

//-------------------------------------------------------------------------------------------------
Session::Session(const F8MetaCntx& ctx, Persister *persist, Logger *logger, Logger *plogger) :
	_ctx(ctx), _connection(), _persist(persist), _logger(logger), _plogger(plogger)	// acceptor
{
	_state = States::st_wait_for_logon;
	_next_sender_seq = _next_target_seq = 1; // atomic init
}

//-------------------------------------------------------------------------------------------------
Session::~Session()
{
	log("Session terminating");
	_logger->stop();
}

//-------------------------------------------------------------------------------------------------
bool Session::plog(const std::string& what) const
{
	return _plogger ? _plogger->send(what) : false;
}

//-------------------------------------------------------------------------------------------------
bool Session::log(const std::string& what) const
{
	return _logger ? _logger->send(what) : false;
}

//-------------------------------------------------------------------------------------------------
int Session::start(Connection *connection, bool wait)
{
	log("Starting session");
	_connection = connection; // takes owership
	if (!_connection->connect()) // if already connected returns true
		return -1;;
	_connection->start();
	log("Session connected");

	if (_connection->get_role() == Connection::cn_initiator)
	{
		Message *msg(generate_logon(_connection->get_hb_interval()));
		send(msg);
		_state = States::st_logon_sent;
	}

	if (wait)	// wait for
	{
		_connection->join();
	}

	return 0;
}

//-------------------------------------------------------------------------------------------------
void Session::stop()
{
	_control |= shutdown;
	_connection->stop();
}

//-------------------------------------------------------------------------------------------------
bool Session::process(const f8String& from)
{
	_last_received = Poco::DateTime();
	plog(from);
	scoped_ptr<Message> msg(Message::factory(_ctx, from));
	msg->print(cout);
	handle_admin(msg.get());
	(this->*_handlers.find_value_ref(msg->get_msgtype()))(msg.get());
	return true;
}

//-------------------------------------------------------------------------------------------------
bool Session::handle_logon(const Message *msg)
{
	if (_connection->get_role() == Connection::cn_initiator)
	{
		heartbeat_interval hbi;
		if (msg->get(hbi))
			_connection->set_hb_interval(hbi.get());
	}
	else // acceptor
	{
		sender_comp_id sci;
		const_cast<Message*>(msg)->Header()->get(sci);
		target_comp_id tci;
		const_cast<Message*>(msg)->Header()->get(tci);
		SessionID id(_ctx._beginStr, tci.get(), sci.get());
		if (authenticate(id))
		{
			_sid = id;
			Message *msg(generate_logon(_connection->get_hb_interval()));
			send(msg);
			_state = States::st_logon_sent;
		}
		else
		{
			ostringstream ostr;
			ostr << id << " failed authentication";
			log(ostr.str());
			stop();
			return false;
		}
	}

	ostringstream ostr;
	ostr << "Heartbeat interval is " << _connection->get_hb_interval();
	log(ostr.str());

	_heartbeat_timer_outbound.scheduleAtFixedRate(
		new Poco::Util::TimerTaskAdapter<Session>(*this, &Session::heartbeat_service),
		1000 * _connection->get_hb_interval(), 1000 * _connection->get_hb_interval());

	_heartbeat_timer_inbound.scheduleAtFixedRate(
		new Poco::Util::TimerTaskAdapter<Session>(*this, &Session::heartbeat_processor),
		1000 * _connection->get_hb_interval(), 1000 * _connection->get_hb_interval());

	return true;
}

//-------------------------------------------------------------------------------------------------
bool Session::handle_heartbeat(const Message *msg)
{
	log("Session::handle_heartbeat");
	return true;
}

//-------------------------------------------------------------------------------------------------
void Session::heartbeat_service(Poco::Util::TimerTask &tt)
{
	Poco::Timespan ts(Poco::DateTime() - _last_sent);
	if (ts.seconds() >= static_cast<long>(_connection->get_hb_interval()))
	{
		const f8String testReqID;
		Message *msg(generate_heartbeat(testReqID));
		send(msg);
	}
	else if (ts.seconds() > 0)	// reschedule
	{
		_heartbeat_timer_outbound.cancel();
		_heartbeat_timer_outbound.scheduleAtFixedRate(
			new Poco::Util::TimerTaskAdapter<Session>(*this, &Session::heartbeat_service),
			1000 * ts.seconds(), 1000 * _connection->get_hb_interval());
	}
}

//-------------------------------------------------------------------------------------------------
void Session::heartbeat_processor(Poco::Util::TimerTask &tt)
{
	Poco::Timespan ts(Poco::DateTime() - _last_received);
	if (ts.seconds() >= static_cast<long>(_connection->get_hb_interval() + _connection->get_hb_interval() / 5))	// 20% rule
	{
		const f8String testReqID("TEST");
		Message *msg(generate_test_request(testReqID));
		send(msg);
		_state = States::st_test_request_sent;
	}
	else if (ts.seconds() > 0)	// reschedule
	{
		_heartbeat_timer_inbound.cancel();
		_heartbeat_timer_inbound.scheduleAtFixedRate(
			new Poco::Util::TimerTaskAdapter<Session>(*this, &Session::heartbeat_processor),
			1000 * ts.seconds(), 1000 * _connection->get_hb_interval());
	}
}

//-------------------------------------------------------------------------------------------------
Message *Session::generate_heartbeat(const f8String& testReqID)
{
	Message *msg(create_msg(Common_MsgType_HEARTBEAT));
	if (!testReqID.empty())
		*msg += new test_request_id(testReqID);

	return msg;
}

//-------------------------------------------------------------------------------------------------
Message *Session::generate_test_request(const f8String& testReqID)
{
	Message *msg(create_msg(Common_MsgType_TEST_REQUEST));
	*msg += new test_request_id(testReqID);

	return msg;
}

//-------------------------------------------------------------------------------------------------
Message *Session::generate_logon(const unsigned heartbtint)
{
	Message *msg(create_msg(Common_MsgType_LOGON));
	*msg += new heartbeat_interval(heartbtint);

	return msg;
}

//-------------------------------------------------------------------------------------------------
Message *Session::generate_logout()
{
	Message *msg(create_msg(Common_MsgType_LOGOUT));
	return msg;
}

//-------------------------------------------------------------------------------------------------
Message *Session::generate_resend_request(const unsigned begin, const unsigned end)
{
	Message *msg(create_msg(Common_MsgType_RESEND_REQUEST));
	*msg += new begin_seq_num(begin);
	*msg += new end_seq_num(end);

	return msg;
}

//-------------------------------------------------------------------------------------------------
Message *Session::generate_sequence_reset(const unsigned newseqnum, const bool gapfillflag)
{
	Message *msg(create_msg(Common_MsgType_SEQUENCE_RESET));
	*msg += new new_seq_num(newseqnum);

	if (gapfillflag)
		*msg += new gap_fill_flag(true);

	return msg;
}

//-------------------------------------------------------------------------------------------------
bool Session::send(Message *tosend)	// takes ownership and destroys
{
	scoped_ptr<Message> msg(tosend);
	*msg->Header() += new msg_seq_num(_next_sender_seq);
	*msg->Header() += new sending_time;
	*msg->Header() += new sender_comp_id(_sid.get_senderCompID());
	*msg->Header() += new target_comp_id(_sid.get_targetCompID());

	modify_outbound(msg.get());
	f8String output;
	unsigned enclen(msg->encode(output));
	if (!_connection->write(output))
	{
		cerr << "Message write failed!: " << enclen << endl;
		return false;
	}
	_last_sent = Poco::DateTime();
	plog(output);

	if (_persist)
	{
		if (!tosend->is_admin())
			_persist->put(_next_sender_seq, output);
		_persist->put(_next_sender_seq + 1, _next_target_seq);
	}

	++_next_sender_seq;

	return true;
}

//-------------------------------------------------------------------------------------------------
bool Session::handle_application(const Message *msg)
{
	return false;
}

