//
// detail/win_iocp_socket_accept_op.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2010 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_WIN_IOCP_SOCKET_ACCEPT_OP_HPP
#define ASIO_DETAIL_WIN_IOCP_SOCKET_ACCEPT_OP_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"

#if defined(ASIO_HAS_IOCP)

#include <boost/utility/addressof.hpp>
#include "asio/detail/bind_handler.hpp"
#include "asio/detail/buffer_sequence_adapter.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_invoke_helpers.hpp"
#include "asio/detail/operation.hpp"
#include "asio/detail/socket_ops.hpp"
#include "asio/detail/win_iocp_socket_service_base.hpp"
#include "asio/error.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

template <typename Socket, typename Protocol, typename Handler>
class win_iocp_socket_accept_op : public operation
{
public:
  ASIO_DEFINE_HANDLER_PTR(win_iocp_socket_accept_op);

  win_iocp_socket_accept_op(win_iocp_io_service& iocp_service,
      socket_type socket, Socket& peer, const Protocol& protocol,
      typename Protocol::endpoint* peer_endpoint,
      bool enable_connection_aborted, Handler handler)
    : operation(&win_iocp_socket_accept_op::do_complete),
      iocp_service_(iocp_service),
      socket_(socket),
      peer_(peer),
      protocol_(protocol),
      peer_endpoint_(peer_endpoint),
      enable_connection_aborted_(enable_connection_aborted),
      handler_(handler)
  {
  }

  socket_holder& new_socket()
  {
    return new_socket_;
  }

  void* output_buffer()
  {
    return output_buffer_;
  }

  DWORD address_length()
  {
    return sizeof(sockaddr_storage_type) + 16;
  }

  static void do_complete(io_service_impl* owner, operation* base,
      asio::error_code ec, std::size_t /*bytes_transferred*/)
  {
    // Take ownership of the operation object.
    win_iocp_socket_accept_op* o(static_cast<win_iocp_socket_accept_op*>(base));
    ptr p = { boost::addressof(o->handler_), o, o };

    if (owner)
    {
      // Map Windows error ERROR_NETNAME_DELETED to connection_aborted.
      if (ec.value() == ERROR_NETNAME_DELETED)
      {
        ec = asio::error::connection_aborted;
      }

      // Restart the accept operation if we got the connection_aborted error
      // and the enable_connection_aborted socket option is not set.
      if (ec == asio::error::connection_aborted
          && !o->enable_connection_aborted_)
      {
        // Reset OVERLAPPED structure.
        o->reset();

        // Create a new socket for the next connection, since the AcceptEx
        // call fails with WSAEINVAL if we try to reuse the same socket.
        o->new_socket_.reset();
        o->new_socket_.reset(socket_ops::socket(o->protocol_.family(),
              o->protocol_.type(), o->protocol_.protocol(), ec));
        if (o->new_socket_.get() != invalid_socket)
        {
          // Accept a connection.
          DWORD bytes_read = 0;
          BOOL result = ::AcceptEx(o->socket_, o->new_socket_.get(),
              o->output_buffer(), 0, o->address_length(),
              o->address_length(), &bytes_read, o);
          DWORD last_error = ::WSAGetLastError();
          ec = asio::error_code(last_error,
              asio::error::get_system_category());

          // Check if the operation completed immediately.
          if (!result && last_error != WSA_IO_PENDING)
          {
            if (last_error == ERROR_NETNAME_DELETED
                || last_error == WSAECONNABORTED)
            {
              // Post this handler so that operation will be restarted again.
              o->iocp_service_.work_started();
              o->iocp_service_.on_completion(o, ec);
              p.v = p.p = 0;
              return;
            }
            else
            {
              // Operation already complete. Continue with rest of this
              // handler.
            }
          }
          else
          {
            // Asynchronous operation has been successfully restarted.
            o->iocp_service_.work_started();
            o->iocp_service_.on_pending(o);
            p.v = p.p = 0;
            return;
          }
        }
      }

      // Get the address of the peer.
      typename Protocol::endpoint peer_endpoint;
      if (!ec)
      {
        LPSOCKADDR local_addr = 0;
        int local_addr_length = 0;
        LPSOCKADDR remote_addr = 0;
        int remote_addr_length = 0;
        GetAcceptExSockaddrs(o->output_buffer(), 0, o->address_length(),
            o->address_length(), &local_addr, &local_addr_length,
            &remote_addr, &remote_addr_length);
        if (static_cast<std::size_t>(remote_addr_length)
            > peer_endpoint.capacity())
        {
          ec = asio::error::invalid_argument;
        }
        else
        {
          using namespace std; // For memcpy.
          memcpy(peer_endpoint.data(), remote_addr, remote_addr_length);
          peer_endpoint.resize(static_cast<std::size_t>(remote_addr_length));
        }
      }

      // Need to set the SO_UPDATE_ACCEPT_CONTEXT option so that getsockname
      // and getpeername will work on the accepted socket.
      if (!ec)
      {
        SOCKET update_ctx_param = o->socket_;
        socket_ops::state_type state = 0;
        socket_ops::setsockopt(o->new_socket_.get(), state,
              SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
              &update_ctx_param, sizeof(SOCKET), ec);
      }

      // If the socket was successfully accepted, transfer ownership of the
      // socket to the peer object.
      if (!ec)
      {
        o->peer_.assign(o->protocol_,
            typename Socket::native_type(
              o->new_socket_.get(), peer_endpoint), ec);
        if (!ec)
          o->new_socket_.release();
      }

      // Pass endpoint back to caller.
      if (o->peer_endpoint_)
        *o->peer_endpoint_ = peer_endpoint;
    }

    // Make a copy of the handler so that the memory can be deallocated before
    // the upcall is made. Even if we're not about to make an upcall, a
    // sub-object of the handler may be the true owner of the memory associated
    // with the handler. Consequently, a local copy of the handler is required
    // to ensure that any owning sub-object remains valid until after we have
    // deallocated the memory here.
    detail::binder1<Handler, asio::error_code> handler(o->handler_, ec);
    p.h = boost::addressof(handler.handler_);
    p.reset();

    // Make the upcall if required.
    if (owner)
    {
      asio::detail::fenced_block b;
      asio_handler_invoke_helpers::invoke(handler, handler.handler_);
    }
  }

private:
  win_iocp_io_service& iocp_service_;
  socket_type socket_;
  socket_holder new_socket_;
  Socket& peer_;
  Protocol protocol_;
  typename Protocol::endpoint* peer_endpoint_;
  unsigned char output_buffer_[(sizeof(sockaddr_storage_type) + 16) * 2];
  bool enable_connection_aborted_;
  Handler handler_;
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // defined(ASIO_HAS_IOCP)

#endif // ASIO_DETAIL_WIN_IOCP_SOCKET_ACCEPT_OP_HPP