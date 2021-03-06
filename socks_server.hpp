#ifndef __SOCKS_SERVER_HPP__
#define __SOCKS_SERVER_HPP__

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/array.hpp>
#include <istream>

#include "io.hpp"


namespace socks {

using boost::asio::ip::tcp;

class socks_session
	: public boost::enable_shared_from_this<socks_session>
{
	enum {
		SOCKS_VERSION_4 = 4,
		SOCKS_VERSION_5 = 5
	};
	enum {
		SOCKS5_AUTH_NONE = 0x00,
		SOCKS5_AUTH = 0x02,
		SOCKS5_AUTH_UNACCEPTABLE = 0xFF
	};
	enum {
		SOCKS_CMD_CONNECT = 0x01,
		SOCKS_CMD_BIND = 0x02,
		SOCKS5_CMD_UDP = 0x03
	};
	enum {
		SOCKS5_ATYP_IPV4 = 0x01,
		SOCKS5_ATYP_DOMAINNAME = 0x03,
		SOCKS5_ATYP_IPV6 = 0x04
	};
	enum {
		SOCKS5_SUCCEEDED = 0x00,
		SOCKS5_GENERAL_SOCKS_SERVER_FAILURE,
		SOCKS5_CONNECTION_NOT_ALLOWED_BY_RULESET,
		SOCKS5_NETWORK_UNREACHABLE,
		SOCKS5_CONNECTION_REFUSED,
		SOCKS5_TTL_EXPIRED,
		SOCKS5_COMMAND_NOT_SUPPORTED,
		SOCKS5_ADDRESS_TYPE_NOT_SUPPORTED,
		SOCKS5_UNASSIGNED
	};
	enum {
		SOCKS4_REQUEST_GRANTED = 90,
		SOCKS4_REQUEST_REJECTED_OR_FAILED,
		SOCKS4_CANNOT_CONNECT_TARGET_SERVER,
		SOCKS4_REQUEST_REJECTED_USER_NO_ALLOW,
	};

public:
	socks_session(boost::asio::io_service &io)
		: m_io_service(io)
		, m_local_socket(io)
		, m_remote_socket(io)
		, m_resolver(io)
		, m_version(-1)
		, m_method(-1)
		, m_verify_passed(false)
	{}
	~socks_session() {}

public:
	void start()
	{
		// read
		//	+----+----------+----------+
		//	|VER | NMETHODS | METHODS  |
		//	+----+----------+----------+
		//	| 1  |    1     | 1 to 255 |
		//	+----+----------+----------+
		//  [               ]
		// or
		//	+----+----+----+----+----+----+----+----+----+----+....+----+
		//	| VN | CD | DSTPORT |      DSTIP        | USERID       |NULL|
		//	+----+----+----+----+----+----+----+----+----+----+....+----+
		//	  1    1      2        4                  variable       1
		//  [         ]
		// 读取[]里的部分.
		boost::asio::async_read(m_local_socket, boost::asio::buffer(m_local_buffer, 2),
			boost::asio::transfer_exactly(2),
				boost::bind(&socks_session::socks_handle_connect_1, shared_from_this(),
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred
				)
			);
	}

	tcp::socket& socket() { return m_local_socket; }

protected:
	void socks_handle_connect_1(const boost::system::error_code& error, int bytes_transferred)
	{
		if (!error)
		{
			char *p = m_local_buffer.data();
			m_version = read_int8(p);
			if (m_version == SOCKS_VERSION_5)	// sock5协议.
			{
				int nmethods = read_int8(p);	// 读取客户端支持的代理方式列表.
				if (nmethods <= 0 || nmethods > 255)
				{
					std::cout << "unsupport any method!\n";
					return;
				}

				//	+----+----------+----------+
				//	|VER | NMETHODS | METHODS  |
				//	+----+----------+----------+
				//	| 1  |    1     | 1 to 255 |
				//	+----+----------+----------+
				//                  [          ]
				boost::asio::async_read(m_local_socket, boost::asio::buffer(m_local_buffer, nmethods),
					boost::asio::transfer_exactly(nmethods),
						boost::bind(&socks_session::socks_handle_connect_2, shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred
						)
					);
			}
			else if (m_version == SOCKS_VERSION_4)	// socks4协议.
			{
				//	+----+----+----+----+----+----+----+----+----+----+....+----+
				//	| VN | CD | DSTPORT |      DSTIP        | USERID       |NULL|
				//	+----+----+----+----+----+----+----+----+----+----+....+----+
				//  | 1  | 1  |    2    |         4         | variable     | 1  |
				//	+----+----+----+----+----+----+----+----+----+----+....+----+
				//            [                             ]

				m_command = read_int8(p);

				boost::asio::async_read(m_local_socket, boost::asio::buffer(m_local_buffer, 6),
					boost::asio::transfer_exactly(6),
						boost::bind(&socks_session::socks_handle_connect_2, shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred
						)
					);
			}
			else
			{
				std::cout << "error unknow protocol.\n";
			}
		}
	}

	void socks_handle_connect_2(const boost::system::error_code& error, int bytes_transferred)
	{
		if (!error)
		{
			if (m_version == SOCKS_VERSION_5)
			{
				// 循环读取客户端支持的代理方式.
				char *p = m_local_buffer.data();
				m_method = SOCKS5_AUTH_UNACCEPTABLE;
				while (bytes_transferred != 0)
				{
					int m = read_int8(p);
					if (m == SOCKS5_AUTH_NONE || m == SOCKS5_AUTH)
						m_method = m;
					bytes_transferred--;
				}

				// 回复客户端, 选择的代理方式.
				p = m_local_buffer.data();
				write_int8(m_version, p);
				write_int8(m_method, p);

				//	+----+--------+
				//	|VER | METHOD |
				//	+----+--------+
				//	| 1  |   1    |
				//	+----+--------+
				//  [             ]
				boost::asio::async_write(m_local_socket, boost::asio::buffer(m_local_buffer, 2),
					boost::asio::transfer_exactly(2),
						boost::bind(&socks_session::socks_handle_send_version, shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred
						)
					);
			}

			if (m_version == SOCKS_VERSION_4)
			{
				char *p = m_local_buffer.data();
				m_address.port(read_int16(p));
				m_address.address(boost::asio::ip::address_v4(read_uint32(p)));

				//	+----+----+----+----+----+----+----+----+----+----+....+----+
				//	| VN | CD | DSTPORT |      DSTIP        | USERID       |NULL|
				//	+----+----+----+----+----+----+----+----+----+----+....+----+
				//  | 1  | 1  |    2    |         4         | variable     | 1  |
				//	+----+----+----+----+----+----+----+----+----+----+....+----+
				//                                          [                   ]
				boost::asio::async_read_until(m_local_socket, m_streambuf, '\0',
					boost::bind(&socks_session::socks_handle_negotiation_2, shared_from_this(),
						boost::asio::placeholders::error,
						boost::asio::placeholders::bytes_transferred
					)
				);
			}
		}
	}

	void socks_handle_send_version(const boost::system::error_code& error, int bytes_transferred)
	{
		if (!error)
		{
			if (m_method == SOCKS5_AUTH)			// 认证模式.
			{
				//	+----+------+----------+------+----------+
				//	|VER | ULEN |  UNAME   | PLEN |  PASSWD  |
				//	+----+------+----------+------+----------+
				//	| 1  |  1   | 1 to 255 |  1   | 1 to 255 |
				//	+----+------+----------+------+----------+
				//  [           ]
				boost::asio::async_read(m_local_socket, boost::asio::buffer(m_local_buffer, 2),
					boost::asio::transfer_exactly(2),
						boost::bind(&socks_session::socks_handle_negotiation_1, shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred
						)
					);
			}
			else if (m_method == SOCKS5_AUTH_NONE || m_verify_passed)	// 非认证模式, 或认证已经通过, 接收socks客户端Requests.
			{
				//	+----+-----+-------+------+----------+----------+
				//	|VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
				//	+----+-----+-------+------+----------+----------+
				//	| 1  |  1  | X'00' |  1   | Variable |    2     |
				//	+----+-----+-------+------+----------+----------+
				//  [                          ]
				boost::asio::async_read(m_local_socket, boost::asio::buffer(m_local_buffer, 5),
					boost::asio::transfer_exactly(5),
						boost::bind(&socks_session::socks_handle_requests_1, shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred
						)
					);
			}
		}
	}

	void socks_handle_negotiation_1(const boost::system::error_code& error, int bytes_transferred)
	{
		if (!error)
		{
			char *p = m_local_buffer.data();
			int auth_version = read_int8(p);
			if (auth_version != 1)
			{
				std::cout << "unsupport socks5 protocol\n";
				return;
			}
			int name_length = read_int8(p);
			if (name_length <= 0 || name_length > 255)
			{
				std::cout << "error unknow protocol.\n";
				return;
			}
			name_length += 1;
			//	+----+------+----------+------+----------+
			//	|VER | ULEN |  UNAME   | PLEN |  PASSWD  |
			//	+----+------+----------+------+----------+
			//	| 1  |  1   | 1 to 255 |  1   | 1 to 255 |
			//	+----+------+----------+------+----------+
			//              [                 ]
			boost::asio::async_read(m_local_socket, boost::asio::buffer(m_local_buffer, name_length),
				boost::asio::transfer_exactly(name_length),
					boost::bind(&socks_session::socks_handle_negotiation_2, shared_from_this(),
						boost::asio::placeholders::error,
						boost::asio::placeholders::bytes_transferred
					)
				);
		}
	}

	void socks_handle_negotiation_2(const boost::system::error_code& error, int bytes_transferred)
	{
		if (!error)
		{
			if (m_version == SOCKS_VERSION_5)
			{
				char *p = m_local_buffer.data();
				for (int i = 0; i < bytes_transferred - 1; i++)
					m_uname.push_back(read_int8(p));
				int passwd_len = read_int8(p);
				if (passwd_len <= 0 && passwd_len > 255)
				{
					std::cout << "error unknow protocol.\n";
					return;
				}
				//	+----+------+----------+------+----------+
				//	|VER | ULEN |  UNAME   | PLEN |  PASSWD  |
				//	+----+------+----------+------+----------+
				//	| 1  |  1   | 1 to 255 |  1   | 1 to 255 |
				//	+----+------+----------+------+----------+
				//                                [          ]
				boost::asio::async_read(m_local_socket, boost::asio::buffer(m_local_buffer, passwd_len),
					boost::asio::transfer_exactly(passwd_len),
						boost::bind(&socks_session::socks_handle_negotiation_3, shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred
						)
					);
			}

			if (m_version == SOCKS_VERSION_4)
			{
				std::string userid;

				userid.resize(bytes_transferred);
				m_streambuf.sgetn(&userid[0], bytes_transferred);

				// TODO: 认证用户.
				m_verify_passed = true;

				// 发起连接.
				if (m_command == SOCKS_CMD_CONNECT)
				{
					tcp::resolver::iterator endpoint_iterator;
					m_remote_socket.async_connect(m_address,
						boost::bind(&socks_session::socks_handle_connect_3,
						shared_from_this(), boost::asio::placeholders::error,
						endpoint_iterator));
					return;
				}

				if (m_command == SOCKS_CMD_BIND)
				{
					// TODO: 实现绑定请求.
				}
			}
		}
	}

	void socks_handle_negotiation_3(const boost::system::error_code& error, int bytes_transferred)
	{
		if (!error)
		{
			char *p = m_local_buffer.data();
			for (int i = 0; i < bytes_transferred; i++)
				m_passwd.push_back(read_int8(p));

			// TODO: 验证用户和密码.
			m_verify_passed = true;

			p = m_local_buffer.data();
			write_int8(0x01, p);		// version 只能是1.
			write_int8(0x00, p);		// 认证通过返回0x00, 其它值为失败.

			// 返回认证状态.
			//	+----+--------+
			//	|VER | STATUS |
			//	+----+--------+
			//	| 1  |   1    |
			//	+----+--------+
			boost::asio::async_write(m_local_socket, boost::asio::buffer(m_local_buffer, 2),
				boost::asio::transfer_exactly(2),
					boost::bind(&socks_session::socks_handle_send_version, shared_from_this(),
						boost::asio::placeholders::error,
						boost::asio::placeholders::bytes_transferred
					)
				);
		}
	}

	void socks_handle_requests_1(const boost::system::error_code& error, int bytes_transferred)
	{
		if (!error)
		{
			char *p = m_local_buffer.data();
			if (read_int8(p) != SOCKS_VERSION_5)
			{
				std::cout << "error unknow protocol.\n";
				return;
			}

			m_command = read_int8(p);		// CONNECT/BIND/UDP
			int reserved = read_int8(p);	// reserved.
			m_atyp = read_int8(p);			// atyp.

			//	+----+-----+-------+------+----------+----------+
			//	|VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
			//	+----+-----+-------+------+----------+----------+
			//	| 1  |  1  | X'00' |  1   | Variable |    2     |
			//	+----+-----+-------+------+----------+----------+
			//                              [                   ]
			int length = 0;
			int prefix = 1;

			// 保存第一个字节.
			m_local_buffer[0] = m_local_buffer[4];

			if (m_atyp == SOCKS5_ATYP_IPV4)
				length = 5;
			else if (m_atyp == SOCKS5_ATYP_DOMAINNAME)
			{
				length = read_int8(p) + 2;
				prefix = 0;
			}
			else if (m_atyp == SOCKS5_ATYP_IPV6)
				length = 17;

			boost::asio::async_read(m_local_socket, boost::asio::buffer(m_local_buffer.begin() + prefix, length),
				boost::asio::transfer_exactly(length),
					boost::bind(&socks_session::socks_handle_requests_2, shared_from_this(),
						boost::asio::placeholders::error,
						boost::asio::placeholders::bytes_transferred
					)
				);
		}
	}

	void socks_handle_requests_2(const boost::system::error_code& error, int bytes_transferred)
	{
		if (!error)
		{
			if (m_version == SOCKS_VERSION_5)
			{
				char *p = m_local_buffer.data();

				if (m_atyp == SOCKS5_ATYP_IPV4)
				{
					bytes_transferred += 1;	// 加上首个字节.
					m_address.address(boost::asio::ip::address_v4(read_uint32(p)));
					m_address.port(read_int16(p));
				}
				else if (m_atyp == SOCKS5_ATYP_DOMAINNAME)
				{
					for (int i = 0; i < bytes_transferred - 2; i++)
						m_domain.push_back(read_int8(p));
					m_port = read_int16(p);
				}
				else if (m_atyp == SOCKS5_ATYP_IPV6)
				{
					bytes_transferred += 1;	// 加上首个字节.
					boost::asio::ip::address_v6::bytes_type addr;
					for (boost::asio::ip::address_v6::bytes_type::iterator i = addr.begin();
						i != addr.end(); i++)
					{
						*i = read_int8(p);
					}

					m_address.address(boost::asio::ip::address_v6(addr));
					m_address.port(read_int16(p));
				}

				// 发起连接.
				if (m_command == SOCKS_CMD_CONNECT)
				{
					if (m_atyp == SOCKS5_ATYP_IPV4 || m_atyp == SOCKS5_ATYP_IPV6)
					{
						tcp::resolver::iterator endpoint_iterator;
						m_remote_socket.async_connect(m_address,
							boost::bind(&socks_session::socks_handle_connect_3,
							shared_from_this(), boost::asio::placeholders::error,
							endpoint_iterator));
						return;
					}
					if (m_atyp == SOCKS5_ATYP_DOMAINNAME)
					{
						std::ostringstream port_string;
						port_string << m_port;
						tcp::resolver::query query(m_domain, port_string.str());

						m_resolver.async_resolve(query, boost::bind(&socks_session::socks_handle_resolve,
							shared_from_this(),	boost::asio::placeholders::error,
							boost::asio::placeholders::iterator));
						return;
					}
				}
				else
				{
					//	+----+-----+-------+------+----------+----------+
					//	|VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
					//	+----+-----+-------+------+----------+----------+
					//	| 1  |  1  | X'00' |  1   | Variable |    2     |
					//	+----+-----+-------+------+----------+----------+
					//  [                                               ]
					p = m_local_buffer.data();
					write_int8(SOCKS_VERSION_5, p);
					write_int8(SOCKS5_COMMAND_NOT_SUPPORTED, p);
					write_int8(0x00, p);
					write_int8(1, p);
					// 没用的东西.
					for (int i = 0; i < 6; i++)
						write_int8(0, p);
					boost::asio::async_write(m_local_socket, boost::asio::buffer(m_local_buffer, 10),
					boost::asio::transfer_exactly(10),
						boost::bind(&socks_session::socks_handle_error, shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred
						)
					);
				}
			}
		}
	}

	void socks_handle_connect_3(const boost::system::error_code &error,
		tcp::resolver::iterator endpoint_iterator)
	{
		if (error)
		{
			tcp::resolver::iterator end;
			if (endpoint_iterator != end)
			{
				boost::asio::async_connect(m_remote_socket,	endpoint_iterator++,
					boost::bind(&socks_session::socks_handle_connect_3,
						shared_from_this(), boost::asio::placeholders::error,
						endpoint_iterator)
				);
			}
			else
			{
				if (m_version == SOCKS_VERSION_5)
				{
					// 连接目标失败!
					//	+----+-----+-------+------+----------+----------+
					//	|VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
					//	+----+-----+-------+------+----------+----------+
					//	| 1  |  1  | X'00' |  1   | Variable |    2     |
					//	+----+-----+-------+------+----------+----------+
					//  [                                               ]
					char *p = m_local_buffer.data();
					write_int8(SOCKS_VERSION_5, p);
					write_int8(SOCKS5_CONNECTION_REFUSED, p);
					write_int8(0x00, p);
					write_int8(1, p);
					// 没用的东西.
					for (int i = 0; i < 6; i++)
						write_int8(0, p);
					boost::asio::async_write(m_local_socket, boost::asio::buffer(m_local_buffer, 10),
					boost::asio::transfer_exactly(10),
						boost::bind(&socks_session::socks_handle_error, shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred
						)
					);

					return;
				}

				// 连接失败.
				if (m_version == SOCKS_VERSION_4)
				{
					//	+----+----+----+----+----+----+----+----+
					//	| VN | CD | DSTPORT |      DSTIP        |
					//	+----+----+----+----+----+----+----+----+
					//  | 1  | 1  |    2    |         4         |
					//	+----+----+----+----+----+----+----+----+
					//  [                                       ]
					char *p = m_local_buffer.data();
					write_int8(SOCKS_VERSION_4, p);
					write_int8(SOCKS4_CANNOT_CONNECT_TARGET_SERVER, p);
					// 没用了, 随便填.
					write_int16(0x00, p);
					write_uint32(0x00, p);
					boost::asio::async_write(m_local_socket, boost::asio::buffer(m_local_buffer, 8),
					boost::asio::transfer_exactly(8),
						boost::bind(&socks_session::socks_handle_error, shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred
						)
					);

					return;
				}
			}
		}
		else
		{
			if (m_version == SOCKS_VERSION_5)
			{
				// 连接成功.
				//	+----+-----+-------+------+----------+----------+
				//	|VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
				//	+----+-----+-------+------+----------+----------+
				//	| 1  |  1  | X'00' |  1   | Variable |    2     |
				//	+----+-----+-------+------+----------+----------+
				//  [                                               ]

				char *p = m_local_buffer.data();
				int len = 4;
				write_int8(SOCKS_VERSION_5, p);
				write_int8(SOCKS5_SUCCEEDED, p);
				write_int8(0x00, p);
				write_int8(m_atyp, p);
				
				if (m_atyp == SOCKS5_ATYP_IPV4)
				{
					len += 6;
					write_uint32(m_remote_socket.remote_endpoint().address().to_v4().to_ulong(), p);
					write_int16(m_remote_socket.remote_endpoint().port(), p);
				}
				if (m_atyp == SOCKS5_ATYP_IPV6)
				{
					len += 18;
					boost::asio::ip::address_v6::bytes_type addr;
					addr = m_remote_socket.remote_endpoint().address().to_v6().to_bytes();
					for (std::size_t i = 0; i < addr.size(); i++)
						write_int8(addr[i], p);
					write_int16(m_remote_socket.remote_endpoint().port(), p);
				}
				if (m_atyp == SOCKS5_ATYP_DOMAINNAME)
				{
					len += (m_domain.size() + 3);
					write_int8(m_domain.size(), p);
					write_string(m_domain, p);
					write_int16(m_port, p);
				}

				// 发送回复.
				boost::asio::async_write(m_local_socket, boost::asio::buffer(m_local_buffer, len),
				boost::asio::transfer_exactly(len),
					boost::bind(&socks_session::socks_handle_succeed, shared_from_this(),
						boost::asio::placeholders::error,
						boost::asio::placeholders::bytes_transferred
					)
				);

				// 投递一个数据接收.
				m_remote_socket.async_read_some(boost::asio::buffer(m_remote_buffer),
					boost::bind(&socks_session::socks_handle_remote_read, shared_from_this(),
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred));

				return;
			}

			if (m_version == SOCKS_VERSION_4)
			{
				//	+----+----+----+----+----+----+----+----+
				//	| VN | CD | DSTPORT |      DSTIP        |
				//	+----+----+----+----+----+----+----+----+
				//  | 1  | 1  |    2    |         4         |
				//	+----+----+----+----+----+----+----+----+
				//  [                                       ]
				char *p = m_local_buffer.data();
				write_int8(SOCKS_VERSION_4, p);
				write_int8(SOCKS4_REQUEST_GRANTED, p);
				write_int16(m_remote_socket.remote_endpoint().port(), p);
				write_uint32(m_remote_socket.remote_endpoint().address().to_v4().to_ulong(), p);

				// 回复成功.
				boost::asio::async_write(m_local_socket, boost::asio::buffer(m_local_buffer, 8),
				boost::asio::transfer_exactly(8),
					boost::bind(&socks_session::socks_handle_succeed, shared_from_this(),
						boost::asio::placeholders::error,
						boost::asio::placeholders::bytes_transferred
					)
				);

				// 投递一个数据接收.
				m_remote_socket.async_read_some(boost::asio::buffer(m_remote_buffer),
					boost::bind(&socks_session::socks_handle_remote_read, shared_from_this(),
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred));

				return;
			}
		}
	}

	void socks_handle_resolve(const boost::system::error_code &error,
		tcp::resolver::iterator endpoint_iterator)
	{
		if (!error)
		{
			boost::asio::async_connect(m_remote_socket,	endpoint_iterator++,
				boost::bind(&socks_session::socks_handle_connect_3,
					shared_from_this(), boost::asio::placeholders::error,
					endpoint_iterator)
			);
		}
	}

	void socks_handle_error(const boost::system::error_code &error, int bytes_transferred)
	{
		// 什么都不用做了, 退了.
	}

	void socks_handle_succeed(const boost::system::error_code &error, int bytes_transferred)
	{
		if (!error)
		{
			// 投递一个数据接收.
			m_local_socket.async_read_some(boost::asio::buffer(m_local_buffer),
				boost::bind(&socks_session::socks_handle_local_read, shared_from_this(),
				boost::asio::placeholders::error,
				boost::asio::placeholders::bytes_transferred));
		}
		else
		{
			close();
		}
	}

	void socks_handle_remote_read(const boost::system::error_code &error, int bytes_transferred)
	{
		if (!error)
		{
			// 发送到本地.
			boost::asio::async_write(m_local_socket, boost::asio::buffer(m_remote_buffer, bytes_transferred),
			boost::asio::transfer_exactly(bytes_transferred),
				boost::bind(&socks_session::socks_handle_remote_write, shared_from_this(),
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred
				)
			);
		}
		else
		{
			close();
		}
	}

	void socks_handle_remote_write(const boost::system::error_code &error, int bytes_transferred)
	{
		if (!error)
		{
			// 从远端读取数据.
			m_remote_socket.async_read_some(boost::asio::buffer(m_remote_buffer),
				boost::bind(&socks_session::socks_handle_remote_read, shared_from_this(),
				boost::asio::placeholders::error,
				boost::asio::placeholders::bytes_transferred));
		}
		else
		{
			close();
		}
	}

	void socks_handle_local_read(const boost::system::error_code &error, int bytes_transferred)
	{
		if (!error)
		{
			// 发送到目标.
			boost::asio::async_write(m_remote_socket, boost::asio::buffer(m_local_buffer, bytes_transferred),
			boost::asio::transfer_exactly(bytes_transferred),
				boost::bind(&socks_session::socks_handle_local_write, shared_from_this(),
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred
				)
			);
		}
		else
		{
			close();
		}
	}

	void socks_handle_local_write(const boost::system::error_code &error, int bytes_transferred)
	{
		if (!error)
		{
			// 从本地读取数据.
			m_local_socket.async_read_some(boost::asio::buffer(m_local_buffer),
				boost::bind(&socks_session::socks_handle_local_read, shared_from_this(),
				boost::asio::placeholders::error,
				boost::asio::placeholders::bytes_transferred));
		}
		else
		{
			close();
		}
	}

	void close()
	{
		boost::system::error_code ignored_ec;
		// 远程和本地链接都将关闭.
		m_local_socket.shutdown(
			boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
		m_local_socket.close(ignored_ec);
		m_remote_socket.shutdown(
			boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
		m_remote_socket.close(ignored_ec);
	}

private:
	boost::asio::io_service &m_io_service;
	tcp::socket m_local_socket;
	boost::array<char, 2048> m_local_buffer;
	tcp::socket m_remote_socket;
	boost::array<char, 2048> m_remote_buffer;
	boost::asio::streambuf m_streambuf;
	tcp::resolver m_resolver;
	int m_version;
	int m_method;
	std::string m_uname;
	std::string m_passwd;
	int m_command;
	int m_atyp;
	tcp::endpoint m_address;
	std::string m_domain;
	short m_port;
	bool m_verify_passed;
};

class socks_server : public boost::noncopyable
{
public:
	socks_server(boost::asio::io_service &io, short server_port)
		: m_io_service(io)
		, m_acceptor(io, tcp::endpoint(tcp::v4(), server_port))
	{
		boost::shared_ptr<socks_session> new_session(new socks_session(m_io_service));
		m_acceptor.async_accept(new_session->socket(),
			boost::bind(&socks_server::handle_accept, this, new_session,
			boost::asio::placeholders::error));
	}
	~socks_server() {}

public:

	void handle_accept(boost::shared_ptr<socks_session> new_session,
		const boost::system::error_code& error)
	{
		new_session->start();
		new_session.reset(new socks_session(m_io_service));
		m_acceptor.async_accept(new_session->socket(),
			boost::bind(&socks_server::handle_accept, this, new_session,
			boost::asio::placeholders::error));
	}

private:
	boost::asio::io_service &m_io_service;
	tcp::acceptor m_acceptor;
};

} // namespace socks

#endif // __SOCKS_SERVER_HPP__
